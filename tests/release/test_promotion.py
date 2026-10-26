"""Exercise publication decisions with an in-memory GitHub; never contact GitHub."""
import copy
import hashlib
import io
import json
import sys
import urllib.error
import zipfile
from email.message import Message
from pathlib import Path
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools/release"))
import guard as g
import promote as p
import test_guard as base
from test_guard import SHA, fixtures


class FakeGitHub:
    def __init__(self, candidate, allow_write=True):
        self.run, self.workflow, self.jobs, self.installer = fixtures()
        self.receipt = copy.deepcopy(self.installer)
        self.receipt.update(id=12, name=g.RECEIPT_ARTIFACT)
        self.allow_write = allow_write
        self.candidate = candidate
        self.tag = None
        self.item = None
        self.bytes = {}
        self.mutations = []
        self.public_links = []
        self.artifact_archives = {}
        self.corrupt_upload = False
        self.red_after_upload = False

    def json(self, path, method="GET", payload=None, missing=False):
        if method != "GET":
            if not self.allow_write:
                raise AssertionError("Dry run attempted a mutation")
            self.mutations.append((method, path, payload))
            if path == "/git/refs":
                if self.tag is not None:
                    raise AssertionError("Attempted to overwrite a tag")
                self.tag = payload["sha"]
                return {"object": {"type": "commit", "sha": self.tag}}
            if method == "POST" and path == "/releases":
                self.item = dict(payload, id=77, assets=[], html_url="https://github.com/" + g.REPOSITORY + "/releases/tag/" + g.TAG)
                return self.item
            if method == "PATCH" and path == "/releases/77":
                self.item.update(payload)
                return self.item
            raise AssertionError("Unexpected mutation " + path)
        if path == "/actions/runs/123":
            run = copy.deepcopy(self.run)
            if self.red_after_upload and self.bytes:
                run["conclusion"] = "failure"
            return run
        if path == "/actions/workflows/9":
            return self.workflow
        if path == "/git/ref/tags/" + g.TAG:
            return {"object": {"type": "commit", "sha": self.tag}} if self.tag else None
        raise AssertionError("Unexpected read " + path)

    def collection(self, path, key=None):
        if path.endswith("/jobs"):
            return self.jobs, len(self.jobs)
        if path.endswith("/artifacts"):
            return [self.installer, self.receipt], 2
        if path == "/releases":
            return ([self.item], 1) if self.item else ([], 0)
        if path == "/releases/77/assets":
            return self.item["assets"], len(self.item["assets"])
        raise AssertionError("Unexpected collection " + path)

    def upload(self, identifier, name, path):
        if not self.allow_write or any(a["name"] == name for a in self.item["assets"]):
            raise AssertionError("Unauthorized overwrite/upload")
        value = Path(path).read_bytes()
        asset = {"id": 100 + len(self.item["assets"]), "name": name, "state": "uploaded", "size": len(value),
                 "digest": "sha256:" + g.file_digest(path),
                 "browser_download_url": "https://github.com/" + g.REPOSITORY + "/releases/download/" + g.TAG + "/" + name}
        self.bytes[name] = (b"X" + value[1:]) if self.corrupt_upload else value
        self.item["assets"].append(asset)
        self.mutations.append(("UPLOAD", name, None))
        return asset

    def download(self, path, target, size):
        if path.startswith("/actions/artifacts/"):
            Path(target).write_bytes(self.artifact_archives[int(path.split("/")[-2])])
            return
        identifier = int(path.rsplit("/", 1)[1])
        name = next(a["name"] for a in self.item["assets"] if a["id"] == identifier)
        Path(target).write_bytes(self.bytes[name])

    def public_download(self, url, target, size):
        self.public_links.append(url)
        Path(target).write_bytes(self.bytes[url.rsplit("/", 1)[1]])


