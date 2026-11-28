#!/usr/bin/env python3
"""Verify the immutable 2026-11-23 search-delete experiment, without rerunning timing.

This is a historical evidence gate. Current production behavior is exercised by
test_search_delete_contracts.py; later source edits do not rewrite old timings.
"""
from datetime import datetime
import hashlib
import io
import json
import math
from pathlib import Path, PurePosixPath
import statistics
import tarfile

ROOT = Path(__file__).resolve().parents[1]
REPORT = ROOT / "docs/research/search-delete-route-2026-11-23.json"
checks = 0


def check(condition, description):
    global checks
    checks += 1
    if not condition:
        raise AssertionError(description)


def sha(data):
    return hashlib.sha256(data).hexdigest()


def equal_number(actual, expected, description):
    check(math.isfinite(actual) and math.isclose(actual, expected, rel_tol=1e-12, abs_tol=1e-9), description)


def p95(values):
    values = sorted(values)
    position = .95 * (len(values) - 1)
    left = int(position)
    return values[left] + (values[min(left + 1, len(values) - 1)] - values[left]) * (position - left)


def main():
    index = json.loads(REPORT.read_text())
    check(index["schema"] == 1 and index["scope"] == "historical-host-component-evidence", "report scope")
    archive = REPORT.parent / index["archive"]["file"]
    raw = archive.read_bytes()
    check(sha(raw) == index["archive"]["sha256"], "archive SHA-256")
    blobs = {}
    with tarfile.open(fileobj=io.BytesIO(raw), mode="r:gz") as handle:
        for item in handle.getmembers():
            path = PurePosixPath(item.name)
            check(item.isfile() and not path.is_absolute() and ".." not in path.parts, "safe regular archive member")
            check(item.name not in blobs, "unique archive member")
            blobs[item.name] = handle.extractfile(item).read()
    manifest = index["archive"]["members"]
    check(set(blobs) == set(manifest), "complete archive manifest")
    for name, expected in manifest.items():
        check(len(blobs[name]) == expected["bytes"] and sha(blobs[name]) == expected["sha256"], name)

    def document(name):
        return json.loads(blobs[name])

    prefix = "evidence/tdelete-final-measure-v1/"
    build = document(prefix + "build.json")
    measurement = document(prefix + "measurement.json")
    summary = document(prefix + "summary.json")
    check(build["complete"] and measurement["complete"], "completed fixed experiment")
    check(sha(blobs[prefix + "build.json"]) == measurement["build_sha256"], "frozen build identity")
    for name, expected in build["hashes"].items():
        check(sha(blobs[prefix + name]) == expected, "frozen build member " + name)
    for name, expected in index["source"].items():
        if name.endswith("_sha256"):
            member = {"baseline_sha256": "baseline.cc", "candidate_sha256": "candidate.cc",
                      "caller_object_sha256": "caller.o", "caller_source_sha256": "caller.cc"}[name]
            check(sha(blobs[prefix + member]) == expected, "measured source/caller " + name)
    check("-O3" in build["flags"] and all("sanitize" not in value and "flto" not in value for value in build["flags"]), "ordinary timed code")
    check(build["flags"] == index["measurement"]["flags"], "recorded exact flags")
    cases = [["drain", size, kind] for kind in ("int", "string") for size in (1, 7, 16, 256, 4096, 65536)]
    cases += [[mode, 4096, kind] for mode in ("churn", "missing", "find", "lifecycle", "duplicate", "mixed25", "mixed75") for kind in ("int", "string")]
    cases += [["empty", 4096, "int"]]
    order = ["baseline", "candidate", "candidate", "baseline"]
    check(build["cases"] == cases == measurement["cases"], "all 27 predeclared cases")
    check(measurement["order"] == order and measurement["blocks"] == 6, "six ABBA blocks")
    check(len(measurement["processes"]) == 648 and len(summary) == len(index["results"]) == 27, "all records retained")
    check(measurement["started_utc"] == index["measurement"]["started_utc"] and measurement["ended_utc"] == index["measurement"]["ended_utc"], "experiment time bounds")
    previous_end = datetime.fromisoformat(measurement["started_utc"])
    for ordinal, row in enumerate(measurement["processes"]):
        block, within = divmod(ordinal, 27 * 4)
        case_number, position = divmod(within, 4)
        check(row["block"] == block and row["case"] == cases[case_number] and row["position"] == position and row["variant"] == order[position], "fixed process order")
        check(row["exit"] == 0 and row["stderr"] == "", "successful ordinary process")
        measured = json.loads(row["stdout"])
        check(measured == row["measurement"], "actual stdout measurement")
        mode, size, kind = row["case"]
        rounds = max(4, 524288 // size)
        check([measured["mode"], measured["size"], measured["kind"]] == row["case"], "actual process case")
        check(measured["operations"] == rounds * size and measured["first_operations"] == size, "complete workload counts")
        check(measured["checksum"] == measured["expected"] == (rounds + 1) * size * size, "complete workload checksum")
        check(Path(row["command"][0]).name == row["variant"] + ".bin" and row["command"][1:] == list(map(str, row["case"])), "actual process invocation")
        start, end = (datetime.fromisoformat(row[key]) for key in ("started_utc", "ended_utc"))
        check(previous_end <= start <= end, "nonoverlapping process interval")
        previous_end = end
    check(previous_end <= datetime.fromisoformat(measurement["ended_utc"]), "final process within receipt")
    for case, old, result in zip(cases, summary, index["results"]):
        check(old["case"] == result["case"] == case, "summary case identity")
        rows = [row for row in measurement["processes"] if row["case"] == case]
        for metric in ("cpu_ns", "wall_ns", "first_cpu_ns", "first_wall_ns"):
            denominator = "first_operations" if metric.startswith("first_") else "operations"
            values = {variant: [row["measurement"][metric] / row["measurement"][denominator] for row in rows if row["variant"] == variant] for variant in ("baseline", "candidate")}
            medians = {variant: statistics.median(values[variant]) for variant in values}
            for variant in values:
                check(len(values[variant]) == 12 and values[variant] == old[metric]["raw_ns_per_operation"][variant], "all raw per-operation values")
                equal_number(old[metric][variant], medians[variant], "original median")
                equal_number(result[metric][variant], medians[variant], "published median")
                equal_number(result[metric]["p95"][variant], p95(values[variant]), "R7 sample p95")
            percent = 100 * (medians["candidate"] / medians["baseline"] - 1)
            equal_number(old[metric]["percent"], percent, "original median ratio")
            equal_number(result[metric]["percent"], percent, "published median ratio")
            equal_number(result[metric]["delta_ns"], medians["candidate"] - medians["baseline"], "absolute delta")
            equal_number(result[metric]["p95"]["percent"], 100 * (p95(values["candidate"]) / p95(values["baseline"]) - 1), "p95 ratio")
            check(result[metric]["raw_ns_per_operation"] == values, "published complete raw values")
            for block in range(6):
                pair = {variant: statistics.median(row["measurement"][metric] / row["measurement"][denominator] for row in rows if row["variant"] == variant and row["block"] == block) for variant in values}
                paired = 100 * (pair["candidate"] / pair["baseline"] - 1)
                equal_number(old[metric]["paired_block_percent"][block], paired, "original block ratio")
                equal_number(result[metric]["paired_block_percent"][block], paired, "published block ratio")
    check(len(build["counted"]) == len(index["counted"]) == 54 and build["counted"] == index["counted"], "all separate counted records")
    for case in cases:
        pair = {row["variant"]: row["measurement"] for row in build["counted"] if row["case"] == case}
        check(set(pair) == {"baseline", "candidate"}, "both counted variants")
        for variant, measured in pair.items():
            check(measured["counted_only"] and measured["operations"] == measured["first_operations"] == case[1], "counted complete batches")
            check(measured["checksum"] == measured["expected"] == 2 * case[1] * case[1], "counted checksum")
        for field in ("allocations", "allocated_bytes", "frees"):
            check(pair["baseline"][field] == pair["candidate"][field], "unchanged dynamic ownership")
        if case[0] in ("missing", "find", "duplicate", "empty"):
            check(pair["baseline"]["comparisons"] == pair["candidate"]["comparisons"], "unchanged control comparator count")

    for directory, negatives in (("tdelete-qualification-gcc-v1", 6), ("tdelete-qualification-llvm-v2", 5)):
        qualification = document("evidence/" + directory + "/qualification.json")
        check(qualification["complete"] and qualification["source_sha256"] == index["source"]["candidate_sha256"], "complete actual-source qualification")
        for name, expected in qualification["hashes"].items():
            check(sha(blobs["evidence/" + directory + "/" + name]) == expected, "qualification member " + name)
        positives = qualification["positive"]
        check(set(positives) == {"candidate", "baseline", "android-reference", "small-route"}, "all actual implementations")
        for value in positives.values():
            check(value["shared_checks"] == 3654000 and value["host_checks"] > 100000, "complete runtime contracts")
        check(positives["candidate"]["trace"] == positives["baseline"]["trace"] == positives["small-route"]["trace"] == "cb5909ac8b767a1e", "exact public traversal trace")
        check(positives["android-reference"]["trace"] == "317c81fece2db47e", "actual unbalanced supplier trace")
        check(len(qualification["negative"]) == negatives and all(value["compiled"] and value["failed_runtime_contract"] for value in qualification["negative"].values()), "compiled runtime negative controls")
        check(qualification["reference"] == index["android_reference"], "pinned supplier manifest")
    failed = document("evidence/tdelete-qualification-llvm-v1/qualification.json")
    failure = document("evidence/tdelete-qualification-llvm-v1/failure-annotation.json")
    check(not failed["complete"] and set(failed["positive"]) == {"candidate", "baseline"}
          and not failed["negative"] and "clangcc" in failure["cause"]
          and failure["source_changed"] is False, "retained first LLVM driver failure")
    for name, expected in (("native", (40, 1096)), ("windows-x86_64", (72, 1128)), ("windows-aarch64", (64, 1136))):
        suffix = "" if name == "native" else "-" + name
        for variant, size in zip(("baseline", "candidate"), expected):
            lines = blobs[prefix + variant + suffix + ".su"].decode().splitlines()
            lines = [line for line in lines if "7TdeleteE" in line]
            check(len(lines) == 1 and int(lines[0].split("\t")[1]) == size, "actual static frame " + name)
        check(index["static_frames_bytes"][name] == {"baseline": expected[0], "candidate": expected[1]}, "published static frame")
    print(json.dumps({"passed": True, "checks": checks, "members": len(blobs), "processes": 648,
                      "scope": "Historical host evidence; run current production contracts separately."}))


if __name__ == "__main__":
    main()
