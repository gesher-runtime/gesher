#!/usr/bin/env python3
"""Measure complete actual key assignment/clear/native-exit components.

Shares the established production extraction, separate counted build, fresh
ABBA processes and CPU-consistency checks with the key lookup measurement.
The driver executes real ArmHostExit and host pthread TLS callbacks; it supplies
only the Windows ThreadRecord boundary, as documented in the source helper.
"""
import argparse
import json
import os
from pathlib import Path

from measure_bionic_key_lookup import build, measure

CASES = ["cold-single", "single", "shared2", "shared8", "distinct8", "distinct32", "clear8",
         "churn8", "tiny-lifecycle", "no-set-lifecycle", "create-delete-control", "read-only-control"]


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
        build(args, "tests/native/pthread_set_measurement.cc", "tools/measure_bionic_key_assignment.py")
    else:
        if not all((args.baseline, args.candidate, args.output)) or args.source_ref or args.output_dir:
            parser.error("measurement requires --baseline, --candidate and --output")
        if not (1 <= args.rounds <= 32 and 3 <= args.samples <= 100):
            parser.error("rounds must be1..32 and samples3..100")
        baseline = json.loads((args.baseline / "build.json").read_text())
        candidate = json.loads((args.candidate / "build.json").read_text())
        if baseline["registry_lock_sites"] != 4 or candidate["registry_lock_sites"] != 3:
            raise ValueError("Assignment comparison requires the reviewed lock-free getter baseline")
        measure(args, CASES, "assigned workers: entry through actual TLS destructor; no-set/churn workers: entry through return")
        data = json.loads(args.output.read_text())
        for process in data["counted_processes"]:
            locked = process["variant"] == "baseline"
            for row in process["observations"]:
                expected = 2 * row["created"] + row["exits"] + (row["sets"] if locked else 0)
                if row["registry_locks"] != expected:
                    raise ValueError("Unexpected actual lock work: " + str(row))
        print("PASS actual assignment lock budgets for every complete component")


if __name__ == "__main__":
    main()
