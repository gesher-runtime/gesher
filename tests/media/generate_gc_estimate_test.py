#!/usr/bin/env python3
"""Generate a CI regression from ART's actual patched CPU estimate block.

Only the input collector means and branch hints are supplied by the envelope.
The arithmetic is copied verbatim from the source compiled into this runtime.
Pristine/patched branch probes replace the assignment with a return, so the
negative control observes the old bad branch without executing undefined math.
"""
import argparse
import hashlib
from pathlib import Path
import subprocess
import textwrap


def estimate_block(source: str) -> str:
    begin = "        double next_gc_cpu_time = next_collector->GetMeanCpuTime();"
    end = "\n      }\n      size_t target_footprint ="
    if source.count(begin) != 1:
        raise ValueError("ART CPU estimate start is missing or ambiguous")
    start = source.index(begin)
    finish = source.index(end, start)
    block = textwrap.dedent(source[start:finish]).rstrip() + "\n"
    assignment = "remaining_bytes = remaining_bytes * (next_gc_cpu_time / cur_gc_cpu_time);"
    if block.count(assignment) != 1:
        raise ValueError("ART CPU estimate assignment changed; review the regression envelope")
    return block


def branch_probe(block: str) -> str:
    return block.replace(
        "remaining_bytes = remaining_bytes * (next_gc_cpu_time / cur_gc_cpu_time);",
        "return true;")


def generate(patched: str, pristine: str, template: str) -> str:
    current = estimate_block(patched)
    original = estimate_block(pristine)
    substitutions = {
        "@PATCHED_ESTIMATE@": textwrap.indent(current.rstrip(), "  "),
        "@PATCHED_GUARD@": textwrap.indent(branch_probe(current).rstrip(), "  "),
        "@ORIGINAL_GUARD@": textwrap.indent(branch_probe(original).rstrip(), "  "),
        "@PATCHED_SHA@": hashlib.sha256(current.encode()).hexdigest(),
        "@ORIGINAL_SHA@": hashlib.sha256(original.encode()).hexdigest(),
    }
    for marker, value in substitutions.items():
        if template.count(marker) != 1:
            raise ValueError("missing/ambiguous template marker: " + marker)
        template = template.replace(marker, value)
    return template


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--art-root", required=True, type=Path)
    parser.add_argument("--template", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    patched = (args.art_root / "runtime/gc/heap.cc").read_text(encoding="utf-8")
    # fetch-aosp applies patches to the working tree; HEAD remains the pinned tag.
    pristine = subprocess.check_output(
        ["git", "-C", str(args.art_root), "show", "HEAD:runtime/gc/heap.cc"]
    ).decode("utf-8")
    generated = generate(patched, pristine, args.template.read_text(encoding="utf-8"))
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(generated, encoding="utf-8", newline="\n")
    print("Generated actual ART CPU estimate regression: " + str(args.output))
    print("Patched GC CPU estimate block SHA256 " + hashlib.sha256(estimate_block(patched).encode()).hexdigest())
    print("Pristine GC CPU estimate block SHA256 " + hashlib.sha256(estimate_block(pristine).encode()).hexdigest())


if __name__ == "__main__":
    main()
