#!/usr/bin/env python3
"""Exercise actual Track unity-rate mixing, compare state/PCM and measure CPU.

The existing audio-state generator supplies complete production Track methods
and declared Engine/clock/CV boundaries. Availability/read counters observe
production statements; benchmark binaries omit those counters. No WASAPI,
Windows device, ART or application-wide performance is inferred.
"""
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import shlex
import statistics
import subprocess
import tempfile

from test_audio_state_contracts import generate

ROOT = Path(__file__).resolve().parents[1]


def source(ref: str | None, observe: bool) -> str:
    production = generate("audio", ref)
    boundary = "namespace {\nusing namespace gesher::media;"
    if production.count(boundary) != 1:
        raise ValueError("audio state fixture boundary changed")
    production = production[:production.index(boundary)]
    if observe:
        for marker, counter in [
            ("int64_t center = static_cast<int64_t>(std::floor(rs.t));", "availability_checks"),
            ("size_t got = ReadSource(tmp.data(), want, &src_ended);", "source_reads"),
        ]:
            if production.count(marker) != 1:
                raise ValueError("unity-rate observation boundary changed: " + marker)
            production = production.replace(marker, "++" + counter + "; " + marker)
    fixture = (ROOT / "tests/media/audio_unity_rate_host.cc.in").read_text()
    return fixture.replace("@PRODUCTION@", production)


def same_bytes(candidate: Path, reference: Path) -> None:
    size = 0
    with candidate.open("rb") as a, reference.open("rb") as b:
        while True:
            aa, bb = a.read(1 << 20), b.read(1 << 20)
            if aa != bb:
                raise SystemExit("Unity-rate PCM, state or callback transcript differs from reference")
            size += len(aa)
            if not aa:
                break
    print(f"Bit-exact unity PCM/state/callback comparison: {size} bytes PASS", flush=True)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-ref", help="Run this unchanged fixture on an older production revision")
    parser.add_argument("--compare-ref", help="Compare complete PCM/state/callback transcripts with this revision")
    parser.add_argument("--measure-only", action="store_true", help="Disable only the deterministic work budget")
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--benchmark", action="store_true", help="Alternate uninstrumented per-thread CPU measurements")
    parser.add_argument("--rounds", type=int, default=7)
    parser.add_argument("--cxx", default=os.environ.get("CXX", "c++"))
    args = parser.parse_args()
    if args.benchmark and (not args.compare_ref or args.sanitize or args.rounds < 3):
        parser.error("--benchmark requires --compare-ref, at least 3 rounds and no --sanitize")
    with tempfile.TemporaryDirectory(prefix="gesher-audio-unity-") as temporary:
        work = Path(temporary)

        def build(label: str, ref: str | None, observe: bool) -> Path:
            cpp = work / (label + ".cc")
            cpp.write_text(source(ref, observe))
            binary = work / label
            command = shlex.split(args.cxx) + [
                "-std=c++20", "-O2", "-g", "-Wall", "-Wextra", "-Werror", "-fno-exceptions",
                "-pthread", str(cpp), "-o", str(binary)]
            if args.sanitize:
                command += ["-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                            "-fno-omit-frame-pointer"]
            subprocess.run(command, check=True, timeout=120)
            return binary

        def contracts(label: str, ref: str | None, gate: bool) -> tuple[int, Path]:
            binary = build(label, ref, True)
            output = work / (label + ".bin")
            result = subprocess.run([str(binary), str(output), "gate" if gate else "measure"],
                                    check=False, timeout=120)
            return result.returncode, output

        result, candidate = contracts("candidate", args.source_ref, not args.measure_only)
        if args.compare_ref:
            reference_result, reference = contracts("reference", args.compare_ref, False)
            if reference_result:
                return reference_result
            same_bytes(candidate, reference)
        if result or not args.benchmark:
            return result
        binaries = {"candidate": build("bench_candidate", args.source_ref, False),
                    "reference": build("bench_reference", args.compare_ref, False)}
        for channels, rate, stream in [(1, 48000, False), (2, 48000, False), (2, 48000, True),
                                       (8, 48000, False), (2, 44100, False), (2, 96000, False)]:
            samples: dict[str, list[int]] = {"candidate": [], "reference": []}
            identity = None
            for iteration in range(args.rounds):
                order = ("reference", "candidate") if iteration % 2 == 0 else ("candidate", "reference")
                for label in order:
                    text = subprocess.check_output([str(binaries[label]), "bench", str(channels),
                                                    str(rate), str(int(stream))], text=True, timeout=60)
                    value = json.loads(text)
                    observed = {key: item for key, item in value.items() if key != "cpu_ns"}
                    if identity is not None and observed != identity:
                        raise SystemExit("Benchmark output, frame count or position differs between repetitions")
                    identity = observed
                    samples[label].append(value["cpu_ns"])
            medians = {key: statistics.median(values) for key, values in samples.items()}
            print(json.dumps({"workload": identity, "rounds": args.rounds, "cpu_ns": samples,
                              "median_cpu_ns": medians,
                              "speedup": medians["reference"] / medians["candidate"]}), flush=True)
        return 0


if __name__ == "__main__":
    raise SystemExit(main())
