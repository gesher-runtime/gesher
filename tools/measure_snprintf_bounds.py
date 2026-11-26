#!/usr/bin/env python3
"""Retain every fresh-process ABBA sample from the actual snprintf host entry.

Build both exact source variants with test_snprintf_bounds.py --build-output.
CPU builds must omit --observe-allocations; allocation receipts are separate.
The first call and steady work include complete result/prefix/canary validation.
Input preparation, startup and report serialization are outside the interval.
"""
import argparse
import hashlib
import json
from pathlib import Path
import platform
import statistics
import subprocess
import time

CASES = {"tiny": 1000000, "short": 250000, "truncated": 16, "sizing": 16,
         "literal": 16, "padding": 64, "medium": 10000, "complete": 16, "mixed": 128}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--baseline", type=Path, required=True)
    parser.add_argument("--current", type=Path, required=True)
    parser.add_argument("--rounds", type=int, default=6)
    parser.add_argument("--allocations", action="store_true",
                        help="Separate observed builds, three calls per case; requires --rounds 1")
    parser.add_argument("--cases", nargs="+", choices=CASES, default=list(CASES))
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if args.rounds < 1 or args.output.exists():
        raise SystemExit("Positive rounds and a new output path are required")
    if args.allocations and args.rounds != 1:
        parser.error("deterministic allocation receipts require --rounds 1")
    folders = {"baseline": args.baseline.resolve(), "current": args.current.resolve()}
    result = {"schema": 1, "started_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
              "platform": platform.platform(), "rounds": args.rounds,
              "order": ["baseline", "current", "current", "baseline"],
              "scope": "Actual guest parser/va-list/entry; glibc conversion boundary; complete output checks; ordinary allocators",
              "sources": {}, "samples": [], "summary": {}}
    if args.allocations:
        result["scope"] = "Separate C++ allocation observation; no CPU inference; complete output checks"
        result["order"] = ["baseline", "current"]
    for variant, folder in folders.items():
        result["sources"][variant] = {
            "inputs": json.loads((folder / "source-hashes.json").read_text()),
            "executable_sha256": hashlib.sha256((folder / "snprintf-bounds").read_bytes()).hexdigest(),
            "build": json.loads((folder / "build.json").read_text())}
    args.output.parent.mkdir(parents=True, exist_ok=True)
    for case in args.cases:
        for round_number in range(args.rounds):
            for order, variant in enumerate(result["order"]):
                completed = subprocess.run([str(folders[variant] / "snprintf-bounds"),
                    "--benchmark", case, str(3 if args.allocations else CASES[case])],
                    text=True, capture_output=True, timeout=120)
                if completed.returncode:
                    result["failed_process"] = {"case": case, "variant": variant,
                        "round": round_number, "stdout": completed.stdout, "stderr": completed.stderr,
                        "returncode": completed.returncode}
                    args.output.write_text(json.dumps(result, indent=2) + "\n")
                    raise SystemExit("Benchmark failed; failure and all preceding samples retained")
                records = [json.loads(line) for line in completed.stdout.splitlines()]
                if len(records) != 2 or [r["phase"] for r in records] != ["first", "steady"]:
                    raise AssertionError("Missing first-call/steady receipt")
                for record in records:
                    if (record["cpp_allocations"] is not None) != args.allocations:
                        raise AssertionError("Allocator observation mode does not match this comparison")
                    record.update(variant=variant, round=round_number, order=order)
                    result["samples"].append(record)
                args.output.write_text(json.dumps(result, indent=2) + "\n")
        medians = {}
        for variant in folders:
            rows = [s for s in result["samples"] if s["case"] == case and
                    s["variant"] == variant and s["phase"] == "steady"]
            metrics = ("cpp_allocations", "cpp_requested_bytes", "cpp_peak_live_bytes") if args.allocations else ("cpu_ns", "wall_ns")
            medians[variant] = {metric: statistics.median(r[metric] /
                (1 if metric == "cpp_peak_live_bytes" else r["iterations"]) for r in rows) for metric in metrics}
        result["summary"][case] = medians
        print(case, json.dumps(medians), flush=True)
    result["finished_utc"] = time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())
    args.output.write_text(json.dumps(result, indent=2) + "\n")
    print("retained", len(result["samples"]), "samples in", args.output, flush=True)


if __name__ == "__main__":
    main()
