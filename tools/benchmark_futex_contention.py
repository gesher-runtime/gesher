#!/usr/bin/env python3
"""Compare actual production futex queue paths with host mutex/wait adapters.

Timing is diagnostic, never a CI threshold. This measures empty wake and
expected-value mismatch paths, not blocking transport or application speed.
Use futex_test.exe --benchmark to measure the same calls on native Windows.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import shlex
import statistics
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parent.parent


def build_source(source, args, directory):
    (directory / "windows.h").write_text('#include "futex_windows_host.h"\n')
    (directory / "gesher").mkdir()
    for name in ("clocks.h", "awake_clock.h"):
        (directory / "gesher" / name).write_text(
            (ROOT / "compat/posix/include/gesher" / name).read_text())
    adapter = (ROOT / "tests/native/futex_windows_host.h").read_text()
    lock = "static void AcquireSRWLockExclusive(SRWLOCK* p) { p->lock(); }"
    if adapter.count(lock) != 1:
        raise ValueError("ambiguous host lock adapter")
    adapter = adapter.replace(lock, """
static thread_local unsigned long long futex_bench_acquisitions, futex_bench_contentions;
static void AcquireSRWLockExclusive(SRWLOCK* p) {
  ++futex_bench_acquisitions;
  if (!p->try_lock()) { ++futex_bench_contentions; p->lock(); }
}
""")
    (directory / "futex_windows_host.h").write_text(adapter)
    generated = """#define _GNU_SOURCE 1
#include <time.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <linux/futex.h>
#include "futex_windows_host.h"
#define syscall gesher_test_syscall
""" + source + """
#undef syscall
static FARPROC GetProcAddress(HMODULE, const char* name) {
  return std::strcmp(name,"gesher_futex_owner_v1") == 0 ?
      reinterpret_cast<FARPROC>(&gesher_futex_owner_v1) : nullptr;
}
static long invoke(volatile int* a,int op,int n,const timespec* t,volatile int* b,int m) {
  return gesher_test_syscall(SYS_futex,a,op,n,t,b,m);
}
#define FUTEX_BENCH_LOCK_COUNTS 1
#include "futex_contention_benchmark.h"
int main() { return futex_contention_benchmark(invoke,""" + str(args.iterations) + ",1); }\n"
    (directory / "benchmark.cc").write_text(generated)
    command = shlex.split(args.cxx) + [
        "-std=c++20", "-O2", "-g", "-Wall", "-Wextra", "-Werror",
        "-Wno-unused-function", "-pthread", "-I", str(directory),
        "-I", str(ROOT / "tests/native"), str(directory / "benchmark.cc"),
        "-o", str(directory / "benchmark")]
    subprocess.run(command, check=True, timeout=120)
    return directory / "benchmark"


def summary(rows):
    result = []
    for count in (1, 2, 8, 32):
        for shared in (False, True):
            for operation in ("wake", "mismatch"):
                samples = [r for r in rows if (r["threads"], r["shared"], r["operation"]) ==
                           (count, shared, operation)]
                result.append({
                    "threads": count, "shared": shared, "operation": operation,
                    "median_wall_seconds": statistics.median(r["wall_seconds"] for r in samples),
                    "wall_range_seconds": [min(r["wall_seconds"] for r in samples),
                                           max(r["wall_seconds"] for r in samples)],
                    "contended_acquisitions": sum(r["contended_acquisitions"] for r in samples),
                    "lock_attempts": sum(r["lock_attempts"] for r in samples),
                    "errors": sum(r["errors"] for r in samples),
                })
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--baseline-ref", help="git ref whose production syscall.cc is compared")
    parser.add_argument("--source", type=Path, default=ROOT / "compat/posix/src/syscall.cc")
    parser.add_argument("--cxx", default=os.environ.get("CXX", "c++"))
    parser.add_argument("--iterations", type=int, default=100000)
    parser.add_argument("--samples", type=int, default=5)
    parser.add_argument("--output", type=Path, help="write raw samples and metadata as JSON")
    args = parser.parse_args()
    if args.iterations <= 0 or args.samples <= 0:
        parser.error("iterations and samples must be positive")
    sources = {"candidate": args.source.read_text()}
    baseline_sha = None
    if args.baseline_ref:
        baseline_sha = subprocess.check_output(
            ["git", "rev-parse", "--verify", args.baseline_ref + "^{commit}"], cwd=ROOT, text=True).strip()
        sources = {"baseline": subprocess.check_output(
            ["git", "show", baseline_sha + ":compat/posix/src/syscall.cc"], cwd=ROOT, text=True), **sources}
    report = {
        "boundary": "production futex entry/queue logic with instrumented host mutex transport",
        "platform": platform.platform(), "compiler": subprocess.check_output(
            shlex.split(args.cxx) + ["--version"], text=True).splitlines()[0],
        "baseline_commit": baseline_sha, "iterations_per_thread": args.iterations,
        "samples_per_case": args.samples, "variants": {},
    }
    rows_by_name = {name: [] for name in sources}
    with tempfile.TemporaryDirectory(prefix="gesher-futex-benchmark-") as temporary:
        binaries = {}
        for name, source in sources.items():
            directory = Path(temporary) / name
            directory.mkdir()
            binaries[name] = build_source(source, args, directory)
        names = list(sources)
        for sample in range(args.samples):
            # Alternate A/B and B/A to reduce ordering and host-load bias.
            order = names if sample % 2 == 0 else list(reversed(names))
            for name in order:
                result = subprocess.run([str(binaries[name])], check=True,
                                        capture_output=True, text=True, timeout=180)
                for line in result.stdout.splitlines():
                    row = json.loads(line)
                    row["sample"] = sample
                    rows_by_name[name].append(row)
    for name, source in sources.items():
        rows = rows_by_name[name]
        report["variants"][name] = {
            "source_sha256": hashlib.sha256(source.encode()).hexdigest(),
            "samples": rows, "summary": summary(rows),
        }
        print(json.dumps({"variant": name, **{k: v for k, v in report["variants"][name].items()
                                             if k != "samples"}}), flush=True)
    if args.output:
        args.output.write_text(json.dumps(report, indent=2) + "\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
