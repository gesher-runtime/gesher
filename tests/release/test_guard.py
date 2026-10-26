"""Own-byte/offline release policy tests: no GitHub calls, installer or tags."""
import copy
import hashlib
import json
import sys
import tempfile
import unittest
import zipfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools/release"))
import guard as g

SHA = "a" * 40


def fixtures():
    run = {"id": 123, "run_attempt": 1, "repository": {"id": 42, "full_name": g.REPOSITORY},
           "head_repository": {"id": 42, "full_name": g.REPOSITORY, "fork": False},
           "head_sha": SHA, "head_branch": "main", "status": "completed", "conclusion": "success",
           "event": "workflow_dispatch", "path": g.WORKFLOW, "workflow_id": 9}
    workflow = {"id": 9, "path": g.WORKFLOW, "state": "active"}
    jobs = [{"id": i + 1, "run_id": 123, "head_sha": SHA, "name": name,
             "status": "completed", "conclusion": "success" if name in g.REQUIRED_JOBS else "skipped",
             "steps": [{"name": step, "status": "completed", "conclusion": "success"}
                       for step in sorted(g.REQUIRED_STEPS.get(name, set()))]}
            for i, name in enumerate(sorted(g.REQUIRED_JOBS | g.ALLOWED_SKIPS))]
    artifact = {"id": 11, "name": "gesher-setup", "expired": False, "size_in_bytes": 100,
                "digest": "sha256:" + "b" * 64,
                "workflow_run": {"id": 123, "repository_id": 42, "head_repository_id": 42,
                                 "head_sha": SHA, "head_branch": "main"}}
    return run, workflow, jobs, artifact


