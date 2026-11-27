"""Select PR CI jobs and verify the resulting required check."""

import argparse
from dataclasses import dataclass
import json
import os
from pathlib import Path
import re
import subprocess
import sys

import yaml

from preflight_plan import GROUPS as PREFLIGHT_GROUPS, SCRIPT_GROUPS


ROOT = Path(__file__).resolve().parents[2]
GATES = {"ci": "ci-required", "arm64-windows": "arm64-required"}
ALWAYS = {
    "ci": {"plan", "source-identity", "automerge-script"},
    "arm64-windows": {"plan", "source-identity"},
}

# build starts in parallel and waits for this same-run artifact after compiling.
ARTIFACT_DEPENDENCIES = {"ci": {"build": {"build-framework-assets"}}}

# Exact audited paths. Keep policies, research data and other unclassified
# documentation on the full path.
DOCS = {
    "docs/automerge-runner.md",
    "docs/installer.md",
    "docs/lifecycle.md",
    "docs/logging.md",
    "docs/network.md",
    "docs/self-hosted-ci.md",
    "docs/webview.md",
}

# Only list a path after finding every consumer in the required workflows.
# These paths do not affect the separate native-only ARM64 Windows build.
SCOPED = {
    # Exercise the Java policy directly and through the packaged application and runtime.
    "app/java/com/gesher/app/webview/WebViewDarkening.java": {
        "webview", "artifact-preflight", "e2e", "test", "web3-query-regression",
    },
    "tests/metered-network/cost_policy_test.cc": {"network-cost-client", "windows-host-contracts"},
    "tests/metered-network/cost_events_windows_test.cc": {"network-cost-client"},
    "tests/metered-network/cost_events_server_test.cc": {"windows-host-contracts"},
    "tests/webview-darkening/WebViewDarkeningTest.java": {"webview"},
    "tools/test_dns_admission.py": {"artifact-preflight", "windows-host-contracts"},
    "tools/test_notification_images.py": {"artifact-preflight"},
    "tests/e2e/test_coverage.py": {"e2e"},
    "tests/e2e/test_evidence_process.py": {"e2e"},
    "tests/e2e/render-readiness.test.ps1": {"e2e"},
}
SHARDS = ("native", "desktop", "windowing", "services", "contracts", "media", "compilation")
WINDOWS = ("windows-2022", "windows-2025")


def track_shards():
    """Use the same manifest as the actual Windows track loader."""
    manifest = json.loads((ROOT / "tests/windows/test-shards.json").read_text(encoding="utf-8"))
    if set(manifest) != set(SHARDS):
        raise ValueError("unknown or missing Windows shard")
    consumers = {}
    for shard, group in manifest.items():
        for track in [*group["tracks"], *group["helpers"], "30-graphics.ps1", "32-fonts.ps1"]:
            consumers.setdefault("tests/windows/tracks/" + track, set()).add(shard)
    return consumers


SHA = re.compile(r"[0-9a-f]{40}")
REGULAR_FILE = re.compile(rb":(?:000000|100644) (?:000000|100644) [0-9a-f]+ [0-9a-f]+ [AMD]")


@dataclass
class Workflow:
    always: set
    needs: dict

    @property
    def work(self):
        return self.needs.keys() - self.always

    def closure(self, jobs):
        required = set(jobs)
        pending = list(jobs)
        while pending:
            for dependency in self.needs[pending.pop()]:
                if dependency not in required:
                    required.add(dependency)
                    pending.append(dependency)
        return required


def load_workflow(name):
    source = ROOT / ".github" / "workflows" / f"{name}.yml"
    jobs = yaml.safe_load(source.read_text(encoding="utf-8"))["jobs"]
    gate = jobs.pop(GATES[name])
    if set(gate["needs"]) != jobs.keys() or not ALWAYS[name] <= jobs.keys():
        raise ValueError("required check does not cover exactly the workflow jobs")
    artifact_dependencies = ARTIFACT_DEPENDENCIES.get(name, {})
    if artifact_dependencies.keys() - jobs.keys():
        raise ValueError("an artifact consumer is missing from the workflow")
    needs = {}
    for job, definition in jobs.items():
        parents = definition.get("needs", [])
        if isinstance(parents, str):
            parents = [parents]
        needs[job] = set(parents) | artifact_dependencies.get(job, set())
        if job in needs[job] or needs[job] - jobs.keys():
            raise ValueError(f"{job} has an invalid dependency")
    workflow = Workflow(ALWAYS[name], needs)
    if workflow.closure(workflow.always) - workflow.always:
        raise ValueError("an unconditional job depends on conditional work")
    if name == "ci" and any(wanted - workflow.work for wanted in SCOPED.values()):
        raise ValueError("a scoped path selects an unknown or unconditional job")
    return workflow


