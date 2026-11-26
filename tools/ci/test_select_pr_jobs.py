"""Selection and required-check tests, including synthesized PR merge commits."""

import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch

import select_pr_jobs as planner


class PlanTests(unittest.TestCase):
    def setUp(self):
        self.ci = planner.load_workflow("ci")
        self.arm = planner.load_workflow("arm64-windows")

    def selection(self, paths, workflow="ci"):
        graph = self.ci if workflow == "ci" else self.arm
        return planner.select_jobs(graph, workflow, set(paths))[0]

    def test_docs_only_disables_work_in_both_workflows(self):
        for workflow in planner.GATES:
            with self.subTest(workflow=workflow):
                self.assertEqual(self.selection(["docs/lifecycle.md"], workflow), set())

    def test_component_paths_select_union_and_dependencies(self):
        self.assertEqual(
            self.selection([
                "docs/lifecycle.md",
                "tests/metered-network/cost_policy_test.cc",
                "tests/webview-darkening/WebViewDarkeningTest.java",
            ]),
            {"network-cost-client", "windows-host-contracts", "webview", "framework"},
        )
        self.assertEqual(
            self.selection(["tests/e2e/test_coverage.py"]),
            {"e2e", "package"} | (self.ci.closure({"package"}) & self.ci.work),
        )
        self.assertTrue({"build", "build-framework-assets"} <=
                        self.selection(["tests/e2e/test_coverage.py"]))
        self.assertNotIn("test-shards", self.selection(["tests/e2e/test_coverage.py"]))
        self.assertEqual(
            self.selection(["tests/metered-network/cost_policy_test.cc"], "arm64-windows"),
            set(),
        )

    def test_webview_production_policy_runs_runtime_and_package_checks(self):
        paths = ["app/java/com/gesher/app/webview/WebViewDarkening.java"]
        selected = self.selection(paths)
        self.assertTrue({"webview", "framework", "artifact-preflight", "build-framework-assets",
                         "package", "e2e",
                         "test", "test-shards", "web3-query-regression"} <= selected)
        self.assertNotIn("network-cost-client", selected)
        self.assertEqual(self.selection(paths, "arm64-windows"), set())

    def test_unmapped_paths_force_full_including_mixed_changes(self):
        for path in [
            "docs/research/runtime-inventory.json", "docs/decisions/new.md",
            ".github/workflows/ci.yml", "CMakeLists.txt", "compat/net/windows_networks.cc",
            "tools/ci/select_pr_jobs.py", "README.md",
        ]:
            with self.subTest(path=path):
                files = ["docs/lifecycle.md", "tests/e2e/test_coverage.py", path]
                self.assertEqual(self.selection(files), self.ci.work)
                self.assertEqual(self.selection(files, "arm64-windows"), self.arm.work)

    def test_unsupported_events_or_missing_diff_use_full_plan(self):
        for event in ["push", "workflow_dispatch", "pull_request"]:
            with self.subTest(event=event), patch.dict(os.environ, {"GITHUB_EVENT_NAME": event}, clear=True):
                self.assertEqual(planner.plan(self.ci, "ci")[0], self.ci.work)

    def test_gate_requires_explicit_matching_outcomes(self):
        selected = list(self.selection(["tests/e2e/test_coverage.py"]))
        results = {
            job: {"result": "success" if job in set(selected) | self.ci.always else "skipped"}
            for job in self.ci.needs
        }
        planner.verify(self.ci, selected, results)
        for job, outcome in [
            ("plan", "failure"), ("source-identity", "cancelled"), ("e2e", "skipped"),
            ("windows-host-contracts", "success"), ("automerge-script", "skipped"),
        ]:
            with self.subTest(job=job, outcome=outcome):
                with self.assertRaisesRegex(ValueError, job):
                    planner.verify(self.ci, selected, results | {job: {"result": outcome}})
        with self.assertRaisesRegex(ValueError, "omit a dependency"):
            planner.verify(self.ci, ["e2e"], results)
        with self.assertRaisesRegex(ValueError, "exactly"):
            planner.verify(self.ci, selected, {job: details for job, details in results.items() if job != "plan"})
        for invalid in [None, {}, ["plan"], ["e2e", "e2e"], ["unknown"], [{}]]:
            with self.subTest(invalid=invalid), self.assertRaisesRegex(ValueError, "invalid selected"):
                planner.verify(self.ci, invalid, results)


