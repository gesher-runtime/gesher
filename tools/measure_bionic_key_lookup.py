#!/usr/bin/env python3
"""Build/replay actual pthread-key source components in fresh-process ABBA order.

The production sections and real host pthread TLS-exit path are unchanged;
only the Windows ThreadRecord boundary is supplied. Separate counted binaries
observe registry-lock entries. Their clocks are never performance evidence.
Timed workloads use separate fresh processes, and each worker records CPU time
through its actual TLS destructor. Process CPU must cover that lower bound.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import shlex
import subprocess
import time

from test_bionic_key_contracts import ROOT, section


def digest(data):
    return hashlib.sha256(data).hexdigest()


def run(command, timeout=180):
    result = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            text=True, timeout=timeout)
    if result.returncode:
        raise RuntimeError(f"Command failed ({result.returncode}): {command}\n{result.stdout}")
    return result.stdout


def build(args):
    folder = args.output_dir.resolve()
    folder.mkdir(parents=True, exist_ok=True)
    path = "compat/bionic/pthread_bionic.cc"
    source = (subprocess.check_output(["git", "show", args.source_ref + ":" + path], cwd=ROOT)
              if args.source_ref else (ROOT / path).read_bytes())
    (folder / "pthread_bionic.cc").write_bytes(source)
    text = source.decode()
    body = "\n".join([
        section(text, "constexpr int kKeysMax", "// --- threads"),
        "struct ThreadRecord {};\nthread_local ThreadRecord* t_record = nullptr;",
        section(text, "pthread_key_t g_host_exit_key;", "struct StartArg"),
        "}\nextern \"C\" {",
        section(text, "int BIONIC_SYSV gesher_bionic_pthread_key_create(",
                "int BIONIC_SYSV gesher_bionic_register_atfork("),
        "}",
    ])
    lock = "std::lock_guard<std::mutex> lock(g_keys_mutex);"
    lock_sites = body.count(lock)
    if lock_sites not in (4, 5):
        raise ValueError("Production registry lock boundaries changed")
    prefix = '''#include "pthread_key_measurement.h"
#include "pthread_cleanup.h"
#include <atomic>
#include <mutex>
#include <pthread.h>
#include <vector>
namespace gesher::bionic {
namespace {
uint64_t g_measure_locks = 0;
'''
    live_array = "g_key_live_generations" if "g_key_live_generations" in text else "g_key_used"
    suffix = '''
uint64_t KeyMeasureRegistryLocks() { return g_measure_locks; }
size_t KeyMeasureGlobalArrays() {
  return sizeof(g_key_dtors) + sizeof(g_key_generations) + sizeof(@LIVE@);
}
size_t KeyMeasureLocalArrays() { return sizeof(t_key_values) + sizeof(t_key_generations); }
bool KeyMeasureInstrumented() { return @COUNTED@; }
// Keep the complete real explicit-exit body in this source unit as well.
void KeyMeasureExplicitExit() { RunThreadExit(); }
}
'''.replace("@LIVE@", live_array)
    cxx = shlex.split(args.cxx)
    receipts = []
    for counted in (False, True):
        name = "counted" if counted else "timed"
        generated_text = prefix + (body.replace(lock, lock + "\n      ++g_measure_locks;")
                                   if counted else body) + suffix.replace("@COUNTED@", str(counted).lower())
        generated = folder / (name + "-production.cc")
        generated.write_text(generated_text)
        command = cxx + ["-std=c++20", "-O2", "-g", "-Wall", "-Wextra", "-Werror",
                         "-fno-exceptions", "-fno-builtin", "-fno-lto", "-pthread",
                         "-I", str(ROOT / "compat/bionic"),
                         "-I", str(ROOT / "compat/arm64/include"),
                         "-I", str(ROOT / "tests/native"), str(generated),
                         str(ROOT / "tests/native/pthread_key_measurement.cc"),
                         "-o", str(folder / name)]
        (folder / (name + "-compile.log")).write_text(run(command))
        receipts.append({"name": name, "command": command, "instrumented": counted,
                         "generated_sha256": digest(generated.read_bytes()),
                         "executable_sha256": digest((folder / name).read_bytes())})
    inputs = ["tests/native/pthread_key_measurement.cc", "tests/native/pthread_key_measurement.h",
              "tools/measure_bionic_key_lookup.py", "tools/test_bionic_key_contracts.py",
              "compat/bionic/shim_internal.h", "compat/bionic/abi_types.h", "compat/bionic/pthread_cleanup.h",
              "compat/arm64/include/gesher/arm64_va_list.h", "compat/arm64/include/gesher/symbol_table.h"]
    manifest = {"source_ref": args.source_ref, "production_sha256": digest(source),
                "registry_lock_sites": lock_sites,
                "inputs": {p: digest((ROOT / p).read_bytes()) for p in inputs},
                "compiler": run(cxx + ["--version"]), "builds": receipts}
    (folder / "build.json").write_text(json.dumps(manifest, indent=2) + "\n")
    print(json.dumps({"built": str(folder), "source_sha256": digest(source), "lock_sites": lock_sites}))


def measure(args):
    output = args.output.resolve()
    output.parent.mkdir(parents=True, exist_ok=True)
    folders = {"baseline": args.baseline.resolve(), "candidate": args.candidate.resolve()}
    cases = ["cold-single", "single", "shared8", "distinct8", "distinct32", "churn8",
             "tiny-lifecycle", "no-read-lifecycle", "mutation-control"]
    report = {"schema": 2, "boundary": "actual key/exit sections; host pthread TLS; supplied Windows ThreadRecord",
              "environment": {"platform": platform.platform(), "cpu_count": os.cpu_count(),
                              "affinity": sorted(os.sched_getaffinity(0)),
                              "process_clock_resolution_seconds": time.clock_getres(time.CLOCK_PROCESS_CPUTIME_ID),
                              "thread_clock_resolution_seconds": time.clock_getres(time.CLOCK_THREAD_CPUTIME_ID)},
              "method": {"rounds": args.rounds, "order": "ABBA", "samples_per_case": args.samples,
                         "warmup_samples": [0, 1], "first_stream_per_process": True,
                         "separate_counted_binaries": True, "cases": cases,
                         "fresh_process_per_workload": True,
                         "worker_cpu_receipts": "entry through actual TLS destructor; churn writer entry through return",
                         "process_cpu_consistency": "must cover caller thread plus all measured worker CPU"},
              "builds": {name: json.loads((path / "build.json").read_text()) for name, path in folders.items()},
              "processes": [], "counted_processes": []}
    def save():
        output.write_text(json.dumps(report, indent=2) + "\n")
    def execute(variant, counted, sequence, case=None, round_number=None):
        binary = folders[variant] / ("counted" if counted else "timed")
        manifest = report["builds"][variant]
        receipt = next(x for x in manifest["builds"] if x["instrumented"] == counted)
        if digest(binary.read_bytes()) != receipt["executable_sha256"]:
            raise ValueError("Measurement executable changed: " + str(binary))
        command = [str(binary), "1" if counted else str(args.samples)]
        if case:
            command.append(case)
        started = time.time_ns()
        result = subprocess.run(command, text=True, stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT, timeout=180)
        log = output.with_name(output.stem + f"-{('counted' if counted else 'timed')}-{sequence:02d}-{variant}.log")
        log.write_text(result.stdout)
        record = {"sequence": sequence, "variant": variant, "command": command,
                  "case": case, "round": round_number,
                  "started_unix_ns": started, "ended_unix_ns": time.time_ns(),
                  "returncode": result.returncode, "log_sha256": digest(log.read_bytes()),
                  "observations": [json.loads(line) for line in result.stdout.splitlines() if line.startswith('{')]}
        report["counted_processes" if counted else "processes"].append(record)
        save()
        expected = len(cases) if counted else (1 if case == "cold-single" else args.samples)
        if result.returncode or len(record["observations"]) != expected:
            raise RuntimeError("Incomplete actual measurement: " + str(log))
        if any(r["instrumented"] != counted for r in record["observations"]):
            raise ValueError("Counted and timed evidence must remain separate")
        for row in record["observations"]:
            if case and row["case"] != case:
                raise ValueError("Wrong measured workload")
            if (not row["cpu_consistent"] or sum(row["worker_cpu_samples_ns"]) != row["worker_cpu_ns"]
                    or row["cpu_ns"] < row["main_cpu_ns"] + row["worker_cpu_ns"]):
                raise ValueError("Process CPU receipt does not cover actual thread receipts")
        print(f"PASS {variant} {'counted' if counted else 'timed'} process={sequence} case={case}", flush=True)
    save()
    sequence = 0
    for round_number in range(args.rounds):
        for case in cases:
            for variant in ("baseline", "candidate", "candidate", "baseline"):
                execute(variant, False, sequence, case, round_number)
                sequence += 1
    for sequence, variant in enumerate(folders):
        execute(variant, True, sequence)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-only", action="store_true")
    parser.add_argument("--source-ref")
    parser.add_argument("--output-dir", type=Path)
    parser.add_argument("--cxx", default=os.environ.get("CXX", "c++"))
    parser.add_argument("--baseline", type=Path)
    parser.add_argument("--candidate", type=Path)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--rounds", type=int, default=4)
    parser.add_argument("--samples", type=int, default=10)
    args = parser.parse_args()
    if args.build_only:
        if not args.output_dir or args.baseline or args.candidate or args.output:
            parser.error("build requires --output-dir and no measurement inputs")
        build(args)
    else:
        if not all((args.baseline, args.candidate, args.output)) or args.source_ref or args.output_dir:
            parser.error("measurement requires --baseline, --candidate and --output")
        if not (1 <= args.rounds <= 32 and 3 <= args.samples <= 100):
            parser.error("rounds must be1..32 and samples3..100")
        measure(args)


if __name__ == "__main__":
    main()
