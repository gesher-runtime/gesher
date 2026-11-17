#!/usr/bin/env python3
"""Check actual guest sleep/clock exports and pre-handler wait observation.

Controlled clocks cover long budgets without long waits. Independent Linux
calls and the shared Android ELF fixture cover real timing and signals.
The host envelope is not a Windows scheduler or ARM64 execution result.
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
GROUPS = ("public", "budgets", "interruptions", "clocks", "extremes", "signal-observer", "combined-signals")


def section(source: str, start: str, end: str) -> str:
    if source.count(start) != 1 or source.count(end) != 1:
        raise ValueError("missing or ambiguous source boundary: " + start)
    first = source.index(start)
    return source[first:source.index(end, first)]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-ref", help="Read original production source as a negative control")
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--group", choices=GROUPS, action="append")
    parser.add_argument("--skip-linux-reference", action="store_true")
    parser.add_argument("--linux-reference-only", action="store_true")
    parser.add_argument("--cxx", default=os.environ.get("CXX", "c++"))
    parser.add_argument("--cc", default=os.environ.get("CC", "cc"))
    args = parser.parse_args()

    def read(path: str) -> str:
        if args.source_ref:
            return subprocess.check_output(["git", "show", args.source_ref + ":" + path], cwd=ROOT).decode()
        return (ROOT / path).read_text()

    shim = read("compat/bionic/shim.cc")
    signal = read("compat/bionic/signal_bionic.cc")
    observed = "bool SleepWithObservation(" in signal
    start = "namespace {\nbool SleepWithObservation(" if observed else "bool InterruptibleSleep("
    registrations = []
    for name in ("nanosleep", "usleep", "sleep", "clock_gettime", "clock_getres"):
        matches = re.findall(r'^  ENTRY\("' + name + r'", [A-Za-z_][A-Za-z0-9_]*\);$', shim, re.M)
        if len(matches) != 1:
            raise ValueError("missing or ambiguous export: " + name)
        registrations.extend(matches)
    parts = {
        "@QPC_IMPLEMENTATION@": section(shim, "int64_t QpcNanos() {" if "int64_t QpcNanos() {" in shim else "int64_t MonotonicNanos() {", "int64_t ThreadCpuNanos() {"),
        "@CLOCK_IMPLEMENTATION@": section(shim, "int BIONIC_SYSV ClockGettime(", "struct LinuxTimeval {"),
        "@SLEEP_IMPLEMENTATION@": section(shim, "int BIONIC_SYSV Nanosleep(", "int64_t BIONIC_SYSV Clock()"),
        "@REGISTRATIONS@": "\n".join(registrations),
        "@SIGNAL_WAIT_IMPLEMENTATION@": section(signal, start, "void DeliverPendingSignals() {"),
    }
    fixture = (ROOT / "tests/native/sleep_contract_host.cc.in").read_text()
    for marker, content in parts.items():
        if fixture.count(marker) != 1:
            raise ValueError("missing or ambiguous fixture marker: " + marker)
        fixture = fixture.replace(marker, content)
        print(marker.strip("@") + " SHA256 " + hashlib.sha256(content.encode()).hexdigest(), flush=True)
    failed = []
    with tempfile.TemporaryDirectory(prefix="gesher-sleep-contracts-") as directory:
        temporary = Path(directory)
        if not args.linux_reference_only:
            source, binary = temporary / "host.cc", temporary / "host"
            source.write_text(fixture)
            command = shlex.split(args.cxx) + [
                "-std=c++20", "-O1", "-g", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function",
                "-fno-builtin", "-fno-exceptions", "-pthread",
                "-DBASELINE_CONTROL=" + str(int(bool(args.source_ref))),
                "-DHAS_OBSERVED_WAIT=" + str(int(observed)),
                "-I" + str(ROOT / "compat/bionic"), "-I" + str(ROOT / "compat/arm64/include"),
                "-I" + str(ROOT / "tests/native"), str(source), "-o", str(binary),
            ]
            if args.sanitize:
                command += ["-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-fno-omit-frame-pointer"]
            subprocess.run(command, check=True, timeout=120)
            groups = list(args.group or GROUPS)
            if args.source_ref and "extremes" in groups:
                groups.remove("extremes")
                print("Baseline control skips signed-overflow/null-dereference paths; candidate checks them.", flush=True)
            for group in groups:
                result = subprocess.run([str(binary), group], capture_output=True, text=True, timeout=20)
                print(result.stdout, end="", flush=True)
                if result.returncode:
                    failed.append(group)
                    print(result.stderr[:10000], end="", flush=True)
        if not args.skip_linux_reference:
            source, binary = temporary / "linux.c", temporary / "linux"
            source.write_text('''
#include <stdio.h>
static int checks, failures;
static void check(const char* name, int ok) {
  ++checks;
  if (!ok) { ++failures; fprintf(stderr, "FAIL %s\\n", name); }
}
#include "sleep_contract_test.inc"
int main(void) {
  test_sleep_contracts();
  printf("Linux sleep: %d checks, %d failures\\n", checks, failures);
  return failures != 0;
}
''')
            command = shlex.split(args.cc) + [
                "-std=c11", "-O1", "-g", "-Wall", "-Wextra", "-Werror", "-fno-builtin",
                "-D_GNU_SOURCE", "-pthread", "-I" + str(ROOT / "tests/native"),
                str(source), "-o", str(binary),
            ]
            subprocess.run(command, check=True, timeout=60)
            result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=15)
            print(result.stdout, end="", flush=True)
            if result.returncode:
                failed.append("linux-reference")
                print(result.stderr[:10000], end="", flush=True)
    if failed:
        print("Failing sleep groups: " + ", ".join(failed), flush=True)
    return int(bool(failed))


if __name__ == "__main__":
    raise SystemExit(main())
