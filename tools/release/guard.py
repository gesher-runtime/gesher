"""Release invariants, independent of network I/O so negative paths run offline."""
from __future__ import annotations

import hashlib
import io
import json
import re
import zipfile
from pathlib import Path

REPOSITORY = "gesher-runtime/gesher"
WORKFLOW = ".github/workflows/ci.yml"
PROMOTION_WORKFLOW = ".github/workflows/release.yml"
VERSION = "0.1.0"
TAG = "v" + VERSION
INSTALLER = "gesher-setup-" + VERSION + ".exe"
MANIFEST = "gesher-" + VERSION + "-manifest.json"
RECEIPT_ARTIFACT = "gesher-package-receipt"
REQUIRED_JOBS = {
    "build", "asm-port", "exec / exec", "framework / host-tools", "framework / framework",
    "angle / build", "angle / probe-build", "surfaces / apps", "graphics / build",
    "native-libs / native-libs", "arm64 / build", "posix / build", "native2 / native2",
    "libs / build", "boot-image / compile", "webview / build", "media / media", "package",
    "test (windows-2022)", "test (windows-2025)", "e2e (windows-2022)", "e2e (windows-2025)",
}
ALLOWED_SKIPS = {"angle / probe", "graphics / probe", "libs / test", "arm64 / smoke",
                 "posix / test", "webview / probe"}
REQUIRED_STEPS = {
    "build": {"Check assembly boundaries use the System V ABI"},
    "package": {"Test release guards", "Install, run Hello, uninstall", "Write package receipt"},
    "test (windows-2022)": {"Run tests"}, "test (windows-2025)": {"Run tests"},
    "e2e (windows-2022)": {"Run real apps"}, "e2e (windows-2025)": {"Run real apps"},
}
PACKAGE_CHECKS = {"installed_boot_image", "hello", "framework_lifecycle", "device_apps",
                  "window_input", "uninstall_cleanup"}


class Refused(ValueError):
    """A candidate cannot be promoted; no tag or asset should be changed."""


def require(condition, message):
    if not condition:
        raise Refused(message)


def sha(value):
    require(isinstance(value, str) and re.fullmatch(r"[0-9a-fA-F]{40}", value), "Expected a full commit SHA")
    return value.lower()


def digest(value):
    require(isinstance(value, str) and re.fullmatch(r"sha256:[0-9a-fA-F]{64}", value), "Missing SHA-256 digest")
    return value[7:].lower()


def ci_context(env, expected):
    expected = sha(expected)
    require(env.get("GITHUB_ACTIONS") == "true", "Promotion is CI-only")
    require(env.get("GITHUB_REPOSITORY") == REPOSITORY, "Wrong publishing repository")
    require(env.get("GITHUB_REF") == "refs/heads/main", "Promotion must run on main")
    require(env.get("GITHUB_EVENT_NAME") == "workflow_dispatch", "Promotion must be explicitly dispatched")
    require(env.get("GITHUB_WORKFLOW_REF") == REPOSITORY + "/" + PROMOTION_WORKFLOW + "@refs/heads/main",
            "Unexpected publishing workflow")
    require(sha(env.get("GITHUB_SHA")) == expected, "Publishing checkout differs from expected SHA")
    return expected


