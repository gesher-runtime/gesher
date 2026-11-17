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


def identity_errors(identity: object, app: dict, observed: dict) -> list[str]:
    """Validate measured SDK identity separately from launch/runtime observations."""
    errors = []
    if not isinstance(identity, dict):
        return ["SDK APK identity must be an object"]
    if type(identity.get("schema_version")) is not int or identity.get("schema_version") != 1 or identity.get("source") != "android_sdk_build_tools":
        errors.append("unsupported SDK APK identity schema/source")
    if identity.get("package") != app["package"] or identity.get("version_code") != app.get("versionCode"):
        errors.append("SDK package/versionCode differs from the manifest")
    pin = app.get("sha256")
    measured = identity.get("apk_sha256")
    if not isinstance(measured, str) or not SHA256.fullmatch(measured) or not isinstance(pin, str) or measured.lower() != pin.lower() or measured != observed.get("sha256"):
        errors.append("SDK APK identity does not match pinned and observed bytes")
    if type(identity.get("version_code")) is not int or identity.get("version_code", -1) < 0:
        errors.append("SDK versionCode must be an integer")
    for key in ("min_sdk", "target_sdk"):
        value = identity.get(key)
        if value is not None and not (type(value) is int and value > 0) and not (
                isinstance(value, str) and re.fullmatch(r"[A-Za-z][A-Za-z0-9_.]*", value)):
            errors.append(f"invalid SDK identity {key}")
    parsed_target = observed.get("target_sdk")
    sdk_target = identity.get("target_sdk")
    if parsed_target is not None and type(parsed_target) is not int:
        errors.append("runtime package parser targetSdk must be an integer")
    if type(parsed_target) is int and type(sdk_target) is int and parsed_target != sdk_target:
        errors.append("SDK and runtime package parser targetSdk disagree")
    abis = identity.get("packaged_native_abis")
    if not isinstance(abis, list) or any(not isinstance(abi, str) or not re.fullmatch(
            r"[A-Za-z0-9_-]+", abi) for abi in abis):
        errors.append("packaged native ABIs must be a list of ABI names")
    elif len(abis) != len(set(abis)):
        errors.append("duplicate packaged native ABI")
    signers = identity.get("signer_certificate_sha256")
    if not isinstance(signers, list) or not signers or any(
            not isinstance(signer, str) or not SHA256.fullmatch(signer) for signer in signers):
        errors.append("SDK identity lacks verified signer certificate hashes")
    if identity.get("signature_verification") != "apksigner_default_supported_platforms":
        errors.append("SDK signature verification scope is missing")
    tools = identity.get("tools")
    if not isinstance(tools, dict) or any(not isinstance(tools.get(key), str) or not SHA256.fullmatch(tools[key])
            for key in ("aapt2_sha256", "apksigner_jar_sha256", "java_executable_sha256")):
        errors.append("SDK identity lacks collector tool hashes")
    if observed.get("identity_error"):
        errors.append("SDK identity has a collector error")
    return errors


