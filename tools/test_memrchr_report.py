#!/usr/bin/env python3
"""Verify recorded component evidence; this is not Windows/ART acceptance."""
from __future__ import annotations
import base64
import gzip
import hashlib
import io
import importlib.util
import json
import math
from pathlib import Path
import statistics
import tarfile
import test_memrchr_contracts as contracts

ROOT = Path(__file__).resolve().parents[1]
checks = 0

def check(condition: bool, message: str) -> None:
    global checks
    checks += 1
    if not condition:
        raise AssertionError(message)

def sha(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()

def campaign(raw: dict, summary: dict) -> None:
    check(raw["complete"] and len(raw["processes"]) == summary["processes"] == 720,
          "retain all 720 successful fresh processes")
    check(raw["round_start"] == 0 and raw["rounds"] == 6 and len(raw["build"]["cases"]) == 30,
          "retain the fixed six-round, thirty-case inventory")
    check(summary["calls"] == sum(r["measurement"]["calls"] + 1 for r in raw["processes"]),
          "retain every first and steady call")
    for row in raw["processes"]:
        value = row["measurement"]
        check(row["returncode"] == 0 and not row["stderr"], "no omitted failed measurement")
        check(json.loads(row["stdout"]) == value, "parsed values match process output")
        check(value["checksum"] == value["expected_checksum"], "complete output checksum")
        check(row["variant"] == ("baseline" if row["position"] in (0, 3) else "candidate"), "ABBA order")
        check(row["case"] == value["case"] and value["calls"] > 0, "case identity and nonempty loop")
    for item in summary["cases"]:
        rows = [r for r in raw["processes"] if r["case"] == item["case"]]
        check(len(rows) == 24, "all per-case processes retained")
        check({(r["block"], r["position"]) for r in rows} ==
              {(b, p) for b in range(6) for p in range(4)}, "complete paired blocks")
        for metric, recorded in item["metrics"].items():
            def value(row):
                m = row["measurement"]
                return m[metric] / (m["calls"] if metric in ("cpu_ns", "wall_ns") else 1)
            medians = {v: statistics.median(value(r) for r in rows if r["variant"] == v)
                       for v in ("baseline", "candidate")}
            expected = {**medians, "delta": medians["candidate"] - medians["baseline"],
                        "percent": (medians["candidate"] / medians["baseline"] - 1) * 100}
            for key, number in expected.items():
                check(math.isclose(recorded[key], number, rel_tol=1e-12, abs_tol=1e-9), "recomputed " + key)
            paired = []
            for block in range(6):
                m = {v: statistics.median(value(r) for r in rows if r["variant"] == v and r["block"] == block)
                     for v in ("baseline", "candidate")}
                paired.append((m["candidate"] / m["baseline"] - 1) * 100)
            check(recorded["paired_blocks_percent"] == paired, "all six paired results")
            check(recorded["paired_median_percent"] == statistics.median(paired), "paired median")

def main() -> None:
    report = json.loads((ROOT / "docs/research/memrchr-word-search-2026-11-23.json").read_text())
    current = (ROOT / contracts.SOURCE).read_bytes()
    # The recorded whole-TU receipts remain historical. Reconstruct that exact
    # source through the independently pinned directory-only transition before
    # every original comparison; also prove the measured component is unchanged.
    spec = importlib.util.spec_from_file_location("scandir_source_policy",
        ROOT / "tools/net-tcp-priority/test-selected-policy.py")
    policy = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(policy)
    source = policy.preceding_scandir_source(current)
    check(contracts.production(current.decode()) == contracts.production(source.decode()),
          "measured Memrchr component unchanged by exact directory transition")
    check(sha(source) == report["production_source_sha256"], "reviewed production source")
    archive = report["evidence_archive"]
    packed = base64.b64decode(archive["data"], validate=True)
    check(len(packed) == archive["compressed_bytes"] and sha(packed) == archive["sha256"], "archive identity")
    contents = {}
    with tarfile.open(fileobj=io.BytesIO(gzip.decompress(packed)), mode="r:") as tar:
        for member in tar.getmembers():
            check(member.isfile() and member.name not in contents, "ordinary unique archive member")
            contents[member.name] = tar.extractfile(member).read()
    check(set(contents) == {m["path"] for m in archive["members"]}, "complete archive manifest")
    for member in archive["members"]:
        data = contents[member["path"]]
        check(len(data) == member["bytes"] and sha(data) == member["sha256"], "member identity: " + member["path"])
    for path in ("tools/test_memrchr_contracts.py", "tools/measure_memrchr.py",
                 "tests/native/memrchr_measure.cc.in", "tests/native/memrchr_contract.inc", "tests/native/memrchr_host.cc.in"):
        check(contents["reviewed-fixtures/" + path] == (ROOT / path).read_bytes(), "reviewed fixture: " + path)
    for directory in ("memrchr-ordinary-isolated", "media-review-memrchr"):
        check(contents[directory + "/candidate.cc"].decode() == contracts.production(source.decode()),
              "actual source extraction including noinline and guest-ABI boundary")
    for name, key in (("memrchr-isolated-final-campaign.json", "measurement"),
                      ("memrchr-final-campaign.json", "earlier_inline_campaign")):
        campaign(json.loads(contents[name]), report[key])
    check(len(json.loads(contents["memrchr-pilot/pilot.json"])["rows"]) == 112,
          "retain the separate original GCC pilot")
    check(report["build"]["caller_object_sha256"] == report["original_inline_build"]["caller_object_sha256"],
          "identical ordinary caller object across both Clang campaigns")
    for path, digest in report["android_reference"]["files"].items():
        check(sha(contents["memrchr-llvm-reference/" + path]) == digest, "pinned Android supplier: " + path)
    expected_negatives = {"wrong_block_order", "borrow_position", "missing_tail",
                          "wrong_int_conversion", "underread", "missing_fortify"}
    for key in ("sanitizers", "ordinary_clang_release"):
        qualification = report["qualification"][key]
        check(qualification["source_sha256"] == sha(source), "qualification source identity")
        negatives = [r for r in qualification["runs"] if r["name"] not in ("baseline", "candidate")]
        check({r["name"] for r in negatives} == expected_negatives, "six specific compiled negatives")
        check(all(r["compiled"] and r["returncode"] != 0 for r in negatives), "all mutants were rejected")
        for row in qualification["runs"][:2]:
            check("1769478 shared and 85771 guarded" in row["stdout"], "complete baseline/candidate calls")
            check(row["fortify_returncode"] == -6, "actual Linux FORTIFY abort")
        check("1769478 shared and 85771 guarded" in qualification["reference"]["stdout"], "actual Android supplier calls")
    transition = report["qualification"]["source_transition"]
    check(transition["original_policy_reconstructed_byte_for_byte"], "original policy retained")
    check(transition["original_71_source_digests_retained"], "all original OFD digests retained")
    check(not report["qualification"]["independent_source_transition"]["native_runtime_approval_added"],
          "source qualification grants no runtime approval")
    builds = report["qualification"]["windows_complete_translation_units"]["builds"]
    check({(r["arch"], r["variant"]) for r in builds} ==
          {(a, v) for a in ("x86_64", "aarch64") for v in ("baseline", "candidate")}, "both complete Windows TUs")
    for build in builds:
        check(build["returncode"] == 0 and len(build["dependencies"]) == 23, "strict full-TU compile/dependencies")
        if build["variant"] == "candidate":
            check(build["source_sha256"] == sha(source), "complete Windows source identity")
    print(f"memrchr evidence: {checks} integrity/arithmetic checks, {len(contents)} archived members passed")

if __name__ == "__main__":
    main()
