#!/usr/bin/env python3
"""Exercise the whole HttpSource with a declared in-process WinHTTP adapter.

This checks protocol decisions and immutable cache ownership, including a
deterministic eviction test. Real WinHTTP loopback execution is a Windows gate.
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
    parser.add_argument("--source-ref")
    parser.add_argument("--cxx", default=os.environ.get("CXX", "c++"))
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="gesher-http-source-") as temporary:
        work = Path(temporary)
        relative = "compat/media/player/http_source.cc"
        source = ROOT / relative
        if args.source_ref:
            source = work / "http_source.cc"
            source.write_bytes(subprocess.run(["git", "show", args.source_ref + ":" + relative],
                                             cwd=ROOT, capture_output=True, check=True).stdout)
        print("HTTP source SHA256 " + hashlib.sha256(source.read_bytes()).hexdigest(), flush=True)
        binary = work / "http_source"
        command = shlex.split(args.cxx) + [
            "-std=c++20", "-O2", "-g", "-Wall", "-Wextra", "-Werror", "-pthread",
            "-DGESHER_MEDIA_STANDALONE", '-DGESHER_HTTP_SOURCE="' + str(source) + '"',
            "-I", str(ROOT / "tests/media/http-source-host"), "-I", str(ROOT),
            "-I", str(ROOT / "compat/media/player"),
            str(ROOT / "tests/media/http_source_host_test.cc"), "-o", str(binary)]
        if args.sanitize:
            command += ["-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                        "-fno-omit-frame-pointer"]
        subprocess.run(command, check=True, timeout=120)
        return subprocess.run([str(binary)], check=False, timeout=60).returncode


if __name__ == "__main__":
    raise SystemExit(main())
