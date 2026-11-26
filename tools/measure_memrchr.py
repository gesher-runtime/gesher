#!/usr/bin/env python3
"""Reproduce the fixed ordinary-build reverse-byte-search component campaign.

Build and measure are separate: qualification/build activity must finish before
measurement. This is a Linux host measurement, not Windows/ART/app acceptance.
All fresh processes, first calls, controls and adverse results are retained.
"""
from __future__ import annotations
import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import platform
import shlex
import subprocess
import time
import test_memrchr_contracts as contracts

ROOT = Path(__file__).resolve().parents[1]

def sha(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()

def utc() -> str:
    return datetime.now(timezone.utc).isoformat()

def build(args: argparse.Namespace) -> None:
    folder = args.build_dir.resolve()
    folder.mkdir(parents=True, exist_ok=False)
    source = (ROOT / contracts.SOURCE).read_text(encoding="utf-8")
    original = subprocess.check_output(["git", "show", f"{args.base_ref}:{contracts.SOURCE}"], cwd=ROOT, text=True)
    if contracts.function(original, "Memrchr") != contracts.BASELINE:
        raise ValueError("the actual baseline search differs from the contract baseline")
    for name in ("MemrchrChk", "FortifyFailure"):
        if contracts.function(original, name) != contracts.function(source, name):
            raise ValueError("the comparison requires unchanged " + name)
    compiler = shlex.split(args.cxx)
    flags = ["-std=c++20", "-O3", "-DNDEBUG", "-Wall", "-Wextra", "-Werror", "-fno-exceptions"]
    caller = folder / "measure.cc"
    caller.write_bytes((ROOT / "tests/native/memrchr_measure.cc.in").read_bytes())
    contracts.run(compiler + flags + ["-c", str(caller), "-o", str(folder / "measure.o")], folder, "caller-compile")
    metadata = {"created_utc": utc(), "base_ref": args.base_ref, "compiler": compiler, "flags": flags,
                "compiler_version": subprocess.check_output(compiler + ["--version"], text=True),
                "platform": platform.platform(), "production_source_sha256": sha(ROOT / contracts.SOURCE),
                "caller_source_sha256": sha(caller), "caller_object_sha256": sha(folder / "measure.o"),
                "artifacts": {}}
    for name, full_source in (("baseline", original), ("candidate", source)):
        target = folder / (name + ".cc")
        target.write_text(contracts.production(full_source), encoding="utf-8")
        obj = folder / (name + ".o")
        binary = folder / (name + ".bin")
        contracts.run(compiler + flags + ["-fstack-usage", "-Wframe-larger-than=128", "-c", str(target), "-o", str(obj)],
                      folder, name + "-compile")
        contracts.run(compiler + flags + [str(folder / "measure.o"), str(obj), "-o", str(binary)],
                      folder, name + "-link")
        metadata["artifacts"][name] = {"source_sha256": sha(target), "object_sha256": sha(obj),
                                       "binary_sha256": sha(binary)}
    names = subprocess.check_output([str(folder / "baseline.bin"), "--list"], text=True).splitlines()
    if names != subprocess.check_output([str(folder / "candidate.bin"), "--list"], text=True).splitlines() or len(names) != 30:
        raise ValueError("the predeclared 30-case inventory differs")
    metadata["cases"] = names
    (folder / "build.json").write_text(json.dumps(metadata, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"build_dir": str(folder), "cases": len(names), "caller_object_sha256": metadata["caller_object_sha256"]}))

def measure(args: argparse.Namespace) -> None:
    folder = args.build_dir.resolve()
    metadata = json.loads((folder / "build.json").read_text(encoding="utf-8"))
    for name in ("baseline", "candidate"):
        if sha(folder / (name + ".bin")) != metadata["artifacts"][name]["binary_sha256"]:
            raise ValueError("a frozen measurement binary changed: " + name)
    if sha(folder / "measure.o") != metadata["caller_object_sha256"]:
        raise ValueError("the common caller object changed")
    if args.output.exists():
        raise ValueError("refusing to overwrite an earlier measurement")
    result = {"build": metadata, "started_utc": utc(), "round_start": args.round_start,
              "rounds": args.rounds, "order": ["baseline", "candidate", "candidate", "baseline"],
              "quiet_lanes": args.quiet_lanes, "processes": [], "complete": False}
    began = time.monotonic()
    try:
        for block in range(args.round_start, args.round_start + args.rounds):
            cases = metadata["cases"]
            shift = block * 7 % len(cases)
            for case in cases[shift:] + cases[:shift]:
                for position, name in enumerate(result["order"]):
                    command = [str(folder / (name + ".bin")), case]
                    started = utc()
                    run = subprocess.run(command, capture_output=True, text=True, timeout=60)
                    row = {"block": block, "position": position, "variant": name, "case": case,
                           "command": command, "started_utc": started, "ended_utc": utc(),
                           "returncode": run.returncode, "stdout": run.stdout, "stderr": run.stderr}
                    result["processes"].append(row)
                    if run.returncode:
                        raise RuntimeError("measurement process failed: " + json.dumps(row))
                    row["measurement"] = json.loads(run.stdout)
                    values = row["measurement"]
                    if values["case"] != case or values["checksum"] != values["expected_checksum"]:
                        raise RuntimeError("measurement identity/checksum mismatch")
        result["complete"] = True
    finally:
        result["ended_utc"] = utc()
        result["elapsed_seconds"] = time.monotonic() - began
        args.output.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"output": str(args.output), "processes": len(result["processes"]),
                      "complete": result["complete"], "elapsed_seconds": result["elapsed_seconds"]}))

def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=("build", "measure"))
    parser.add_argument("--build-dir", required=True, type=Path)
    parser.add_argument("--cxx", default=os.environ.get("CXX", "c++"))
    parser.add_argument("--base-ref", default=contracts.BASELINE_REF)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--round-start", type=int, default=0)
    parser.add_argument("--rounds", type=int, default=6)
    parser.add_argument("--quiet-lanes", default="not independently established")
    args = parser.parse_args()
    if args.action == "build":
        build(args)
    else:
        if args.output is None or args.round_start < 0 or args.rounds < 1:
            parser.error("measure needs a new --output and positive fixed round count")
        measure(args)

if __name__ == "__main__":
    main()
