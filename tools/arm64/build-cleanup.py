#!/usr/bin/env python3
# Copyright (C) 2026 The Gesher Authors
# SPDX-License-Identifier: Apache-2.0
"""Build the isolated pinned native cleanup qualification; no guest runtime."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import tarfile

ROOT = Path(__file__).resolve().parents[2]
NAME = "llvm-mingw-20261006-ucrt-ubuntu-22.04-x86_64"
ARCHIVE_SHA = "5f9c6ed95b2d4bdb2869a488c5fd5857fbdabcf288a0aa3eb1da43f6a08d8ab4"
LLVM_COMMIT = "0d261d1ca552c95a8f007e061c787ac7132fbcbc"
URL = f"https://github.com/mstorsjo/llvm-mingw/releases/download/20261006/{NAME}.tar.xz"
RUNTIME_HASHES = {
    # llvm-mingw's static libc++.a includes the libc++abi personality objects.
    "x86_64-w64-mingw32/lib/libc++.a":
        "63179bb71d2f8f56f422890454b7b2ba0ff07cc6ec6b87bf309166cef92220da",
    "x86_64-w64-mingw32/lib/libunwind.a":
        "a117299985922ea893d6af181d9d8b0ae55cedb2bd32a0d03c30eb64f7becebe",
    "x86_64-w64-mingw32/lib/libc++abi.a":
        "4a6e5f21c077f22659fcaa423d44bfd16ef47556b59b45483d5de9faa724ab6a",
    "x86_64-w64-mingw32/include/unwind.h":
        "e4a58637fc8ca1d29cd600c3c4416aafed50085edf276fa076214c8d81c263f0",
    "x86_64-w64-mingw32/include/unwind_itanium.h":
        "43ad2c8ca672c8fb9a4eb80d7e05a687ae9134f4899641357c86cecde4efbd1f",
}
FIXTURE = "tests/native/host_callback_cleanup"
SOURCES = [
    "compat/arm64/dll/thunks.S", "compat/arm64/dll/host_call.h",
    "compat/arm64/dll/CMakeLists.txt",
    *[f"{FIXTURE}/{name}" for name in (
        "cleanup_probe.h", "cleanup_main.cc", "cleanup_dll.cc",
        "cleanup_c_bridge.c", "cleanup_checkpoint.S")],
    "tools/arm64/build-cleanup.py", "tools/arm64/run-cleanup.ps1",
    ".github/workflows/native-callback-cleanup.yml",
    "docs/research/native-callback-cleanup-2026-11-17.md",
]


def sha(path):
    with Path(path).open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def require(ok, message):
    if not ok:
        raise RuntimeError(message)


def git(*args):
    return subprocess.check_output(["git", *args], cwd=ROOT, text=True).strip()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--toolchain", type=Path, required=True)
    parser.add_argument("--archive", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    toolchain, out = args.toolchain.resolve(), args.out.resolve()
    require(sha(args.archive) == ARCHIVE_SHA, "compiler archive checksum mismatch")
    require(not out.exists() or not any(out.iterdir()), "use an empty output directory")
    out.mkdir(parents=True, exist_ok=True)
    compiler = toolchain / "bin/x86_64-w64-mingw32-clang++"
    c_compiler = toolchain / "bin/x86_64-w64-mingw32-clang"
    readobj, nm = toolchain / "bin/llvm-readobj", toolchain / "bin/llvm-nm"

    # Verify the executables actually used and the relevant static runtimes
    # against members of the digest-pinned official archive, including symlinks.
    paths = [compiler, c_compiler, readobj, nm, toolchain / "bin/clang",
             toolchain / "bin/ld.lld", *[toolchain / p for p in RUNTIME_HASHES]]
    members = {}
    for path in paths:
        relative = path.resolve().relative_to(toolchain)
        members[f"{NAME}/{relative.as_posix()}"] = path
    verified = {}
    with tarfile.open(args.archive, "r|xz") as archive:
        for member in archive:
            if member.name not in members:
                continue
            require(member.isfile(), f"expected regular archive member: {member.name}")
            with archive.extractfile(member) as stream:
                expected = hashlib.file_digest(stream, "sha256").hexdigest()
            path = members[member.name]
            actual = sha(path)
            require(actual == expected, f"installed tool/runtime differs: {path}")
            verified[str(path.relative_to(toolchain))] = {
                "archive_member": member.name, "sha256": actual,
            }
    require(len(verified) == len(members), "missing selected official archive members")
    for name, expected in RUNTIME_HASHES.items():
        require(sha(toolchain / name) == expected, f"pinned runtime mismatch: {name}")

    ci = {key: os.environ.get(key) for key in (
        "GITHUB_SHA", "GITHUB_RUN_ID", "GITHUB_RUN_ATTEMPT", "GITHUB_REPOSITORY",
        "GITHUB_REPOSITORY_ID", "GITHUB_WORKFLOW", "GITHUB_JOB")}
    commit, tree = git("rev-parse", "HEAD"), git("rev-parse", "HEAD^{tree}")
    dirty = bool(git("status", "--porcelain"))
    if os.environ.get("GITHUB_ACTIONS") == "true":
        require(ci["GITHUB_SHA"] == commit and not dirty, "CI source must be exact and clean")
    commands = []

    def run(argv, log):
        argv = [str(arg) for arg in argv]
        result = subprocess.run(argv, cwd=ROOT, text=True, stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT, timeout=120)
        (out / log).write_text(result.stdout)
        commands.append({"argv": argv, "cwd": str(ROOT), "exit_code": result.returncode,
                         "output": log})
        require(result.returncode == 0, f"{log}: {result.stdout}")
        return result.stdout

    version = run([compiler, "--version"], "compiler-version.txt")
    require("clang version 23.1.3" in version and LLVM_COMMIT in version,
            "unexpected compiler identity")
    flags = ["-O2", "-Wall", "-Wextra", "-Werror"]
    run([c_compiler, "-std=c11", *flags, "-fms-extensions", "-c",
         ROOT / FIXTURE / "cleanup_c_bridge.c", "-o", out / "bridge.obj"], "compile-bridge.txt")
    for source, obj in (("cleanup_main.cc", "main"), ("cleanup_dll.cc", "helper")):
        run([compiler, "-std=c++20", *flags, "-c", ROOT / FIXTURE / source,
             "-o", out / f"{obj}.obj"], f"compile-{obj}.txt")
    for source, obj in ((f"{FIXTURE}/cleanup_checkpoint.S", "checkpoint"),
                        ("compat/arm64/dll/thunks.S", "thunks")):
        run([compiler, "-c", ROOT / source, "-o", out / f"{obj}.obj"], f"compile-{obj}.txt")
    for obj in ("main", "helper", "bridge", "checkpoint", "thunks"):
        info = run([readobj, "--unwind", out / f"{obj}.obj"], f"{obj}-unwind.txt")
        if obj in ("main", "helper"):
            require("Handler: __gxx_personality_seh0" in info,
                    f"{obj}: missing real LLVM C++ cleanup personality")
        elif obj == "bridge":
            require("Handler: __C_specific_handler" in info,
                    "foreign characterization lacks the real C SEH personality")
        elif obj == "thunks":
            require(info.count("RuntimeFunction {") == 2 and
                    "SET_FPREG reg=RBP, offset=0xC0" in info and
                    "SET_FPREG reg=RBP, offset=0xA0" in info and
                    info.count("SAVE_XMM128") == 10,
                    "exact production thunks lack the qualified unwind prerequisite")
    binaries = {
        "native_cleanup_test.exe": ["main.obj", "bridge.obj", "checkpoint.obj", "thunks.obj"],
        "native_cleanup_helper.dll": ["helper.obj", "bridge.obj"],
    }
    for name, objects in binaries.items():
        link_flags = ["-shared"] if name.endswith(".dll") else []
        trace = run([compiler, *link_flags, "-static", "-Wl,--no-insert-timestamp", "-Wl,--verbose",
             f"-Wl,-Map,{out / (name + '.map')}", *[out / obj for obj in objects],
             "-o", out / name], f"link-{name}.txt")
        imports = run([readobj, "--coff-imports", out / name], f"{name}-imports.txt")
        symbols = run([nm, "--defined-only", out / name], f"{name}-symbols.txt")
        require(not re.search(r"Name: (?:libunwind|libc\+\+|libgcc|libstdc\+\+)",
                              imports, flags=re.IGNORECASE),
                f"{name}: unexpected shared language runtime")
        for symbol in ("_Unwind_ForcedUnwind", "_Unwind_Resume", "_GCC_specific_handler"):
            require(re.search(rf"\bT {re.escape(symbol)}$", symbols, re.MULTILINE),
                    f"{name}: missing owned static {symbol}")
        link_map = (out / (name + ".map")).read_text()
        require("Unwind-seh.cpp.obj:" in link_map and "cxa_personality.cpp.obj:" in link_map and
                "Loaded libunwind.a(Unwind-seh.cpp.obj)" in trace and
                "Loaded libc++.a(cxa_personality.cpp.obj)" in trace,
                f"{name}: no static runtime ownership in actual link map")
    receipt = {
        "schema": "gesher-native-callback-cleanup-build-v1",
        "source": {"commit": commit, "tree": tree, "dirty": dirty,
                   "files": {name: sha(ROOT / name) for name in SOURCES}},
        "ci": ci,
        "compiler": {"archive_url": URL, "archive_sha256": ARCHIVE_SHA,
                     "llvm_commit": LLVM_COMMIT, "version": version,
                     "verified_archive_members": verified},
        "runtime_hashes": RUNTIME_HASHES,
        "commands": commands,
        "outputs": {path.name: sha(path) for path in sorted(out.iterdir()) if path.is_file()},
        "runtime": "not executed by this cross-compile recipe",
        "scope": "native LLVM cleanup qualification; no guest translator transfer",
    }
    (out / "build-receipt.json").write_text(json.dumps(receipt, indent=2) + "\n")
    print(f"Built the native cleanup EXE, separate static helper DLL and exact production thunks: {out}")


if __name__ == "__main__":
    main()
