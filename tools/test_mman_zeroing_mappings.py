#!/usr/bin/env python3
"""Exercise production mmap zeroing/protection code with a scheduled Win32 seam.

The host seam models one Win32 page and pauses after MEM_DECOMMIT, so the
protection race is reproducible without timing-dependent Windows stress.
Real VirtualAlloc/VEH behavior is covered separately by mman_probe in Windows CI.
"""

import argparse
import os
from pathlib import Path
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[1]


def block(source, signature):
    start = source.index(signature)
    brace = source.index("{", start)
    depth = 1
    end = brace + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, default=ROOT / "compat/posix/src/mman.cc")
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    source = args.source.read_text()
    functions = [block(source, signature) for signature in (
        "DWORD PageProtection(int prot)",
        "std::map<uintptr_t, Region>::iterator FindRegion(State& s, uintptr_t addr)",
        "template <typename Fn>\nbool ForEachRun(void* addr, size_t len, Fn fn)",
    )]
    start = source.index("struct ZeroingRange {")
    end = source.index("}  // namespace", start)
    functions.append(source[start:end])
    functions += [block(source, signature) for signature in (
        "bool RetryAfterMadvise(uintptr_t addr, ULONG_PTR access)",
        "int mprotect(void* addr, size_t length, int prot)",
        "int madvise(void* addr, size_t length, int advice)",
    )]
    fixture = (ROOT / "tests/mman/zeroing_mapping_test.cc.in").read_text()
    assert fixture.count("@PRODUCTION@") == 1
    fixture = fixture.replace("@PRODUCTION@", "\n\n".join(functions))
    with tempfile.TemporaryDirectory(prefix="gesher-mman-zeroing-") as temporary:
        build = Path(temporary)
        (build / "test.cc").write_text(fixture)
        command = [os.environ.get("CXX", "c++"), "-std=c++20", "-O1", "-g", "-pthread",
                   "-Wall", "-Wextra", "-Werror", str(build / "test.cc"), "-o", str(build / "test")]
        if args.sanitize:
            command += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-fno-pie", "-no-pie"]
        subprocess.run(command, check=True)
        subprocess.run([str(build / "test")], check=True, timeout=20)


if __name__ == "__main__":
    main()
