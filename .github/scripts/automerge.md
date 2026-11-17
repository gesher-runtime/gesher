# Native merge queue with label approval

Maintainers approve a PR by applying `automerge`. GitHub waits for required CI,
validates the combined source in its native merge queue, and squash-merges it.
The controller never calls the merge API and no longer polls CI or uses cron.

## Approval and QA

The label must be applied after PR creation by an account with write/admin
access. Labels supplied at creation, ambiguous same-second events, stale heads,
force pushes, reopening and retargeting cannot authorize merging. A new head
requires a new label. Remove and reapply it after reviewing the new diff.

The existing author approval routes are retained: an author must either
explicitly reapply an earlier label or first submit an unedited exact-head QA
review. A different authorized collaborator can approve with their first label.
The controller records the opt-in as a native bot approval bound to the exact
head, target, label event and actor. This is authorization, not a scope verdict.

The author first-label QA review uses COMMENT and must precede labeling. State
the reviewed commit, scope verdict and evidence, actionable findings, tests and
validation limits as required by CONTRIBUTING.md. Include one standalone line:

```text
<!-- gesher-automerge-qa-v1 {"repository":1426153186,"number":77,"head":"FULL_40_CHARACTER_HEAD_SHA","base":"main"} -->
```

Use the actual PR and head. Duplicate, edited, mismatched or malformed proof is
rejected. Existing legacy opening boundaries and unedited approval receipts
remain valid; migration does not invent reviews or waive project policy.

`qa/label-approval` verifies these conditions on the PR head and on each native
merge group. Group membership is read from GitHub's queue API and bound to the
group's exact SHA. All preceding entries included in a group are checked.
Removing the label, invalidating the review or changing the head revokes native
waiting/queue membership when the event handler runs. New commits also dismiss
stale bot reviews. GitHub's stale-review protection applies independently.

## Required CI and branch protection

The deployed ruleset is `.github/merge-queue-ruleset.json`:

- `main` requires PRs, one approving review, resolved discussions, and a queue.
- `ci-required` covers every root job of ci.yml, including the real Windows test
  shards, packaging and e2e. A failed, cancelled or skipped root job fails it.
- `arm64-required` covers both source identity and the Windows ARM64 build.
- `qa/label-approval` is also required. All contexts are bound to GitHub Actions.
- CI runs on pull_request and merge_group, testing the actual combined SHA.
- Queue source receipts use the group SHA for checkout and API identity, while
  PR receipts continue distinguishing the PR head and synthetic merge checkout.
- No permanent bypass actors; branch deletion and force pushes are blocked.

The queue starts conservatively: one concurrent group, one PR per merge,
ALLGREEN, SQUASH, and a six-hour check timeout for the long Windows pipeline.
This bounds added build load. It does not remove Actions runner waits.

Keep Rulesets and the JSON configuration aligned. When adding a root CI job,
add it to its final gate's needs; workflow contract tests enforce coverage.
Only explicitly required checks block the native queue. Optional/manual
experiments no longer block merging simply because they ran on the same head.
Promote any new mandatory check into a gate or the ruleset deliberately.

## Operations

Enable Allow auto-merge for API compatibility and Allow GitHub Actions to create
and approve pull requests. `automerge.yml` always checks out default-branch
scripts with persist-credentials disabled; it never executes PR code.

A workflow_dispatch reconciles open PRs using existing approvals. It never
creates approval from a label snapshot. This is useful for migration/backfill;
no scheduled polling is necessary. Old heads without the new aggregate checks
need current-main CI before entering the queue.

Tests:

```text
node --test .github/scripts/native-merge-queue.test.js
python .github/scripts/test_merge_queue_workflows.py
python -m unittest discover -s tools/ci -p test_source_identity.py
```

The fixtures stub all GitHub mutations. Live CI/queue execution must be checked
separately; passing host tests does not prove a Windows build or runtime test.
