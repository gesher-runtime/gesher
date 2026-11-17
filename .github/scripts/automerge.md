# Label-driven automerge

A repository collaborator with write or admin access can apply the automerge label to opt in a specific PR head. Maintain access is reported as write by GitHub's permission API. Triage access does not authorize merging.

A label supplied when the PR is opened cannot approve it. The label event must
be strictly later than the PR's creation timestamp, including when the opener
has write or admin access. GitHub timestamps have second precision, so a label
added in that same second must also be removed and reapplied. Opening events,
label events, CI completion and scheduled sweeps enforce this rule. The opened
event also removes a label already present in its opening snapshot, even when
the snapshot's update timestamp is later than creation. A delayed opening
event preserves a later explicit replacement label.

The PR author can opt in by removing and reapplying an earlier label, with a
strictly older application by the same stable account ID in the event history.
An author's first application instead requires the separate exact-head QA
review described below. Other collaborators can approve with their first
application. Every route requires write or admin access and a recorded bot
approval. Missing author identity blocks approval. This is asynchronous label
removal and approval enforcement, not a restriction on GitHub's label UI.

The controller posts no issue comments: opening, initialization, waiting for CI,
label removal and failures stay silent on the PR. Diagnostic reasons remain in
Actions logs. The native bot approval review is the opt-in signal; GitHub's merge
activity is the completion signal. Native review and dismissal events may still
produce GitHub notifications according to subscribers' settings.

New PRs need no initialization write. The approval boundary is their creation
time. Existing unedited bot opening receipts remain conservative boundaries:
the latest recorded boundary or receipt creation time still applies. Edited,
malformed or conflicting legacy opening records fail closed. This migration
preserves earlier boundaries without creating more comments. An author's first
label still needs the exact-head QA review below or a deliberate reapplication;
a separate label request made by `gh pr create --label` alone is insufficient.

On a uniquely bound, authorized label delivery, the controller submits an
`APPROVE` review as github-actions[bot], with `commit_id` set to the exact PR head
and a hidden receipt containing repository, PR, head, base, label event and actor.
This records the collaborator's automerge opt-in; it is not a code-review or
project-scope verdict. CI, permissions and repository merge requirements remain
separate gates. The bot review's REST and GraphQL identity, body, state, commit,
submission time and absence of edits are verified again before merging. A
dismissed review cannot be recreated by replaying the same label delivery.
Old approval comments no longer authorize merging; reapply the label to record
a native review. Missing, edited or dismissed approval requires a fresh label.
The delivered update timestamp must be zero or one second after the label time,
with exactly one label event in that interval and the same sender.

The repository must enable **Allow GitHub Actions to create and approve pull
requests** in Actions workflow permissions. If approval is denied (including a
PR authored by the bot itself), the controller fails closed without a comment
fallback. Squash merging must also be enabled.

Pushing a different head, force-pushing, reopening or retargeting requires a
new approval. On synchronize, stale controller reviews are dismissed, including
when the label is already absent; human reviews are untouched. This does not
require enabling stale-review dismissal for the entire branch. The merge gate
also checks the exact head even if a synchronize event was missed. Delayed
synchronize jobs re-read the PR and label so they preserve a fresh approval.

## Author's first-label QA review

After the approval boundary, the author may submit a pull-request
review with the `COMMENT` event and then apply the label. GitHub does not allow
authors to approve their own PR; an `APPROVE` review is not this proof. The
submitted review must have REST state `COMMENTED`, a positive review ID, the
same stable account ID as the label actor, and the exact current head SHA in
`commit_id`. Its submission must be strictly after the effective opening
boundary and strictly before the label. Same-second timestamps fail either
comparison. A QA review cannot replace the bot approval review.

Include the concrete QA findings and exactly one standalone marker line. This
example uses placeholder identities; replace all four values with the actual
repository ID, PR number, full head SHA and target branch:

```text
<!-- gesher-automerge-qa-v1 {"repository":10,"number":7,"head":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa","base":"main"} -->
```

