"""Contract tests for coverage accounting, using isolated synthetic observations."""
from __future__ import annotations

import copy
import importlib.util
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

SPEC = importlib.util.spec_from_file_location("e2e_coverage", Path(__file__).with_name("coverage.py"))
coverage = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(coverage)
HASH = "a" * 64
TIME = "2026-01-01T00:00:00Z"


def fixture():
    manifest = {"apps": [
        {"package": "fixture.a", "versionCode": 1, "versionName": "one", "sha256": HASH},
        {"package": "fixture.b", "versionCode": 2, "versionName": "two", "sha256": HASH,
         "surfaceTrace": True},
    ]}
    checks = [
        {"package": package, "scenario": scenario, "status": "passed", "problems": [],
         "started_at": TIME, "finished_at": TIME}
        for package, scenario in [
            ("fixture.a", "launch"), ("fixture.b", "launch"), ("fixture.b", "surface-trace"),
            ("fixture.a", "association"), ("fixture.a", "shortcut")]
    ]
    observed = {
        "schema_version": 1, "invocation_id": "1" * 32,
        "manifest_sha256": HASH, "completed": True,
        "runner": {"only": []}, "runtime": {"source_revision": None},
        "apks": [{"package": app["package"], "sha256": HASH} for app in manifest["apps"]],
        "checks": checks,
    }
    return manifest, observed


