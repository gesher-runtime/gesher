# Label-approved native auto-merge

Apply `automerge` after reviewing the current PR head. GitHub performs a native
SQUASH merge after the required PR CI and exact-head QA gate succeed.
There is no merge queue and no second full merge-group CI run. In App mode the
trusted label controller selects GitHub's protected immediate merge API when the
PR is already ready, or native auto-merge waiting when another native rule still
blocks it. It never bypasses a rule or starts another CI build. Its only schedule
is the recovery described under API quota.

## Label authorization and QA

A label must be strictly later than PR creation and applied by a collaborator
with write/admin access. Creation-time labels, ambiguous events, changed heads,
force pushes, reopening and retargeting cannot reuse an earlier approval.
Pushes remove stale approval labels automatically. Review the updated diff and
apply the label again to approve the new head. Delayed push deliveries preserve
a later label application. A pending label in the same/adjacent timestamp second
is conservatively retained when GitHub supplies no proof that it predates the
push; it cannot authorize a new head without a matching approval receipt.

Any collaborator with write/admin access, including the PR author, approves the
current head by applying the label after the PR was opened. The controller
removes only a label applied at creation (or in the same second, or before the
recorded opening boundary) and labels invalidated by a push, reopening or
retargeting. A creation-time label is the only route an author without write
access could use to request automerge, and the permission check rejects such an
author's later labels. No separate author COMMENT review is required. The bot
records a native approval binding the repository, PR, head, base and label event.
This receipt records authorization; it is not a code-review or scope verdict.

Legacy unedited receipts and opening boundaries retain their meaning. Maintainer
approval cannot waive the Android-visible contract requirements in
CONTRIBUTING.md.

## One PR CI run and final QA

The label records approval for that head. In legacy-only mode it also enables
native SQUASH waiting while `qa/label-approval` remains pending. With the App
status enabled, native waiting stays disarmed through status publication and
verification. Its final release either submits the protected native SQUASH
merge or enables native SQUASH waiting. A small metadata-only
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

