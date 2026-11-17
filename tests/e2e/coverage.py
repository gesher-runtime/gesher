#!/usr/bin/env python3
"""Validate the real-app e2e ledger without treating smoke checks as compatibility.

The denominator comes from apps.json, including -Only exclusions and missing
observations. This schema deliberately has no route from a smoke PASS to full
functional compatibility; a future functional-oracle protocol needs its own
reviewed schema.
"""
from __future__ import annotations

import argparse
from collections import Counter
import hashlib
import json
import os
from pathlib import Path
import re
import sys

STATUSES = {"not_run", "running", "passed", "failed", "blocked", "skipped"}
INTEGRATION = ("association", "shortcut")
SHA256 = re.compile(r"[0-9a-fA-F]{64}\Z")


def hash_file(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def summarize(manifest: dict, observations: dict | None, manifest_sha256: str,
              expected_run: dict | None = None, expected_invocation: str | None = None) -> dict:
    errors: list[str] = []
    if not isinstance(manifest, dict):
        raise ValueError("manifest must be an object")
    apps = manifest.get("apps")
    if not isinstance(apps, list) or not apps:
        raise ValueError("manifest apps must be a non-empty list")
    packages: dict[str, dict] = {}
    for app in apps:
        if not isinstance(app, dict) or not isinstance(app.get("package"), str) or not app["package"]:
            raise ValueError("each manifest app needs a package")
        if app["package"] in packages:
            raise ValueError(f"duplicate manifest package: {app['package']}")
        packages[app["package"]] = app

    source = observations or {}
    valid_source = isinstance(source, dict)
    if not valid_source:
        source = {}
    if not observations:
        errors.append("observations are missing")
    elif not valid_source or source.get("schema_version") != 1:
        errors.append("unsupported observation schema")
        source = {}
    elif source.get("manifest_sha256") != manifest_sha256:
        errors.append("observation manifest hash does not match the supplied manifest")
        source = {}
    invocation = source.get("invocation_id")
    if not isinstance(invocation, str) or not re.fullmatch(r"[0-9a-f]{32}", invocation):
        errors.append("observations lack a valid invocation ID")
        source = {}
    elif expected_invocation and invocation != expected_invocation:
        errors.append("observations belong to a different runner invocation")
        source = {}

    runner = source.get("runner", {})
    if not isinstance(runner, dict):
        errors.append("runner must be an object")
        runner = {}
    if any(runner.get(key) != value for key, value in (expected_run or {}).items() if value):
        errors.append("observations belong to a different CI run, attempt or test checkout")
        source, runner = {}, {}
    only = runner.get("only", [])
    if not isinstance(only, list) or any(not isinstance(item, str) for item in only):
        errors.append("runner.only must be a list of packages")
        only = []
    if len(set(only)) != len(only) or set(only) - packages.keys():
        errors.append("runner.only contains duplicate or unknown packages")
    selected = {package for package in packages if not only or package in only}

    expected = {(package, scenario) for package, app in packages.items()
                for scenario in (["launch", "surface-trace"] if app.get("surfaceTrace") else ["launch"])}
    expected.update((None, scenario) for scenario in INTEGRATION)
    records: dict[tuple, dict] = {}
    duplicates = set()
    raw_checks = source.get("checks", [])
    if not isinstance(raw_checks, list):
        errors.append("checks must be a list")
        raw_checks = []
    for record in raw_checks:
        if not isinstance(record, dict):
            errors.append("each check must be an object")
            continue
        scenario, package = record.get("scenario"), record.get("package")
        if not isinstance(scenario, str) or (package is not None and not isinstance(package, str)):
            errors.append("check package and scenario must be strings")
            continue
        key = (None if scenario in INTEGRATION else package, scenario)
        if key not in expected:
            errors.append(f"unplanned check: {package}/{scenario}")
        elif key in records or key in duplicates:
            errors.append(f"duplicate check: {package}/{scenario}")
            duplicates.add(key)
            records.pop(key, None)
        elif not isinstance(record.get("status"), str) or record["status"] not in STATUSES:
            errors.append(f"invalid status: {package}/{scenario}")
        else:
            records[key] = record

    apks: dict[str, dict] = {}
    raw_apks = source.get("apks", [])
    if not isinstance(raw_apks, list):
        errors.append("apks must be a list")
        raw_apks = []
    for apk in raw_apks:
        if not isinstance(apk, dict) or not isinstance(apk.get("package"), str):
            errors.append("each observed APK needs a package")
            continue
        package = apk["package"]
        if package not in selected or package in apks:
            errors.append(f"unselected or duplicate APK: {package}")
            continue
        apks[package] = apk

    def check(package: str | None, scenario: str) -> dict:
        record = records.get((package, scenario), {})
        status = record.get("status", "not_run")
        actual_package = record.get("package", package)
        problems = record.get("problems", [])
        invalid = False
        if not isinstance(problems, list) or any(not isinstance(item, str) for item in problems):
            errors.append(f"problems must be strings: {package}/{scenario}")
            problems, invalid = [], True
        if status in {"running", "passed", "failed"} and not record.get("started_at"):
            errors.append(f"missing start time: {package}/{scenario}")
            invalid = True
        if status in {"passed", "failed", "blocked"} and not record.get("finished_at"):
            errors.append(f"missing finish time: {package}/{scenario}")
            invalid = True
        if status == "passed" and problems:
            errors.append(f"passing check has problems: {package}/{scenario}")
            invalid = True
        if scenario in INTEGRATION:
            if status in {"running", "passed", "failed"} and actual_package not in selected:
                errors.append(f"integration check has no selected package: {scenario}")
                invalid = True
            if status == "skipped":
                errors.append(f"integration check cannot be skipped: {scenario}")
                invalid = True
            if status == "passed" and records.get((actual_package, "launch"), {}).get("status") != "passed":
                errors.append(f"integration check lacks a passing default launch: {scenario}")
                invalid = True
        elif package not in selected:
            if status != "skipped" or record.get("reason") != "excluded_by_only":
                errors.append(f"excluded package lacks its explicit skip: {package}/{scenario}")
                invalid = True
        elif status == "skipped":
            errors.append(f"selected check cannot be skipped: {package}/{scenario}")
            invalid = True
        if status == "passed":
            observed = apks.get(actual_package, {}).get("sha256")
            pin = packages.get(actual_package, {}).get("sha256")
            if not isinstance(observed, str) or not SHA256.fullmatch(observed) or not isinstance(pin, str) or observed.lower() != pin.lower():
                errors.append(f"passing check lacks a matching pinned APK hash: {actual_package}/{scenario}")
                invalid = True
        known = packages.get(actual_package, {}).get("knownFailure") or None
        # Match the runner's existing waiver: only the default app check. A
        # diagnostic failure remains unexpected, even for a known-failing app.
        waived = bool(known and scenario == "launch" and status in {"failed", "blocked"})
        if scenario == "surface-trace" and status == "blocked" and record.get("reason") == "launch_not_reached":
            launch = records.get((package, "launch"), {})
            waived = bool(known and launch.get("status") in {"failed", "blocked"})
        return {
            "package": actual_package, "scenario": scenario,
            "evidence_kind": "visual_lifecycle_smoke",
            "status": "invalid" if invalid else status,
            "observed_status": status, "ci_waived": waived and not invalid,
            "known_failure_reason": known, "reason": record.get("reason"),
            "problems": problems, "started_at": record.get("started_at"),
            "finished_at": record.get("finished_at"),
        }

    rows = []
    for package, app in packages.items():
        scenarios = [check(package, "launch")]
        if app.get("surfaceTrace"):
            scenarios.append(check(package, "surface-trace"))
        states = {item["status"] for item in scenarios}
        if states == {"passed"}:
            outcome = "smoke_passed"
        elif states == {"skipped"}:
            outcome = "skipped"
        else:
            outcome = next((state for state in ("invalid", "failed", "blocked", "running", "not_run")
                            if state in states), "not_run")
            if outcome == "running" or (outcome == "not_run" and "passed" in states):
                outcome = "incomplete"
        rows.append({
            "package": package, "selected": package in selected,
            "expected_apk": {key: app.get(key) for key in ("versionName", "versionCode", "sha256")},
            "observed_apk": apks.get(package),
            "smoke_outcome": outcome,
            "functional_compatibility": "not_tested",
            "scenarios": scenarios,
        })
    integration = [check(None, scenario) for scenario in INTEGRATION]
    checks = [item for row in rows for item in row["scenarios"]] + integration
    incomplete = sum(item["status"] in {"running", "not_run", "invalid"} for item in checks
                     if item["package"] in selected or item["scenario"] in INTEGRATION)
    unexpected = sum(item["status"] in {"failed", "blocked"} and not item["ci_waived"] for item in checks)
    completed = source.get("completed") is True
    counts = Counter(row["smoke_outcome"] for row in rows)
    return {
        "schema_version": 1,
        "invocation_id": source.get("invocation_id"),
        "scope": "manifest app versions; visual/lifecycle smoke only, no functional or Android reference oracle",
        "manifest_sha256": manifest_sha256,
        "run_completed": completed,
        "runner": runner,
        "runtime": source.get("runtime", {}),
        "summary": {
            "manifest_apps": len(rows), "selected_apps": len(selected),
            "apps_by_smoke_outcome": dict(sorted(counts.items())),
            "full_functional_compatibility": {"passed": 0, "denominator": len(rows), "not_tested": len(rows)},
            "planned_scenarios": len(checks),
            "scenarios_by_status": dict(sorted(Counter(item["status"] for item in checks).items())),
            "ci_waived_failures_or_blocks": sum(item["ci_waived"] for item in checks),
            "unexpected_failures_or_blocks": unexpected,
            "incomplete_selected_scenarios": incomplete,
        },
        "apps": rows, "integration_checks": integration, "validation_errors": errors,
        "gate_passed": completed and not errors and not incomplete and not unexpected,
    }


def render(report: dict) -> str:
    summary = report["summary"]
    lines = [
        "## Real-app e2e coverage", "",
        f"Manifest denominator: **{summary['manifest_apps']} app versions**; "
        f"selected: **{summary['selected_apps']}**.",
        f"Full functional compatibility: **0/{summary['manifest_apps']} demonstrated** "
        "(all not tested by this visual/lifecycle smoke runner).",
        f"Run completed: **{report['run_completed']}**. Evidence gate passed: **{report['gate_passed']}**.",
        "", "| Package | Smoke outcome | Scenarios |", "| --- | --- | --- |",
    ]
    def cell(value: str) -> str:
        return value.replace("|", "\\|").replace("\n", " ").replace("\r", " ")
    for app in report["apps"]:
        states = ", ".join(item["scenario"] + ": " + item["status"] +
                           (" (CI waiver)" if item["ci_waived"] else "") for item in app["scenarios"])
        lines.append(f"| {cell(app['package'])} | {app['smoke_outcome']} | {cell(states)} |")
    lines += ["", f"CI-waived failures/blocks: {summary['ci_waived_failures_or_blocks']}; "
              f"unexpected failures/blocks: {summary['unexpected_failures_or_blocks']}; "
              f"incomplete selected scenarios: {summary['incomplete_selected_scenarios']}.",
              "Unknown runtime/source, signer, selected ABI, providers and Android-reference fields "
              "remain unknown; declared build properties are not measured hardware capabilities."]
    if report["validation_errors"]:
        lines += ["", "Evidence validation errors:"] + [
            "- " + cell(error) for error in report["validation_errors"]]
    return "\n".join(lines) + "\n"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--manifest", type=Path, default=Path(__file__).with_name("apps.json"))
    parser.add_argument("--observations", type=Path, required=True)
    parser.add_argument("--out-dir", type=Path, required=True)
    parser.add_argument("--expected-invocation-id", default=os.environ.get("GESHER_E2E_INVOCATION_ID"))
    args = parser.parse_args()
    manifest = json.loads(args.manifest.read_text(encoding="utf-8-sig"))
    observation_error = None
    try:
        observations = json.loads(args.observations.read_text(encoding="utf-8-sig"))
    except (OSError, ValueError) as error:
        observations = None
        observation_error = f"cannot read observation ledger: {type(error).__name__}"
    report = summarize(manifest, observations, hash_file(args.manifest), {
        "github_run_id": os.environ.get("GITHUB_RUN_ID"),
        "github_run_attempt": os.environ.get("GITHUB_RUN_ATTEMPT"),
        "test_checkout_sha": os.environ.get("GITHUB_SHA"),
    }, args.expected_invocation_id)
    if observation_error:
        report["validation_errors"].append(observation_error)
    args.out_dir.mkdir(parents=True, exist_ok=True)
    (args.out_dir / "coverage.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    markdown = render(report)
    (args.out_dir / "coverage.md").write_text(markdown, encoding="utf-8")
    print(markdown)
    return 0 if report["gate_passed"] else 1


if __name__ == "__main__":
    sys.exit(main())
