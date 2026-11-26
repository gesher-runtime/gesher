#!/usr/bin/env python3
"""Run actual Track resampling against scalar taps and the complete old source.

Only the established Engine attachment, clock and callback transport are host
boundaries. Correctness executables observe real Kernel calls; ordinary timing
executables are built separately with normal allocators and no work counters.
No WASAPI, ART or application-wide result is inferred from this host fixture.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

from test_audio_state_contracts import generate

ROOT = Path(__file__).resolve().parents[1]
BASELINE = "bcfb5044029d486e17138dc417f3ced81524cf3f"


def production(ref: str | None, observe: bool) -> str:
    value = generate("audio", ref)
    boundary = "namespace {\nusing namespace gesher::media;"
    if value.count(boundary) != 1:
        raise ValueError("actual audio-state fixture boundary changed")
    value = value[:value.index(boundary)]
    if value.count("ObservedConditionVariable room_;") != 1:
        raise ValueError("actual Track condition variable boundary changed")
    value = value.replace("ObservedConditionVariable room_;", "std::condition_variable room_;")
    if observe:
        before = "inline float Kernel(const float* table, double x) {"
        if value.count(before) != 1:
            raise ValueError("actual coefficient observation boundary changed")
        value = value.replace(before, before + "\n  ++resampler_kernel_calls;")
    return value


def source(ref: str | None) -> str:
    common = (ROOT / "tests/media/audio_unity_rate_host.cc.in").read_text()
    start = "namespace {\nusing namespace gesher::media;"
    end = "// A valid retained history can end one ULP below an integer"
    if common.count(start) != 1 or common.count(end) != 1:
        raise ValueError("maintained scalar/source/state helpers changed")
    common = common[common.index(start):common.index(end)]
    fixture = (ROOT / "tests/media/audio_resampler_weights_host.cc.in").read_text()
    actual = production(ref, True)
    if "int ExactHalfPhase(double position, int64_t center)" not in actual:
        # The unchanged old source has no optimization classifier. Do not
        # synthesize it or claim that the baseline executes these new checks.
        begin = fixture.index("void ExactPhaseBounds() {")
        end = fixture.index("void ZeroWeightNonfinite", begin)
        fixture = fixture[:begin] + fixture[end:]
        fixture = fixture.replace("@PHASE_BOUNDS@", "")
    else:
        fixture = fixture.replace("@PHASE_BOUNDS@", "ExactPhaseBounds();")
    return fixture.replace("@PRODUCTION@", actual).replace("@COMMON@", common)


def replace_once(text: str, before: str, after: str) -> str:
    if text.count(before) != 1:
        raise ValueError("ambiguous production mutation boundary: " + before)
    return text.replace(before, after, 1)


def run(args: argparse.Namespace, work: Path) -> None:
    receipts = {"baseline": args.compare_ref, "runs": []}
    candidate = source(None)
    baseline = source(args.compare_ref)

    def execute(label: str, text: str, gate: bool, full: bool, expected: str | None = None) -> Path:
        cpp = work / (label + ".cc"); cpp.write_text(text)
        binary = work / label
        command = shlex.split(args.cxx) + ["-std=c++20", "-O2", "-g", "-Wall", "-Wextra", "-Werror",
                                         "-fno-exceptions", "-pthread", str(cpp), "-o", str(binary)]
        if args.sanitize:
            command += ["-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-fno-omit-frame-pointer"]
        build = subprocess.run(command, capture_output=True, text=True, timeout=120)
        (work / (label + ".compile.log")).write_text(build.stdout + build.stderr)
        if build.returncode:
            raise RuntimeError(label + " compilation failed: " + build.stderr)
        output = work / (label + ".bin")
        invocation = [str(binary), str(output), "gate" if gate else "reference", "full" if full else "focus"]
        result = subprocess.run(invocation, capture_output=True, text=True, timeout=120)
        (work / (label + ".log")).write_text(result.stdout + result.stderr)
        receipt = {"label": label, "source_sha256": hashlib.sha256(text.encode()).hexdigest(),
                   "binary_sha256": hashlib.sha256(binary.read_bytes()).hexdigest(),
                   "compile": command, "invocation": invocation, "returncode": result.returncode,
                   "stdout": result.stdout, "stderr": result.stderr, "required_assertion": expected,
                   "transcript_bytes": output.stat().st_size,
                   "transcript_sha256": hashlib.sha256(output.read_bytes()).hexdigest()}
        receipts["runs"].append(receipt)
        (work / "source.json").write_text(json.dumps(receipts, indent=2) + "\n")
        if expected:
            if result.returncode != 1 or "FAIL unity mixer: " + expected not in result.stderr or "Sanitizer" in result.stderr:
                raise AssertionError(label + " did not fail at its required assertion: " + result.stdout + result.stderr)
        elif result.returncode or result.stderr:
            raise AssertionError(label + " failed: " + result.stdout + result.stderr)
        print(result.stdout.strip(), flush=True)
        if expected: print("RESAMPLER_NEGATIVE_PASS", label, expected, flush=True)
        return output

    current_output = execute("candidate", candidate, True, True)
    old_output = execute("baseline", baseline, False, True)
    if current_output.read_bytes() != old_output.read_bytes():
        raise AssertionError("Complete resampler PCM/state/fenv/callback transcript differs from original source")
    print("Bit-exact complete resampler transcript:", current_output.stat().st_size, "bytes PASS", flush=True)
    if args.negative_controls:
        phase = "return fraction == 0 ? 0 : fraction == 0.5 ? 1 : -1;"
        mutants = [
            ("disabled_reuse", replace_once(candidate, "const bool reusable_step = !passthrough && width <= kReusableWidth &&",
                                             "const bool reusable_step = false && !passthrough && width <= kReusableWidth &&"),
             "repeated-phase coefficient work is bounded by two actual phases per mix"),
            ("collapsed_half_phase", replace_once(candidate, phase,
                                                   "return (fraction == 0 || fraction == 0.5) ? 0 : -1;"),
             "uncached scalar tap oracle matches complete PCM"),
            ("admitted_fractional_phase", replace_once(candidate, phase, "return fraction == 0 ? 0 : 1;"),
             "uncached scalar tap oracle matches complete PCM"),
            ("retained_cross_mix_weights", replace_once(replace_once(candidate,
                "float weights[2][2 * kReusableWidth];", "static thread_local float weights[2][2 * kReusableWidth];"),
                "bool weights_ready[2] = {};", "static thread_local bool weights_ready[2] = {};"),
             "uncached scalar tap oracle matches complete PCM"),
            ("shifted_coefficient", replace_once(candidate, "const int64_t k = first + static_cast<int64_t>(tap);",
                                                 "const int64_t k = first + static_cast<int64_t>(tap) + 1;"),
             "uncached scalar tap oracle matches complete PCM"),
            ("zero_weight_multiplied", replace_once(candidate,
                "const float w = repeated[static_cast<size_t>(k - first)];\n          if (w == 0) continue;",
                "const float w = repeated[static_cast<size_t>(k - first)];"),
             "zero-weight tap does not introduce a nonfinite source sample"),
            ("unbounded_exact_phase", replace_once(candidate,
                "if (!(position >= 0 && position < 0x1p51)) return -1;", "if (!(position >= 0)) return -1;"),
             "fractional or out-of-range phase cannot enter coefficient reuse"),
        ]
        for label, text, expected in mutants:
            execute(label, text, True, False, expected)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--negative-controls", action="store_true")
    parser.add_argument("--compare-ref", default=BASELINE)
    parser.add_argument("--output-dir", type=Path)
    parser.add_argument("--cxx", default=os.environ.get("CXX", "c++"))
    args = parser.parse_args()
    if args.output_dir:
        args.output_dir.mkdir(parents=True, exist_ok=True)
        if any(args.output_dir.iterdir()): parser.error("output directory must be empty")
        run(args, args.output_dir)
    else:
        with tempfile.TemporaryDirectory(prefix="gesher-resampler-weights-") as path:
            run(args, Path(path))


if __name__ == "__main__":
    main()