class CoverageContractTest(unittest.TestCase):
    def setUp(self):
        self.manifest, self.observed = fixture()

    def report(self):
        return coverage.summarize(self.manifest, self.observed, HASH)

    def test_all_smoke_passes_never_become_functional_compatibility(self):
        result = self.report()
        self.assertTrue(result["gate_passed"])
        self.assertEqual(result["summary"]["apps_by_smoke_outcome"], {"smoke_passed": 2})
        self.assertEqual(result["summary"]["full_functional_compatibility"],
                         {"passed": 0, "denominator": 2, "not_tested": 2})
        self.assertTrue(all(app["functional_compatibility"] == "not_tested" for app in result["apps"]))
        self.assertIsNone(result["runtime"]["source_revision"])

    def test_only_filter_retains_denominator_and_explicit_skips(self):
        self.observed["runner"]["only"] = ["fixture.a"]
        self.observed["apks"] = self.observed["apks"][:1]
        for check in self.observed["checks"]:
            if check["package"] == "fixture.b":
                check.update(status="skipped", reason="excluded_by_only", started_at=None, finished_at=None)
        result = self.report()
        self.assertTrue(result["gate_passed"])
        self.assertEqual(result["summary"]["manifest_apps"], 2)
        self.assertEqual(result["summary"]["selected_apps"], 1)
        self.assertEqual(result["summary"]["apps_by_smoke_outcome"], {"skipped": 1, "smoke_passed": 1})

    def test_known_failure_is_failed_and_waived_not_passed(self):
        self.manifest["apps"][0]["knownFailure"] = "fixture expected failure"
        self.observed["checks"][0].update(status="failed", problems=["wrong result"])
        # The runner selects a different passing app for integration checks.
        for check in self.observed["checks"][-2:]:
            check["package"] = "fixture.b"
        result = self.report()
        self.assertTrue(result["gate_passed"])
        self.assertEqual(result["apps"][0]["smoke_outcome"], "failed")
        self.assertEqual(result["summary"]["ci_waived_failures_or_blocks"], 1)
        self.assertEqual(result["summary"]["full_functional_compatibility"]["passed"], 0)

    def test_diagnostic_pass_cannot_replace_default_failure(self):
        self.observed["checks"][1].update(status="failed", problems=["blank default frame"])
        result = self.report()
        self.assertFalse(result["gate_passed"])
        self.assertEqual(result["apps"][1]["smoke_outcome"], "failed")
        self.assertEqual(result["apps"][1]["scenarios"][1]["status"], "passed")

    def test_known_failure_never_waives_traced_failure(self):
        self.manifest["apps"][1]["knownFailure"] = "fixture known failure"
        self.observed["checks"][2].update(status="failed", problems=["traced crash"])
        result = self.report()
        self.assertFalse(result["gate_passed"])
        self.assertEqual(result["summary"]["ci_waived_failures_or_blocks"], 0)

    def test_prelaunch_block_stays_visible_with_dependent_diagnostic(self):
        self.manifest["apps"][1]["knownFailure"] = "fixture known failure"
        self.observed["checks"][1].update(status="blocked", problems=["download unavailable"])
        self.observed["checks"][2].update(status="blocked", reason="launch_not_reached",
                                         started_at=None, problems=[])
        result = self.report()
        self.assertTrue(result["gate_passed"])
        self.assertEqual(result["apps"][1]["smoke_outcome"], "blocked")
        self.assertEqual(result["summary"]["scenarios_by_status"]["blocked"], 2)

    def test_interrupted_run_preserves_missing_and_running_checks(self):
        self.observed["completed"] = False
        self.observed["checks"][1].update(status="running", finished_at=None)
        del self.observed["checks"][2]
        result = self.report()
        self.assertFalse(result["gate_passed"])
        self.assertEqual(result["summary"]["manifest_apps"], 2)
        self.assertEqual(result["summary"]["incomplete_selected_scenarios"], 2)

    def test_partially_executed_app_is_incomplete_not_entirely_unrun(self):
        self.observed["checks"][2].update(status="not_run", started_at=None, finished_at=None)
        result = self.report()
        self.assertEqual(result["apps"][1]["smoke_outcome"], "incomplete")
        self.assertFalse(result["gate_passed"])

    def test_shell_success_must_name_an_app_whose_default_launch_passed(self):
        self.manifest["apps"][0]["knownFailure"] = "fixture known failure"
        self.observed["checks"][0].update(status="failed", problems=["fixture failure"])
        result = self.report()
        self.assertEqual(result["integration_checks"][0]["status"], "invalid")
        self.assertFalse(result["gate_passed"])

    def test_missing_evidence_does_not_shrink_to_empty_success(self):
        result = coverage.summarize(self.manifest, None, HASH)
        self.assertFalse(result["gate_passed"])
        self.assertEqual(result["summary"]["manifest_apps"], 2)
        self.assertEqual(result["summary"]["scenarios_by_status"], {"not_run": 5})

    def test_manifest_identity_change_rejects_stale_successes(self):
        self.observed["manifest_sha256"] = "b" * 64
        result = self.report()
        self.assertFalse(result["gate_passed"])
        self.assertNotIn("smoke_passed", result["summary"]["apps_by_smoke_outcome"])
        self.assertTrue(result["validation_errors"])

    def test_different_ci_attempt_cannot_reuse_prior_successes(self):
        self.observed["runner"].update(github_run_id="123", github_run_attempt="1",
                                       test_checkout_sha="c" * 40)
        for key, value in [("github_run_id", "124"), ("github_run_attempt", "2"),
                           ("test_checkout_sha", "d" * 40)]:
            with self.subTest(key=key):
                result = coverage.summarize(self.manifest, self.observed, HASH, {key: value})
                self.assertFalse(result["gate_passed"])
                self.assertNotIn("smoke_passed", result["summary"]["apps_by_smoke_outcome"])

    def test_different_runner_invocation_cannot_reuse_a_successful_report(self):
        result = coverage.summarize(self.manifest, self.observed, HASH,
                                    expected_invocation="2" * 32)
        self.assertFalse(result["gate_passed"])
        self.assertNotIn("smoke_passed", result["summary"]["apps_by_smoke_outcome"])

    def test_nonstring_status_is_reported_as_invalid_evidence(self):
        self.observed["checks"][0]["status"] = ["passed"]
        result = self.report()
        self.assertFalse(result["gate_passed"])
        self.assertTrue(result["validation_errors"])

    def test_wrong_or_missing_apk_hash_cannot_count_as_smoke_pass(self):
        for apks in ([], [{"package": "fixture.a", "sha256": "b" * 64}]):
            with self.subTest(apks=apks):
                self.observed["apks"] = apks
                result = self.report()
                self.assertFalse(result["gate_passed"])
                self.assertNotIn("smoke_passed", result["summary"]["apps_by_smoke_outcome"])

    def test_duplicate_or_unplanned_checks_fail_closed(self):
        for extra in [copy.deepcopy(self.observed["checks"][0]),
                      {"package": "fixture.a", "scenario": "undeclared", "status": "passed"}]:
            with self.subTest(extra=extra):
                observed = copy.deepcopy(self.observed)
                observed["checks"].append(extra)
                result = coverage.summarize(self.manifest, observed, HASH)
                self.assertFalse(result["gate_passed"])
                self.assertTrue(result["validation_errors"])

    def test_selected_skip_and_pass_with_problems_are_rejected(self):
        for update in [dict(status="skipped"), dict(problems=["failed assertion"])]:
            with self.subTest(update=update):
                observed = copy.deepcopy(self.observed)
                observed["checks"][0].update(update)
                result = coverage.summarize(self.manifest, observed, HASH)
                self.assertFalse(result["gate_passed"])
                self.assertEqual(result["apps"][0]["smoke_outcome"], "invalid")

    def test_partial_integration_results_are_not_ignored(self):
        for status in ["not_run", "running", "blocked", "failed"]:
            with self.subTest(status=status):
                self.observed["checks"][-1]["status"] = status
                result = self.report()
                self.assertFalse(result["gate_passed"])
                self.assertEqual(result["integration_checks"][-1]["status"], status)

    def test_cli_writes_report_and_fails_for_missing_or_malformed_ledger(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            manifest = root / "apps.json"
            manifest.write_text(json.dumps(self.manifest), encoding="utf-8")
            ledger = root / "observations.json"
            for content in (None, "{"):
                with self.subTest(content=content):
                    if content:
                        ledger.write_text(content, encoding="utf-8")
                    result = subprocess.run([
                        sys.executable, str(Path(__file__).with_name("coverage.py")),
                        "--manifest", str(manifest), "--observations", str(ledger),
                        "--out-dir", str(root)], capture_output=True, text=True)
                    self.assertEqual(result.returncode, 1, result.stderr)
                    report = json.loads((root / "coverage.json").read_text())
                    self.assertEqual(report["summary"]["manifest_apps"], 2)
                    self.assertFalse(report["gate_passed"])
                    self.assertIn("0/2 demonstrated", (root / "coverage.md").read_text())

    def test_real_manifest_is_not_inferred_as_tested(self):
        manifest = json.loads(Path(__file__).with_name("apps.json").read_text())
        report = coverage.summarize(manifest, None, HASH)
        self.assertEqual(report["summary"]["manifest_apps"], len(manifest["apps"]))
        self.assertEqual(report["summary"]["full_functional_compatibility"]["passed"], 0)
        self.assertFalse(report["gate_passed"])


if __name__ == "__main__":
    unittest.main()
