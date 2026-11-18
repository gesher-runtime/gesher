#!/usr/bin/env python3
"""Run NativeActivity's actual entry/callback adapter with original ELF code.

The focused driver reuses the complete production ELF and translated guest
boundary from test_guest_libdl_namespaces.py. --arm64 needs a compiler targeting
Android AArch64 ELF and the pinned, Gesher-patched Linux Dynarmic build. The
Linux entry stack seam supports at most six integer arguments, which covers
these NativeActivity callback signatures. This does not execute the Windows
ART NativeActivity/UI lifecycle.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def run(command: list[str]) -> None:
    print("COMMAND", json.dumps(command), flush=True)
    subprocess.run(command, check=True, timeout=180)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", default=os.environ.get("CC", "gcc"))
    parser.add_argument("--cxx", default=os.environ.get("CXX", "g++"))
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--arm64", action="store_true")
    parser.add_argument("--dynarmic-source", type=Path)
    parser.add_argument("--dynarmic-build", type=Path)
    args = parser.parse_args()
    if args.arm64 and not (args.dynarmic_source and args.dynarmic_build):
        parser.error("--arm64 requires --dynarmic-source and --dynarmic-build")
    compiler = shlex.split(args.cc)
    run(compiler + ["--version"])
    with tempfile.TemporaryDirectory(prefix="gesher-native-activity-") as temporary:
        folder = Path(temporary)
        for name in ("native_activity_guest_fixture.c", "native_activity_guest_contract.h"):
            target = folder / name
            shutil.copy2(ROOT / "tests/native" / name, target)
            print("FIXTURE_SOURCE_SHA256", name, hashlib.sha256(target.read_bytes()).hexdigest(), flush=True)
        run(compiler + ["-shared", "-fPIC", "-ffreestanding", "-nostdlib",
            "-Wl,--hash-style=both", "-Wl,-soname,libnativeactivity_fixture.so",
            str(folder / "native_activity_guest_fixture.c"),
            "-o", str(folder / "libnativeactivity_fixture.so")])
        command = [sys.executable, str(ROOT / "tools/test_guest_libdl_namespaces.py"),
            "--fixtures", str(folder), "--cc", args.cc, "--cxx", args.cxx,
            "--driver", str(ROOT / "tests/native/native_activity_guest_test.cc")]
        for relative in ("compat/bionic/native_activity_dispatch.h", "compat/bionic/abi_types.h",
                         "tests/native/native_activity_guest_contract.h"):
            command += ["--driver-header", str(ROOT / relative)]
        if args.sanitize:
            command.append("--sanitize")
        if args.arm64:
            command += ["--arm64", "--dynarmic-source", str(args.dynarmic_source.resolve()),
                        "--dynarmic-build", str(args.dynarmic_build.resolve())]
        run(command)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