def summarize(manifest: dict, observations: dict | None, manifest_sha256: str,
              expected_run: dict | None = None, expected_invocation: str | None = None,
              require_apk_identity: bool = False) -> dict:
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
    required_identity = runner.get("require_apk_identity", False)
    if type(required_identity) is not bool:
        errors.append("runner.require_apk_identity must be a boolean")
    if require_apk_identity and required_identity is not True:
        errors.append("this gate requires SDK APK identity, but the runner did not require it")
    required_identity = require_apk_identity or required_identity is True

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

    identities = {}
    identity_states = {}
    for package, app in packages.items():
        observed = apks.get(package, {})
        identity = observed.get("sdk_identity")
        if identity is None:
            identity_states[package] = "unavailable" if observed.get("identity_error") else "not_collected"
            if required_identity and package in selected:
                errors.append(f"required SDK APK identity is missing: {package}")
        else:
            problems = identity_errors(identity, app, observed)
            errors.extend(f"{package}: {problem}" for problem in problems)
            identity_states[package] = "invalid" if problems else "verified"
            if not problems:
                identities[package] = identity

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
            if identity_states.get(actual_package) == "invalid" or (
                    required_identity and identity_states.get(actual_package) != "verified"):
                errors.append(f"passing check lacks valid required APK identity: {actual_package}/{scenario}")
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
            "apk_identity_status": identity_states[package],
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
    runtime = source.get("runtime", {})
    if not isinstance(runtime, dict):
        errors.append("runtime must be an object")
        runtime = {}
    properties = runtime.get("declared_properties", {})
    if not isinstance(properties, dict):
        errors.append("runtime.declared_properties must be an object")
        properties = {}
    # One profile row per manifest entry, including excluded/unrun versions.
    # SDK package inspection is independent of runtime capability claims. None
    # of these dimensions, by itself, promotes smoke checks to functional proof.
    matrix = []
    for row in rows:
        package = row["package"]
        identity = identities.get(package, {})
        observed = apks.get(package, {})
        matrix.append({
            "package": package, "version_code": packages[package].get("versionCode"),
            "apk_sha256": packages[package].get("sha256"),
            "sdk_identity_status": identity_states[package],
            "min_sdk": identity.get("min_sdk"), "target_sdk": identity.get("target_sdk"),
            "runtime_parsed_target_sdk": observed.get("target_sdk"),
            "signer_certificate_sha256": identity.get("signer_certificate_sha256"),
            "packaged_native_abis": identity.get("packaged_native_abis"),
            "selected_abi": observed.get("selected_abi"),
            "declared_sdk_int": properties.get("ro.build.version.sdk"),
            "declared_sdk_int_full": properties.get("ro.build.version.sdk_full"),
            "aosp_tag": runtime.get("aosp_tag"), "modules": runtime.get("modules"),
            "providers": runtime.get("providers"), "hardware_profile": runtime.get("hardware_profile"),
            "android_reference": runtime.get("android_reference"),
            "smoke_outcome": row["smoke_outcome"], "functional_compatibility": "not_tested",
        })
    return {
        "schema_version": 1,
        "invocation_id": source.get("invocation_id"),
        "scope": "manifest app versions; visual/lifecycle smoke only, no functional or Android reference oracle",
        "manifest_sha256": manifest_sha256,
        "run_completed": completed,
        "runner": runner,
        "runtime": runtime,
        "compatibility_matrix": matrix,
        "summary": {
            "manifest_apps": len(rows), "selected_apps": len(selected),
            "apps_by_smoke_outcome": dict(sorted(counts.items())),
            "apk_identity_by_status": dict(sorted(Counter(identity_states.values()).items())),
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
              "Unknown runtime/source, selected ABI, providers and Android-reference fields "
              "remain unknown; declared build properties are not measured hardware capabilities."]
    lines += ["", "### APK identity and profile matrix", "",
              "Packaged native ABIs describe APK contents, not the ABI selected or executed by Gesher. "
              "Certificate hashes identify the verified signer set; they are not an endorsement of the signer.",
              "", "| Package / version code | SDK identity | minSdk / targetSdk | Packaged native ABIs | Verified signers |",
              "| --- | --- | --- | --- | --- |"]
    for profile in report["compatibility_matrix"]:
        abis = profile["packaged_native_abis"]
        abi_label = "unknown" if abis is None else (", ".join(abis) or "none packaged")
        certificates = profile["signer_certificate_sha256"]
        signers = "unknown" if certificates is None else str(len(certificates))
        sdk = " / ".join(str(profile[key]) if profile[key] is not None else "unknown"
                         for key in ("min_sdk", "target_sdk"))
        lines.append(f"| {cell(profile['package'])} / {profile['version_code']} | "
                     f"{profile['sdk_identity_status']} | {cell(sdk)} | {cell(abi_label)} | {signers} |")
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
    parser.add_argument("--require-apk-identity", action="store_true")
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
    }, args.expected_invocation_id, args.require_apk_identity)
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
