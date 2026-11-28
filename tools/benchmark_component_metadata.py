#!/usr/bin/env python3
# Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0
"""Compare actual ApkPackage metadata queries in fresh JVMs and retain every sample.

The baseline and candidate compile the complete production ApkPackage/PMS sources
against the same framework/core/app dependencies. No metadata generator is
substituted. CPU/allocation counters cover the calling thread; they are not
whole-process, Windows/ART, retained-heap or application-startup measurements.
"""
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import platform
import statistics
import subprocess
import tempfile

from test_intent_candidate_index import run_intent_candidates

CASES = [("activity-broad", 128, 500), ("service-broad", 128, 500),
         ("receiver-broad", 128, 500), ("activity-broad", 2, 500000),
         ("activity-single", 128, 10000), ("activity-sparse", 128, 5000),
         ("activity-miss", 128, 100000), ("activity-disabled", 128, 500),
         ("activity-broad", 8, 20000)]
ORDER = ("baseline", "candidate", "candidate", "baseline")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("java-home", "framework-classes", "core-classes", "app-classes", "apksig"):
        parser.add_argument("--" + name, type=Path, required=True)
    parser.add_argument("--baseline-ref", required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--samples", type=int, default=7)
    parser.add_argument("--cold-rounds", type=int, default=0,
                        help="Measure only first-query cold controls in this many ABBA rounds")
    parser.add_argument("--cold-components", type=int, nargs="+", default=[2, 8],
                        help="Family sizes for first-query controls (default: 2 8)")
    args = parser.parse_args()
    if args.samples < 1 or args.cold_rounds < 0 or any(count < 1 for count in args.cold_components):
        parser.error("Samples and component counts must be positive; cold rounds must be nonnegative")
    report = {"baseline_ref": args.baseline_ref, "platform": platform.platform(),
              "protocol": "ABBA; each scenario/arm runs in a fresh JVM; all query results are checked",
              "counter_scope": "Calling-thread CPU/allocated work and elapsed query time; not whole-process memory/CPU",
              "cold_scope": "First query before warmup; excludes VM startup and manifest construction",
              "arms": [], "medians": []}
    args.output.parent.mkdir(parents=True, exist_ok=True)

    def save() -> None:
        args.output.write_text(json.dumps(report, indent=2) + "\n")

    with tempfile.TemporaryDirectory(prefix="gesher-component-metadata-") as temporary:
        work = Path(temporary)
        dependencies = {key: getattr(args, key) for key in
                        ("java_home", "framework_classes", "core_classes", "app_classes", "apksig")}
        for mode in ("baseline", "candidate"):
            run_intent_candidates(**dependencies, compile_output=work / mode,
                                  source_ref=args.baseline_ref if mode == "baseline" else None)
        shared = [args.app_classes, args.framework_classes, args.core_classes, args.apksig]
        schedule = ([(round_number, position, mode, "activity-broad", count, 1)
                     for count in args.cold_components for round_number in range(args.cold_rounds)
                     for position, mode in enumerate(ORDER)] if args.cold_rounds else
                    [(0, position, mode, scenario, count, queries)
                     for position, mode in enumerate(ORDER) for scenario, count, queries in CASES])
        for round_number, position, mode, scenario, count, queries in schedule:
            compiled = work / mode
            command = [str(args.java_home / "bin/java"), "-XX:-UsePerfData", "-Xms256m", "-Xmx512m",
                       "-cp", os.pathsep.join(str(p.resolve()) for p in [compiled / "classes", *shared]),
                       "IntentCandidateHost", str(compiled / "libintent_candidates_host.so"),
                       "metadata-benchmark", str(count), str(queries),
                       str(1 if args.cold_rounds else args.samples), scenario]
            env = os.environ.copy()
            env["ANDROID_DATA"] = str(compiled / "data"); env["GESHER_MICROG_SPOOF"] = "0"
            result = subprocess.run(command, env=env, text=True, capture_output=True, check=True, timeout=120)
            cold = [json.loads(line.split(" ", 1)[1]) for line in result.stdout.splitlines()
                    if line.startswith("COMPONENT_METADATA_COLD ")]
            samples = [json.loads(line.split(" ", 1)[1]) for line in result.stdout.splitlines()
                       if line.startswith("COMPONENT_METADATA_SAMPLE ")]
            if len(cold) != 1 or len(samples) != (1 if args.cold_rounds else args.samples):
                raise RuntimeError("Missing complete metadata workload receipt: " + result.stdout + result.stderr)
            report["arms"].append({"round": round_number, "position": position, "mode": mode,
                    "scenario": scenario, "components": count, "queries": queries, "command": command,
                    "compiled_source_manifest": json.loads((compiled / "compiled-sources.json").read_text()),
                    "cold": cold, "samples": samples})
            save()
            print(f"{mode} {scenario} components={count} round={round_number} position={position}", flush=True)

    keys = list(dict.fromkeys((a["scenario"], a["components"], a["queries"]) for a in report["arms"]))
    for scenario, count, queries in keys:
        row = {"scenario": scenario, "components": count, "queries": queries}
        for mode in ("baseline", "candidate"):
            samples = [s for a in report["arms"] if a["mode"] == mode and a["scenario"] == scenario
                       and a["components"] == count for s in a["cold" if args.cold_rounds else "samples"]]
            row[mode] = {key: statistics.median(s[key] for s in samples)
                         for key in ("cpu_ns", "elapsed_ns", "allocated_bytes")}
        row["cpu_change_percent"] = 100 * (row["candidate"]["cpu_ns"] / row["baseline"]["cpu_ns"] - 1)
        row["allocation_change_percent"] = 100 * (row["candidate"]["allocated_bytes"]
                                                / row["baseline"]["allocated_bytes"] - 1)
        report["medians"].append(row)
    save()
    print(json.dumps(report["medians"], indent=2))


if __name__ == "__main__":
    main()