class PullRequestDiffTests(unittest.TestCase):
    def setUp(self):
        self.workflow = planner.load_workflow("ci")
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        root_patch = patch.object(planner, "ROOT", self.root)
        root_patch.start()
        self.addCleanup(root_patch.stop)
        planner.git("init", "-q", "-b", "main")
        planner.git("config", "user.name", "CI planner test")
        planner.git("config", "user.email", "ci-planner@example.invalid")
        (self.root / "docs").mkdir()
        (self.root / "docs" / "logging.md").write_text("existing note", encoding="utf-8")
        (self.root / "shared.cc").write_text("original", encoding="utf-8")
        self.commit("common ancestor")
        planner.git("branch", "topic")

    def commit(self, subject):
        planner.git("add", "--all")
        planner.git("commit", "-q", "-m", subject)
        return self.sha()

    def sha(self):
        return planner.git("rev-parse", "HEAD").decode("ascii").strip()

    def merge(self):
        head = self.sha()
        planner.git("switch", "main")
        base = self.sha()
        planner.git("merge", "--no-ff", "-qm", "merge topic", head)
        event = {"pull_request": {"base": {"sha": base}, "head": {"sha": head}}}
        return event, self.sha()

    def test_merge_checkout_excludes_base_only_edits(self):
        (self.root / "shared.cc").write_text("base-only change", encoding="utf-8")
        self.commit("base advanced")
        planner.git("switch", "topic")
        (self.root / "docs" / "logging.md").write_text("PR docs change", encoding="utf-8")
        self.commit("edit documentation")
        event, merged = self.merge()
        self.assertEqual(planner.changed_paths(event, merged), {"docs/logging.md"})
        with self.assertRaisesRegex(ValueError, "merge commit"):
            planner.changed_paths(event, event["pull_request"]["head"]["sha"])

    def test_rename_covers_both_paths_and_deletion(self):
        planner.git("switch", "topic")
        planner.git("mv", "shared.cc", "docs/lifecycle.md")
        planner.git("rm", "docs/logging.md")
        self.commit("rename and remove files")
        event, merged = self.merge()
        self.assertEqual(
            planner.changed_paths(event, merged),
            {"shared.cc", "docs/lifecycle.md", "docs/logging.md"},
        )

    def test_merge_result_paths_and_unsafe_file_mode(self):
        planner.git("switch", "topic")
        (self.root / "docs" / "logging.md").write_text("PR docs change", encoding="utf-8")
        self.commit("edit documentation")
        event, _ = self.merge()
        (self.root / "shared.cc").write_text("merge-only change", encoding="utf-8")
        planner.git("add", "shared.cc")
        planner.git("commit", "-q", "--amend", "--no-edit")
        self.assertEqual(planner.changed_paths(event, self.sha()), {"docs/logging.md", "shared.cc"})
        planner.git("update-index", "--chmod=+x", "docs/logging.md")
        planner.git("commit", "-q", "--amend", "--no-edit")
        path = self.root / "event.json"
        path.write_text(json.dumps(event), encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "file type or mode"):
            planner.changed_paths(event, self.sha())
        env = {"GITHUB_EVENT_NAME": "pull_request", "GITHUB_EVENT_PATH": str(path), "GITHUB_SHA": self.sha()}
        with patch.dict(os.environ, env, clear=True):
            self.assertEqual(planner.plan(self.workflow, "ci")[0], self.workflow.work)


if __name__ == "__main__":
    unittest.main()
