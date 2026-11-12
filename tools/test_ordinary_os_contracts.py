#!/usr/bin/env python3
"""Exercise verbatim clock_nanosleep and host/guest affinity adapters.

Controlled Windows boundaries cover extreme durations, clock changes, signal
remainders, and short masks under sanitizers. The public fixture is also compiled
as C against Linux, and can be included by the Android ELF libc test suites.
Passing here does not establish Windows clock/signal integration.
"""
import argparse
import hashlib
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parent.parent
CASES = ("public", "clock-validation", "clock-budgets", "clock-interruption", "affinity")


def section(source: str, first: str, following: str) -> str:
    if source.count(first) != 1 or source.count(following) != 1:
        raise ValueError("production section boundaries changed: " + first)
    start = source.index(first)
    return source[start:source.index(following, start)]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, default=ROOT / "compat/bionic/extra_sys_bionic.cc")
    parser.add_argument("--host-source", type=Path, default=ROOT / "compat/posix/src/system.cc")
    parser.add_argument("--header", type=Path, default=ROOT / "compat/posix/include/sched.h")
    parser.add_argument("--cxx", default=os.environ.get("CXX", "c++"))
    parser.add_argument("--cc", default=os.environ.get("CC", "cc"))
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--case", action="append", choices=CASES)
    parser.add_argument("--skip-linux-reference", action="store_true")
    args = parser.parse_args()
    source = args.source.read_text(encoding="utf-8")
    host = args.host_source.read_text(encoding="utf-8")
    header = args.header.read_text(encoding="utf-8")
    registrations = []
    for name in ("clock_nanosleep", "sched_getaffinity"):
        matches = re.findall(r'^  E\("' + name + r'", [A-Za-z_][A-Za-z0-9_]*\);$', source, re.M)
        if len(matches) != 1:
            raise ValueError("missing or ambiguous public registration: " + name)
        registrations.extend(matches)
    cpu_set = re.findall(r"^#define CPU_SET\(cpu, set\).*?$", header, re.M)
    if len(cpu_set) != 1:
        raise ValueError("missing or ambiguous production CPU_SET macro")
    replacements = {
        "@CLOCK_HELPERS@": section(source, "int64_t NowRealtimeNs() {", "// --- POSIX semaphores"),
        "@CLOCK_IMPLEMENTATION@": section(source, "int BIONIC_SYSV ClockNanosleep(",
                                         "int BIONIC_SYSV ClockGetcpuclockid("),
        "@GUEST_AFFINITY_IMPLEMENTATION@": section(source, "int ProcessorCount() {",
                                                   "int BIONIC_SYSV SchedSetaffinity("),
        "@PUBLIC_REGISTRATIONS@": "\n".join(registrations),
        "@HOST_AFFINITY_TYPES@": section(header, "#define CPU_SETSIZE 1024", "#define CPU_ZERO(set)"),
        "@HOST_CPU_SET@": cpu_set[0],
        "@HOST_AFFINITY_IMPLEMENTATION@": section(host, "int sched_getaffinity(pid_t,", "int unshare("),
    }
    template = (ROOT / "tests/native/ordinary_os_host_test.cc.in").read_text(encoding="utf-8")
    for marker, value in replacements.items():
        if template.count(marker) != 1:
            raise ValueError("missing or ambiguous template marker: " + marker)
        template = template.replace(marker, value)
        print(marker.strip("@") + " SHA256 " + hashlib.sha256(value.encode()).hexdigest(), flush=True)
    failed = []
    with tempfile.TemporaryDirectory(prefix="gesher-ordinary-os-") as temporary:
        folder = Path(temporary)
        generated = folder / "ordinary_os_test.cc"
        executable = folder / "ordinary_os_test"
        generated.write_text(template, encoding="utf-8")
        command = shlex.split(args.cxx) + [
            "-std=c++20", "-O1", "-g", "-Wall", "-Wextra", "-Werror", "-fno-exceptions",
            "-fno-builtin", "-pthread", "-D_GNU_SOURCE",
            "-I", str(ROOT / "compat/bionic"), "-I", str(ROOT / "compat/arm64/include"),
            "-I", str(ROOT / "tests/native"), str(generated), "-o", str(executable),
        ]
        if args.sanitize:
            command += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
        subprocess.run(command, check=True, timeout=120)
        env = os.environ.copy()
        if args.sanitize:
            env["UBSAN_OPTIONS"] = "halt_on_error=1:" + env.get("UBSAN_OPTIONS", "")
        for case in args.case or CASES:
            result = subprocess.run([str(executable), case], text=True, env=env,
                                    stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=30)
            print(result.stdout, end="", flush=True)
            if result.returncode:
                failed.append(case)
                print(f"FAIL {case}: exit={result.returncode}\n{result.stderr[:12000]}", flush=True)
            else:
                print("PASS " + case, flush=True)
        if not args.skip_linux_reference:
            reference = folder / "ordinary_os_linux_reference.c"
            reference.write_text("""
#include <stdio.h>
static int failures;
static void check(const char* name, int ok) {
  printf("%-88s %s\\n", name, ok ? "ok" : "FAIL");
  if (!ok) ++failures;
}
#include "ordinary_os_contract_test.inc"
int main(void) { test_ordinary_os_contracts(); return failures ? 1 : 0; }
""", encoding="utf-8")
            reference_exe = folder / "ordinary_os_linux_reference"
            command = shlex.split(args.cc) + [
                "-std=c11", "-O1", "-g", "-Wall", "-Wextra", "-Werror", "-fno-builtin",
                "-D_GNU_SOURCE", "-pthread", "-I", str(ROOT / "tests/native"),
                str(reference), "-o", str(reference_exe),
            ]
            subprocess.run(command, check=True, timeout=60)
            result = subprocess.run([str(reference_exe)], text=True, stdout=subprocess.PIPE,
                                    stderr=subprocess.PIPE, timeout=30)
            print(result.stdout, end="", flush=True)
            if result.returncode:
                failed.append("linux-reference")
                print("FAIL linux-reference\n" + result.stderr[:12000], flush=True)
            else:
                print("PASS linux-reference", flush=True)
    return int(bool(failed))


if __name__ == "__main__":
    raise SystemExit(main())
