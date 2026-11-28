"""Exact-input cache retirement, provenance and current-run publication contracts."""
import copy
import json
from pathlib import Path
import shutil
import tempfile
import unittest
from unittest.mock import patch

import host_tools_cache as cache
import re


class HostToolsCacheTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.environment = {
            "GITHUB_REPOSITORY": "gesher-runtime/gesher",
            "GITHUB_REF": "refs/heads/main",
            "GITHUB_SHA": "a" * 40,
            "GITHUB_RUN_ID": "123",
        }
        self.key = "host-generators-v1-" + "b" * 64
        for name in cache.FILES:
            path = self.root / "dist-host" / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(name.encode())
        cache.seal(self.root, self.key, self.environment)
        shutil.rmtree(self.root / "dist-host")

    def restore(self):
        return cache.restore(self.root, self.key, self.environment["GITHUB_REPOSITORY"])

    def receipt(self):
        return self.root / cache.CACHE / "receipt.json"

    def test_exact_inputs_reuse_all_generators(self):
        receipt = self.restore()
        self.assertEqual(receipt["source"], self.environment["GITHUB_SHA"])
        self.assertEqual(cache.inventory(self.root / "dist-host"), receipt["files"])
        self.assertFalse((self.root / "dist-host" / "receipt.json").exists())

    def test_changed_key_and_producer_are_rejected(self):
        original = json.loads(self.receipt().read_text())
        for field, value in (("key", "different"), ("ref", "refs/pull/1/merge"),
                             ("source", "../secret"), ("run", "0"),
                             ("repository", "other/repository")):
            with self.subTest(field=field):
                receipt = copy.deepcopy(original)
                receipt[field] = value
                self.receipt().write_text(json.dumps(receipt))
                with self.assertRaisesRegex(ValueError, "provenance"):
                    self.restore()
                self.assertFalse((self.root / "dist-host").exists())

    def test_changed_missing_and_extra_files_are_rejected_before_copying(self):
        folder = self.root / cache.CACHE / "files"
        path = folder / cache.FILES[0]
        original = path.read_bytes()
        path.write_bytes(b"changed")
        with self.assertRaisesRegex(ValueError, "digest"):
            self.restore()
        path.unlink()
        with self.assertRaisesRegex(ValueError, "Incomplete"):
            self.restore()
        path.write_bytes(original)
        (folder / "unexpected").write_bytes(b"extra")
        with self.assertRaisesRegex(ValueError, "Unexpected"):
            self.restore()
        self.assertFalse((self.root / "dist-host").exists())

    def test_branch_cannot_publish_main_receipt(self):
        environment = dict(self.environment, GITHUB_REF="refs/pull/1/merge")
        with self.assertRaisesRegex(ValueError, "Only main"):
            cache.seal(self.root, self.key, environment)

    def test_manifest_path_cannot_escape_known_inventory(self):
        receipt = json.loads(self.receipt().read_text())
        receipt["files"]["../../escape"] = "0" * 64
        self.receipt().write_text(json.dumps(receipt))
        with self.assertRaisesRegex(ValueError, "digest"):
            self.restore()

    def test_source_and_toolchain_changes_invalidate_key(self):
        names = ("aosp.lock", "tools/host/CMakeLists.txt", ".github/workflows/framework.yml")
        for name in names:
            path = self.root / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(name)
        output = ("\0".join(names) + "\0").encode()
        with patch.object(cache.subprocess, "check_output", return_value=output):
            before = cache.key(self.root, {"compiler": "one"})
            (self.root / "aosp.lock").write_text("new revision")
            self.assertNotEqual(before, cache.key(self.root, {"compiler": "one"}))
            self.assertNotEqual(cache.key(self.root, {"compiler": "one"}),
                                cache.key(self.root, {"compiler": "two"}))

    def test_cli_corrupt_cache_falls_back_to_build(self):
        for malformed in ('{', '1', 'null', '"scalar"', '[1, 2]', 'true'):
            with self.subTest(receipt=malformed):
                self.receipt().write_text(malformed)
                with patch.object(cache, "ROOT", self.root), \
                        patch.object(cache, "output") as output, \
                        patch.object(cache.os, "environ", self.environment), \
                        patch("sys.argv", ["cache", "restore", "--key", self.key]):
                    cache.main()
                output.assert_called_once_with("hit", "false")

    def test_workflow_republishes_current_artifact_after_verification(self):
        workflow = (cache.ROOT / ".github/workflows/framework.yml").read_text()
        host = workflow.split("  framework:\n", 1)[0]
        by_name = {}
        for match in re.finditer(r"(?m)^      - name: (.+)\n", host):
            end = host.find("\n      - ", match.end())
            body = host[match.end():None if end < 0 else end]
            conditions = re.search(r"(?m)^        if: (.+)$", body)
            by_name[match[1]] = {"if": conditions[1] if conditions else "", "body": body}
        self.assertIn("steps.host-generators.outputs.hit != 'true'", by_name["Build"]["if"])
        self.assertNotIn("host-generators", by_name["Smoke-test the tools"]["if"])
        self.assertNotIn("host-generators", by_name["Publish the build for other branches"]["if"])
        save = by_name["Save reusable main host generators"]
        self.assertIn("github.ref == 'refs/heads/main'", save["if"])
        restore = by_name["Restore main host generators"]
        self.assertNotIn("restore-keys", restore["body"])
        self.assertIn("run-$GITHUB_RUN_ID-attempt-$GITHUB_RUN_ATTEMPT",
                      by_name["Host tools' sources"]["body"])


if __name__ == "__main__":
    unittest.main()
