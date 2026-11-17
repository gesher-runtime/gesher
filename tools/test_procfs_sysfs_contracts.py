#!/usr/bin/env python3
"""Run virtual procfs & sysfs contract tests (Roadmap N04, Issue #56).

Tests production guest contracts:
  - /proc/self/auxv and /proc/<pid>/auxv: ELF auxiliary vectors
  - /proc/self/maps and /proc/<pid>/maps: formatted memory maps
  - /proc/self/cmdline and /proc/<pid>/cmdline: null-separated arguments and setArgV0
  - /proc/cpuinfo: host CPU architecture and feature flags
  - /proc/meminfo: standard Linux memory statistics
  - /sys/devices/system/cpu/online, present, possible: cpu topology lists
"""
from __future__ import annotations

import argparse
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX", "c++"))
    parser.add_argument("--source-ref")
    parser.add_argument("--compile-only", action="store_true")
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()

    if args.compile_only and args.output is None:
        parser.error("--compile-only requires --output")
    if os.name != "nt" and not args.compile_only:
        parser.error("this fixture executes Windows procfs contracts")

    if args.source_ref:
        source = subprocess.check_output(
            ["git", "show", f"{args.source_ref}:compat/posix/src/procfs.cc"],
            cwd=ROOT,
            text=True,
        )
    else:
        source = (ROOT / "compat/posix/src/procfs.cc").read_text(encoding="utf-8")

    start_marker = "namespace gesher {"
    if source.count(start_marker) < 1:
        raise ValueError("procfs source boundary changed: namespace gesher not found")
    procfs_body = source[source.index(start_marker):]

    template = (ROOT / "tests/native/procfs_sysfs_contract_host_test.cc.in").read_text(encoding="utf-8")
    content = template.replace("@PROCFS_SOURCE@", procfs_body)

    with tempfile.TemporaryDirectory(prefix="gesher-procfs-sysfs-") as temporary:
        folder = Path(temporary)
        cpp = folder / "test.cc"
        executable = args.output.resolve() if args.output else folder / "test.exe"
        executable.parent.mkdir(parents=True, exist_ok=True)
        cpp.write_text(content, encoding="utf-8")

        command = (
            shlex.split(args.cxx)
            + [
                "-std=c++20",
                "-O2",
                "-Wall",
                "-Wextra",
                "-Werror",
                "-fno-exceptions",
                "-static",
                str(cpp),
                "-o",
                str(executable),
                "-lpsapi",
                "-lbcrypt",
                "-ladvapi32",
                "-lshell32",
                "-lole32",
            ]
        )
        subprocess.run(command, check=True, timeout=120)
        if args.compile_only:
            print(f"Compiled procfs/sysfs test: {executable}", flush=True)
        else:
            subprocess.run([str(executable)], check=True, timeout=45)


if __name__ == "__main__":
    main()