def git(*args):
    return subprocess.run(
        ["git", *args], cwd=ROOT, check=True, capture_output=True,
    ).stdout


def diff_paths(start, end):
    # Disable rename collapsing so both names participate in the selection.
    diff = git("diff", "--raw", "-z", "--no-renames", "--ignore-submodules=none",
               "--no-ext-diff", start, end, "--")
    if not diff:
        return set()
    fields = diff.split(b"\0")
    if fields.pop() or len(fields) % 2:
        raise ValueError("unexpected raw git diff format")
    paths = set()
    for header, path in zip(fields[::2], fields[1::2]):
        if not REGULAR_FILE.fullmatch(header):
            raise ValueError("the PR changes a file type or mode outside the selective policy")
        paths.add(path.decode("utf-8"))
    return paths


def changed_paths(event, checkout_sha):
    pr = event["pull_request"]
    base = pr["base"]["sha"]
    head = pr["head"]["sha"]
    if not all(SHA.fullmatch(sha) for sha in (base, head, checkout_sha)):
        raise ValueError("missing or invalid Git commit identity")
    if git("rev-parse", "--is-shallow-repository").strip() != b"false":
        raise ValueError("the checkout has incomplete history")
    parents = git("rev-list", "--parents", "-n", "1", "HEAD").decode("ascii").split()
    if parents != [checkout_sha, base, head]:
        raise ValueError("checked-out merge commit does not match the PR event")
    ancestors = git("merge-base", "--all", base, head).decode("ascii").split()
    if len(ancestors) != 1 or not SHA.fullmatch(ancestors[0]):
        raise ValueError("the PR has no unique merge base")
    return diff_paths(ancestors[0], head) | diff_paths(base, checkout_sha)


def select_jobs(workflow, name, paths):
    tracks = track_shards()
    known = DOCS | SCOPED.keys() | tracks.keys() | SCRIPT_GROUPS.keys()
    if not paths or paths - known:
        return workflow.work, "full CI for an empty or unmapped change"
    component_paths = paths - DOCS
    if not component_paths:
        return set(), "allowlisted documentation only"
    wanted = set()
    if name == "ci":
        for path in component_paths:
            if path in tracks:
                # Tracks are also consumed by supplier source guards and package probes.
                # Retain those jobs; only narrow the separately inventoried test matrix.
                wanted.update(workflow.work)
            elif path in SCRIPT_GROUPS:
                wanted.add("artifact-preflight")
            wanted.update(SCOPED.get(path, set()))
    return workflow.closure(wanted) & workflow.work, "scoped component checks"


def matrix_selection(jobs, name, paths, full=False):
    if name != "ci":
        return [], []
    shards = set()
    groups = set()
    tracks = {} if full else track_shards()
    for path in paths - DOCS:
        if path in tracks:
            shards.update(tracks[path])
            # Source-contract checks and track-manifest checks consume these scripts.
            groups.update(PREFLIGHT_GROUPS)
        elif path in SCRIPT_GROUPS:
            groups.update(SCRIPT_GROUPS[path])
        elif path in SCOPED:
            shards.update(SHARDS)
            groups.update(PREFLIGHT_GROUPS)
    if full:
        shards.update(SHARDS)
        groups.update(PREFLIGHT_GROUPS)
    return (sorted(shards) if "test-shards" in jobs else [],
            sorted(groups) if "artifact-preflight" in jobs else [])


def plan_details(workflow, name):
    if os.environ.get("GITHUB_EVENT_NAME") != "pull_request":
        jobs, reason, paths, full = workflow.work, "full CI for a non-PR event", set(), True
    else:
        try:
            event = json.loads(Path(os.environ["GITHUB_EVENT_PATH"]).read_text(encoding="utf-8"))
            paths = changed_paths(event, os.environ["GITHUB_SHA"])
            jobs, reason = select_jobs(workflow, name, paths)
            full = reason.startswith("full CI")
        except (OSError, KeyError, ValueError, TypeError, subprocess.CalledProcessError):
            jobs, reason, paths, full = workflow.work, "full CI because the PR diff could not be verified", set(), True
    shards, groups = matrix_selection(jobs, name, paths, full)
    return jobs, reason, shards, groups


def plan(workflow, name):
    jobs, reason, _, _ = plan_details(workflow, name)
    return jobs, reason


