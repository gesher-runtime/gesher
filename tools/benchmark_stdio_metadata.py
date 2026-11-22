#!/usr/bin/env python3
"""A/B actual guest Fgetc and shared-owner bodies against an immutable revision.

Both executables use the stdio contract fixture's explicit Linux boundaries,
including real descriptors and the declared CRT lock-lifetime envelope. The
complete production owner is executed; its metadata lookup is never replaced.
File creation and thread startup are outside timing, every byte/EOF and close
are checked, and the order alternates. These are host-envelope measurements,
not Windows application performance or a runtime approval gate.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import statistics
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
INPUTS = (
    "compat/bionic/stdio_bionic.cc", "compat/posix/src/stdio_output.cc",
    "compat/posix/src/fsops.cc", "compat/posix/src/fd.cc",
    "compat/posix/include/gesher/stdio.h", "compat/posix/include/gesher/cancellation.h",
    "compat/posix/src/stdio_internal.h", "compat/posix/src/stdio_stream_buffer.h",
    "compat/posix/src/stdio_output_buffer.h",
)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--baseline-ref", default="537936e89a450107821b0bcf4cd40cec0d060f24")
    parser.add_argument("--cxx", default=os.environ.get("CXX", "c++"))
    parser.add_argument("--bytes", type=int, default=256 * 1024)
    parser.add_argument("--runs", type=int, default=7)
    args = parser.parse_args()
    if not 1 <= args.bytes <= 16 * 1024 * 1024 or not 1 <= args.runs <= 31:
        parser.error("--bytes must be 1..16777216 and --runs must be 1..31")
    baseline = subprocess.check_output(
        ["git", "rev-parse", "--verify", args.baseline_ref + "^{commit}"], cwd=ROOT, text=True).strip()
    sources = {}
    for name, revision in (("before", baseline), ("after", None)):
        sources[name] = {}
        for path in INPUTS:
            raw = subprocess.check_output(["git", "show", revision + ":" + path], cwd=ROOT) \
                if revision else (ROOT / path).read_bytes()
            sources[name][path] = hashlib.sha256(raw).hexdigest()
    print(json.dumps({"baseline_commit": baseline, "input_sha256": sources}, sort_keys=True), flush=True)
    with tempfile.TemporaryDirectory(prefix="gesher-stdio-metadata-bench-") as directory:
        executables = {name: Path(directory) / name for name in ("before", "after")}
        for name, executable in executables.items():
            command = [sys.executable, str(ROOT / "tools/test_stdio_contracts.py"),
                       "--cxx", args.cxx, "--adapter-case", "owner-cache", "--build-adapter", str(executable)]
            if name == "before":
                command += ["--source-ref", baseline]
            subprocess.run(command, cwd=ROOT, check=True, timeout=180)
        for threads in (1, 4):
            samples = {name: [] for name in executables}
            for run in range(args.runs):
                for name in (("before", "after") if run % 2 == 0 else ("after", "before")):
                    output = subprocess.check_output(
                        [str(executables[name]), "--owner-cache-benchmark", str(args.bytes), str(threads)],
                        cwd=directory, text=True, timeout=120)
                    result = json.loads(output)
                    if result["threads"] != threads or result["bytes_each"] != args.bytes or \
                            result["checksum"] != threads * args.bytes * ord("q") or result["errors"] != 0:
                        raise AssertionError("benchmark did not preserve actual byte/EOF/close effects")
                    samples[name].append(result["milliseconds"])
                    print(json.dumps({"variant": name, "run": run, **result}, sort_keys=True), flush=True)
            medians = {name: statistics.median(values) for name, values in samples.items()}
            print(json.dumps({"threads": threads, "median_ms": medians,
                              "after_before_ratio": medians["after"] / medians["before"]}, sort_keys=True), flush=True)


if __name__ == "__main__":
    main()