GitHub's stale-review dismissal (`dismiss_stale_reviews_on_push`) is off. With
it on, GitHub dismissed the controller's exact-head approval with "The
merge-base changed after approval" whenever `main` advanced, and kept dismissing
every fresh approval of the same head until the branch was updated, although
`git merge-base` was unchanged (PR 120 after #218). Approval stays exact-head
without it: `qa/label-approval` is required and bound to the reviewed head, a
push removes the label, disarms native auto-merge and dismisses the controller's
own approvals of older heads, and only a fresh label on the new head can turn
the gate green again. An approval therefore survives `main` advancing but never
authorizes a different PR head.

## API quota

Every reconciliation rereads labels, reviews, permission and CI evidence. These
reads use a separate read-only Gesher App token (`actions`, `checks`, `metadata`
and `pull-requests` read; PR issue events and comments need no `issues` access), so they count against the App installation's
quota rather than the repository's shared `GITHUB_TOKEN` quota. Exhausting that
shared quota previously left `qa/label-approval` pending after CI succeeded.
The required `qa/label-approval` status and its readback still use
`github-actions[bot]`, because the ruleset binds that context to GitHub Actions.
If the installation lacks one of those read permissions, token creation fails
without stopping the job, a notice is logged and reads fall back to `GITHUB_TOKEN`.

Same-repository `workflow_run` events use the PRs GitHub names in the event
instead of listing every open PR; fork runs still list open PRs. In App mode,
only the first named PR shares that event's concurrency key; any other matched
PR is routed to its own metadata job as described below. A CI completion for
an open PR at the run's head with no `automerge` label, no native waiting and
no `success` in either QA context (trusted or not) ends after that PR read and
one current-context scan, the same rule the recovery schedule applies to
unlabeled PRs. Such a PR's gate is already not green, so a full pass would only
repeat its revocation with an App status write and a legacy write on the shared
`GITHUB_TOKEN` quota. Every other matched PR is reconciled as before. Native
auto-merge cancellation reads its state from the REST pull request it already
fetched. Registered workflow identities, paths and enabled states are reread at
each CI evidence pass, including after App status publication. Title or body
edits do not start the job; base changes still do.

### Recovery schedule

A controller run that fails after CI completion, for example on an exhausted API
quota, would otherwise leave a labeled PR pending (or a failed PR labeled) with no
further event. Every 20 minutes (`7,27,47 * * * *`) the controller lists open PRs
and reconciles labeled pending, missing or failed gates when the registered
CI workflows reach a result for the exact head. A failure gate is included because, with the App status on, a
transient API error during reconciliation revokes to failure although the label
and CI remain valid; an invalid label is re-derived and stays failed.
The gate checked is `qa/gesher-label-approval` while the App status is enabled,
otherwise `qa/label-approval` (read with the Actions token, since the App read
token requests Statuses access only while the App status is on). A green gate is
settled only if its numeric writer identity, bot type, context, repository/head
URL, current CI and ordinary approval validation all remain valid. A foreign
same-name success cannot suppress recovery. A failed same-head rerun removes
the stale label; a pending rerun restores pending rather than keeping old green.
Recovery also resumes a valid green PR if a controller stopped after publication
but before the final native SQUASH release. A valid green gate with native SQUASH
waiting needs no status rewrites or native mutations. Each labeled
candidate has one current combined-status scan and a CI evidence pass; settled
green candidates also recheck approval and current native waiting. Scans paginate
current contexts rather than full status history. Drafts are skipped. In App
mode, an unlabeled PR with native waiting or leftover green is also recovered,
so a missed unlabel delivery cannot keep old authorization indefinitely. An
unlabeled PR without native waiting costs only a current-context scan and no CI
or approval reads; without old green, it is left alone. Legacy-only recovery
still skips unlabeled PRs. Schedule payloads carry no repository object, so the job derives
the repository name for App token scoping from `GITHUB_REPOSITORY` and the App
requirement from `GITHUB_REPOSITORY_ID`. Reconciliation
applies exactly the event path's checks; it never creates an approval. A failure
for one PR is logged and does not stop the others. Manual recovery remains:
`gh workflow run automerge.yml -f pull_request_number=<PR>` (or no number for all
open PRs).

### Phase 3: required App gate

The legacy `qa/label-approval` write must use `GITHUB_TOKEN`, so an exhausted
repository quota can still block it. When a legacy write fails while the App
status is enabled, the controller reads the active rules for `main` once. Only if
they require `qa/gesher-label-approval` bound to App integration `5816428` does
the failure become a warning; the App status, written and read back with the App
credentials, is then the gate. Otherwise, including an unreadable ruleset or the
context bound to another integration, the failure stays fatal exactly as before.
Normal disarmed publication does not read rules while legacy writes succeed.
If native cancellation is unconfirmed, revocation separately reads the current
rules before any status mutation: an App shadow cannot safely block an old
required legacy success.

#### Per-PR publication ordering

App-mode schedule and bulk manual jobs only select and enqueue candidates.
Fork CI completions without a trusted PR-number concurrency key do the same.
They send `gesher-automerge-reconcile` repository dispatches using the App's
existing `contents: write`; no additional token permission or CI run is needed.
The default-branch worker uses the same PR-number concurrency key as label,
review, named CI and targeted manual events. It checks the supplied number/head,
fetches current state and performs the ordinary approval and CI checks. A stale
head is skipped. Dispatch payloads never authorize approval or label removal.
Thus another controller cannot rearm the same PR during status publication,
while unrelated PRs retain independent queues.

GitHub's [`queue: max`](https://docs.github.com/en/actions/how-tos/write-workflows/choose-when-workflows-run/control-workflow-concurrency)
permits up to 100 pending jobs per group, then rejects new arrivals. It orders
admitted jobs by when they begin waiting on the group, not webhook dispatch
order. This is bounded admission, not unlimited or guaranteed event delivery.
The controller never relies on delivery order: it rereads current evidence.
The schedule retries incomplete work and detects missed review, permission,
CI and unlabel invalidation. Rejected or delayed jobs can postpone recovery;
there is no exact recovery deadline. A missing original approval receipt stays
failed and requires the normal fresh label/QA route rather than being invented.
Per-PR errors stay isolated, and the dispatcher itself never publishes success
or changes native waiting.

Repository dispatch runs only the configured default-branch metadata workflow;
see GitHub's [event semantics](https://docs.github.com/en/actions/reference/workflows-and-actions/events-that-trigger-workflows#repository_dispatch)
and [required permission](https://docs.github.com/en/rest/repos/repos#create-a-repository-dispatch-event).

App-only gating uses the same disarmed publication protocol as dual gating.
An existing native request is cancelled and its removal verified before any
status publication or new native approval receipt. The controller authenticates the App success, checks
approval and CI again, writes the legacy success, then rechecks current approval,
registered CI and the exact App receipt before releasing native SQUASH pinned to
the reviewed head. GitHub rejects `enablePullRequestAutoMerge` for an already
ready `CLEAN` PR. Following the [GitHub CLI selection](https://github.com/cli/cli/blob/trunk/pkg/cmd/pr/merge/merge.go),
`CLEAN`, `HAS_HOOKS` and `UNSTABLE` use `mergePullRequest`; `BLOCKED` and `BEHIND`
use native waiting. Unknown, conflicting, changed, closed or draft targets do not
release. Both mutations supply `expectedHeadOid` and remain subject to all native
required checks, approvals and other rules. This is the authorized label
controller, not an administrator/manual merge fallback.

The native-state read is a selection hint, not an override of required evidence.
Current approval, CI and the exact App receipt are checked after it. The chosen
mutation is the final release action, with no further required controller checks.
If it fails or its response is lost, the controller first rereads the PR. A
closed or changed target receives no cleanup writes; an unreadable target is
reported and deferred. An open unchanged target is disarmed and its status is
revoked only by the safe protocol below. It never retries through the other
release API. A subsequent ordinary reconciliation starts again from live state.
App-enabled controller jobs use the per-PR ordering above through the complete
publication sequence. Legacy-only publication behavior remains unchanged.

Operational order: complete migration phase 2 step 5 (dual gate), merge this
change, then remove only the legacy `qa/label-approval` requirement from ruleset
`27482394`. While both contexts are required, a failed legacy success write still
holds the PR until the recovery schedule or a dispatch rewrites it.

Rollback: a swallowed legacy revocation can leave `qa/label-approval` green while
`qa/gesher-label-approval` is failure. That is safe only while the App context is
required. Before removing the App requirement from the ruleset, dispatch the
controller for all open PRs with both contexts writable, so every legacy context
is rewritten from current evidence.

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
status cannot release the existing Actions gate or native waiting. The readback
uses the configured read-only App client, with the Actions fallback, and
paginates current combined-status contexts, avoiding full history scans without
trusting a partial page.

With native waiting confirmed off, pending and failure are published to both
contexts, and both credentials are attempted even if one fails. If native
cancellation is unconfirmed, the controller first verifies that the current
rules require the App context from its exact integration. Only then can it write
and authenticate a blocking App status. The legacy write is permitted only after
that required failure/pending status has been read back. If the App is merely a
shadow, the rules are unreadable, or App revocation/readback fails, no unrelated
status write is issued; the unresolved revocation is reported. An old armed
request can still complete independently during lost permissions or delayed
events, but cleanup must not itself wake it with a still-green required gate.
Success publication runs with native waiting
disarmed, including requests that existed before this reconciliation. The App
success and its authenticated readback do not themselves release an armed PR.
Current label, review, permission, source and both workflow registrations are
rechecked after the App write and again after legacy publication. The exact App
receipt is read back once more before the final expected-head native release.
Changes or API errors before release cause conservative revocation; a newer
pending CI attempt restores pending. Guarded label cleanup still preserves
newer label events and run attempts. API operations are separate requests, not
an atomic transaction; live event reconciliation and GitHub's required checks
remain necessary.

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

Phase 3 (see API quota) requires its own reviewed change, live dual-gate proof and drainage of
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
