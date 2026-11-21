#!/usr/bin/env python3
"""Compile complete production x86 TLS with isolated host Win32 primitives.

Checks pending ownership, allocation/commit rollback, stale ids, thread image
initialization, nested committed-module preservation, and published cache
independence during concurrent registry work and thread teardown. This host fixture does
not execute Windows FLS or guest code; it does execute the actual TLS allocator,
registry, caches, publication and rollback from compat/elf/tls.cc unchanged.
"""
from __future__ import annotations

import argparse
import hashlib
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX", "c++"))
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    if os.name == "nt":
        parser.error("this fixture uses an explicit POSIX boundary; run the Windows loader fixture instead")
    source = ROOT / "compat/elf/tls.cc"
    print("production TLS SHA256 " + hashlib.sha256(source.read_bytes()).hexdigest(), flush=True)
    with tempfile.TemporaryDirectory(prefix="gesher-elf-tls-rollback-") as temporary:
        executable = Path(temporary) / "tls-rollback"
        command = shlex.split(args.cxx) + [
            "-std=c++20", "-O1", "-g", "-Wall", "-Wextra", "-Werror", "-fno-exceptions", "-pthread",
            "-I", str(ROOT / "tests/native/elf_tls_rollback_platform"),
            "-I", str(ROOT / "compat/elf"),
            str(source), str(ROOT / "tests/native/elf_tls_rollback_host.cc"), "-o", str(executable),
        ]
        if args.sanitize:
            command += ["-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-fno-omit-frame-pointer"]
        subprocess.run(command, check=True, timeout=120)
        subprocess.run([str(executable)], check=True, timeout=45)


if __name__ == "__main__":
    main()
