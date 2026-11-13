#!/usr/bin/env python3
"""Compile production timer/registry code with declared Windows host boundaries.

The host fixture controls only Win32 clocks, threadpool/events and CRT descriptor
allocation; it executes the actual timer core, event registry and bionic syscall
dispatch. A separate real Linux fixture checks public timerfd contracts. This
does not replace execution through the real Windows host and both guest ABIs.
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
CASES = ("ownership", "contracts", "deadlines", "quiescence")


def section(source: str, first: str, following: str) -> str:
    if source.count(first) != 1 or source.count(following) != 1:
        raise ValueError("ambiguous production boundary: " + first)
    start = source.index(first)
    return source[start:source.index(following, start)]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX", "c++"))
    parser.add_argument("--cc", default=os.environ.get("CC", "cc"))
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--source-ref", help="compile original production sources as a negative control")
    parser.add_argument("--case", choices=CASES, action="append")
    parser.add_argument("--skip-linux-reference", action="store_true")
    args = parser.parse_args()

    def read(path: str) -> str:
        if args.source_ref:
            return subprocess.run(["git", "show", args.source_ref + ":" + path], cwd=ROOT,
                                  check=True, capture_output=True, text=True).stdout
        return (ROOT / path).read_text()

    epoll = read("compat/posix/src/epoll.cc")
    bionic = read("compat/bionic/time_bionic.cc")
    old = "struct TimerState {" in bionic
    core = "" if old else read("compat/posix/src/timerfd.cc")
    timer_core = re.sub(r"^#include[^\n]*\n", "", core, flags=re.M)
    replacements = {
        "EVENT_FD": section(epoll, "struct EventFd {", "namespace {\n\nstruct Epoll;"),
        "OBJECT_REGISTRY": section(epoll, "struct Object {", "// Every live epoll"),
        "OBJECT_LOOKUP": section(epoll, "bool Lookup(int fd, Object* object)", "std::shared_ptr<Epoll> LookupEpoll("),
        "OBJECT_INSERT": section(epoll, "void Insert(int fd, Object object)", "int ReadEventFd("),
        "EVENT_IO": section(epoll, "int ReadEventFd(", "// epoll's event bits"),
        "OBJECT_OPERATIONS": section(epoll, "std::shared_ptr<EventFd> LookupEventFd(", "// Linux drops an entry"),
        "EVENT_CREATE": section(epoll, "int eventfd(unsigned int initval", "int eventfd_read("),
        "TIMER_CORE": timer_core,
        "TIMER_ADAPTER": section(bionic, "// --- timerfd ", "\n}  // namespace\n\nbool DispatchTimerfdSyscall"),
        "TIMER_DISPATCH": section(bionic, "bool DispatchTimerfdSyscall(", "void RegisterTime("),
        "BASELINE_CLEANUP": "  gesher::bionic::g_timers.clear();" if old else "",
    }
    template = (ROOT / "tests/native/timerfd_host.cc.in").read_text()
    for key, value in replacements.items():
        marker = "@" + key + "@"
        if template.count(marker) != 1:
            raise ValueError("missing/ambiguous fixture marker: " + key)
        template = template.replace(marker, value)
    for name, source in (("event registry", epoll), ("bionic adapters", bionic), ("timer core", core)):
        if source:
            print(name + " SHA256 " + hashlib.sha256(source.encode()).hexdigest(), flush=True)

    failed = []
    with tempfile.TemporaryDirectory(prefix="gesher-timerfd-") as temporary:
        work = Path(temporary)
        generated = work / "timerfd_host.cc"
        generated.write_text(template)
        exe = work / "timerfd_host"
        # Only the small stable API header comes from the overlay. Do not shadow
        # Linux's real libc/time/eventfd headers with the Windows POSIX overlay.
        (work / "gesher").mkdir()
        (work / "gesher/timerfd.h").write_text((ROOT / "compat/posix/include/gesher/timerfd.h").read_text())
        command = shlex.split(args.cxx) + ["-std=c++20", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                    "-fno-exceptions", "-fno-builtin", "-pthread", "-I", str(work), str(generated), "-o", str(exe)]
        if args.sanitize:
            command += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
        subprocess.run(command, check=True, timeout=120)
        environment = os.environ.copy()
        if args.sanitize:
            environment["UBSAN_OPTIONS"] = "halt_on_error=1:" + environment.get("UBSAN_OPTIONS", "")
        for case in args.case or CASES:
            result = subprocess.run([str(exe), case], text=True, capture_output=True, env=environment, timeout=15)
            print(result.stdout, end="", flush=True)
            if result.returncode:
                failed.append(case)
                print(f"FAIL {case}: exit {result.returncode}\n{result.stderr[:12000]}", flush=True)
        if not args.skip_linux_reference:
            reference = work / "timerfd_reference"
            subprocess.run(shlex.split(args.cc) + ["-std=gnu11", "-O1", "-Wall", "-Wextra", "-Werror",
                           "-DGESHER_TIMERFD_REFERENCE_MAIN", str(ROOT / "tests/native/timerfd_syscall_test.c"),
                           "-o", str(reference)], check=True, timeout=60)
            result = subprocess.run([str(reference)], text=True, capture_output=True, timeout=20)
            print(result.stdout, end="", flush=True)
            if result.returncode:
                failed.append("linux-reference")
                print(result.stderr[:12000], flush=True)
    return int(bool(failed))


if __name__ == "__main__":
    raise SystemExit(main())
