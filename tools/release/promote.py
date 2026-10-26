"""CI-only promotion of already tested installer bytes. No build or overwrite path."""
from __future__ import annotations

import argparse
import http.client
import json
import os
import re
import tempfile
import urllib.error
import urllib.parse
import urllib.request
from pathlib import Path

import guard as g


class NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, request, fp, code, message, headers, newurl):
        return None


class GitHub:
    def __init__(self, token, allow_write=False):
        g.require(bool(token), "Missing CI token")
        self.token, self.allow_write = token, allow_write
        self.opener = urllib.request.build_opener(NoRedirect())

    def request(self, path, method="GET", payload=None, missing=False, accept="application/vnd.github+json"):
        g.require(path.startswith("/") and not path.startswith("//") and ".." not in path.split("/"), "Invalid API path")
        if method != "GET":
            g.require(self.allow_write and method in ("POST", "PATCH") and
                      re.fullmatch(r"/(?:git/refs|releases(?:/[0-9]+)?)", path), "Mutation is not authorized")
        data = json.dumps(payload).encode() if payload is not None else None
        request = urllib.request.Request("https://api.github.com/repos/" + g.REPOSITORY + path, data=data, method=method,
                  headers={"Accept": accept, "Authorization": "Bearer " + self.token,
                           "X-GitHub-Api-Version": "2022-11-28", "User-Agent": "Gesher-tested-release",
                           "Content-Type": "application/json", "Accept-Encoding": "identity"})
        try:
            return self.opener.open(request, timeout=60)
        except urllib.error.HTTPError as error:
            if missing and method == "GET" and error.code == 404:
                return None
            if method == "GET" and error.code == 302 and accept == "application/octet-stream":
                return error
            raise g.Refused("GitHub API HTTP " + str(error.code)) from None

    def json(self, path, method="GET", payload=None, missing=False):
        response = self.request(path, method, payload, missing)
        if response is None:
            return None
        with response:
            return json.load(response)

    def collection(self, path, key=None):
        items, total = [], None
        for page in range(1, 101):
            result = self.json(path + ("&" if "?" in path else "?") + f"per_page=100&page={page}")
            rows = result[key] if key else result
            g.require(isinstance(rows, list), "Malformed paginated response")
            if key:
                if total is None:
                    total = result["total_count"]
                g.require(result.get("total_count") == total, "Collection changed during pagination")
            items.extend(rows)
            if len(rows) < 100:
                g.require(total is None or len(items) == total, "Partial API pagination")
                return items, len(items)
        raise g.Refused("API pagination exceeded its bound")

    def download(self, path, target, expected_size):
        g.require(0 < expected_size <= 2 * 1024 ** 3, "Unexpected download size")
        response = self.request(path, accept="application/octet-stream")
        if response.code == 302:
            location = response.headers.get("Location", "")
            response.close()
            url = urllib.parse.urlparse(location)
            g.require(url.scheme == "https" and url.hostname and
                      (url.hostname.endswith(".githubusercontent.com") or url.hostname.endswith(".blob.core.windows.net")),
                      "Unexpected GitHub byte-download redirect")
            # The signed storage URL never receives the repository token.
            request = urllib.request.Request(location, headers={"User-Agent": "Gesher-tested-release", "Accept-Encoding": "identity"})
            try:
                response = urllib.request.urlopen(request, timeout=60)
            except urllib.error.HTTPError as error:
                raise g.Refused("Byte download HTTP " + str(error.code)) from None
        self.save(response, target, expected_size)

    @staticmethod
    def save(response, target, expected_size):
        size = 0
        with response, Path(target).open("xb") as output:
            while True:
                block = response.read(1024 * 1024)
                if not block:
                    break
                size += len(block)
                g.require(size <= expected_size, "Downloaded bytes exceed the advertised size")
                output.write(block)
        g.require(size == expected_size, "Downloaded byte count differs")

    def public_download(self, url, target, expected_size):
        prefix = "https://github.com/" + g.REPOSITORY + "/releases/download/" + g.TAG + "/"
        g.require(url in {prefix + name for name in (g.INSTALLER, g.MANIFEST, "SHA256SUMS")}, "Unexpected public download URL")
        request = urllib.request.Request(url, headers={"User-Agent": "Gesher-tested-release", "Accept-Encoding": "identity"})
        try:
            response = urllib.request.urlopen(request, timeout=60)
        except urllib.error.HTTPError as error:
            raise g.Refused("Public release download HTTP " + str(error.code)) from None
        self.save(response, target, expected_size)

    def upload(self, release_id, name, path):
        g.require(self.allow_write and isinstance(release_id, int) and release_id > 0, "Upload is not authorized")
        g.require(name in (g.INSTALLER, g.MANIFEST, "SHA256SUMS"), "Unexpected upload filename")
        path = Path(path)
        connection = http.client.HTTPSConnection("uploads.github.com", timeout=60)
        route = "/repos/" + g.REPOSITORY + f"/releases/{release_id}/assets?name=" + urllib.parse.quote(name, safe="")
        try:
            connection.putrequest("POST", route)
            for key, value in {"Authorization": "Bearer " + self.token, "Accept": "application/vnd.github+json",
                               "X-GitHub-Api-Version": "2022-11-28", "User-Agent": "Gesher-tested-release",
                               "Content-Type": "application/octet-stream", "Content-Length": str(path.stat().st_size)}.items():
                connection.putheader(key, value)
            connection.endheaders()
            with path.open("rb") as source:
                for block in iter(lambda: source.read(1024 * 1024), b""):
                    connection.send(block)
            response = connection.getresponse()
            g.require(response.status == 201, "Asset upload did not create a new asset")
            return json.loads(response.read())
        finally:
            connection.close()


