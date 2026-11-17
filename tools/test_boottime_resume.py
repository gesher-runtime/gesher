#!/usr/bin/env python3
"""Compile actual BOOTTIME sleep, signal wait and resume dispatcher together.

The Win32 power notification and relative condition wait are controlled. Native
wait eligibility consumes only awake time. No timer callback or timeout is
injected merely because suspended time advanced. This is not physical Windows
suspend/hibernate evidence.
"""
from __future__ import annotations

import argparse
import hashlib
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
CASES = ("resume", "partial", "wait-race", "registration-race", "signals", "failure")


def section(source: str, first: str, following: str) -> str:
    if source.count(first) != 1 or source.count(following) != 1:
        raise ValueError("production source boundary changed: " + first)
    start = source.index(first)
    return source[start:source.index(following, start)]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-ref")
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--case", choices=CASES, action="append")
    parser.add_argument("--cxx", default=os.environ.get("CXX", "c++"))
    args = parser.parse_args()

    def read(path: str) -> str:
        return subprocess.check_output(["git", "show", args.source_ref + ":" + path], cwd=ROOT,
                                       text=True) if args.source_ref else (ROOT / path).read_text()

    signal = read("compat/bionic/signal_bionic.cc")
    extra = read("compat/bionic/extra_sys_bionic.cc")
    resume = (ROOT / "compat/posix/src/resume.cc").read_text()
    parts = {
        "RESUME_CORE": re.sub(r"^#include[^\n]*\n", "", resume, flags=re.M),
        "SIGNAL_SLEEP": section(signal, "namespace {\nbool SleepWithObservation(", "void DeliverPendingSignals() {"),
        "SLEEP_HELPERS": section(extra, "int64_t NowRealtimeNs() {", "// Milliseconds until an absolute time"),
        "CLOCK_SLEEP": section(extra, "int BIONIC_SYSV ClockNanosleep(", "int BIONIC_SYSV ClockGetcpuclockid("),
    }
    fixture = (ROOT / "tests/native/boottime_resume_host.cc.in").read_text()
    for name, body in parts.items():
        marker = "@" + name + "@"
        if fixture.count(marker) != 1:
            raise ValueError("fixture boundary changed: " + marker)
        # The host chrono clock is also a transport boundary. Its epoch advances
        # across suspend, while a previously entered native wait retains its
        # original awake budget, matching Windows' relative timeout transport.
        fixture = fixture.replace(marker, body.replace("std::chrono::steady_clock", "HostClock"))
        print(name + " SHA256 " + hashlib.sha256(body.encode()).hexdigest(), flush=True)
    failed = False
    with tempfile.TemporaryDirectory(prefix="gesher-boottime-resume-") as temporary:
        work = Path(temporary)
        (work / "gesher").mkdir()
        (work / "gesher/resume.h").write_text((ROOT / "compat/posix/include/gesher/resume.h").read_text())
        source, binary = work / "host.cc", work / "host"
        source.write_text(fixture)
        command = shlex.split(args.cxx) + ["-std=c++20", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
            "-Wno-unused-function", "-fno-exceptions", "-pthread", "-I", str(work),
            "-I", str(ROOT / "tests/native"), str(source), "-o", str(binary)]
        if args.sanitize:
            command += ["-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-fno-omit-frame-pointer"]
        subprocess.run(command, check=True, timeout=120)
        for case in args.case or CASES:
            result = subprocess.run([str(binary), case], capture_output=True, text=True, timeout=15)
            print(result.stdout, end="", flush=True)
            if result.returncode:
                print(result.stderr, end="", flush=True)
                failed = True
    return int(failed)


if __name__ == "__main__":
    raise SystemExit(main())
