#!/usr/bin/env python3
"""Compile the actual ELF handle lookup; the host seam observes graph order.

The Windows native/ARM64 guest graph and a Linux dlopen oracle independently
exercise real ELF objects built by tests/native/build-lookup-graph.sh.
"""
import argparse
import hashlib
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

from test_elf_lookup_performance import lookup_section

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, default=ROOT / "compat/elf/elf_loader.cc")
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--linux-reference", action="store_true")
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="gesher-elf-lookup-") as temporary:
        work = Path(temporary)
        if args.linux_reference:
            subprocess.run(["bash", str(ROOT / "tests/native/build-lookup-graph.sh"),
                            os.environ.get("CC", "cc"), str(work)], check=True, timeout=90)
            subprocess.run(["python3", "-c", "import ctypes,sys; "
                            "lib=ctypes.CDLL(sys.argv[1]); "
                            "sys.exit(lib.gesher_selftest())", str(work / "lookup_root.so")],
                           check=True, timeout=15)
            return
        source = args.source.read_text()
        scope = (args.source.with_name("lookup_scope_internal.h").read_text()
                 if '#include "lookup_scope_internal.h"' in source else None)
        implementation = lookup_section(source, scope)
        print("BOUNDARY actual production handle BFS; observing symbols and all-visible namespace seam", flush=True)
        print("SOURCE_SHA256 elf_loader.cc " + hashlib.sha256(source.encode()).hexdigest(), flush=True)
        if scope is not None:
            print("SOURCE_SHA256 lookup_scope_internal.h " + hashlib.sha256(scope.encode()).hexdigest(), flush=True)
        print("LOOKUP_SHA256 " + hashlib.sha256(implementation.encode()).hexdigest(), flush=True)
        fixture = (ROOT / "tests/native/elf_lookup_host_test.cc.in").read_text()
        if fixture.count("@LOOKUP_IMPLEMENTATION@") != 1:
            raise ValueError("ELF lookup fixture boundary changed")
        cpp, executable = work / "test.cc", work / "test"
        cpp.write_text(fixture.replace("@LOOKUP_IMPLEMENTATION@", implementation))
        command = shlex.split(os.environ.get("CXX", "c++")) + [
            "-std=c++17", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
            str(cpp), "-o", str(executable)]
        if args.sanitize:
            command += ["-fsanitize=address,undefined", "-fno-sanitize-recover=undefined",
                        "-fno-omit-frame-pointer", "-fno-pie", "-no-pie"]
        subprocess.run(command, check=True, timeout=90)
        subprocess.run([str(executable)], check=True, timeout=15)


if __name__ == "__main__":
    main()