def source(client, identifier, expected):
    run = client.json(f"/actions/runs/{identifier}")
    g.require(run.get("id") == identifier, "Source run identity differs")
    workflow = client.json(f"/actions/workflows/{run.get('workflow_id')}")
    attempt = run.get("run_attempt")
    g.require(isinstance(attempt, int) and attempt > 0, "Missing source attempt")
    jobs, total = client.collection(f"/actions/runs/{identifier}/attempts/{attempt}/jobs", "jobs")
    g.source_run(run, workflow, jobs, total, expected)
    artifacts, _ = client.collection(f"/actions/runs/{identifier}/artifacts", "artifacts")
    return run, g.artifact(artifacts, "gesher-setup", run), g.artifact(artifacts, g.RECEIPT_ARTIFACT, run)


def prepare(client, identifier, expected, directory):
    run, installer, receipt = source(client, identifier, expected)
    directory = Path(directory)
    g.require(not directory.exists() or not any(directory.iterdir()), "Candidate directory must be fresh")
    directory.mkdir(parents=True, exist_ok=True)
    temporary = Path(tempfile.mkdtemp(prefix="gesher-release-download-", dir=str(directory.parent))).resolve()
    g.require(temporary.parent == directory.parent.resolve(), "Unexpected temporary directory")
    try:
        for item, filename, allowed in ((installer, "installer.zip", {g.INSTALLER}),
                                        (receipt, "receipt.zip", {g.MANIFEST, "SHA256SUMS"})):
            archive = temporary / filename
            client.download(f"/actions/artifacts/{item['id']}/zip", archive, item["size_in_bytes"])
            g.archive(archive, item, allowed, directory)
        assets = g.candidate(directory, run, installer)
    finally:
        # Only the two exact temporary files are removed; no recursive cleanup.
        for name in ("installer.zip", "receipt.zip"):
            path = temporary / name
            if path.exists():
                path.unlink()
        temporary.rmdir()
    return run, installer, receipt, assets


def tag_commit(client):
    reference = client.json("/git/ref/tags/" + g.TAG, missing=True)
    if reference is None:
        return None
    obj, seen = reference.get("object", {}), set()
    for _ in range(8):
        value = g.sha(obj.get("sha"))
        if obj.get("type") == "commit":
            return value
        g.require(obj.get("type") == "tag" and value not in seen, "Invalid/cyclic release tag")
        seen.add(value)
        obj = client.json("/git/tags/" + value).get("object", {})
    raise g.Refused("Annotated tag nesting exceeded its bound")


def release(client):
    releases, _ = client.collection("/releases")
    found = [item for item in releases if item.get("tag_name") == g.TAG]
    g.require(len(found) <= 1, "Ambiguous release tag")
    if not found:
        return None
    item = found[0]
    item["assets"], _ = client.collection(f"/releases/{item['id']}/assets")
    return item


