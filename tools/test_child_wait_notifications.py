#!/usr/bin/env python3
"""Exercise actual Bionic child tracking/reaping and signal wait production code.

Win32 process/watcher calls, guest thread registration and signal dispatch are
declared host boundaries. The condition variable and competing host threads are
real. This is not Windows execution or a claim of complete wait-family parity.
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


def section(source: str, start: str, end: str) -> str:
    if source.count(start) != 1 or source.count(end) != 1:
        raise ValueError("Production source boundary changed: " + start)
    at = source.index(start)
    return source[at:source.index(end, at)]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-ref", help="Compile unchanged historical production code")
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--benchmark", action="store_true")
    parser.add_argument("--cxx", default=os.environ.get("CXX", "c++"))
    parser.add_argument("--cc", default=os.environ.get("CC", "cc"))
    args = parser.parse_args()

    def read(path: str) -> str:
        if args.source_ref:
            return subprocess.check_output(["git", "show", args.source_ref + ":" + path],
                                           cwd=ROOT, text=True)
        return (ROOT / path).read_text()

    process = read("compat/bionic/process_bionic.cc")
    signal = read("compat/bionic/signal_bionic.cc")
    parts = {
        "CHILD_REGISTRY": section(process, "struct Child {", "// --- the environment and working directory"),
        "CHILD_WAIT": section(process, "constexpr int kWNOHANG = 1;", "// --- system / popen"),
        "SIGNAL_WAIT": section(signal, "namespace {\nbool SleepWithObservation(", "void DeliverPendingSignals() {"),
    }
    fixture = (ROOT / "tests/native/child_wait_notifications_host.cc.in").read_text()
    for name, body in parts.items():
        marker = "@" + name + "@"
        if fixture.count(marker) != 1:
            raise ValueError("Fixture source boundary changed: " + marker)
        fixture = fixture.replace(marker, body)
        print(name + " SHA256 " + hashlib.sha256(body.encode()).hexdigest(), flush=True)
    with tempfile.TemporaryDirectory(prefix="gesher-child-wait-") as temporary:
        work = Path(temporary)
        source, binary = work / "host.cc", work / "host"
        source.write_text(fixture)
        command = shlex.split(args.cxx) + [
            "-std=c++20", "-O2", "-g", "-Wall", "-Wextra", "-Werror",
            "-Wno-unused-function", "-pthread", "-fno-builtin",
            "-DHAS_CHILD_NOTIFICATIONS=" + str(int("g_children_generation" in process)),
            str(source), "-o", str(binary),
        ]
        if args.sanitize:
            command += ["-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                        "-fno-omit-frame-pointer"]
        subprocess.run(command, check=True, timeout=120)
        result = subprocess.run([str(binary), "benchmark" if args.benchmark else "contracts"],
                                timeout=45)
        if args.benchmark:
            return result.returncode
        reference = work / "linux-reference"
        subprocess.run(shlex.split(args.cc) + ["-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
            "-pthread", "-I", str(ROOT / "tests/exec"),
            str(ROOT / "tests/native/child_wait_reference.c"), "-o", str(reference)], check=True, timeout=60)
        linux = subprocess.run([str(reference)], timeout=20)
        return int(result.returncode != 0 or linux.returncode != 0)


if __name__ == "__main__":
    raise SystemExit(main())
