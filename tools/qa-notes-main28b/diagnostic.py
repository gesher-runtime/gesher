#!/usr/bin/env python3
"""Observe the unchanged main Notes scenario; do not qualify a product repair."""
from __future__ import annotations
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
SOURCE = "28b2617e35df63a1cb0fb7c9678b5d3f41b90a9a"
SOURCE_TREE = "39f576078bd16a37fb770a566f19dd504313a223"
RUN, ATTEMPT, PRODUCER, ARTIFACT = 44730332552, 1, 136274349647, 14627081129
REPO, REPO_ID = "gesher-runtime/gesher", "1426153186"
ARCHIVE_SHA = "da11302cfdb91a8f5b064cf3338df5e9b48648cb26b1f66e9c6346b35efc4e69"
ARCHIVE_SIZE = 105350319
INSTALLER_SHA = "8d365400eb3d3802399341db5b83942360f764da1ac9ca0de9ad583cd329107a"
APK_SHA = "ae8aa96794fc55d58916d8fe3a217912fbcc9edd5ec9fb72c547f90d97d3c0ea"
DIAGNOSTIC = {
    ".github/workflows/qa-notes-main28b.yml",
    "tools/qa-notes-main28b/run.ps1",
    "tools/qa-notes-main28b/diagnostic.py",
}
SCOPES = ["tests/e2e", "tests/windows/graphics", "tools/ci", "tools/surface",
          "app/java/com/gesher/app/webview", "compat/webview", ".gitattributes",
          "tools/qa-notes-main28b", ".github/workflows/qa-notes-main28b.yml"]
EXPECTED_INSTALLED = {
    "dalvikvm.exe": "fb98fde4f83cd0874541167c0f30fa86f46fd9ea2b97261e9b0bff72edf77f5e",
    "gesher-app.jar": "81640fcabb42149917572cae8b1e4c1ab668139b3c7bf7bfda40ae0dc9b72f52",
    "framework.jar": "9217f6743dabafe229c3d194ff8413695a77bc3b33934df784c0b45b7221b6e5",
    "build.prop": "060a7a8bc067a52ffd7599e3b961a2bad7b301f33c2d275d598dc9c1ca186045",
}

