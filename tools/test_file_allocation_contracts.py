#!/usr/bin/env python3
"""Run the actual Windows file-allocation adapter over a real Linux filesystem.

The handle/CRT envelope translates size and growth calls to Linux syscalls and
injects failures at that boundary. Shared public fixtures run in the Windows/
translated ARM64 ELF suite; these checks do not prove physical reservation.
"""
import argparse
import hashlib
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parent.parent


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, default=ROOT / "compat/posix/src/system.cc")
    parser.add_argument("--cxx", default=os.environ.get("CXX", "c++"))
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    source = args.source.read_text(encoding="utf-8")
    first, last = "int posix_fallocate(", "// --- uio "
    if source.count(first) != 1 or source.count(last) != 1:
        raise ValueError("file-allocation source boundaries changed")
    section = source[source.index(first):source.index(last, source.index(first))]
    template = (ROOT / "tests/native/file_allocation_host_test.cc.in").read_text(encoding="utf-8")
    if template.count("@FILE_ALLOCATION_IMPLEMENTATION@") != 1:
        raise ValueError("file-allocation template marker changed")
    print("Production system.cc SHA256 " + hashlib.sha256(source.encode()).hexdigest(), flush=True)
    with tempfile.TemporaryDirectory(prefix="gesher-file-allocation-") as temporary:
        folder = Path(temporary)
        generated = folder / "file_allocation.cc"
        generated.write_text(template.replace("@FILE_ALLOCATION_IMPLEMENTATION@", section), encoding="utf-8")
        executable = folder / "file_allocation"
        command = shlex.split(args.cxx) + ["-std=c++20", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
            "-Wno-unused-variable", "-fno-exceptions", "-D_GNU_SOURCE",
            "-I", str(ROOT / "tests/native"), str(generated), "-o", str(executable)]
        if args.sanitize:
            command += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
        subprocess.run(command, check=True, timeout=120)
        env = os.environ.copy()
        if args.sanitize:
            env["UBSAN_OPTIONS"] = "halt_on_error=1:" + env.get("UBSAN_OPTIONS", "")
        failures = []
        for case in ("contents", "errors", "overflow", "faults"):
            result = subprocess.run([str(executable), case], cwd=folder, env=env,
                                    capture_output=True, text=True, timeout=30)
            print(result.stdout, end="", flush=True)
            if result.returncode:
                failures.append(case)
                print(f"FAIL {case}: exit={result.returncode}\n{result.stderr[:6000]}", flush=True)
            else:
                print("PASS " + case, flush=True)
        return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