def verify_existing_bytes(client, item, assets, directory):
    directory = Path(directory)
    directory.mkdir(parents=True, exist_ok=True)
    for asset in item.get("assets", []):
        name = asset["name"]
        target = directory / name
        if item.get("draft"):
            client.download(f"/releases/assets/{asset['id']}", target, assets[name]["size"])
        else:
            expected_url = "https://github.com/" + g.REPOSITORY + "/releases/download/" + g.TAG + "/" + name
            g.require(asset.get("browser_download_url") == expected_url, "Unexpected released download link")
            client.public_download(expected_url, target, assets[name]["size"])
        g.require(g.file_digest(target) == assets[name]["sha256"], "Published bytes differ: " + name)


def authenticate_receipt(client, item, parent):
    # Candidate execution can write the workspace, including prepare.json and the
    # extracted manifest. Fetch the original receipt again using fresh API metadata.
    # Its authenticated members remain immutable Python bytes for the whole promotion.
    temporary = Path(tempfile.mkdtemp(prefix="gesher-release-receipt-", dir=str(parent))).resolve()
    g.require(temporary.parent == Path(parent).resolve(), "Unexpected receipt directory")
    archive = temporary / "receipt.zip"
    try:
        g.require(0 < item["size_in_bytes"] <= 1024 * 1024, "Unexpected source receipt archive size")
        client.download(f"/actions/artifacts/{item['id']}/zip", archive, item["size_in_bytes"])
        return g.receipt_members(archive, item)
    finally:
        if archive.exists():
            archive.unlink()
        temporary.rmdir()


def promote(client, identifier, expected, directory, notes, verify_directory, prepared, candidate_pin):
    # Re-fetch all source gates immediately before any write. A new/incomplete
    # CI attempt or changed artifact identity invalidates the prepared receipt.
    run, installer, receipt = source(client, identifier, expected)
    trusted_receipt = authenticate_receipt(client, receipt, Path(directory).resolve().parent)
    assets = g.candidate(directory, run, installer, trusted_receipt)
    current_pin = g.source_pin({"source_run": identifier, "source_sha": expected,
                 "source_attempt": run["run_attempt"], "installer_artifact": installer,
                 "receipt_artifact": receipt, "assets": assets})
    g.require(candidate_pin == current_pin,
              "Candidate verification source changed; run candidate verification again")
    g.require(prepared.get("source_run") == identifier and prepared.get("source_sha") == expected and
              prepared.get("source_attempt") == run["run_attempt"] and prepared.get("assets") == assets,
              "Prepared source or bytes changed")
    for key, item in (("installer_artifact", installer), ("receipt_artifact", receipt)):
        original = prepared.get(key) or {}
        g.require(all(original.get(field) == item.get(field) for field in ("id", "name", "digest", "size_in_bytes")),
                  "Prepared artifact identity changed: " + key)
    commit, item = tag_commit(client), release(client)
    plan = g.existing_release(commit, item, expected, assets)
    if item:
        verify_existing_bytes(client, item, assets, verify_directory)
    body = None
    if plan["create_draft"]:
        body = Path(notes).read_text(encoding="utf-8").replace("__SOURCE_SHA__", expected).replace("__CI_RUN__", str(identifier))
        g.require("__SOURCE_" not in body and "__CI_RUN__" not in body, "Unresolved release-note provenance")
    if not client.allow_write:
        return {"dry_run": True, "plan": plan, "source_run": identifier, "source_sha": expected, "assets": assets}
    if plan["create_tag"]:
        client.json("/git/refs", "POST", {"ref": "refs/tags/" + g.TAG, "sha": expected})
    if plan["create_draft"]:
        item = client.json("/releases", "POST", {"tag_name": g.TAG, "target_commitish": expected,
                    "name": "Gesher " + g.VERSION, "body": body, "draft": True, "prerelease": False})
    for name in plan["upload"]:
        client.upload(item["id"], name, Path(directory) / name)
    item = release(client)
    final = g.existing_release(tag_commit(client), item, expected, assets)
    g.require(not final["upload"], "Release is still missing tested assets")
    # Download every uploaded asset and hash it before making a new draft public.
    final_directory = Path(verify_directory).parent / (Path(verify_directory).name + "-uploaded")
    verify_existing_bytes(client, item, assets, final_directory)
    if item.get("draft"):
        latest_run, latest_installer, latest_receipt = source(client, identifier, expected)
        g.require(latest_receipt == receipt and latest_installer == installer, "Source artifacts changed before publication")
        g.require(g.candidate(directory, latest_run, latest_installer, trusted_receipt) == assets,
                  "Candidate changed before publication")
        client.json(f"/releases/{item['id']}", "PATCH", {"draft": False, "make_latest": "true"})
    item = release(client)
    g.require(item and item.get("draft") is False and item.get("prerelease") is False, "Release is not published")
    return {"dry_run": False, "release_url": item["html_url"], "source_run": identifier, "source_sha": expected,
            "installer_url": "https://github.com/" + g.REPOSITORY + "/releases/download/" + g.TAG + "/" + g.INSTALLER,
            "artifacts": {"installer": installer["id"], "receipt": receipt["id"]}, "assets": assets}