def source_run(run, workflow, jobs, total_count, expected):
    expected = sha(expected)
    repository = run.get("repository") or {}
    head = run.get("head_repository") or {}
    require(repository.get("full_name") == REPOSITORY and head.get("full_name") == REPOSITORY,
            "Source repository/fork mismatch")
    require(repository.get("id") and repository.get("id") == head.get("id") and not head.get("fork"),
            "Forked source run")
    require(run.get("head_branch") == "main" and sha(run.get("head_sha")) == expected,
            "Source ref/SHA mismatch")
    require(run.get("event") == "workflow_dispatch", "Release input requires a dispatched full main CI")
    require(run.get("status") == "completed" and run.get("conclusion") == "success", "Source CI is not green/completed")
    require(run.get("path") == WORKFLOW and workflow.get("path") == WORKFLOW and workflow.get("state") == "active",
            "Unexpected source workflow")
    require(run.get("workflow_id") == workflow.get("id"), "Workflow identity differs")
    require(isinstance(run.get("run_attempt"), int) and run["run_attempt"] > 0, "Missing source attempt")
    require(isinstance(total_count, int) and len(jobs) == total_count, "Partial job pagination")
    names, identifiers = set(), set()
    for job in jobs:
        name, identifier = job.get("name"), job.get("id")
        require(name and identifier and name not in names and identifier not in identifiers, "Duplicate/missing job identity")
        names.add(name)
        identifiers.add(identifier)
        require(job.get("run_id") == run.get("id") and sha(job.get("head_sha")) == expected,
                "Job provenance differs")
        require(job.get("status") == "completed", "A job is incomplete")
        conclusion = job.get("conclusion")
        require(conclusion == "success" or (conclusion == "skipped" and name in ALLOWED_SKIPS),
                "Unsuccessful or unexpectedly skipped job: " + name)
        if name in REQUIRED_JOBS:
            require(conclusion == "success", "Required job was skipped: " + name)
        successful_steps = set()
        for step in job.get("steps", []):
            require(step.get("status") == "completed" and step.get("conclusion") in ("success", "skipped"),
                    "Hidden failed/incomplete step in " + name)
            if step.get("conclusion") == "success":
                successful_steps.add(step.get("name"))
        require(REQUIRED_STEPS.get(name, set()) <= successful_steps, "Required step did not run: " + name)
    require(REQUIRED_JOBS <= names, "Full CI matrix is missing required jobs")
    return {"run_id": run["id"], "attempt": run["run_attempt"], "sha": expected}


def artifact(artifacts, name, run):
    found = [item for item in artifacts if item.get("name") == name]
    require(len(found) == 1, "Missing/ambiguous artifact: " + name)
    item = found[0]
    origin = item.get("workflow_run") or {}
    require(item.get("id") and item.get("expired") is False and item.get("size_in_bytes", 0) > 0,
            "Expired/empty artifact")
    require(origin.get("id") == run.get("id") and origin.get("head_branch") == "main" and
            sha(origin.get("head_sha")) == sha(run.get("head_sha")) and
            origin.get("repository_id") == run["repository"]["id"] and
            origin.get("head_repository_id") == run["repository"]["id"], "Artifact provenance differs")
    digest(item.get("digest"))
    return item


def file_digest(path):
    result = hashlib.sha256()
    with Path(path).open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            result.update(block)
    return result.hexdigest()


def source_pin(proof):
    """Pre-execution job output binding candidate verification to exact source bytes."""
    result = {key: proof[key] for key in ("source_run", "source_attempt", "source_sha", "assets")}
    for key in ("installer_artifact", "receipt_artifact"):
        result[key] = {field: proof[key][field] for field in ("id", "name", "digest", "size_in_bytes")}
    return result


def archive(path, item, allowed, output):
    path, output = Path(path), Path(output).resolve()
    require(path.stat().st_size == item["size_in_bytes"] and file_digest(path) == digest(item["digest"]),
            "Artifact archive size/digest mismatch")
    with zipfile.ZipFile(path) as zipped:
        entries = zipped.infolist()
        require(len(entries) == len(allowed) and {entry.filename for entry in entries} == set(allowed),
                "Unexpected/duplicate archive contents")
        output.mkdir(parents=True, exist_ok=True)
        for entry in entries:
            require(not entry.is_dir() and entry.file_size <= (2 * 1024 ** 3 if entry.filename == INSTALLER else 65536) and
                    ((entry.external_attr >> 16) & 0o170000) != 0o120000, "Invalid archive entry")
            target = (output / entry.filename).resolve()
            require(target.parent == output and not target.exists(), "Unsafe archive extraction path")
            with zipped.open(entry) as source, target.open("xb") as destination:
                for block in iter(lambda: source.read(1024 * 1024), b""):
                    destination.write(block)


def receipt_members(path, item):
    """Authenticate the small source receipt once, then keep its members in memory."""
    require(0 < item["size_in_bytes"] <= 1024 * 1024, "Unexpected source receipt archive size")
    raw = Path(path).read_bytes()
    require(len(raw) == item["size_in_bytes"] and hashlib.sha256(raw).hexdigest() == digest(item["digest"]),
            "Source receipt archive size/digest mismatch")
    with zipfile.ZipFile(io.BytesIO(raw)) as zipped:
        entries = zipped.infolist()
        require(len(entries) == 2 and {entry.filename for entry in entries} == {MANIFEST, "SHA256SUMS"},
                "Unexpected/duplicate source receipt members")
        require(all(not entry.is_dir() and entry.file_size <= 65536 and
                    ((entry.external_attr >> 16) & 0o170000) != 0o120000 for entry in entries),
                "Invalid source receipt entry")
        return {entry.filename: zipped.read(entry) for entry in entries}


