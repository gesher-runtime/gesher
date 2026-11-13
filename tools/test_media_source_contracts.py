#!/usr/bin/env python3
"""Build the entire production FD source with real files and syscall-boundary cases.

The ordinary fixture also links against Gesher's actual descriptor layer on
Windows. The second host fixture explicitly adapts only its OS syscalls.
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


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-ref", help="Use an old production file as a negative control")
    parser.add_argument("--cxx", default=os.environ.get("CXX", "c++"))
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    relative = "compat/media/player/data_source.cc"
    with tempfile.TemporaryDirectory(prefix="gesher-media-source-") as temporary:
        work = Path(temporary)
        source = ROOT / relative
        if args.source_ref:
            source = work / "data_source.cc"
            source.write_bytes(subprocess.run(["git", "show", args.source_ref + ":" + relative],
                                             cwd=ROOT, capture_output=True, check=True).stdout)
        print("source SHA256 " + hashlib.sha256(source.read_bytes()).hexdigest(), flush=True)
        flags = ["-std=c++20", "-O2", "-g", "-Wall", "-Wextra", "-Werror",
                 "-I", str(ROOT), "-I", str(ROOT / "compat/media/player")]
        if args.sanitize:
            flags += ["-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                      "-fno-omit-frame-pointer"]
        failed = False
        for name, sources in [
            ("file", [str(ROOT / "tests/media/source_contract_test.cc"), str(source)]),
            ("boundary", [str(ROOT / "tests/media/source_boundary_host_test.cc"),
                          '-DGESHER_DATA_SOURCE="' + str(source) + '"'])]:
            binary = work / name
            subprocess.run(shlex.split(args.cxx) + flags + sources + ["-o", str(binary)],
                           check=True, timeout=120)
            failed |= subprocess.run([str(binary)], cwd=work, check=False, timeout=30).returncode != 0
        return int(failed)


if __name__ == "__main__":
    raise SystemExit(main())