def main(argv=None):
    parser = argparse.ArgumentParser()
    parser.add_argument("mode", choices=("prepare", "publish", "fetch-release"))
    parser.add_argument("--source-run", required=True, type=int)
    parser.add_argument("--expected-sha", required=True)
    parser.add_argument("--directory", required=True)
    parser.add_argument("--evidence", default="release-evidence")
    parser.add_argument("--notes", default="docs/releases/0.1.0.md")
    args = parser.parse_args(argv)
    expected = g.ci_context(os.environ, args.expected_sha)
    g.require(args.source_run > 0, "Invalid source run ID")
    workspace = Path(os.environ["GITHUB_WORKSPACE"]).resolve()
    directory, evidence = Path(args.directory).resolve(), Path(args.evidence).resolve()
    for path in (directory, evidence):
        g.require(path != workspace and path.is_relative_to(workspace), "Output must stay inside the CI workspace")
    allow_write = args.mode == "publish" and os.environ.get("GESHER_ALLOW_RELEASE_PUBLISH") == "1"
    client = GitHub(os.environ.get("GH_TOKEN"), allow_write)
    evidence.mkdir(parents=True, exist_ok=True)
    if args.mode == "prepare":
        run, installer, receipt, assets = prepare(client, args.source_run, expected, directory)
        proof = {"source_run": args.source_run, "source_attempt": run["run_attempt"], "source_sha": expected,
                 "installer_artifact": installer, "receipt_artifact": receipt, "assets": assets}
        if os.environ.get("GESHER_RECORD_CANDIDATE_PIN") == "1":
            # GitHub captures this step output before any installer executes.
            with Path(os.environ["GITHUB_OUTPUT"]).open("a", encoding="utf-8", newline="\n") as output:
                output.write("source_pin=" + json.dumps(g.source_pin(proof), sort_keys=True, separators=(",", ":")) + "\n")
    elif args.mode == "publish":
        prepared = json.loads((evidence / "prepare.json").read_text(encoding="utf-8"))
        candidate_pin = json.loads(os.environ.get("GESHER_CANDIDATE_SOURCE_PIN", "null"))
        g.require(isinstance(candidate_pin, dict), "Missing pre-execution candidate verification pin")
        proof = promote(client, args.source_run, expected, directory, args.notes,
                        directory.parent / (directory.name + "-existing"), prepared, candidate_pin)
    else:
        reference = directory.parent / (directory.name + "-source-candidate")
        _, _, _, assets = prepare(client, args.source_run, expected, reference)
        item = release(client)
        plan = g.existing_release(tag_commit(client), item, expected, assets)
        g.require(item and not item.get("draft") and not plan["upload"], "Published release is incomplete")
        verify_existing_bytes(client, item, assets, directory)
        g.require(set(assets) == {path.name for path in directory.iterdir()}, "Released asset set differs")
        proof = {"source_run": args.source_run, "source_sha": expected, "release_url": item["html_url"], "assets": assets}
    (evidence / (args.mode + ".json")).write_text(json.dumps(proof, indent=2) + "\n", encoding="utf-8")
    print("PASS release " + args.mode + ": validated exact source SHA, full CI, receipts and tested bytes")


if __name__ == "__main__":
    try:
        main()
    except (g.Refused, OSError, ValueError, KeyError, urllib.error.URLError) as error:
        raise SystemExit("Release refused: " + str(error)) from None
