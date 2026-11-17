#!/usr/bin/env python3
"""Compile the actual fetched libcore System.nanoTime C body with distinct host clocks.

The supplied source is the same patched AOSP checkout built into libopenjdk.
Clock boundaries are controlled; ART and physical suspend remain Windows gates.
"""
import argparse
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--cc", default=os.environ.get("CC", "cc"))
    args = parser.parse_args()
    text = args.source.read_text(encoding="utf-8")
    matches = re.findall(r"static jlong System_nanoTime\(\) \{.*?^\}", text, re.S | re.M)
    if len(matches) != 1:
        raise ValueError("System_nanoTime source boundary changed")
    body = matches[0]
    with tempfile.TemporaryDirectory(prefix="gesher-system-nanotime-") as directory:
        work = Path(directory)
        (work / "windows.h").write_text("typedef unsigned long long ULONGLONG;\nstatic long long awake = 10000000000LL;\nstatic void QueryUnbiasedInterruptTimePrecise(ULONGLONG* value) { *value = awake / 100; }\n")
        (work / "gesher").mkdir()
        (work / "gesher/awake_clock.h").write_text((ROOT / "compat/posix/include/gesher/awake_clock.h").read_text())
        preamble = "#include <stdint.h>\n#include <stdio.h>\n#include <gesher/awake_clock.h>\ntypedef int64_t jlong;\n"
        # The negative control keeps the original host CLOCK_MONOTONIC branch.
        preamble += "struct timespec { long long tv_sec; long tv_nsec; };\n#define CLOCK_MONOTONIC 1\nstatic int clock_gettime(int id, struct timespec* t) { (void)id; t->tv_sec=700; t->tv_nsec=0; return 0; }\n"
        checks = "int main(void) { int failures=0; failures += System_nanoTime()!=awake; awake=10000000000LL; failures += System_nanoTime()!=awake; awake+=123400; failures += System_nanoTime()!=awake; printf(\"libcore System.nanoTime: 3 checks, %d failures\\n\",failures); return failures!=0; }\n"
        source = work / "nano.c"
        source.write_text(preamble + body + "\n" + checks)
        binary = work / ("nano.exe" if os.name == "nt" else "nano")
        subprocess.run(shlex.split(args.cc) + ["-std=c11", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function", "-DGESHER", "-I", str(work), str(source), "-o", str(binary)], check=True)
        subprocess.run([str(binary)], check=True, timeout=15)

if __name__ == "__main__":
    main()
