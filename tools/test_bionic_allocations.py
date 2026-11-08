#!/usr/bin/env python3
"""Run the production bionic allocation section with the shared ELF regressions.

This Linux host check needs a C++20 compiler, not AOSP or a Windows runtime.
It copies the memory implementation and its public registrations verbatim from
shim.cc. Only _msize is adapted to the host's heap accounting in the envelope.
Use --source with an old shim.cc to reproduce the defects in disposable child
processes; failed realloc must not take the test driver down with the child.
The full Windows/ELF tests remain necessary for end-to-end ABI verification.
"""
import argparse
import hashlib
import os
from pathlib import Path
import shlex
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parent.parent


def section(source: str, start: str, end: str) -> str:
    if source.count(start) != 1 or source.count(end) != 1:
        raise ValueError("production allocation section boundaries changed")
    begin = source.index(start) + len(start)
    finish = source.index(end, begin)
    return source[begin:finish]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, default=ROOT / "compat/bionic/shim.cc")
    parser.add_argument("--cxx", default=os.environ.get("CXX", "c++"))
    parser.add_argument("--sanitize", action="store_true", help="enable ASan and UBSan")
    args = parser.parse_args()
    source = args.source.read_text(encoding="utf-8")
    implementation = section(source, "// --- memory -------------------------------------", "// --- strings and numbers with `long`")
    # The opening divider's remaining dashes are part of its C++ comment.
    implementation = implementation[implementation.index("\n") + 1:]
    registrations = section(source, "void RegisterLibc(SymbolTable& t) {", "  // mem/str.")
    template = (ROOT / "tests/native/allocation_host_test.cc.in").read_text(encoding="utf-8")
    for marker, value in (("@MEMORY_IMPLEMENTATION@", implementation),
                          ("@MEMORY_REGISTRATIONS@", registrations)):
        if template.count(marker) != 1:
            raise ValueError("missing or ambiguous template marker: " + marker)
        template = template.replace(marker, value)

    print("Production shim SHA256 " + hashlib.sha256(source.encode()).hexdigest(), flush=True)
    print("Production allocation section SHA256 " + hashlib.sha256(implementation.encode()).hexdigest(), flush=True)
    with tempfile.TemporaryDirectory(prefix="gesher-allocations-") as temporary:
        folder = Path(temporary)
        generated = folder / "allocation_test.cc"
        executable = folder / "allocation_test"
        generated.write_text(template, encoding="utf-8")
        command = shlex.split(args.cxx) + [
            "-std=c++20", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
            "-fno-exceptions", "-fno-builtin", "-pthread",
            "-I", str(ROOT / "compat/bionic"),
            "-I", str(ROOT / "compat/arm64/include"),
            "-I", str(ROOT / "tests/native"),
            str(generated), "-o", str(executable),
        ]
        if args.sanitize:
            command += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
        subprocess.run(command, check=True, timeout=120)
        env = os.environ.copy()
        if args.sanitize:
            env["ASAN_OPTIONS"] = "allocator_may_return_null=1:" + env.get("ASAN_OPTIONS", "")
            env["UBSAN_OPTIONS"] = "halt_on_error=1:" + env.get("UBSAN_OPTIONS", "")
        failed = []
        for case in ("calloc", "overflow", "memalign", "aligned-alloc", "realloc", "reallocarray", "zero"):
            result = subprocess.run([str(executable), case], env=env, text=True,
                                    stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=30)
            print(result.stdout, end="", flush=True)
            if result.returncode:
                failed.append(case)
                print(f"FAIL {case}: exit={result.returncode}\n{result.stderr[:12000]}", flush=True)
            else:
                print(f"PASS {case}", flush=True)
        if failed:
            print("Failing allocation contracts: " + ", ".join(failed), flush=True)
            return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