The marker must contain exactly those four fields as compact JSON, with no
duplicate keys, extra fields, indentation or text on its line. Field order
does not matter. Prose alone never establishes approval. Missing, malformed,
mismatched or multiple qualifying markers or reviews block the first-label
route. Review history uses the same bounded pagination as the other listings.

The review must be unedited. The controller reads its GraphQL node and checks
the node ID, body, state, commit and submission time against the REST review,
requiring `lastEditedAt` to be explicitly null. REST `submitted_at` alone
cannot establish that the marker existed before labeling because editing a
review does not change that submission timestamp. Any edit, missing metadata
or failed API request blocks this proof; removing and reapplying the label
remains available without an author-QA proof query.

The proof is checked when recording approval and again during merge
evaluation, including after asynchronous CI checks. That final proof read
precedes the final approval-receipt and permission checks and the last PR and
label reads, so changes during review queries remain visible to those checks.
The controller uses its existing permissions and never executes review text.

## CI evidence

The controller resolves required workflows by their registered repository IDs and file paths. The required ci workflow, plus arm64-windows for main and feature/arm64-windows, must have successful pull_request runs for the approved head and source repository. A manual run or another workflow with the same display name cannot substitute.

Both required workflows call local reusable workflows. GitHub records the resolved source SHA and refs/pull/NUMBER/merge in referenced_workflows. The controller verifies that this immutable Git commit has a valid tested base and the exact approved head as its two parents. The tested base must equal the current target tip or be its ancestor, proven by GitHub's comparison API using both immutable commit SHAs. An advanced target must report ahead with the tested base as both base_commit and merge_base_commit. A target behind or diverged from the tested base, malformed comparison, or unavailable ancestry evidence blocks merging.

Ordinary target-branch advancement does not invalidate successful CI for an unchanged approved head. The reviewer should require fresh combined testing when code overlap, a shared contract, or another concrete interaction warrants it; unrelated merges do not automatically require a full CI rerun. GitHub still enforces merge conflicts and repository merge requirements.

The controller resolves the actual target through the Git reference API because PR base.sha may retain an earlier snapshot, and rereads that target after checking CI. A target change during those checks defers the merge to a later evaluation. The ARM64 source-identity call supplies immutable tested-source metadata without checking out or executing PR code in the privileged controller.

The pull_requests array in a workflow run is live association data and is empty for some fork runs. It is checked when available, but is never treated as historical proof of the tested base. Missing or mixed immutable source metadata blocks merging; legacy runs before the source-identity call was added need fresh CI.

All other workflows, check runs and commit-status contexts must finish without failure. Optional neutral/skipped checks are allowed, while the two required workflows must conclude success. Only this registered controller's trusted runs and their exact check-suite IDs are excluded, preventing the controller from waiting on itself.

## Execution and limits

The privileged job checks out only the default branch's scripts. It never downloads or executes PR code or artifacts. Permissions, approval, head, base, target, draft state and label identity are re-read before merging. The controller uses `merge_method: squash` to create one commit on the target. GitHub's merge API pins the head and still applies repository merge rules; the controller does not bypass required reviews or branch protection. The API does not offer an atomic expected-base parameter, so repository protections or a merge queue remain the server-side control for concurrent base updates.

Workflow completion normally wakes the controller. A five-minute scheduled sweep also observes external checks and manually dispatched workflows that finish last; GitHub may delay scheduled jobs. Each API listing is bounded to 1000 items, and malformed, missing or excessive history blocks the PR for manual handling.

A confirmed merge into the default branch explicitly dispatches ci.yml because the workflow's GITHUB_TOKEN does not trigger ordinary push workflows.

The ci workflow cancels superseded runs for the same PR or branch. Its
root-specific concurrency group stays distinct from reusable workflow groups,
so a child cannot cancel its parent. ARM64 already cancels its superseded
runs independently. The approval controller keeps cancel-in-progress disabled:
new CI must not interrupt a pending approval capture or a merge operation.

Run the deterministic REST and GraphQL fixture regression suite with:

    node --test .github/scripts/automerge.test.js

The suite stubs every write; it does not merge a PR, change a label, submit or dismiss a review, or post a comment during tests.
