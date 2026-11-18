# Label-approved native auto-merge

Apply `automerge` after reviewing the current PR head. GitHub performs a native
SQUASH auto-merge after the required PR CI and exact-head QA gate succeed.
There is no merge queue and no second full merge-group CI run. The controller
never calls the merge API, starts another CI build, or uses scheduled polling.

## Label authorization and QA

A label must be strictly later than PR creation and applied by a collaborator
with write/admin access. Creation-time labels, ambiguous events, changed heads,
force pushes, reopening and retargeting cannot reuse an earlier approval.
Pushes remove stale approval labels automatically. Review the updated diff and
apply the label again to approve the new head. Delayed push deliveries preserve
a later label application. A pending label in the same/adjacent timestamp second
is conservatively retained when GitHub supplies no proof that it predates the
push; it cannot authorize a new head without a matching approval receipt.

The existing author routes are unchanged: an author may deliberately reapply an
earlier label, or precede their first label with an unedited exact-head COMMENT
review. A different authorized collaborator may apply their first label. The bot
records a native approval binding the repository, PR, head, base and label event.
This receipt records authorization; it is not a code-review or scope verdict.

An author first-label review states the reviewed commit, scope verdict, contract
evidence, actionable findings, tests and validation limits required by
CONTRIBUTING.md. Include exactly one standalone compact marker with actual values:

```text
<!-- gesher-automerge-qa-v1 {"repository":1426153186,"number":115,"head":"FULL_40_CHARACTER_HEAD_SHA","base":"main"} -->
```

Edited, duplicate or mismatched proof fails closed. Legacy unedited receipts and
opening boundaries retain their meaning. Maintainer approval cannot waive the
Android-visible contract requirements in CONTRIBUTING.md.

## One PR CI run and final QA

The label records approval and enables native SQUASH waiting pinned to that head.
`qa/label-approval` remains pending while required CI runs. A small metadata-only
workflow, triggered when ci or arm64-windows completes, verifies the registered
workflow IDs/paths, pull_request event, exact head/branch, target/source repository
IDs and successful aggregate check suites from GitHub Actions. Display-name
collisions, manual runs, another repository or another check suite cannot supply
that evidence. It then rechecks current label, permission, receipt and QA proof
before turning the required gate green. GitHub applies all branch protections.

When either registered PR workflow finishes with a failure, timeout or startup
failure, the controller removes `automerge`, cancels native auto-merge and posts
one short bot comment linking to the failed run. It reads the current run attempt
before acting, so a delayed failure event cannot revoke a newer pending or
successful rerun. Newer heads and label applications are preserved. Pending or
cancelled runs cannot release the QA gate. After fixing or rerunning failed CI,
review the result and reapply the label; success alone does not restore approval.
`action_required` (including fork CI awaiting maintainer approval) preserves the
label and keeps the QA gate pending. Approve and run both required PR workflows;
their completion events recheck the existing exact-head approval automatically.

`ci-required` covers every root production CI job, including Windows shards,
packaging and e2e; failed, cancelled or skipped root jobs fail it. `arm64-required`
covers source identity and the ARM64 build. The deployed configuration is
`.github/automerge-ruleset.json`: PR review and resolved discussions are required,
all three contexts are bound to GitHub Actions, force pushes/deletion are blocked,
and there are no permanent bypass actors. Branch updates may need fresh CI;
there is no automatic second full CI run simply for merging.

## Fork pull requests

Fork PRs follow the same label, QA and CI requirements. A maintainer applies the
label in the target repository. The source fork's repository ID stays distinct
from the target's ID throughout CI validation; a run from a different fork cannot
supply approval evidence.

The write-capable approval workflow checks out only the default branch's scripts,
with credentials persistence disabled. It never checks out or executes fork code,
review text or artifacts. Fork PR CI retains GitHub's normal read-only token and
workflow approval policy. Fork pull_request_review events have read-only tokens
and perform no privileged writes: the target repository's workflow_run event
rechecks their proof after CI, while the required QA gate remains pending.

A push to a new head needs fresh label authorization and CI. GitHub may also
disable native waiting when a fork contributor without target write access
pushes. A new label restores waiting only after the new head is reviewed.

## Operations and validation

Enable Allow auto-merge and Allow GitHub Actions to create and approve pull
requests. Use workflow_dispatch to recheck existing receipts; the optional
pull_request_number input targets one PR. It never manufactures approval from a
label snapshot. Old PR heads without aggregate checks need updated CI.

### Pull requests that modify workflows

GitHub's built-in `GITHUB_TOKEN` cannot enable auto-merge for a PR changing
`.github/workflows/*`: it lacks the Workflows permission, which cannot be added
to the workflow's `permissions` block. Configure the repository Actions secret
`AUTOMERGE_WORKFLOWS_TOKEN` with a fine-grained PAT restricted to this repository,
with Contents, Pull requests and Workflows read/write permissions. Use an account
with repository write access; configure expiration and rotate before expiry.
Do not paste the credential into source, issues or logs.

The controller uses this credential only to enable/disable native auto-merge.
Reviews, authorization reads, CI evidence and `qa/label-approval` still use
`GITHUB_TOKEN`, preserving the ruleset's GitHub Actions app identity and the
bot receipt format. It does not bypass protections or approve workflow runs.
Without the secret, ordinary PRs retain the built-in token behavior; workflow
PRs require this setup or native auto-merge enabled by an authorized maintainer.
After setup/deployment, dispatch `automerge` for the existing PR number to recheck
its current approval without rerunning CI or reapplying an intact label.

```text
node --test .github/scripts/label-automerge.test.js
python .github/scripts/test_automerge_workflows.py
```

Fixtures stub GitHub mutations. Live API reads validate the production CI reader
against published runs separately; local fixtures do not prove native merging,
fresh Windows/ART tests or a successful live fork merge.

GitHub references:
- https://docs.github.com/en/pull-requests/how-tos/merge-and-close-pull-requests/automatically-merging-a-pull-request
- https://docs.github.com/en/actions/reference/security/securely-using-pull_request_target