def candidate(directory, run, installer_artifact, trusted_receipt=None):
    directory = Path(directory)
    require({entry.name for entry in directory.iterdir()} == {INSTALLER, MANIFEST, "SHA256SUMS"},
            "Candidate must contain exactly the installer and two receipts")
    manifest = (directory / MANIFEST).read_bytes()
    sums = (directory / "SHA256SUMS").read_bytes()
    if trusted_receipt is not None:
        require(manifest == trusted_receipt[MANIFEST] and sums == trusted_receipt["SHA256SUMS"],
                "Candidate receipt bytes differ from the authenticated source artifact")
        manifest, sums = trusted_receipt[MANIFEST], trusted_receipt["SHA256SUMS"]
    data = json.loads(manifest.decode("utf-8-sig"))
    require(data.get("schema") == 1 and data.get("product") == "Gesher" and data.get("version") == VERSION and
            data.get("version_selection") == "release", "Not the explicit 0.1.0 release candidate")
    origin = data.get("source") or {}
    require(origin.get("repository") == REPOSITORY and origin.get("ref") == "refs/heads/main" and
            origin.get("workflow") == WORKFLOW and origin.get("event") == "workflow_dispatch" and
            origin.get("sha") == run.get("head_sha") and origin.get("run_id") == run.get("id") and
            origin.get("run_attempt") == run.get("run_attempt"), "Receipt provenance differs")
    proof = data.get("artifact") or {}
    require(proof.get("name") == "gesher-setup" and proof.get("id") == installer_artifact["id"] and
            proof.get("digest") == installer_artifact["digest"], "Receipt names a different installer artifact")
    require(set(data.get("package_checks", [])) == PACKAGE_CHECKS, "Incomplete package smoke receipt")
    installer = data.get("installer") or {}
    path = directory / INSTALLER
    with path.open("rb") as binary:
        require(path.stat().st_size >= 64 and binary.read(2) == b"MZ", "Installer is not a PE executable")
    require(installer.get("name") == INSTALLER and installer.get("size") == path.stat().st_size and
            installer.get("sha256") == file_digest(path), "Installer size/digest mismatch")
    require(installer.get("product_name") == "Gesher", "Wrong installer product")
    for key in ("product_version", "file_version"):
        require(installer.get(key) in (VERSION, VERSION + ".0"), "Wrong installer PE metadata")
    manifest_hash = hashlib.sha256(manifest).hexdigest()
    expected_sums = f"{installer['sha256']}  {INSTALLER}\n{manifest_hash}  {MANIFEST}\n"
    require(sums.decode("utf-8").replace("\r\n", "\n") == expected_sums, "Checksum receipt differs")
    # The promotion caller supplies API-authenticated member bytes. Derive expected
    # hashes from those bytes, never by rereading mutable workspace receipts.
    return {INSTALLER: {"sha256": installer["sha256"], "size": installer["size"]},
            MANIFEST: {"sha256": manifest_hash, "size": len(manifest)},
            "SHA256SUMS": {"sha256": hashlib.sha256(sums).hexdigest(), "size": len(sums)}}


def existing_release(tag_commit, release, expected_sha, assets):
    """Plan safe resume/idempotence. Never move a tag or replace an asset."""
    expected_sha = sha(expected_sha)
    require(tag_commit is None or sha(tag_commit) == expected_sha, "Existing tag points elsewhere")
    if release is None:
        return {"create_tag": tag_commit is None, "create_draft": True, "upload": sorted(assets), "publish": True}
    require(tag_commit is not None and release.get("tag_name") == TAG and release.get("prerelease") is False and isinstance(release.get("draft"), bool),
            "Unexpected existing release/tag")
    found = set()
    for item in release.get("assets", []):
        name = item.get("name")
        require(name in assets and name not in found, "Unexpected/duplicate published asset")
        found.add(name)
        require(item.get("state") == "uploaded" and item.get("size") == assets[name]["size"] and
                digest(item.get("digest")) == assets[name]["sha256"], "Existing asset bytes differ: " + name)
    missing = sorted(set(assets) - found)
    require(not release.get("immutable") or not missing, "Immutable release is incomplete")
    return {"create_tag": False, "create_draft": False, "upload": missing, "publish": release.get("draft") is True}
