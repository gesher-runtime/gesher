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

## API quota

Every controller event rereads labels, reviews, permission and CI evidence. These
reads use a separate read-only Gesher App token (`actions`, `checks`, `metadata`
and `pull-requests` read; PR issue events and comments need no `issues` access), so they count against the App installation's
quota rather than the repository's shared `GITHUB_TOKEN` quota. Exhausting that
shared quota previously left `qa/label-approval` pending after CI succeeded.
The required `qa/label-approval` status and its readback still use
`github-actions[bot]`, because the ruleset binds that context to GitHub Actions.
If the installation lacks one of those read permissions, token creation fails
without stopping the job, a notice is logged and reads fall back to `GITHUB_TOKEN`.

Same-repository `workflow_run` events reconcile the PRs GitHub names in the event
instead of listing every open PR; fork runs still list open PRs. Native
auto-merge cancellation reads its state from the REST pull request it already
fetched. Registered workflow identities are read once per job. Title or body
edits do not start the job; base changes still do.

If the gate is stuck pending after CI because a controller run failed, rerun it:
`gh workflow run automerge.yml -f pull_request_number=<PR>`.

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

Enable Allow auto-merge and configure the Gesher App for this repository.
Use workflow_dispatch to recheck existing receipts; the optional
pull_request_number input targets one PR. It never manufactures approval from a
label snapshot. Old PR heads without aggregate checks need updated CI.

### GitHub App for pull request updates

GitHub's built-in `GITHUB_TOKEN` cannot enable auto-merge for a PR changing
`.github/workflows/*`: it lacks the Workflows permission, which cannot be added
to the workflow's `permissions` block.

Use `gesher-runtime-automation`, the dedicated GitHub App installed on the
Gesher organization. It has Contents, Pull requests and Workflows write
permissions (Metadata read is implicit). If the organization adds repositories,
select only `gesher-runtime/gesher` for this installation. No webhook subscription or
branch-protection bypass is required. Set the repository Actions variable
`GESHER_BOT_APP_ID` to its App ID and the Actions secret
`GESHER_BOT_PRIVATE_KEY` to its generated PEM private key. The
pinned official `actions/create-github-app-token` action creates a short-lived
token scoped to `gesher-runtime/gesher` and those three permissions, and revokes it at
job completion. Set the App ID only after installing the App and saving its key;
incomplete App configuration fails visibly.

The App writes approval receipts, dismisses stale bot approvals, removes stale
approval labels, comments on CI failures, and enables or disables native
auto-merge. The controller still recognizes unedited receipts and legacy
opening and failure comments from GitHub Actions. It authenticates artifacts
from either bot by login, numeric account ID, and bot account type.
The job skips the App's own review submissions and dismissals and its own
`automerge` removals. Other events continue through the controller. The job
queues up to 100 pending events per concurrency group in GitHub Actions.

Authorization reads, CI evidence and `qa/label-approval` use `GITHUB_TOKEN`.
The required status and both CI contexts retain the ruleset's GitHub Actions
integration ID. Keep the App key and installation token in the trusted controller
workflow. Ordinary build/test workers use their existing credentials.

### Optional App QA status: migration phase 2

`GESHER_APP_QA_STATUS` is off when absent or set to anything other than the exact
string `true`. Merge this phase with the variable unset. No ruleset or default
workflow-permission change is part of this implementation.

When explicitly enabled, the controller also writes
`qa/gesher-label-approval` with the App token. The workflow then requests
Statuses write permission, in addition to its existing three permissions. The
pinned token action omits an empty permission input, so installations without
Statuses access continue to work while this feature is off. Verify the actual
installation grant before enabling it; a missing grant fails the run, without
falling back to Actions or a PAT for the App status.

The enabled writer requires App ID `5816428`, installation ID `183304638`, and
the separate App client. The workflow retains the App-slug check. Each App status
response and readback must match the exact repository, head, context, state,
numeric status ID, and bot identity (`gesher-runtime-automation[bot]`, user
`358279050`, account type `Bot`). A missing, replaced, pending, failed, or foreign
status cannot release the existing Actions gate. The readback uses the Actions
client and paginates current combined-status contexts, avoiding full history
scans without trusting a partial page.

Pending and failure are published to both contexts. The controller attempts both
revocations even if one write fails. For success, it first keeps the Actions gate
pending, publishes and reads back the App success, rechecks the current label,
review, permission, source and both registered CI workflows, then reads back the
same App status again before releasing the Actions gate. Changes or API errors
after the App write cause conservative revocation; a newer pending CI attempt
restores pending. Guarded label cleanup still preserves newer label events and
run attempts. API operations are separate requests, not an atomic transaction;
live event reconciliation and GitHub's required checks remain necessary.

The App status is initially a shadow signal. The existing ruleset continues to
require `qa/label-approval`, `ci-required` and `arm64-required`, all from GitHub
Actions integration `15368`. Local tests do not prove the App installation's
permissions, live source attribution or branch-protection enforcement.

Deployment evidence must be collected in this order:

1. Verify a live canonical App `APPROVED` receipt bound to the reviewed head and
   current label event, with the existing `qa/label-approval` green. Only after
   that phase-1 proof may the temporary repository/organization permission for
   Actions to approve reviews be restored; leave both default token permissions
   at read.
2. Merge phase 2 through the normal protected PR path while
   `GESHER_APP_QA_STATUS` is unset. Verify the main commit and drain/reconcile
   queued runs that use older workflow YAML.
3. Grant and verify Statuses write for installation `183304638`, then set
   `GESHER_APP_QA_STATUS=true`. Reconcile a reviewed canary with its exact current
   receipt; `workflow_dispatch` only checks an existing approval and cannot
   create one. Retain the already useful CI evidence.
4. Record live success with the exact head, label, approval, workflow attempt and
   both status identities. Use genuine canary transitions to demonstrate missing
   or revoked approval and missing/pending/failed status cases remaining blocked.
   Do not manufacture a green status or reuse evidence from another head to
   demonstrate a negative case. Keep fixture results and live observations
   separately attributed.
5. Only after that proof, add `qa/gesher-label-approval` bound to integration
   `5816428` to ruleset `27482394`, alongside the old QA gate and both CI gates.
   Read the rule back and prove the live dual gate. Do not remove the old gate
   as part of this phase.

Phase 3 requires its own reviewed change, live dual-gate proof and drainage of
old runs before removing only the legacy QA requirement. Keep this flag enabled
while the App context is required; disabling it is not a migration procedure.

Other repository deployments without the App retain the built-in token behavior.
That requires Allow GitHub Actions to create and approve pull requests. For native
auto-merge on workflow changes, those deployments can use the
`AUTOMERGE_WORKFLOWS_TOKEN` secret with a repository-scoped fine-grained PAT with
Contents, Pull requests and Workflows write access. The PAT is used only for
native auto-merge mutations.

Queued runs and reruns can use workflow YAML from before this App migration
while checking out the new default-branch scripts. The controller rejects
these runs when the old YAML did not supply a PR client for the App. After
deployment, dispatch `automerge` from `main` with a PR number to recheck an
existing receipt. If the current label has no receipt, reapply it after
exact-head review.

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
