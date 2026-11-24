#!/usr/bin/env python3
# Copyright (C) 2026 The Gesher Authors
# SPDX-License-Identifier: Apache-2.0
"""Build separately distributed, unmodified upstream Android filesystem tools."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import stat
import struct
import subprocess
import sys
import tarfile
import tempfile
import urllib.request
import zipfile


HERE = Path(__file__).resolve().parent
REPO = HERE.parent.parent
MANIFEST = HERE / "sources.json"
TOOLS = {
    "mkfs.fat": ("dosfstools", "src/mkfs.fat"),
    "fsck.fat": ("dosfstools", "src/fsck.fat"),
    "mkfs.exfat": ("exfatprogs", "mkfs/mkfs.exfat"),
    "fsck.exfat": ("exfatprogs", "fsck/fsck.exfat"),
    "mke2fs": ("e2fsprogs", "misc/mke2fs"),
    "e2fsck": ("e2fsprogs", "e2fsck/e2fsck"),
}


def digest(path: Path) -> str:
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def require(condition: bool, message: str) -> None:
    if not condition:
        raise RuntimeError(message)


def write_json(path: Path, value: object) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def authenticate(path: Path, spec: dict) -> None:
    require(path.is_file() and not path.is_symlink(), f"Not an ordinary archive: {path}")
    require(path.stat().st_size == spec["bytes"], f"Archive length mismatch: {path}")
    require(digest(path) == spec["sha256"], f"Archive SHA256 mismatch: {path}")


def fetch(cache: Path, spec: dict) -> Path:
    path = cache / spec["filename"]
    if not path.exists():
        temporary = path.with_name(path.name + f".download-{os.getpid()}")
        try:
            request = urllib.request.Request(spec["url"], headers={"User-Agent": "Gesher-source-builder/1"})
            with urllib.request.urlopen(request, timeout=120) as response, temporary.open("xb") as output:
                require(response.url.startswith("https://"), "Source download left HTTPS")
                shutil.copyfileobj(response, output)
            authenticate(temporary, spec)
            temporary.replace(path)
        finally:
            temporary.unlink(missing_ok=True)
    authenticate(path, spec)
    return path


def validate_ndk(ndk: Path, archive: Path, spec: dict) -> dict:
    """A valid cache archive alone cannot authenticate an already extracted compiler."""
    authenticate(archive, spec)
    root = spec["directory"] + "/"
    subtree = "toolchains/llvm/prebuilt/linux-x86_64/"
    expected_files: set[str] = set()
    tree_hash = hashlib.sha256()
    checked = 0
    with zipfile.ZipFile(archive) as supplier:
        for entry in supplier.infolist():
            require(entry.filename.startswith(root), "Unexpected NDK archive root")
            relative = entry.filename[len(root):]
            if not relative.startswith(subtree) and relative not in {
                "source.properties", "NOTICE", "NOTICE.toolchain"
            }:
                continue
            path = ndk / relative
            mode = entry.external_attr >> 16
            if entry.is_dir():
                require(path.is_dir() and not path.is_symlink(), f"Changed NDK directory: {relative}")
                continue
            expected_files.add(relative)
            if stat.S_ISLNK(mode):
                target = supplier.read(entry).decode("utf-8")
                require(path.is_symlink() and os.readlink(path) == target,
                        f"Changed NDK symlink: {relative}")
                require(path.resolve().is_relative_to(ndk), f"NDK symlink escapes its tree: {relative}")
                file_hash = hashlib.sha256(target.encode()).hexdigest()
            else:
                require(path.is_file() and not path.is_symlink(), f"Missing/changed NDK file: {relative}")
                require(path.stat().st_size == entry.file_size, f"Changed NDK file size: {relative}")
                checksum = hashlib.sha256()
                with supplier.open(entry) as official, path.open("rb") as extracted:
                    while block := official.read(1024 * 1024):
                        require(extracted.read(len(block)) == block, f"Changed NDK contents: {relative}")
                        checksum.update(block)
                    require(not extracted.read(1), f"Changed NDK length: {relative}")
                if mode & 0o111:
                    require(os.access(path, os.X_OK), f"NDK tool is not executable: {relative}")
                file_hash = checksum.hexdigest()
            tree_hash.update(f"{relative}\0{file_hash}\n".encode())
            checked += 1
    actual_files = {
        str(path.relative_to(ndk)) for path in (ndk / subtree).rglob("*")
        if path.is_symlink() or not path.is_dir()
    }
    require(actual_files == {name for name in expected_files if name.startswith(subtree)},
            "NDK toolchain contains files absent from its authenticated archive")
    require(f"Pkg.Revision = {spec['revision']}" in (ndk / "source.properties").read_text(),
            "Unexpected NDK revision")
    return {"archive_sha256": spec["sha256"], "revision": spec["revision"],
            "verified_files": checked, "verified_tree_sha256": tree_hash.hexdigest()}


def run(command: list[str], cwd: Path, environment: dict, log: Path) -> None:
    with log.open("a", encoding="utf-8") as stream:
        stream.write(json.dumps(command) + "\n")
        stream.flush()
        result = subprocess.run(command, cwd=cwd, env=environment, stdout=stream,
                                stderr=subprocess.STDOUT, check=False)
    if result.returncode:
        tail = "\n".join(log.read_text(errors="replace").splitlines()[-70:])
        raise RuntimeError(f"Command failed ({result.returncode}), log {log}:\n{tail}")


def registrations(repo: Path) -> tuple[set[str], dict[str, str]]:
    """Conservative source audit; Windows loader execution remains a separate gate."""
    registered: set[str] = set()
    inputs: dict[str, str] = {}
    paths = sorted((repo / "compat/bionic").glob("*.cc"))
    paths.append(repo / "compat/elf/exec/gesher_exec.cc")
    for path in paths:
        text = path.read_text(encoding="utf-8")
        # Registration macros in the current Bionic translation units and the
        # executable's startup overrides. No substring/all-string acceptance.
        registered.update(re.findall(r'\b(?:ENTRY|E|L|ADD|ADDSYM|AddBionicSymbol)\("([^"]+)"', text))
        registered.update(re.findall(r'\bt\["([^"]+)"\]', text))
        registered.update(re.findall(r'\b(?:SELF|P|MATH|MATHF|MATH2|MATH2F)\((\w+)\)', text))
        inputs[str(path.relative_to(repo))] = digest(path)
    return registered, inputs


def inspect_elf(path: Path, readelf: Path, symbols: set[str]) -> dict:
    data = path.read_bytes()
    require(len(data) >= 64 and data[:6] == b"\x7fELF\x02\x01", f"Not ELF64 little-endian: {path}")
    kind, machine = struct.unpack_from("<HH", data, 16)
    require(kind == 3 and machine == 62, f"Not x86-64 ET_DYN/PIE: {path}")
    offset = struct.unpack_from("<Q", data, 32)[0]
    size, count = struct.unpack_from("<HH", data, 54)
    require(size == 56 and count > 0 and offset + size * count <= len(data), f"Bad ELF headers: {path}")
    interpreters = []
    for index in range(count):
        start = offset + index * size
        if struct.unpack_from("<I", data, start)[0] == 3:
            at = struct.unpack_from("<Q", data, start + 8)[0]
            length = struct.unpack_from("<Q", data, start + 32)[0]
            require(at + length <= len(data), f"Invalid ELF interpreter: {path}")
            interpreters.append(data[at:at + length])
    require(interpreters == [b"/system/bin/linker64\0"], f"Not an Android dynamic executable: {path}")
    result = subprocess.check_output([str(readelf), "--dyn-syms", "--wide", "--dynamic-table",
                                      "--relocations", str(path)], text=True)
    imports = sorted({name.split("@", 1)[0] for name in re.findall(r"\bUND[ \t]+([^\s]+)", result)})
    needed = re.findall(r"NEEDED.*?\[([^]]+)\]", result)
    require(needed == ["libc.so"], f"Unexpected dynamic dependencies for {path.name}: {needed}")
    require("RUNPATH" not in result and "RPATH" not in result, f"Unexpected loader search path: {path}")
    relocations = sorted(set(re.findall(r"\bR_X86_64_\w+", result)))
    supported = {"R_X86_64_RELATIVE", "R_X86_64_64", "R_X86_64_GLOB_DAT", "R_X86_64_JUMP_SLOT"}
    require(set(relocations) <= supported, f"Unreviewed executable relocations: {relocations}")
    missing = sorted(set(imports) - symbols)
    require(not missing, f"Unregistered Gesher runtime imports in {path.name}: {missing}")
    return {"bytes": len(data), "sha256": digest(path), "machine": "EM_X86_64", "type": "ET_DYN",
            "interpreter": "/system/bin/linker64", "needed": needed, "imports": imports,
            "relocations": relocations, "runtime_executed": False}


def file_inventory(root: Path) -> dict:
    inventory = {}
    for path in sorted(root.rglob("*")):
        require(not path.is_symlink(), f"Unexpected package symlink: {path}")
        if path.is_dir():
            continue
        require(stat.S_ISREG(path.stat().st_mode), f"Unexpected package file type: {path}")
        inventory[str(path.relative_to(root))] = {"sha256": digest(path), "bytes": path.stat().st_size}
    return inventory


def verify_bundle(bundle: Path, readelf: Path, symbols: set[str]) -> None:
    receipt = json.loads((bundle / "receipts/build.json").read_text())
    require(receipt["schema"] == 1, "Unknown filesystem tool receipt schema")
    require(receipt["recipe_sha256"] == digest(HERE / "build.py") and
            receipt["manifest_sha256"] == digest(MANIFEST), "Bundle is from another recipe/manifest")
    inventory = file_inventory(bundle)
    inventory.pop("receipts/build.json")
    require(inventory == receipt["files"], "Packaged filesystem tool file integrity mismatch")
    for name in TOOLS:
        inspect_elf(bundle / "bin" / name, readelf, symbols)
    print("FILESYSTEM_TOOLS_VERIFIED six Android x86_64 PIE programs; Windows execution not implied")


def build(args: argparse.Namespace) -> None:
    manifest = json.loads(MANIFEST.read_text())
    repo = args.gesher_source.resolve(strict=True)
    require((repo / "compat/bionic/shim.cc").is_file(), "--gesher-source must name a Gesher checkout")
    work = args.work.resolve()
    output = args.output.resolve()
    for directory in (work, output):
        require(not directory.is_relative_to(repo), f"Upstream artifacts must stay outside checkout: {directory}")
        directory.mkdir(parents=True, exist_ok=True)
    cache = work / "downloads"
    cache.mkdir(exist_ok=True)
    ndk = args.ndk.resolve(strict=True)
    archive = args.ndk_archive or ndk.parent / manifest["ndk"]["filename"]
    if not archive.exists():
        archive = fetch(cache, manifest["ndk"])
    print("[filesystem-tools] authenticating NDK archive and extracted toolchain", flush=True)
    ndk_receipt = validate_ndk(ndk, archive, manifest["ndk"])
    compiler_bin = ndk / "toolchains/llvm/prebuilt/linux-x86_64/bin"
    compiler = compiler_bin / (manifest["target"] + "-clang")
    readelf = compiler_bin / "llvm-readelf"
    bundle = output / "filesystem-tools"
    if args.verify_only:
        symbols, _ = registrations(repo)
        verify_bundle(bundle, readelf, symbols)
        return
    require(not bundle.exists(), f"Output exists; use --verify-only or choose an empty output: {bundle}")
    archives = {}
    provenance = {}
    for package in manifest["packages"]:
        archives[package["name"]] = fetch(cache, package)
        provenance[package["name"]] = fetch(cache, package["provenance"])
    # Always build from freshly authenticated sources. Build trees are never
    # considered trusted cache entries, and are kept for failure diagnostics.
    run_root = Path(tempfile.mkdtemp(prefix="build-", dir=work))
    sources = run_root / "sources"
    sources.mkdir()
    builds = run_root / "build"
    builds.mkdir()
    logs = run_root / "logs"
    logs.mkdir()
    source_dirs = {}
    for package in manifest["packages"]:
        with tarfile.open(archives[package["name"]]) as source:
            source.extractall(sources, filter="data")
        directory = sources / package["directory"]
        require(directory.is_dir(), "Unexpected source archive layout")
        source_dirs[package["name"]] = directory
    stage = run_root / "stage"
    iconv_stage = run_root / "iconv-stage"
    environment = {"PATH": "/usr/bin:/bin", "LANG": "C", "LC_ALL": "C",
                   "SOURCE_DATE_EPOCH": str(manifest["source_date_epoch"]),
                   "CC": str(compiler), "AR": str(compiler_bin / "llvm-ar"),
                   "RANLIB": str(compiler_bin / "llvm-ranlib"),
                   "STRIP": str(compiler_bin / "llvm-strip"), "BUILD_CC": "cc",
                   "CFLAGS": "-O2 -fPIE -fstack-protector-strong -D_FILE_OFFSET_BITS=64 " +
                             f"-ffile-prefix-map={run_root}=/filesystem-tools-build",
                   "LDFLAGS": "-pie -Wl,--as-needed,-z,relro,-z,now",
                   "PKG_CONFIG_LIBDIR": str(run_root / "empty-pkgconfig")}
    configurations = {}

    def configure(package: str, options: list[str], env: dict) -> Path:
        directory = builds / package
        directory.mkdir()
        command = [str(source_dirs[package] / "configure"), "--host=x86_64-linux-android",
                   "--prefix=/system", *options]
        print(f"[filesystem-tools] configuring {package}", flush=True)
        configurations[package] = {"options": command[1:], "cflags": env["CFLAGS"],
                                   "ldflags": env["LDFLAGS"]}
        run(command, directory, env, logs / (package + ".log"))
        return directory

    def make(package: str, arguments: list[str], env: dict) -> None:
        print(f"[filesystem-tools] building {package}: {' '.join(arguments)}", flush=True)
        run(["make", f"-j{args.jobs}", *arguments], builds / package, env,
            logs / (package + ".log"))

    iconv_env = dict(environment, CFLAGS=environment["CFLAGS"].replace("-fPIE", "-fPIC"))
    configure("libiconv", ["--disable-shared", "--enable-static", "--with-pic", "--disable-nls"], iconv_env)
    make("libiconv", [], iconv_env)
    make("libiconv", ["install", f"DESTDIR={iconv_stage}"], iconv_env)
    iconv_prefix = iconv_stage / "system"
    dos_env = dict(environment, CPPFLAGS=f"-I{iconv_prefix / 'include'}")
    configure("dosfstools", [f"--with-libiconv-prefix={iconv_prefix}", "--disable-rpath"], dos_env)
    make("dosfstools", ["-C", "src", "mkfs.fat", "fsck.fat"], dos_env)
    # These options omit unrelated frontends/message catalogs, not filesystem
    # check passes, repair support, journals, metadata checksums or MMP.
    configure("e2fsprogs", ["--enable-libuuid", "--enable-libblkid", "--disable-elf-shlibs",
              "--disable-nls", "--disable-fuse2fs", "--disable-debugfs", "--disable-imager",
              "--disable-resizer", "--disable-defrag", "--disable-uuidd", "--disable-e2initrd-helper",
              "--disable-testio-debug", "--without-libarchive"], environment)
    make("e2fsprogs", ["libs"], environment)
    make("e2fsprogs", ["-C", "misc", "mke2fs"], environment)
    make("e2fsprogs", ["-C", "e2fsck", "e2fsck"], environment)
    for subdir in ("lib/uuid", "lib/blkid"):
        make("e2fsprogs", ["-C", subdir, "install", f"DESTDIR={stage}"], environment)
    include = stage / "system/include"
    libraries = stage / "system/lib"
    exfat_env = dict(environment, PKG_CONFIG_LIBDIR=str(libraries / "pkgconfig"),
                     PKG_CONFIG_SYSROOT_DIR=str(stage),
                     EXT2_BLKID_CFLAGS=f"-I{include / 'blkid'} -I{include}",
                     EXT2_BLKID_LIBS=f"{libraries / 'libblkid.a'} {libraries / 'libuuid.a'}")
    configure("exfatprogs", ["--disable-shared", "--enable-static"], exfat_env)
    make("exfatprogs", ["-C", "lib"], exfat_env)
    # Upstream configure selects HAVE_EXT2_BLKID; upstream mkfs/Makefile uses
    # the common BLKID_* names even in that branch. Supply its original inputs.
    make("exfatprogs", ["-C", "mkfs", "BLKID_CFLAGS=" + exfat_env["EXT2_BLKID_CFLAGS"],
                        "BLKID_LIBS=" + exfat_env["EXT2_BLKID_LIBS"]], exfat_env)
    make("exfatprogs", ["-C", "fsck"], exfat_env)
    symbols, symbol_sources = registrations(repo)
    elf_info = {name: inspect_elf(builds / package / relative, readelf, symbols)
                for name, (package, relative) in TOOLS.items()}
    # Publish only after every binary passes the architecture/dependency/import
    # gate. Keep a sibling staging directory so a failed build is not packaged.
    temporary = Path(tempfile.mkdtemp(prefix=".filesystem-tools-", dir=output))
    try:
        for directory in ("bin", "sources", "licenses", "receipts"):
            (temporary / directory).mkdir()
        for name, (package, relative) in TOOLS.items():
            shutil.copy2(builds / package / relative, temporary / "bin" / name)
        for package in manifest["packages"]:
            name = package["name"]
            for archive_file in (archives[name], provenance[name]):
                shutil.copy2(archive_file, temporary / "sources" / archive_file.name)
            for notice in package["license_files"]:
                destination = temporary / "licenses" / package["directory"] / notice
                destination.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(source_dirs[name] / notice, destination)
        for name in ("build.py", "sources.json", "README.md"):
            shutil.copy2(HERE / name, temporary / "sources" / name)
        for name in ("NOTICE", "NOTICE.toolchain"):
            destination = temporary / "licenses" / manifest["ndk"]["directory"] / name
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(ndk / name, destination)
        shutil.copytree(logs, temporary / "receipts/logs")
        receipt = {"schema": 1, "manifest_sha256": digest(MANIFEST),
                   "recipe_sha256": digest(HERE / "build.py"), "ndk": ndk_receipt,
                   "compiler": subprocess.check_output([str(compiler), "--version"], text=True),
                   "target": manifest["target"], "source_date_epoch": manifest["source_date_epoch"],
                   "source_patches": [], "configurations": configurations, "programs": elf_info,
                   "runtime_registration_sources": symbol_sources, "runtime_executed": False,
                   "files": file_inventory(temporary)}
        write_json(temporary / "receipts/build.json", receipt)
        temporary.replace(bundle)
    finally:
        if temporary.exists():
            shutil.rmtree(temporary)
    verify_bundle(bundle, readelf, symbols)
    print(f"FILESYSTEM_TOOLS_BUILT {bundle}")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--ndk", type=Path, required=True, help="Extracted official Android NDK r27c")
    parser.add_argument("--ndk-archive", type=Path, help="Pinned publisher ZIP; defaults to NDK sibling")
    parser.add_argument("--work", type=Path, required=True, help="External source cache and fresh build trees")
    parser.add_argument("--output", type=Path, required=True, help="External artifact root")
    parser.add_argument("--jobs", type=int, default=4)
    parser.add_argument("--gesher-source", type=Path, default=REPO,
                        help="Gesher source for import audit; required when using the packaged recipe")
    parser.add_argument("--verify-only", action="store_true")
    args = parser.parse_args()
    require(1 <= args.jobs <= 64, "--jobs must be between 1 and 64")
    build(args)


if __name__ == "__main__":
    try:
        main()
    except (RuntimeError, OSError, subprocess.SubprocessError, tarfile.TarError, zipfile.BadZipFile) as error:
        print(f"filesystem-tools: {error}", file=sys.stderr)
        sys.exit(1)
