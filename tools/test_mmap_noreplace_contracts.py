#!/usr/bin/env python3
"""Exercise production NOREPLACE admission with a recording Win32 boundary.

Registry types, placeholder operations, MapAnonymous and munmap are copied
verbatim from production. The boundary records native calls, retains plain
Windows reservations and injects allocation failures; it does not decide Linux
page ownership. Real Windows behavior is exercised separately by native probes.
Use --source-ref with the pre-fix revision as an unchanged-assertion control.
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
SOURCE = "compat/posix/src/mman.cc"


def block(source: str, signature: str) -> str:
    if source.count(signature) != 1:
        raise ValueError("production signature changed: " + signature)
    start = source.index(signature)
    brace = source.index("{", start)
    depth = 1
    end = brace + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    source_args = parser.add_mutually_exclusive_group()
    source_args.add_argument("--source", type=Path)
    source_args.add_argument("--source-ref")
    parser.add_argument("--cxx", default=os.environ.get("CXX", "c++"))
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    source = (subprocess.check_output(["git", "show", args.source_ref + ":" + SOURCE],
                                     cwd=ROOT, text=True) if args.source_ref
              else (args.source or ROOT / SOURCE).read_text())
    types = "\n\n".join(block(source, signature) + ";" for signature in (
        "enum class Kind : uint8_t", "enum class Origin : uint8_t",
        "struct Region {", "struct State {",
    ))
    signatures = [
        "size_t RoundUp(size_t x, size_t n)",
        "DWORD PageProtection(int prot)",
        "DWORD AnonymousCommit(int prot)",
        "std::map<uintptr_t, Region>::iterator FindRegion(State& s, uintptr_t addr)",
        "std::map<uintptr_t, Region>::iterator AddRegion(State& s, uintptr_t start, size_t size,",
        ("void ForgetLocks(State& s, uintptr_t begin, uintptr_t end)"
         if "void ForgetLocks(" in source else "void Release(uintptr_t start, Region& region)"),
        "std::map<uintptr_t, Region>::iterator SplitEntry(State& s,",
        "uintptr_t Reserve(State& s, void* at, size_t len)",
        "uintptr_t ReserveNear(State& s, void* hint, size_t len)",
        "void ReleaseGroupIfEmpty(State& s, uintptr_t group)",
        "uintptr_t CoveringGroup(State& s, uintptr_t begin, uintptr_t end)",
        "bool RangeIsOurs(State& s, uintptr_t begin, uintptr_t end)",
    ]
    if "bool RangeIsHole(" in source:
        signatures.append("bool RangeIsHole(State& s, uintptr_t begin, uintptr_t end)")
    signatures += [
        "std::map<uintptr_t, Region>::iterator Punch(State& s, uintptr_t begin, uintptr_t end)",
        "bool FillAnonymous(std::map<uintptr_t, Region>::iterator it, int prot, Origin origin)",
        "void* GroupAnonymous(State& s, void* start, size_t len, int prot, int flags)",
        "void* MapAnonymous(State& s, void* start, size_t len, int prot, int flags)",
    ]
    if "int Unmap(" in source:
        signatures += [
            "bool AdviceRange(uintptr_t begin, size_t length, size_t page, size_t* rounded)",
            "int Unmap(State& s, uintptr_t begin, uintptr_t end)",
        ]
    signatures.append("int munmap(void* addr, size_t length)")
    production = "\n\n".join(block(source, signature) for signature in signatures)
    template = (ROOT / "tests/mman/noreplace_mapping_host.cc.in").read_text()
    for marker, code in [("@TYPES@", types), ("@PRODUCTION@", production)]:
        if template.count(marker) != 1:
            raise ValueError("fixture marker changed: " + marker)
        template = template.replace(marker, code)
    print("NOREPLACE production SHA256 " + hashlib.sha256((types + production).encode()).hexdigest(), flush=True)
    with tempfile.TemporaryDirectory(prefix="gesher-mmap-noreplace-") as temporary:
        folder = Path(temporary)
        cpp = folder / "test.cc"
        cpp.write_text(template)
        executable = folder / "test"
        command = shlex.split(args.cxx) + ["-std=c++20", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                                         "-fno-exceptions", str(cpp), "-o", str(executable)]
        if args.sanitize:
            command += ["-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                        "-fno-omit-frame-pointer", "-fno-pie", "-no-pie"]
        subprocess.run(command, check=True, timeout=120)
        subprocess.run([str(executable)], check=True, timeout=20)


if __name__ == "__main__":
    main()