def verify(workflow, selected, results):
    if (not isinstance(selected, list)
            or any(not isinstance(job, str) for job in selected)
            or len(set(selected)) != len(selected)
            or set(selected) - workflow.work):
        raise ValueError("invalid selected job list")
    required = set(selected) | workflow.always
    if workflow.closure(required) != required:
        raise ValueError("selected jobs omit a dependency")
    if not isinstance(results, dict) or results.keys() != workflow.needs.keys():
        raise ValueError("job results do not cover exactly the workflow jobs")
    mismatches = []
    for job, details in results.items():
        actual = details.get("result") if isinstance(details, dict) else None
        expected = "success" if job in required else "skipped"
        if actual != expected:
            mismatches.append(f"{job} reported {actual!r}, expected {expected}")
    if mismatches:
        raise ValueError("\n".join(mismatches))


def verify_matrix_jobs(jobs, shards, groups, context=None):
    """Require every selected matrix cell, including retained rerun successes."""
    expected = {f"test-{shard} ({os_name})" for shard in shards for os_name in WINDOWS}
    expected.update(f"artifact-preflight ({group})" for group in groups)
    observed = set()
    for job in jobs:
        name = job["name"]
        matrix_cell = re.fullmatch(r"(?:test-[^()\r\n]+|artifact-preflight) \([^()\r\n]+\)", name)
        if not matrix_cell:
            continue
        if context is not None and (job.get("run_id") != context["run_id"]
                or job.get("head_sha") != context["head_sha"]
                or type(job.get("run_attempt")) is not int
                or not 1 <= job["run_attempt"] <= context["attempt"]):
            raise ValueError("matrix cell source/run/attempt mismatch: " + name)
        if name in observed:
            raise ValueError("duplicate matrix cell: " + name)
        observed.add(name)
        if name in expected:
            if job["status"] != "completed" or job["conclusion"] != "success":
                raise ValueError("selected matrix cell did not succeed: " + name)
        elif job["conclusion"] != "skipped":
            raise ValueError("unselected matrix cell ran: " + name)
    if expected - observed:
        raise ValueError("missing selected matrix cells: " + ", ".join(sorted(expected - observed)))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", choices=["plan", "verify"])
    parser.add_argument("--workflow", required=True, choices=GATES)
    args = parser.parse_args()
    try:
        workflow = load_workflow(args.workflow)
        jobs, reason, shards, groups = plan_details(workflow, args.workflow)
        values = {"selected": sorted(jobs), "shards": shards, "preflight_groups": groups}
        if args.command == "plan":
            with Path(os.environ["GITHUB_OUTPUT"]).open("a", encoding="utf-8") as outputs:
                for key, value in values.items():
                    print(f"{key}={json.dumps(value)}", file=outputs)
            message = f"{reason}. Selected jobs: {json.dumps(values['selected'])}"
            print(message)
            print(f"Windows shards: {json.dumps(shards)}; preflight groups: {json.dumps(groups)}")
            if summary := os.environ.get("GITHUB_STEP_SUMMARY"):
                with Path(summary).open("a", encoding="utf-8") as output:
                    print(message, file=output)
                    print(f"Windows shards: {shards}; preflight groups: {groups}", file=output)
        else:
            for key, expected in values.items():
                actual = json.loads(os.environ[key.upper()])
                if actual != expected:
                    raise ValueError("selection differs from the independently recomputed plan: " + key)
            verify(workflow, values["selected"], json.loads(os.environ["JOB_RESULTS"]))
            if args.workflow == "ci":
                from github_jobs import get_jobs
                repo = os.environ["GITHUB_REPOSITORY"]
                run = os.environ["GITHUB_RUN_ID"]
                attempt = os.environ["GITHUB_RUN_ATTEMPT"]
                # The latest per-job inventory retains successful cells when only failed
                # jobs rerun. Every cell must still belong to this run and source.
                inventory = get_jobs(f"repos/{repo}/actions/runs/{run}/jobs")
                source = os.environ["GITHUB_SHA"]
                if os.environ.get("GITHUB_EVENT_NAME") == "pull_request":
                    event = json.loads(Path(os.environ["GITHUB_EVENT_PATH"]).read_text(encoding="utf-8"))
                    source = event["pull_request"]["head"]["sha"]
                context = {"run_id": int(run), "attempt": int(attempt), "head_sha": source}
                verify_matrix_jobs(inventory["jobs"], shards, groups, context)
    except (KeyError, OSError, TypeError, ValueError, yaml.YAMLError) as error:
        print(f"CI {args.command} failed: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
