#!/usr/bin/env python3
"""Freeze complete search TUs/common callers, then run a fixed six-block comparison."""
import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import platform
import shutil
import statistics
import subprocess

ROOT = Path(__file__).resolve().parents[1]
CASES = [("drain", size, kind) for kind in ("int", "string")
         for size in (1, 7, 16, 256, 4096, 65536)]
CASES += [(mode, 4096, kind) for mode in
          ("churn", "missing", "find", "lifecycle", "duplicate", "mixed25", "mixed75")
          for kind in ("int", "string")]
CASES += [("empty", 4096, "int")]
assert len(CASES) == 27
ORDER = ("baseline", "candidate", "candidate", "baseline")


def utc():
    return datetime.now(timezone.utc).isoformat()


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    action = parser.add_mutually_exclusive_group(required=True)
    action.add_argument("--prepare", action="store_true")
    action.add_argument("--run", action="store_true")
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--baseline", type=Path)
    parser.add_argument("--compiler", default="clang++")
    parser.add_argument("--cross-bin", type=Path)
    parser.add_argument("--quiet-confirmed", help="UTC of the last explicit all-lane idle confirmation")
    args = parser.parse_args()
    out = args.output_dir.resolve()
    if args.prepare:
        if args.baseline is None:
            parser.error("--prepare requires a complete --baseline source file")
        out.mkdir(parents=True, exist_ok=False)
        compiler = shutil.which(args.compiler)
        if not compiler:
            raise RuntimeError("Compiler is unavailable")
        include = out / "include"
        include.mkdir()
        for name in ("search_bionic.h", "abi_types.h"):
            shutil.copyfile(ROOT / "compat/bionic" / name, include / name)
        shutil.copyfile(args.baseline, out / "baseline.cc")
        shutil.copyfile(ROOT / "compat/bionic/search_bionic.cc", out / "candidate.cc")
        shutil.copyfile(ROOT / "tests/native/search_delete_measure.cc", out / "caller.cc")
        shutil.copyfile(Path(__file__), out / "driver-used.py")
        data = {"started_utc": utc(), "complete": False, "cases": CASES,
                "flags": ["-std=gnu++20", "-O3", "-DNDEBUG", "-Wall", "-Wextra", "-Werror",
                          "-fno-exceptions", "-fsized-deallocation", "-mno-ms-bitfields"],
                "commands": [], "counted": [], "smokes": [], "host": platform.platform(),
                "cpu_count": os.cpu_count()}

        def run(label, command):
            result = subprocess.run([str(x) for x in command], capture_output=True, text=True, timeout=120)
            row = {"label": label, "command": [str(x) for x in command], "exit": result.returncode,
                   "stdout": result.stdout, "stderr": result.stderr}
            data["commands"].append(row)
            (out / (label + ".stdout")).write_text(result.stdout)
            (out / (label + ".stderr")).write_text(result.stderr)
            if result.returncode != 0 or result.stderr:
                raise RuntimeError(label + " failed: " + result.stderr[-4000:])
            return result.stdout

        try:
            data["compiler"] = run("compiler-version", [compiler, "--version"])
            flags = [*data["flags"], "-I", str(include)]
            run("caller-build", [compiler, *flags, "-c", out / "caller.cc", "-o", out / "caller.o"])
            run("counted-caller-build", [compiler, *flags, "-DTREE_DELETE_COUNTED", "-c",
                                         out / "caller.cc", "-o", out / "counted-caller.o"])
            for variant in ("baseline", "candidate"):
                obj = out / (variant + ".o")
                run(variant + "-build", [compiler, *flags, "-fstack-usage", "-MMD", "-c",
                                         out / (variant + ".cc"), "-o", obj])
                run(variant + "-link", [compiler, out / "caller.o", obj, "-o", out / (variant + ".bin")])
                run(variant + "-counted-link", [compiler, out / "counted-caller.o", obj,
                    "-Wl,--wrap=malloc", "-Wl,--wrap=free", "-o", out / (variant + "-counted.bin")])
                for index, case in enumerate(CASES):
                    text = run(f"{variant}-counted-{index:02}", [out / (variant + "-counted.bin"), *case])
                    measured = json.loads(text)
                    assert measured["counted_only"] and measured["checksum"] == measured["expected"]
                    data["counted"].append({"variant": variant, "case": case, "measurement": measured})
                smoke = json.loads(run(variant + "-ordinary-smoke",
                    [out / (variant + ".bin"), "drain", 4096, "string"]))
                assert smoke["checksum"] == smoke["expected"]
                data["smokes"].append({"variant": variant, "measurement": smoke})
                if args.cross_bin:
                    for arch in ("x86_64", "aarch64"):
                        cross = args.cross_bin / (arch + "-w64-mingw32-clang++")
                        run(variant + "-windows-" + arch, [cross, *flags, "-fstack-usage", "-MMD", "-c",
                            out / (variant + ".cc"), "-o", out / (variant + "-windows-" + arch + ".o")])
            data["complete"] = True
        except Exception as error:
            data["error"] = repr(error)
            raise
        finally:
            data["ended_utc"] = utc()
            data["hashes"] = {str(path.relative_to(out)): sha(path) for path in sorted(out.rglob("*"))
                              if path.is_file()}
            (out / "build.json").write_text(json.dumps(data, indent=2) + "\n")
        print(json.dumps({"prepared": data["complete"], "cases": len(CASES), "counted_processes": len(data["counted"]),
                          "ordinary_smokes": len(data["smokes"]), "source": data["hashes"]["candidate.cc"]}))
        return

    if not args.quiet_confirmed:
        parser.error("--run requires an explicit --quiet-confirmed timestamp")
    build = json.loads((out / "build.json").read_text())
    assert build["complete"]
    for name, expected in build["hashes"].items():
        assert sha(out / name) == expected, name
    assert sha(ROOT / "compat/bionic/search_bionic.cc") == build["hashes"]["candidate.cc"]
    target = out / "measurement.json"
    if target.exists():
        raise RuntimeError("The fixed comparison already has a receipt; do not overwrite/retry it")
    data = {"started_utc": utc(), "complete": False, "blocks": 6, "cases": CASES, "order": ORDER,
            "processes": [], "quiet_confirmed_utc": args.quiet_confirmed,
            "quiet_lanes": ["root", "java_efficiency", "media_efficiency", "native_efficiency"],
            "build_sha256": sha(out / "build.json"), "scope": "Host complete public operations; first means first whole workload batch after tree setup."}
    try:
        for block in range(6):
            for case in CASES:
                for position, variant in enumerate(ORDER):
                    command = [str(out / (variant + ".bin")), *map(str, case)]
                    started = utc()
                    result = subprocess.run(command, capture_output=True, text=True, timeout=120)
                    row = {"block": block, "case": case, "position": position, "variant": variant,
                           "command": command, "started_utc": started, "ended_utc": utc(),
                           "exit": result.returncode, "stdout": result.stdout, "stderr": result.stderr}
                    data["processes"].append(row)
                    assert result.returncode == 0 and not result.stderr, row
                    row["measurement"] = json.loads(result.stdout)
                    assert row["measurement"]["checksum"] == row["measurement"]["expected"]
            print(json.dumps({"completed_block": block + 1, "processes": len(data["processes"]), "utc": utc()}), flush=True)
        data["complete"] = True
    except Exception as error:
        data["error"] = repr(error)
        raise
    finally:
        data["ended_utc"] = utc()
        target.write_text(json.dumps(data, indent=2) + "\n")
    summary = []
    for case in CASES:
        rows = [r for r in data["processes"] if tuple(r["case"]) == case]
        metrics = {}
        for metric in ("cpu_ns", "wall_ns", "first_cpu_ns", "first_wall_ns"):
            denominator = "first_operations" if metric.startswith("first_") else "operations"
            values = {v: [r["measurement"][metric] / r["measurement"][denominator]
                          for r in rows if r["variant"] == v] for v in ("baseline", "candidate")}
            medians = {v: statistics.median(values[v]) for v in values}
            paired = []
            for block in range(6):
                pair = {v: statistics.median(r["measurement"][metric] / r["measurement"][denominator]
                    for r in rows if r["variant"] == v and r["block"] == block) for v in values}
                paired.append(100 * (pair["candidate"] / pair["baseline"] - 1))
            metrics[metric] = {**medians, "percent": 100 * (medians["candidate"] / medians["baseline"] - 1),
                               "paired_block_percent": paired, "raw_ns_per_operation": values}
        summary.append({"case": case, **metrics})
    (out / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")
    print(json.dumps({"complete": True, "processes": len(data["processes"]), "started": data["started_utc"],
                      "ended": data["ended_utc"], "summary_file": str(out / "summary.json")}))


if __name__ == "__main__":
    main()