def sha(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()

def git(*args: str, raw=False):
    result = subprocess.check_output(["git", *args], cwd=ROOT)
    return result if raw else result.decode().strip()

def source_proof() -> dict:
    if git("rev-parse", SOURCE + "^{tree}") != SOURCE_TREE:
        raise ValueError("Original main source tree changed")
    subprocess.run(["git", "merge-base", "--is-ancestor", SOURCE, "HEAD"], cwd=ROOT, check=True)
    if set(git("diff", "--name-only", SOURCE, "HEAD").splitlines()) != DIAGNOSTIC:
        raise ValueError("Only the three isolated observation files may differ")
    if git("status", "--porcelain"):
        raise ValueError("Diagnostic checkout must be clean")
    head = git("rev-parse", "HEAD")
    if os.environ.get("GITHUB_ACTIONS") == "true" and head != os.environ["GITHUB_SHA"]:
        raise ValueError("Actual diagnostic checkout mismatch")
    paths = sorted(x for x in git("ls-files", "-z", "--", *SCOPES).split("\0") if x)
    if any("\\" in p or Path(p).is_absolute() or ".." in Path(p).parts for p in paths):
        raise ValueError("Noncanonical repository path")
    sources = {p: sha(ROOT / p) for p in paths}
    for p in paths:
        if p not in DIAGNOSTIC and hashlib.sha256(git("show", f"{SOURCE}:{p}", raw=True)).hexdigest() != sources[p]:
            raise ValueError("Unchanged source bytes differ: " + p)
    manifest = json.loads((ROOT / "tests/e2e/apps.json").read_text())
    notes = [a for a in manifest["apps"] if a["package"] == "org.billthefarmer.notes"]
    if len(notes) != 1 or notes[0]["sha256"] != APK_SHA or notes[0]["versionCode"] != 141 or "waitLog" in notes[0]:
        raise ValueError("The original Notes scenario changed")
    return {"source": SOURCE, "source_tree": SOURCE_TREE, "diagnostic_head": head,
            "source_bytes": sources, "notes": notes[0]}

def api(path: str):
    if os.environ.get("GITHUB_REPOSITORY") != REPO:
        raise ValueError("Diagnostic repository mismatch")
    sys.path.insert(0, str(ROOT / "tools/ci"))
    from github_read import get_json
    return get_json("repos/" + REPO + path)

def supplier() -> dict:
    sys.path.insert(0, str(ROOT / "tools/ci"))
    from github_jobs import get_jobs
    from artifact_candidates import read_artifacts, select_artifact
    run = api(f"/actions/runs/{RUN}")
    if (run["id"] != RUN or run["run_attempt"] != ATTEMPT or run["head_sha"] != SOURCE
            or run["head_branch"] != "main" or run["event"] != "push"
            or run["status"] != "completed" or run["repository"]["full_name"] != REPO
            or str(run["repository"]["id"]) != REPO_ID):
        raise ValueError("Exact historical main run changed")
    jobs = get_jobs(f"repos/{REPO}/actions/runs/{RUN}/attempts/{ATTEMPT}/jobs")
    selected = [j for j in jobs["jobs"] if j["id"] == PRODUCER or j["name"] == "package"]
    if len(selected) != 1:
        raise ValueError("Historical package producer missing or ambiguous")
    job = selected[0]
    if (job["id"] != PRODUCER or job["name"] != "package" or job["run_id"] != RUN
            or job["run_attempt"] != ATTEMPT or job["head_sha"] != SOURCE
            or job["status"] != "completed" or job["conclusion"] != "success"):
        raise ValueError("Exact historical package producer changed")
    inventory = read_artifacts(f"repos/{REPO}/actions/runs/{RUN}/artifacts", name="gesher-setup")
    artifact = select_artifact(inventory, "gesher-setup", job, run_id=str(RUN),
                              repo_id=REPO_ID, head_repo_id=REPO_ID, head=SOURCE)
    if (artifact["id"] != ARTIFACT or artifact["digest"] != "sha256:" + ARCHIVE_SHA
            or artifact["size_in_bytes"] != ARCHIVE_SIZE):
        raise ValueError("Exact historical installer artifact changed")
    return {"run": run, "producer": job, "artifact": artifact}

def input(out: Path) -> None:
    proof = source_proof()
    out.mkdir(parents=True, exist_ok=True)
    results = out / "results"
    results.mkdir()
    before = supplier()
    # Qualified downloader rejects redirects carrying credentials, invalid
    # member paths and nonmatching raw ZIP bytes before extraction.
    subprocess.run([sys.executable, str(ROOT / "tools/surface/download-verified-artifact.py"),
                    "--id", str(ARTIFACT), "--digest", "sha256:" + ARCHIVE_SHA,
                    "--destination", str(out / "installer")], cwd=ROOT, check=True)
    after = supplier()
    if before != after:
        raise ValueError("Historical source/producer/output changed during download")
    installers = list((out / "installer").glob("gesher-setup-*.exe"))
    if len(installers) != 1 or sha(installers[0]) != INSTALLER_SHA:
        raise ValueError("Installer bytes differ from the original failed main execution")
    receipt = {"diagnostic_run": os.environ["GITHUB_RUN_ID"],
               "diagnostic_attempt": os.environ["GITHUB_RUN_ATTEMPT"], "source": proof,
               "historical_supplier": after, "archive_sha256": ARCHIVE_SHA,
               "installer_sha256": INSTALLER_SHA,
               "purpose": "Exact unchanged main package; no product overlay or new-build claim"}
    (results / "source-and-supplier-receipt.json").write_text(json.dumps(receipt, indent=2) + "\n")
    print(f"NOTES_EXACT_INPUT source={SOURCE} run={RUN} attempt={ATTEMPT} producer={PRODUCER} artifact={ARTIFACT} digest=sha256:{ARCHIVE_SHA}")

def summarize(out: Path, mode: str, os_name: str) -> int:
    results = out / "results"
    receipt = json.loads((results / "source-and-supplier-receipt.json").read_text(encoding="utf-8-sig"))
    invocation = json.loads((results / "invocation.json").read_text(encoding="utf-8-sig"))
    proof = source_proof()
    faults = []
    if (receipt["source"] != proof or invocation["source"] != SOURCE
            or invocation["diagnostic_head"] != proof["diagnostic_head"]
            or invocation["diagnostic_run"] != os.environ["GITHUB_RUN_ID"]
            or invocation["diagnostic_attempt"] != os.environ["GITHUB_RUN_ATTEMPT"]
            or invocation["mode"] != mode or invocation["debug_enabled"] != (mode == "debug")
            or invocation["installer_sha256"] != INSTALLER_SHA):
        faults.append("Source, invocation or exact installed package identity mismatch")
    for name, digest in EXPECTED_INSTALLED.items():
        if invocation["installed_files"].get(name) != digest:
            faults.append("Original installed binary mismatch: " + name)
    if not invocation["installed_files"].get("libgesher_webview_bridge.so"):
        faults.append("Actual installed WebView bridge digest is missing")
    e2e = results / "e2e"
    # Existing coverage validator preserves exact invocation, APK identity,
    # selected/excluded cases and the original suite's failure/incomplete rules.
    validation = subprocess.run([sys.executable, str(ROOT / "tests/e2e/coverage.py"),
        "--manifest", str(ROOT / "tests/e2e/apps.json"), "--observations", str(e2e / "observations.json"),
        "--out-dir", str(e2e), "--expected-invocation-id", invocation["invocation_id"],
        "--require-apk-identity"], cwd=ROOT)
    if validation.returncode or invocation["original_exit"] != 0:
        faults.append("Original Notes scenario or unchanged evidence validator failed")
    paths = [e2e / "org.billthefarmer.notes.launch.log", e2e / "org.billthefarmer.notes.app.log"]
    text = "\n".join(p.read_text(encoding="utf-8-sig", errors="replace") for p in paths if p.exists())
    versions = sorted(set(re.findall(r"gesher-webview: WebView2 Runtime (\d+\.\d+\.\d+\.\d+)", text)))
    if len(versions) != 1:
        faults.append("Actual WebView2 runtime version is missing or ambiguous")
    if os_name == "windows-2025" and (len(versions) != 1 or not versions[0].startswith("157.")):
        faults.append("The required actual WebView2 157 family was not observed")
    lines = [line for line in text.splitlines() if "gesher-webview:" in line]
    # This is existing application debug output only. Do not fabricate native
    # navigation HRESULTs/ids or phase timestamps that this binary does not log.
    phases = {
        "ready": bool(re.search(r"gesher-webview: WebView2 .* ready", text)),
        "bounds": "gesher-webview: bounds:" in text,
        "first_page_finished": "gesher-webview: first page loaded: file:///" in text,
        "preview": "gesher-webview: preview of " in text,
        "native_failure_line": any(line.startswith("gesher-webview: WebView2: ") for line in lines),
    }
    artifacts = {p.relative_to(results).as_posix(): sha(p)
                 for p in sorted(results.rglob("*")) if p.is_file()}
    report = {"source": SOURCE, "diagnostic_head": proof["diagnostic_head"],
              "run": os.environ["GITHUB_RUN_ID"], "attempt": os.environ["GITHUB_RUN_ATTEMPT"],
              "os": os_name, "mode": mode, "webview_versions": versions,
              "original_exit": invocation["original_exit"], "observed_phases": phases,
              "existing_webview_lines": lines, "faults": faults, "artifacts": artifacts,
              "limits": ["No production change or repair approval",
                         "No added waits, relaxed assertions or fabricated page success",
                         "Native queue/Navigation HRESULT phases remain unknown when not logged",
                         "Debug mode includes the existing preview request and is separately identified"]}
    (results / "diagnostic-summary.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report, indent=2))
    return int(bool(faults))

def main() -> int:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("operation", choices=("source-proof", "input", "summarize"))
    p.add_argument("--out", type=Path)
    p.add_argument("--mode", choices=("original", "debug"))
    p.add_argument("--os", dest="os_name", choices=("windows-2022", "windows-2025"))
    args = p.parse_args()
    if args.operation == "source-proof":
        print(json.dumps(source_proof(), indent=2)); return 0
    if args.out is None:
        p.error("--out is required")
    if args.operation == "input":
        input(args.out.resolve()); return 0
    if not args.mode or not args.os_name:
        p.error("--mode and --os are required")
    return summarize(args.out.resolve(), args.mode, args.os_name)

if __name__ == "__main__":
    raise SystemExit(main())
