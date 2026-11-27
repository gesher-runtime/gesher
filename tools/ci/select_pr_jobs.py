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
    if not paths or paths - DOCS - SCOPED.keys():
        return workflow.work, "full CI for an empty or unmapped change"
    component_paths = paths - DOCS
    if not component_paths:
        return set(), "allowlisted documentation only"
    wanted = set()
    if name == "ci":
        for path in component_paths:
            wanted.update(SCOPED[path])
    return workflow.closure(wanted) & workflow.work, "scoped component checks"


def plan(workflow, name):
    if os.environ.get("GITHUB_EVENT_NAME") != "pull_request":
        return workflow.work, "full CI for a non-PR event"
    try:
        event = json.loads(Path(os.environ["GITHUB_EVENT_PATH"]).read_text(encoding="utf-8"))
        paths = changed_paths(event, os.environ["GITHUB_SHA"])
    except (OSError, KeyError, ValueError, TypeError, subprocess.CalledProcessError):
        return workflow.work, "full CI because the PR diff could not be verified"
    return select_jobs(workflow, name, paths)


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


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", choices=["plan", "verify"])
    parser.add_argument("--workflow", required=True, choices=GATES)
    args = parser.parse_args()
    try:
        workflow = load_workflow(args.workflow)
        if args.command == "plan":
            jobs, reason = plan(workflow, args.workflow)
            selected = json.dumps(sorted(jobs))
            with Path(os.environ["GITHUB_OUTPUT"]).open("a", encoding="utf-8") as outputs:
                print(f"selected={selected}", file=outputs)
            print(f"{reason}. Selected jobs: {selected}")
            if summary := os.environ.get("GITHUB_STEP_SUMMARY"):
                with Path(summary).open("a", encoding="utf-8") as output:
                    print(f"{reason.capitalize()}.", file=output)
                    print(f"\nSelected jobs: `{selected}`", file=output)
        else:
            verify(workflow, json.loads(os.environ["SELECTED"]), json.loads(os.environ["JOB_RESULTS"]))
    except (KeyError, OSError, TypeError, ValueError, yaml.YAMLError) as error:
        print(f"CI {args.command} failed: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
