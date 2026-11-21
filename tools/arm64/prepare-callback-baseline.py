#!/usr/bin/env python3
# Copyright (C) 2026 The Gesher Authors
# SPDX-License-Identifier: Apache-2.0
"""Seal genuine PR120 runtime artifacts with the unchanged callback guest fixture."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import urllib.error
import urllib.parse
import urllib.request
import zipfile

ROOT = Path(__file__).resolve().parents[2]
REPO = "gesher-runtime/gesher"
REPO_ID = 1426153186
RUN = 44162241853
HEAD = "9c226d06c2f4f176d29a58ac51803be3df49fd6f"
BASE = "e80cd1854163bd18e4ea5392c25f423184048804"
MERGE = "097fc3e5d936147162cf0379150292fd1e26d274"
TREE = "fd04f3b0e111f8e9e403a57b6e547a193c883c71"
FIXTURE_COMMIT = "3620f93dc27cb4b6660d8ffabeec972498d20c2b"
FIXTURE = "tests/native/callback_longjmp_contract.c"
FIXTURE_SHA = "0ed72ca245058757984f8ee72cec975a87949a4a88717a8c8c04c23115fade33"
NDK_SHA = "59c2f6dc96743b5daf5d1626684640b20a6bd2b1d85b13156b90333741bad5cc"
NDK_SIZE = 663987688
ARTIFACTS = {
    "windows-x64": {"id": 14372697124, "bytes": 104612855, "job": 134356044139,
                    "job_name": "build",
                    "sha256": "5e1b3c3e1af5054d79bd6ebedce82060c29dc35c25fcc5e9d818cde1e7427818",
                    "files": {"arm64_elf_test.exe":
                              "c728d876d9eb76047456d01298d824d78513ee0d67df37e995e24129c987b94a",
                              "web3-production-role.json":
                              "39b12e534093a3549b5180bb45d30d1656734ced5087b297d3c66497388b4ffa"}},
    "arm64": {"id": 14367839302, "bytes": 3451578, "job": 134355264246,
              "job_name": "arm64 / build",
              "sha256": "ec81eff345532cf0f729da6a08b73a5a676c7164509c9d4933b0c5bb7b9a15d5",
              "files": {"gesher_arm64.dll":
                        "8be9a12bb0193598d4009b77fefc7ab70b231f25a2d64c113f61e758ace48a7b",
                        "trivial_arm64.so":
                        "add74a49fd15562c267d1821861e6b346329bea83203d5c93a351dfe9541c4ec",
                        "DYNARMIC-NOTICES.txt":
                        "14ef8e5ce8e564bbbe28bc7a306de80c786124de34955b2b4e868cb64bbe9faa"}},
}
SOURCES = [FIXTURE, "tools/arm64/prepare-callback-baseline.py",
           "tools/arm64/run-callback-baseline.ps1",
           ".github/workflows/native-callback-cleanup.yml",
           "docs/research/native-callback-cleanup-2026-11-17.md"]


def require(ok, message):
    if not ok:
        raise RuntimeError(message)


def sha(path):
    with Path(path).open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def git(*args):
    return subprocess.check_output(["git", *args], cwd=ROOT, text=True).strip()


def request(path):
    headers = {"Accept": "application/vnd.github+json", "User-Agent": "gesher-callback-baseline",
               "X-GitHub-Api-Version": "2022-11-28"}
    if os.environ.get("GH_TOKEN"):
        headers["Authorization"] = "Bearer " + os.environ["GH_TOKEN"]
    return urllib.request.Request(f"https://api.github.com/repos/{REPO}/{path}", headers=headers)


def fetch_json(path):
    with urllib.request.urlopen(request(path), timeout=30) as response:
        return json.load(response)


class NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        return None


def download_artifact(artifact, target):
    # Do not forward the GitHub token to the signed artifact-storage redirect.
    opener = urllib.request.build_opener(NoRedirect())
    try:
        response = opener.open(request(f"actions/artifacts/{artifact}/zip"), timeout=30)
    except urllib.error.HTTPError as error:
        require(error.code in (301, 302, 303, 307, 308), "artifact download did not redirect")
        location = error.headers["Location"]
        require(urllib.parse.urlsplit(location).scheme == "https", "non-HTTPS artifact redirect")
        response = urllib.request.urlopen(location, timeout=60)
    with response, target.open("wb") as stream:
        shutil.copyfileobj(response, stream, 1024 * 1024)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--ndk-archive", type=Path, required=True)
    parser.add_argument("--work", type=Path, required=True, help="empty disposable NDK/download directory")
    parser.add_argument("--out", type=Path, required=True, help="empty sealed bundle directory")
    parser.add_argument("--metadata", type=Path, help="local copies of the four raw API JSON responses")
    parser.add_argument("--artifacts", type=Path, help="local digest-pinned artifact ZIPs")
    args = parser.parse_args()
    work, out = args.work.resolve(), args.out.resolve()
    require(work != out and work not in out.parents and out not in work.parents,
            "work and bundle directories must be separate")
    for path in (work, out):
        require(not path.exists() or not any(path.iterdir()), f"use an empty directory: {path}")
        path.mkdir(parents=True, exist_ok=True)
    paths = {"run": f"actions/runs/{RUN}",
             "jobs": f"actions/runs/{RUN}/jobs?per_page=100&page=1",
             "artifacts": f"actions/runs/{RUN}/artifacts?per_page=100&page=1",
             "merge": f"git/commits/{MERGE}"}
    metadata = {}
    for name, path in paths.items():
        data = json.loads((args.metadata / f"{name}.json").read_text()) if args.metadata else fetch_json(path)
        metadata[name] = data
        (out / f"baseline-{name}.json").write_text(json.dumps(data, indent=2) + "\n")
    run, merge = metadata["run"], metadata["merge"]
    require(run["id"] == RUN and run["run_attempt"] == 1 and run["head_sha"] == HEAD and
            run["event"] == "pull_request" and run["path"] == ".github/workflows/ci.yml" and
            run["repository"]["id"] == REPO_ID and run["repository"]["full_name"] == REPO and
            run["head_repository"]["id"] == REPO_ID, "historical workflow identity mismatch")
    require(any(pr["number"] == 120 and pr["head"]["sha"] == HEAD and
                pr["base"]["sha"] == BASE for pr in run["pull_requests"]), "historical PR source mismatch")
    require(merge["sha"] == MERGE and merge["tree"]["sha"] == TREE and
            [parent["sha"] for parent in merge["parents"]] == [BASE, HEAD],
            "historical merge commit/tree/parents mismatch")
    require(metadata["jobs"]["total_count"] <= 100 and metadata["artifacts"]["total_count"] <= 100,
            "historical metadata pagination changed; review before proceeding")
    for name, expected in ARTIFACTS.items():
        found = [a for a in metadata["artifacts"]["artifacts"] if a["id"] == expected["id"]]
        jobs = [j for j in metadata["jobs"]["jobs"] if j["id"] == expected["job"]]
        require(len(found) == 1 and len(jobs) == 1, f"missing exact {name} producer or artifact")
        artifact, job = found[0], jobs[0]
        require(artifact["name"] == name and artifact["size_in_bytes"] == expected["bytes"] and
                artifact["digest"] == "sha256:" + expected["sha256"] and not artifact["expired"] and
                artifact["workflow_run"]["id"] == RUN and artifact["workflow_run"]["head_sha"] == HEAD and
                artifact["workflow_run"]["repository_id"] == REPO_ID and
                artifact["workflow_run"]["head_repository_id"] == REPO_ID,
                f"{name} artifact provenance mismatch")
        require(job["name"] == expected["job_name"] and job["run_id"] == RUN and
                job["head_sha"] == HEAD and job["status"] == "completed" and
                job["conclusion"] == "success" and
                job["started_at"] <= artifact["created_at"] <= job["completed_at"],
                f"{name} exact producer did not succeed")
        archive = (args.artifacts / f"{name}-{RUN}.zip") if args.artifacts else (work / f"{name}.zip")
        if not args.artifacts:
            download_artifact(expected["id"], archive)
        require(archive.stat().st_size == expected["bytes"] and sha(archive) == expected["sha256"],
                f"{name} ZIP size/digest mismatch")
        with zipfile.ZipFile(archive) as bundle:
            for member, expected_hash in expected["files"].items():
                require(bundle.namelist().count(member) == 1, f"missing/duplicate artifact member: {member}")
                data = bundle.read(member)
                require(hashlib.sha256(data).hexdigest() == expected_hash, f"member digest mismatch: {member}")
                (out / member).write_bytes(data)
    role = json.loads((out / "web3-production-role.json").read_text())
    require(role["source"] == MERGE and role["supplier_head"] == HEAD and role["repository"] == REPO and
            str(role["repository_id"]) == str(REPO_ID) and str(role["run"]) == str(RUN) and
            str(role["attempt"]) == "1" and role["event"] == "pull_request" and
            role["role"] == "production" and role["test_hook_compiled"] is False,
            "producer embedded source receipt differs from the API merge identity")

    require(sha(ROOT / FIXTURE) == FIXTURE_SHA, "committed callback fixture was changed")
    require(args.ndk_archive.stat().st_size == NDK_SIZE and sha(args.ndk_archive) == NDK_SHA,
            "official NDK archive size/digest mismatch")
    subprocess.run(["unzip", "-q", str(args.ndk_archive.resolve()), "-d", str(work)], check=True, timeout=180)
    ndk = work / "android-ndk-r27c"
    properties = (ndk / "source.properties").read_text()
    require("Pkg.Revision = 27.2.12479018" in properties, "wrong NDK revision")
    compiler = ndk / "toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android29-clang"
    guest = out / "callback_longjmp_arm64.so"
    command = [str(compiler), "-shared", "-fPIC", "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
               "-fno-builtin", "-fstack-protector-strong", "-Wl,-soname,libcallback_longjmp_arm64.so",
               str(ROOT / FIXTURE), "-o", str(guest)]
    result = subprocess.run(command, cwd=ROOT, text=True, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, timeout=60)
    (out / "compile-guest.txt").write_text(result.stdout)
    require(result.returncode == 0, "callback ELF compilation failed")
    version = subprocess.check_output([str(compiler), "--version"], text=True)
    (out / "ndk-compiler-version.txt").write_text(version)
    readelf = compiler.parent / "llvm-readelf"
    elf = subprocess.check_output([str(readelf), "-h", "-d", "--dyn-syms", str(guest)], text=True)
    require("AArch64" in elf and "gesher_selftest" in elf, "not the actual ARM64 guest fixture")
    (out / "guest-elf.txt").write_text(elf)
    shutil.copyfile(ROOT / FIXTURE, out / "callback_longjmp_contract.c")
    commit, tree = git("rev-parse", "HEAD"), git("rev-parse", "HEAD^{tree}")
    dirty = bool(git("status", "--porcelain"))
    ci = {key: os.environ.get(key) for key in (
        "GITHUB_SHA", "GITHUB_RUN_ID", "GITHUB_RUN_ATTEMPT", "GITHUB_REPOSITORY",
        "GITHUB_REPOSITORY_ID", "GITHUB_WORKFLOW", "GITHUB_JOB")}
    if os.environ.get("GITHUB_ACTIONS") == "true":
        require(ci["GITHUB_SHA"] == commit and not dirty and not args.metadata and not args.artifacts,
                "CI requires clean exact source and fresh GitHub metadata/downloads")
    receipt = {
        "schema": "gesher-callback-pr120-baseline-build-v1",
        "source": {"commit": commit, "tree": tree, "dirty": dirty,
                   "files": {name: sha(ROOT / name) for name in SOURCES}},
        "ci": ci,
        "historical": {"repository": REPO, "repository_id": REPO_ID, "run": RUN, "attempt": 1,
                       "head": HEAD, "merge": MERGE, "tree": TREE, "artifacts": ARTIFACTS,
                       "embedded_role_receipt_scope": "producer source corroboration; not a loader-specific receipt"},
        "fixture": {"original_commit": FIXTURE_COMMIT, "source_sha256": FIXTURE_SHA,
                    "compiler_version": version, "compiler_sha256": sha(compiler),
                    "ndk_archive_sha256": NDK_SHA, "ndk_properties": properties,
                    "command": command, "exit_code": result.returncode},
        "outputs": {path.name: sha(path) for path in sorted(out.iterdir()) if path.is_file()},
        "runtime": "not executed; historical Gesher characterization pending",
    }
    (out / "baseline-build-receipt.json").write_text(json.dumps(receipt, indent=2) + "\n")
    print(f"Prepared genuine PR120 runtime and exact 80-case guest fixture: {out}")


if __name__ == "__main__":
    main()