class PolicyTests(unittest.TestCase):
    def setUp(self):
        self.run, self.workflow, self.jobs, self.artifact = fixtures()

    def validate(self, jobs=None, total=None):
        jobs = self.jobs if jobs is None else jobs
        return g.source_run(self.run, self.workflow, jobs, len(jobs) if total is None else total, SHA)

    def test_full_main_ci_and_documented_skips(self):
        self.assertEqual(self.validate(), {"run_id": 123, "attempt": 1, "sha": SHA})

    def test_red_cancelled_partial_and_wrong_workflow_runs(self):
        for key, value in (("status", "in_progress"), ("conclusion", "failure"), ("conclusion", "cancelled"),
                           ("conclusion", "timed_out"), ("path", ".github/workflows/other.yml"),
                           ("workflow_id", 10), ("event", "pull_request"), ("head_branch", "topic"),
                           ("head_sha", "c" * 40), ("run_attempt", 0)):
            with self.subTest(key=key, value=value):
                self.run = fixtures()[0]
                self.run[key] = value
                with self.assertRaises(g.Refused):
                    self.validate()

    def test_forked_source_and_cross_repository_source(self):
        for key, value in (("id", 99), ("full_name", "someone/gesher"), ("fork", True)):
            with self.subTest(key=key):
                self.run = fixtures()[0]
                self.run["head_repository"][key] = value
                with self.assertRaises(g.Refused):
                    self.validate()

    def test_partial_pagination_and_partial_reruns(self):
        with self.assertRaisesRegex(g.Refused, "pagination"):
            self.validate(self.jobs[:-1], len(self.jobs))
        with self.assertRaisesRegex(g.Refused, "matrix"):
            self.validate([self.jobs[0]])

    def test_every_job_and_required_steps_must_succeed(self):
        for conclusion in ("failure", "cancelled", "neutral", "skipped"):
            with self.subTest(conclusion=conclusion):
                jobs = copy.deepcopy(self.jobs)
                next(j for j in jobs if j["name"] == "test (windows-2022)")["conclusion"] = conclusion
                with self.assertRaises(g.Refused):
                    self.validate(jobs)
        jobs = copy.deepcopy(self.jobs)
        next(j for j in jobs if j["name"] == "package")["steps"] = []
        with self.assertRaisesRegex(g.Refused, "Required step"):
            self.validate(jobs)
        jobs = copy.deepcopy(self.jobs)
        jobs[0]["steps"].append({"name": "continue-on-error check", "status": "completed", "conclusion": "failure"})
        with self.assertRaisesRegex(g.Refused, "Hidden"):
            self.validate(jobs)

    def test_unknown_skips_duplicates_and_job_provenance(self):
        for change in ("extra_skip", "duplicate", "different_sha", "different_run"):
            jobs = copy.deepcopy(self.jobs)
            if change == "extra_skip":
                jobs.append({"id": 99, "run_id": 123, "head_sha": SHA, "name": "new check", "status": "completed", "conclusion": "skipped"})
            elif change == "duplicate":
                jobs.append(copy.deepcopy(jobs[0]))
            elif change == "different_sha":
                jobs[0]["head_sha"] = "d" * 40
            else:
                jobs[0]["run_id"] = 999
            with self.subTest(change=change), self.assertRaises(g.Refused):
                self.validate(jobs)

    def test_publisher_is_ci_main_expected_sha_and_explicit_only(self):
        env = {"GITHUB_ACTIONS": "true", "GITHUB_REPOSITORY": g.REPOSITORY, "GITHUB_REF": "refs/heads/main",
               "GITHUB_EVENT_NAME": "workflow_dispatch", "GITHUB_SHA": SHA,
               "GITHUB_WORKFLOW_REF": g.REPOSITORY + "/" + g.PROMOTION_WORKFLOW + "@refs/heads/main"}
        self.assertEqual(g.ci_context(env, SHA), SHA)
        for key, value in (("GITHUB_ACTIONS", "false"), ("GITHUB_REPOSITORY", "fork/gesher"),
                           ("GITHUB_REF", "refs/pull/1/merge"), ("GITHUB_EVENT_NAME", "pull_request"),
                           ("GITHUB_SHA", "d" * 40), ("GITHUB_WORKFLOW_REF", "different/workflow")):
            with self.subTest(key=key), self.assertRaises(g.Refused):
                g.ci_context(dict(env, **{key: value}), SHA)
        for invalid in ("main", "a" * 7, SHA + "; command", "-" * 40):
            with self.subTest(invalid=invalid), self.assertRaises(g.Refused):
                g.ci_context(env, invalid)

    def test_artifacts_expiry_identity_digest_and_duplicates(self):
        self.assertEqual(g.artifact([self.artifact], "gesher-setup", self.run)["id"], 11)
        for field, value in (("expired", True), ("size_in_bytes", 0), ("digest", "no-digest")):
            item = copy.deepcopy(self.artifact)
            item[field] = value
            with self.subTest(field=field), self.assertRaises(g.Refused):
                g.artifact([item], "gesher-setup", self.run)
        for field, value in (("head_sha", "c" * 40), ("head_branch", "topic"), ("id", 999), ("head_repository_id", 99)):
            item = copy.deepcopy(self.artifact)
            item["workflow_run"][field] = value
            with self.subTest(field=field), self.assertRaises(g.Refused):
                g.artifact([item], "gesher-setup", self.run)
        with self.assertRaises(g.Refused):
            g.artifact([self.artifact, self.artifact], "gesher-setup", self.run)

    def test_safe_existing_tag_and_release_resume(self):
        assets = {g.INSTALLER: {"sha256": "b" * 64, "size": 20}, "SHA256SUMS": {"sha256": "c" * 64, "size": 30}}
        self.assertTrue(g.existing_release(None, None, SHA, assets)["create_tag"])
        release = {"tag_name": g.TAG, "draft": True, "prerelease": False, "assets": []}
        plan = g.existing_release(SHA, release, SHA, assets)
        self.assertEqual(set(plan["upload"]), set(assets))
        self.assertTrue(plan["publish"])
        release["assets"] = [{"name": name, "state": "uploaded", "size": value["size"], "digest": "sha256:" + value["sha256"]}
                             for name, value in assets.items()]
        release["draft"] = False
        plan = g.existing_release(SHA, release, SHA, assets)
        self.assertFalse(plan["publish"])
        self.assertEqual(plan["upload"], [])
        with self.assertRaises(g.Refused):
            g.existing_release("d" * 40, release, SHA, assets)
        release["assets"][0]["digest"] = "sha256:" + "d" * 64
        with self.assertRaisesRegex(g.Refused, "bytes differ"):
            g.existing_release(SHA, release, SHA, assets)
        release["assets"] = []
        release["immutable"] = True
        with self.assertRaisesRegex(g.Refused, "Immutable"):
            g.existing_release(SHA, release, SHA, assets)


class ByteTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="gesher-release-fixture-")
        self.root = Path(self.temp.name).resolve()
        # Verify the exact fresh cleanup target before TemporaryDirectory removes it.
        self.assertEqual(self.root.parent, Path(tempfile.gettempdir()).resolve())
        self.run, _, _, self.artifact = fixtures()
        self.candidate = self.root / "candidate"
        self.candidate.mkdir()
        (self.candidate / g.INSTALLER).write_bytes(b"MZ" + b"own offline fixture bytes" * 8)
        self.manifest = {"schema": 1, "product": "Gesher", "version": g.VERSION, "version_selection": "release",
                         "installer": {"name": g.INSTALLER, "size": (self.candidate / g.INSTALLER).stat().st_size,
                                       "sha256": g.file_digest(self.candidate / g.INSTALLER), "product_name": "Gesher",
                                       "product_version": "0.1.0", "file_version": "0.1.0.0"},
                         "source": {"repository": g.REPOSITORY, "ref": "refs/heads/main", "sha": SHA,
                                    "workflow": g.WORKFLOW, "event": "workflow_dispatch", "run_id": 123, "run_attempt": 1},
                         "artifact": {"name": "gesher-setup", "id": 11, "digest": self.artifact["digest"]},
                         "package_checks": sorted(g.PACKAGE_CHECKS)}
        self.write_manifest()

    def tearDown(self):
        self.temp.cleanup()

    def write_manifest(self):
        (self.candidate / g.MANIFEST).write_text(json.dumps(self.manifest) + "\n", encoding="utf-8")
        (self.candidate / "SHA256SUMS").write_text(
            f"{g.file_digest(self.candidate / g.INSTALLER)}  {g.INSTALLER}\n"
            f"{g.file_digest(self.candidate / g.MANIFEST)}  {g.MANIFEST}\n", encoding="utf-8")

    def test_own_bytes_match_receipts(self):
        self.assertEqual(set(g.candidate(self.candidate, self.run, self.artifact)), {g.INSTALLER, g.MANIFEST, "SHA256SUMS"})

    def test_installer_and_checksum_corruption_refused(self):
        (self.candidate / g.INSTALLER).write_bytes(b"MZ wrong bytes")
        with self.assertRaises(g.Refused):
            g.candidate(self.candidate, self.run, self.artifact)
        (self.candidate / g.INSTALLER).write_bytes(b"MZ" + b"own offline fixture bytes" * 8)
        (self.candidate / "SHA256SUMS").write_text("wrong", encoding="utf-8")
        with self.assertRaisesRegex(g.Refused, "Checksum"):
            g.candidate(self.candidate, self.run, self.artifact)

    def test_receipt_must_match_run_attempt_version_and_installer_artifact(self):
        original = copy.deepcopy(self.manifest)
        changes = [("source", "sha", "c" * 40), ("source", "run_attempt", 2), ("source", "run_id", 999),
                   ("source", "ref", "refs/heads/topic"), ("artifact", "id", 22),
                   ("installer", "file_version", "0.1.0.9"), ("installer", "product_name", "Other")]
        for section, key, value in changes:
            self.manifest = copy.deepcopy(original)
            self.manifest[section][key] = value
            self.write_manifest()
            with self.subTest(section=section, key=key), self.assertRaises(g.Refused):
                g.candidate(self.candidate, self.run, self.artifact)
        self.manifest = copy.deepcopy(original)
        self.manifest["version_selection"] = "ci-run-number"
        self.write_manifest()
        with self.assertRaisesRegex(g.Refused, "explicit"):
            g.candidate(self.candidate, self.run, self.artifact)

    def test_archive_digest_and_contents_before_extraction(self):
        path = self.root / "artifact.zip"
        with zipfile.ZipFile(path, "w") as zipped:
            zipped.writestr(g.INSTALLER, b"MZ" + b"fixture" * 20)
        item = {"size_in_bytes": path.stat().st_size, "digest": "sha256:" + g.file_digest(path)}
        output = self.root / "extracted"
        g.archive(path, item, {g.INSTALLER}, output)
        self.assertTrue((output / g.INSTALLER).exists())
        with self.assertRaisesRegex(g.Refused, "digest"):
            g.archive(path, dict(item, digest="sha256:" + "d" * 64), {g.INSTALLER}, self.root / "bad")
        with zipfile.ZipFile(path, "w") as zipped:
            zipped.writestr("../outside.exe", b"fixture")
        item = {"size_in_bytes": path.stat().st_size, "digest": "sha256:" + g.file_digest(path)}
        with self.assertRaisesRegex(g.Refused, "contents"):
            g.archive(path, item, {g.INSTALLER}, self.root / "unsafe")
        self.assertFalse((self.root / "outside.exe").exists())


if __name__ == "__main__":
    unittest.main()
