"""Ensure selective preflight retains every check and scopes only owned entry points."""
import ast
from pathlib import Path
import re
import unittest

import yaml

import preflight_plan as plan

ROOT = Path(__file__).resolve().parents[2]


class PreflightPlanTests(unittest.TestCase):
    def setUp(self):
        self.workflow = yaml.safe_load(
            (ROOT / ".github/workflows/ci.yml").read_text(encoding="utf-8"))
        self.steps = self.workflow["jobs"]["artifact-preflight"]["steps"]

    def test_every_original_check_has_one_group(self):
        plan.validate_steps(self.steps)
        groups = {group: [] for group in plan.GROUPS}
        for step in self.steps:
            if step.get("name") in plan.STEP_GROUPS:
                groups[plan.STEP_GROUPS[step["name"]]].append(step["run"])
        self.assertTrue(all(groups.values()))
        self.assertEqual(sum(map(len, groups.values())), len(plan.STEP_GROUPS))

    def test_omitted_duplicate_and_new_checks_are_rejected(self):
        first = next(step for step in self.steps if step.get("name") in plan.STEP_GROUPS)
        without = [step for step in self.steps if step is not first]
        for changed in (without, self.steps + [first],
                        self.steps + [{"name": "New unclassified check", "run": "false"}]):
            with self.subTest(changed=changed[-1].get("name")):
                with self.assertRaisesRegex(ValueError, "classified exactly once"):
                    plan.validate_steps(changed)

    def test_check_conditions_cover_each_selected_group_once(self):
        for step in self.steps:
            if step.get("name") in plan.STEP_GROUPS:
                group = plan.STEP_GROUPS[step["name"]]
                self.assertEqual(step.get("if"), "matrix.group == '" + group + "'")
            elif "uses" in step or step.get("name") in plan.SETUP_STEPS:
                self.assertNotIn("if", step)

    def test_direct_script_mapping_and_unknown_fallback(self):
        self.assertEqual(plan.direct_script_groups(
            {"tools/test_notification_images.py"}), {"media"})
        self.assertEqual(plan.direct_script_groups(
            {"tools/test_notification_images.py",
             "tools/test_settings_context_bootstrap.py"}), {"media", "java"})
        for changed in (set(), {"tools/ci/receipt_identity.py"},
                        {"compat/media/video/video_decoder.cc"},
                        {"tools/test_notification_images.py", "unknown.py"}):
            with self.subTest(changed=changed):
                self.assertIsNone(plan.direct_script_groups(changed))

    def test_script_mapping_has_exact_workflow_owners(self):
        other_runs = []
        for path in (ROOT / ".github/workflows").glob("*.yml"):
            workflow = yaml.safe_load(path.read_text(encoding="utf-8"))
            for name, job in workflow.get("jobs", {}).items():
                if path.name == "ci.yml" and name == "artifact-preflight":
                    continue
                other_runs.extend(step.get("run", "") for step in job.get("steps", []))
        for script, groups in plan.SCRIPT_GROUPS.items():
            owners = {plan.STEP_GROUPS[step["name"]] for step in self.steps
                      if script in step.get("run", "")}
            with self.subTest(script=script):
                self.assertEqual(groups, owners)
                self.assertTrue((ROOT / script).is_file())
                self.assertTrue(re.fullmatch(r"test_[A-Za-z0-9_]+", Path(script).stem))
                self.assertFalse(any(script in run for run in other_runs))

    def test_shared_imported_scripts_are_not_exclusive(self):
        scripts = {Path(path).stem: path for path in plan.SCRIPT_GROUPS}
        for folder in (ROOT / "tools", ROOT / ".github/scripts"):
            for path in folder.rglob("*.py"):
                # Avoid outputs or dependency trees created inside a test worktree.
                if any(part in {"out", ".venv", "node_modules"} for part in path.parts):
                    continue
                try:
                    tree = ast.parse(path.read_text(encoding="utf-8-sig"))
                except SyntaxError:
                    continue
                for node in ast.walk(tree):
                    imported = []
                    if isinstance(node, ast.ImportFrom) and node.module:
                        imported.append(node.module.split(".")[-1])
                    if isinstance(node, ast.Import):
                        imported.extend(alias.name.split(".")[-1] for alias in node.names)
                    for name in imported:
                        with self.subTest(consumer=str(path), module=name):
                            self.assertNotIn(name, scripts)


if __name__ == "__main__":
    unittest.main()