class PromotionTests(unittest.TestCase):
    write_manifest = base.ByteTests.write_manifest
    tearDown = base.ByteTests.tearDown

    def setUp(self):
        base.ByteTests.setUp(self)
        self.notes = self.root / "notes.md"
        self.notes.write_text("Source __SOURCE_SHA__, run __CI_RUN__", encoding="utf-8")
        self.client = FakeGitHub(self.candidate)

    def promote(self):
        return p.promote(self.client, 123, SHA, self.candidate, self.notes, self.root / "existing")

    def source_archives(self):
        def build(item, names):
            output = io.BytesIO()
            with zipfile.ZipFile(output, "w", zipfile.ZIP_DEFLATED) as archive:
                for name in names:
                    archive.writestr(name, (self.candidate / name).read_bytes())
            value = output.getvalue()
            self.client.artifact_archives[item["id"]] = value
            item.update(size_in_bytes=len(value), digest="sha256:" + hashlib.sha256(value).hexdigest())
        build(self.client.installer, [g.INSTALLER])
        self.manifest["artifact"]["digest"] = self.client.installer["digest"]
        self.write_manifest()
        build(self.client.receipt, [g.MANIFEST, "SHA256SUMS"])

    def test_prepare_consumes_both_exact_archives_and_preserves_installer_bytes(self):
        self.source_archives()
        target = self.root / "prepared"
        run, installer, receipt, assets = p.prepare(self.client, 123, SHA, target)
        self.assertEqual((run["id"], installer["id"], receipt["id"]), (123, 11, 12))
        self.assertEqual(set(assets), {g.INSTALLER, g.MANIFEST, "SHA256SUMS"})
        for name in assets:
            self.assertEqual((target / name).read_bytes(), (self.candidate / name).read_bytes())
        self.assertEqual(self.client.mutations, [])
        self.assertFalse(list(self.root.glob("gesher-release-download-*")))

    def test_prepare_refuses_either_corrupt_archive_and_cleans_only_temporary_files(self):
        self.source_archives()
        for identifier in (11, 12):
            original = self.client.artifact_archives[identifier]
            self.client.artifact_archives[identifier] = b"X" + original[1:]
            with self.subTest(artifact=identifier), self.assertRaisesRegex(g.Refused, "archive size/digest"):
                p.prepare(self.client, 123, SHA, self.root / str(identifier))
            self.client.artifact_archives[identifier] = original
            self.assertFalse(list(self.root.glob("gesher-release-download-*")))
            self.assertTrue((self.candidate / g.INSTALLER).exists())
        self.assertEqual(self.client.mutations, [])

    def test_dry_run_never_mutates(self):
        self.client.allow_write = False
        proof = self.promote()
        self.assertTrue(proof["dry_run"])
        self.assertEqual(self.client.mutations, [])
        self.assertIsNone(self.client.tag)

    def test_new_release_uploads_exact_bytes_before_publication(self):
        proof = self.promote()
        self.assertFalse(proof["dry_run"])
        self.assertEqual(self.client.tag, SHA)
        self.assertFalse(self.client.item["draft"])
        self.assertEqual(set(self.client.bytes), {g.INSTALLER, g.MANIFEST, "SHA256SUMS"})
        self.assertEqual(self.client.bytes[g.INSTALLER], (self.candidate / g.INSTALLER).read_bytes())
        self.assertEqual(self.client.mutations[-1][0:2], ("PATCH", "/releases/77"))
        self.assertNotIn("__SOURCE_SHA__", self.client.item["body"])

    def test_red_source_never_creates_tag_or_downloads_assets(self):
        self.client.run["conclusion"] = "failure"
        with self.assertRaises(g.Refused):
            self.promote()
        self.assertEqual(self.client.mutations, [])

    def test_changed_attempt_before_publish_leaves_draft_unpublished(self):
        self.client.red_after_upload = True
        with self.assertRaises(g.Refused):
            self.promote()
        self.assertTrue(self.client.item["draft"])
        self.assertFalse(any(method == "PATCH" for method, _, _ in self.client.mutations))

    def test_corrupt_uploaded_bytes_never_publish(self):
        self.client.corrupt_upload = True
        with self.assertRaisesRegex(g.Refused, "bytes differ"):
            self.promote()
        self.assertTrue(self.client.item["draft"])
        self.assertFalse(any(method == "PATCH" for method, _, _ in self.client.mutations))

    def test_existing_matching_partial_draft_resumes_without_overwrite(self):
        self.client.tag = SHA
        self.client.item = {"id": 77, "tag_name": g.TAG, "draft": True, "prerelease": False, "assets": [],
                            "html_url": "https://github.com/" + g.REPOSITORY + "/releases/tag/" + g.TAG}
        self.client.upload(77, g.INSTALLER, self.candidate / g.INSTALLER)
        self.client.mutations.clear()
        self.promote()
        uploads = [name for method, name, _ in self.client.mutations if method == "UPLOAD"]
        self.assertEqual(set(uploads), {g.MANIFEST, "SHA256SUMS"})
        self.assertNotIn(g.INSTALLER, uploads)
        self.assertFalse(self.client.item["draft"])

    def test_published_identical_release_is_read_only_and_public_links_checked(self):
        self.promote()
        self.client.mutations.clear()
        self.promote_again_directory = self.root / "second-existing"
        proof = p.promote(self.client, 123, SHA, self.candidate, self.notes, self.promote_again_directory)
        self.assertFalse(proof["dry_run"])
        self.assertEqual(self.client.mutations, [])
        self.assertEqual(len(self.client.public_links), 6)

    def test_wrong_existing_tag_blocks_before_asset_writes(self):
        self.client.tag = "d" * 40
        with self.assertRaisesRegex(g.Refused, "tag points elsewhere"):
            self.promote()
        self.assertEqual(self.client.mutations, [])

    def test_read_only_http_client_cannot_post(self):
        client = p.GitHub("own-fixture-token")
        client.opener = mock.Mock()
        with self.assertRaisesRegex(g.Refused, "not authorized"):
            client.json("/releases", "POST", {})
        client.opener.open.assert_not_called()

    def test_redirected_artifact_request_drops_repository_token(self):
        client = p.GitHub("own-fixture-token")
        body = b"own archive fixture bytes"
        headers = Message()
        headers["Location"] = "https://production-fixture.blob.core.windows.net/artifact?sig=fixture"
        client.opener = mock.Mock()
        client.opener.open.side_effect = urllib.error.HTTPError("https://api.github.com/fixture", 302, "Found", headers, io.BytesIO())
        def storage(request, timeout):
            self.assertIsNone(request.get_header("Authorization"))
            self.assertEqual(request.full_url, headers["Location"])
            return io.BytesIO(body)
        with mock.patch("urllib.request.urlopen", side_effect=storage):
            target = self.root / "download.zip"
            client.download("/actions/artifacts/11/zip", target, len(body))
            self.assertEqual(target.read_bytes(), body)
        api_request = client.opener.open.call_args.args[0]
        self.assertEqual(api_request.get_header("Authorization"), "Bearer own-fixture-token")

    def test_unknown_download_host_and_public_url_refused(self):
        client = p.GitHub("own-fixture-token")
        headers = Message()
        headers["Location"] = "https://attacker.invalid/bytes"
        client.opener = mock.Mock()
        client.opener.open.side_effect = urllib.error.HTTPError("https://api.github.com/fixture", 302, "Found", headers, io.BytesIO())
        with mock.patch("urllib.request.urlopen") as network:
            with self.assertRaisesRegex(g.Refused, "redirect"):
                client.download("/actions/artifacts/11/zip", self.root / "bad.zip", 12)
            with self.assertRaisesRegex(g.Refused, "public download"):
                client.public_download("https://attacker.invalid/setup.exe", self.root / "bad.exe", 12)
            network.assert_not_called()

    def test_cli_refuses_local_artifact_consumption_before_network(self):
        with mock.patch.dict(p.os.environ, {}, clear=True), mock.patch.object(p, "GitHub") as api:
            with self.assertRaisesRegex(g.Refused, "CI-only"):
                p.main(["prepare", "--source-run", "123", "--expected-sha", SHA, "--directory", str(self.root / "local")])
            api.assert_not_called()
