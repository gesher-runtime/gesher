#!/usr/bin/env python3
"""Build and verify the isolated N28 shared-provider component.

The publisher compiler is read-only. Every ABI gets a private full supplier
build and explicitly links its import library, never the publisher static
winpthread archive. This does not select a production Gesher provider.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tarfile

ROOT = Path(__file__).resolve().parents[2]
HERE = Path(__file__).resolve().parent
PIN = json.loads((HERE / "inputs.json").read_text())
REL = "mingw-w64-libraries/winpthreads"
LLVM_NAME = "llvm-mingw-20261006-ucrt-ubuntu-22.04-x86_64"
ABI = {"x86_64": "IMAGE_FILE_MACHINE_AMD64", "aarch64": "IMAGE_FILE_MACHINE_ARM64"}


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def require(condition, message):
    if not condition:
        raise ValueError(message)


def run(argv, cwd, log, env=None):
    argv = [str(value) for value in argv]
    print("COMMAND " + json.dumps({"argv": argv, "cwd": str(cwd), "log": str(log)}), flush=True)
    with log.open("wb") as output:
        result = subprocess.run(argv, cwd=cwd, env=env, stdout=output, stderr=subprocess.STDOUT)
    if result.returncode:
        print(log.read_text(errors="replace")[-12000:], file=sys.stderr)
        result.check_returncode()


def extract_source(archive, destination):
    require(archive.stat().st_size == PIN["source_archive_bytes"] and
            digest(archive) == PIN["source_archive_sha256"], "pinned source archive mismatch")
    with tarfile.open(archive, "r:gz") as source:
        for member in source.getmembers():
            parts = Path(member.name).parts
            require(parts and parts[0] == "mingw-w64-" + PIN["upstream_commit"], "unexpected archive root")
            if len(parts) < 3 or parts[1:3] != ("mingw-w64-libraries", "winpthreads"):
                continue
            require(".." not in parts and not member.issym() and not member.islnk(), "unsafe supplier archive member")
            member.name = str(Path(*parts[1:]))
            source.extract(member, destination, filter="data")
    for name, expected in PIN["source_sha256"].items():
        require(digest(destination / name) == expected, "pinned source mismatch: " + name)


def imports(readobj, image):
    text = subprocess.check_output([str(readobj), "--coff-imports", str(image)], text=True)
    return sorted(re.findall(r"^  Name: (.+)$", text, re.MULTILINE)), text


def build(args):
    llvm = args.llvm.resolve()
    output = args.output.resolve()
    require(not output.exists(), "use a fresh output directory")
    require(llvm.name == LLVM_NAME, "unexpected publisher compiler name")
    require(digest(args.compiler_archive) == PIN["compiler_archive_sha256"] and
            args.compiler_archive.stat().st_size == PIN["compiler_archive_bytes"], "publisher compiler archive mismatch")
    # Bind the actual compiler/includes/libraries to the verified archive,
    # so pointing --llvm at a patched compiler cannot launder the receipt.
    with tarfile.open(args.compiler_archive, "r:xz") as archive:
        expected = {}
        for member in archive.getmembers():
            relative = Path(member.name).relative_to(LLVM_NAME)
            if member.issym():
                require((llvm / relative).is_symlink() and os.readlink(llvm / relative) == member.linkname,
                        "publisher compiler symlink mismatch: " + str(relative))
            if not member.isfile():
                continue
            require(not (llvm / relative).is_symlink(), "publisher regular file replaced by symlink")
            expected[str(relative)] = hashlib.sha256(archive.extractfile(member).read()).hexdigest()
        for name, value in expected.items():
            require(digest(llvm / name) == value, "publisher compiler file mismatch: " + name)
    output.mkdir(parents=True)
    source = output / "source"
    source.mkdir()
    extract_source(args.source_archive, source)
    source_before = {str(p.relative_to(source)): digest(p) for p in (source / REL).rglob("*") if p.is_file()}
    require(digest(HERE / "supplier.patch") == PIN["patch_sha256"], "supplier patch pin mismatch")
    run(["git", "apply", "--check", HERE / "supplier.patch"], source, output / "patch-check.log")
    run(["git", "apply", HERE / "supplier.patch"], source, output / "patch.log")
    for name, expected_hash in PIN["output_source_sha256"].items():
        require(digest(source / name) == expected_hash, "patched supplier mismatch: " + name)
    source_after = {str(p.relative_to(source)): digest(p) for p in (source / REL).rglob("*") if p.is_file()}
    changed = {name for name in source_before if source_before[name] != source_after[name]}
    require(changed == set(PIN["output_source_sha256"]), "unplanned supplier source delta")
    commit = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=ROOT, text=True).strip()
    sources = [HERE / "component.py", HERE / "inputs.json", HERE / "supplier.patch"]
    sources += sorted((ROOT / "tests/thread-cpu-clock-shared").glob("*"))
    recipe = {str(p.relative_to(ROOT)): digest(p) for p in sources if p.is_file()}
    dirty = subprocess.check_output(["git", "status", "--porcelain", "--untracked-files=normal"], cwd=ROOT, text=True)
    if os.environ.get("GITHUB_ACTIONS") == "true":
        require(not dirty and os.environ.get("GITHUB_SHA") == commit, "CI source must be clean and match GITHUB_SHA")
    for abi in args.abis:
        triple = abi + "-w64-mingw32"
        role = output / abi
        role.mkdir()
        build_dir = role / "build"
        build_dir.mkdir()
        install = role / "supplier"
        env = dict(os.environ, PATH=str(llvm / "bin") + os.pathsep + os.environ["PATH"])
        cc = llvm / "bin" / (triple + "-clang")
        cxx = llvm / "bin" / (triple + "-clang++")
        flags = "-g -O2 -mguard=cf -Werror"
        configure = [source / REL / "configure", "--host=" + triple, "--prefix=" + str(install),
                     "--enable-shared", "--disable-static", "CFLAGS=" + flags, "CXXFLAGS=" + flags,
                     "CC=" + str(cc), "CXX=" + str(cxx), "AR=" + str(llvm / "bin/llvm-ar"),
                     "RANLIB=" + str(llvm / "bin/llvm-ranlib"), "RC=" + str(llvm / "bin" / (triple + "-windres"))]
        run(configure, build_dir, role / "configure.log", env)
        # Upstream libtool's -Wc,-no-pthread is GCC-only; Clang injects no
        # winpthread without -pthread. Preserve every other upstream flag.
        ldflags = "libwinpthread_la_LDFLAGS=-no-undefined -version-info 1:0:0 -L$(builddir)/fakelib"
        run(["make", "-j2", "V=1", ldflags], build_dir, role / "build.log", env)
        run(["make", "install", ldflags], build_dir, role / "install.log", env)
        fixture = role / "fixture"
        fixture.mkdir()
        dll = install / "bin/libwinpthread-1.dll"
        import_library = install / "lib/libwinpthread.dll.a"
        require(dll.exists() and import_library.exists() and not (install / "lib/libwinpthread.a").exists(),
                "shared-only supplier outputs required")
        shutil.copy2(dll, fixture / dll.name)
        shutil.copy2(source / REL / "COPYING", fixture / "COPYING.winpthreads.txt")
        fixture_sources = ROOT / "tests/thread-cpu-clock-shared"
        common = ["-O2", "-Wall", "-Wextra", "-Werror", "-D_WIN32_WINNT=0x0A00",
                  "-DWINPTHREADS_USE_DLLIMPORT=1", "-I" + str(install / "include")]
        peer_command = [cc, "-std=c11", *common, "-shared", fixture_sources / "peer.c", import_library,
                        "-Wl,--verbose", "-Wl,-Map," + str(role / "peer.map"),
                        "-o", fixture / "clock-peer.dll"]
        host_command = [cxx, "-std=c++20", *common, "-static-libstdc++", "-static-libgcc",
                        fixture_sources / "windows.cc", import_library, "-Wl,--verbose", "-Wl,-Map," + str(role / "host.map"),
                        "-o", fixture / "clock-shared.exe"]
        run(peer_command, ROOT, role / "peer-build.log", env)
        run(host_command, ROOT, role / "host-build.log", env)
        for name in ["peer-build.log", "host-build.log"]:
            linked = (role / name).read_text()
            require("Reading " + str(import_library) in linked and
                    not re.search(r"(?:^|[/\s])lib(?:win)?pthread\.a(?:[\s(]|$)", linked, re.MULTILINE),
                    "a consumer embedded a static pthread provider")
        binary = {}
        readobj = llvm / "bin/llvm-readobj"
        for image in [fixture / "clock-shared.exe", fixture / "clock-peer.dll", fixture / dll.name]:
            names, raw = imports(readobj, image)
            (role / (image.name + ".imports.txt")).write_text(raw)
            header = subprocess.check_output([str(readobj), "--file-headers", str(image)], text=True)
            require(ABI[abi] in header, "wrong output ABI: " + image.name)
            if image.name != dll.name:
                require(names.count(dll.name) == 1, "consumer must import the one shared supplier")
                provider_imports = next(block for block in raw.split("Import {") if "Name: " + dll.name in block)
                for symbol in ["pthread_self", "pthread_getcpuclockid", "clock_gettime64", "clock_getres64"]:
                    require("Symbol: " + symbol + " " in provider_imports, "public clock API does not import from shared supplier")
            else:
                require(not any("pthread" in name.lower() for name in names), "supplier must not import itself")
            binary[image.name] = {"sha256": digest(image), "imports": names, "machine": ABI[abi]}
        exports = subprocess.check_output([str(readobj), "--coff-exports", str(fixture / dll.name)], text=True)
        (role / "supplier-exports.txt").write_text(exports)
        for symbol in ["pthread_getcpuclockid", "gesher_winpthread_dup_cpu_handle_v1", "__pth_gpointer_locked", "clock_gettime", "clock_gettime32",
                       "clock_gettime64", "clock_getres", "clock_getres32", "clock_getres64"]:
            require(re.search(r"^  Name: " + symbol + r"$", exports, re.MULTILINE), "missing literal export: " + symbol)
        require("gesher_clock_anchor" not in exports and "gesher_pthread_owner" not in exports,
                "whole-provider routing prototype is outside this component")
        metadata = {"schema": "gesher-thread-cpu-clock-shared-v1", "abi": abi, "commit": commit,
                    "dirty_local_source": bool(dirty), "recipe": recipe, "pin": PIN,
                    "context": {name: os.environ.get(name) for name in ["GITHUB_REPOSITORY", "GITHUB_SHA", "GITHUB_RUN_ID", "GITHUB_RUN_ATTEMPT"]},
                    "source_before": source_before, "source_after": source_after,
                    "compiler_driver": {"c": digest(cc), "cxx": digest(cxx), "clang": digest(llvm / "bin/clang")},
                    "commands": {"configure": [str(x) for x in configure], "peer": [str(x) for x in peer_command],
                                 "host": [str(x) for x in host_command]},
                    "import_library_sha256": digest(import_library), "binaries": binary,
                    "members": {p.name: digest(p) for p in fixture.iterdir() if p.is_file()}}
        (fixture / "manifest.json").write_text(json.dumps(metadata, indent=2) + "\n")
        verify(fixture)
        print("BUILT " + abi + " " + str(fixture), flush=True)


def verify(directory):
    directory = Path(directory)
    data = json.loads((directory / "manifest.json").read_text())
    require(data["schema"] == "gesher-thread-cpu-clock-shared-v1" and data["abi"] in ABI, "manifest type/ABI")
    require(data["pin"] == PIN, "manifest supplier identity")
    require(set(p.name for p in directory.iterdir()) == set(data["members"]) | {"manifest.json"}, "fixture member set")
    require(set(data["members"]) == {"clock-shared.exe", "clock-peer.dll", "libwinpthread-1.dll", "COPYING.winpthreads.txt"},
            "unexpected component payload")
    for name, expected_hash in data["members"].items():
        require(digest(directory / name) == expected_hash, "fixture hash mismatch: " + name)
    for name, expected_hash in data["recipe"].items():
        require(digest(ROOT / name) == expected_hash, "consumer source mismatch: " + name)
    if os.environ.get("GITHUB_ACTIONS") == "true":
        require(not data["dirty_local_source"], "dirty source cannot qualify in CI")
        for name, expected in data["context"].items():
            require(expected is not None and expected == os.environ.get(name), "producer/consumer context mismatch: " + name)
    print("VERIFIED " + data["abi"] + " " + data["commit"], flush=True)
    return data


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    compile_parser = commands.add_parser("build")
    for name in ["llvm", "compiler-archive", "source-archive", "output"]:
        compile_parser.add_argument("--" + name, type=Path, required=True)
    compile_parser.add_argument("--abis", nargs="+", choices=ABI, default=list(ABI))
    check = commands.add_parser("verify")
    check.add_argument("directory", type=Path)
    args = parser.parse_args()
    if args.command == "build":
        build(args)
    else:
        verify(args.directory)


if __name__ == "__main__":
    main()
