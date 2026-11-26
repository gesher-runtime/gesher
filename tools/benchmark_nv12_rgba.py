#!/usr/bin/env python3
"""Retain ordinary fresh-process ABBA receipts for the complete camera NV12 delivery."""
import argparse
import datetime
import hashlib
import json
import os
from pathlib import Path
import platform
import statistics
import subprocess
import time

import test_nv12_rgba as fixture

CASES = ["minimum", "tiny", "720p", "1080p", "4k", "window_1080p",
         "scaled_down_control", "scaled_up_control", "odd_target_control", "empty_control"]


def stamp():
    return datetime.datetime.now(datetime.timezone.utc).isoformat()


def load_build(directory, baseline):
    metadata = json.loads((directory / "build.json").read_text())
    binary = directory / "nv12-rgba"
    source, hashes = fixture.generate(metadata["source_ref"])
    assert bool(metadata["source_ref"]) == baseline
    assert metadata["timing"] and not metadata["sanitized"]
    assert metadata["generated_sha256"] == hashlib.sha256(source.encode()).hexdigest()
    assert metadata["source_sha256"] == hashes
    assert metadata["executable_sha256"] == hashlib.sha256(binary.read_bytes()).hexdigest()
    return binary, metadata


def summarize(receipts):
    summary = {}
    for case in CASES:
        summary[case] = {}
        for phase in ("first", "steady"):
            row = summary[case][phase] = {}
            for metric in ("cpu_ns", "wall_ns"):
                values = {}
                for variant in ("baseline", "candidate"):
                    values[variant] = sorted(
                        record[metric] / record["frames"]
                        for receipt in receipts if receipt["variant"] == variant
                        and receipt["case"] == case
                        for record in receipt["observations"] if record["phase"] == phase)
                    sample = values[variant]
                    row[f"{variant}_{metric}_per_frame"] = {
                        "count": len(sample), "median": statistics.median(sample),
                        "min": min(sample), "max": max(sample),
                        "p95_nearest_rank": sample[max(0, (95*len(sample)+99)//100-1)]}
                row[f"{metric}_median_change_percent"] = (
                    statistics.median(values["candidate"]) / statistics.median(values["baseline"])-1)*100
    return summary


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--baseline-build", type=Path, required=True)
    parser.add_argument("--candidate-build", type=Path, required=True)
    parser.add_argument("--blocks", type=int, default=6)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if args.blocks < 1 or args.output.exists():
        parser.error("positive block count and a new output path are required")
    binaries, metadata = {}, {}
    for variant, directory in (("baseline", args.baseline_build), ("candidate", args.candidate_build)):
        binaries[variant], metadata[variant] = load_build(directory, variant == "baseline")
    report = {"schema": 1, "started_utc": stamp(), "host": platform.platform(),
              "machine": platform.machine(), "cpu_affinity": sorted(os.sched_getaffinity(0)),
              "builds": metadata, "blocks": args.blocks,
              "order": "ABBA per case/block; case order rotated by block; fresh process each run",
              "boundary": "Owner creation, actual CameraFrames_deliver including declared JNI array/ANativeWindow adapters, actual QueueFrame/EnqueueImage and native acquisition, full pixel/metadata or padded-buffer oracle, every release, owner close/destruction. Input/oracle preparation outside timing. Ordinary allocators, no observers or sanitizers. First single-frame then steady multi-frame lifecycle in each fresh process. Not ART, GPU, IPC or a physical camera.",
              "clock": "Linux CLOCK_PROCESS_CPUTIME_ID and CLOCK_MONOTONIC, nanoseconds",
              "receipts": []}
    args.output.parent.mkdir(parents=True, exist_ok=True)
    references = {}
    for block in range(args.blocks):
        for case in CASES[block % len(CASES):] + CASES[:block % len(CASES)]:
            for position, variant in enumerate(("baseline", "candidate", "candidate", "baseline")):
                command = [str(binaries[variant]), "--bench", case]
                started, wall_start = stamp(), time.monotonic_ns()
                result = subprocess.run(command, capture_output=True, text=True, timeout=120)
                receipt = {"block": block, "position": position, "variant": variant,
                           "case": case, "started_utc": started, "finished_utc": stamp(),
                           "process_wall_ns": time.monotonic_ns()-wall_start,
                           "command": command, "returncode": result.returncode,
                           "stdout": result.stdout, "stderr": result.stderr}
                report["receipts"].append(receipt)
                args.output.write_text(json.dumps(report, indent=2)+"\n")
                assert result.returncode == 0 and not result.stderr, receipt
                observations = [json.loads(line) for line in result.stdout.splitlines()]
                assert [v["phase"] for v in observations] == ["first", "steady"]
                for observation in observations:
                    assert observation["case"] == case and observation["failures"] == 0
                    assert observation["cpu_ns"] > 0 and observation["wall_ns"] > 0
                    signature = (observation["frames"], observation["checksum"], observation["checks"])
                    key = (case, observation["phase"])
                    assert references.setdefault(key, signature) == signature
                receipt["observations"] = observations
                args.output.write_text(json.dumps(report, indent=2)+"\n")
        print(f"NV12_RGBA_ABBA_BLOCK_PASS {block+1}/{args.blocks}", flush=True)
    report["finished_utc"] = stamp()
    report["summary"] = summarize(report["receipts"])
    args.output.write_text(json.dumps(report, indent=2)+"\n")
    print(json.dumps(report["summary"], indent=2), flush=True)


if __name__ == "__main__":
    main()
