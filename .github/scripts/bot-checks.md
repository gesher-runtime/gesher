# Bot identity for required checks and automerge

The required signals are published by `gesher-runtime-automation` (App
`5816428`, installation `183304638`), so GitHub displays its avatar. The
underlying Actions jobs retain the GitHub Actions identity.

After the live rules require `qa/gesher-label-approval` from App `5816428` and
no longer require `qa/label-approval` from any source, the QA controller stops
publishing the legacy status. It reads and caches this publication decision once
per controller invocation. Dual-gate and shadow deployments retain legacy
publication; an unreadable ruleset fails the invocation rather than assuming
the migration is complete. App receipt readbacks, approval/CI rechecks and
protected exact-head merge remain unchanged.

Only QA had a redundant manual legacy writer. `CLA` already uses one context.
`ci-required` and `arm64-required` are actual Actions aggregate jobs whose
results the bot must verify; their run rows remain visible. Actions job rows
cannot have their App identity replaced. Existing commit statuses are retained
by GitHub, so the old QA row may remain on commits which already received it;
new commits in App-only mode receive only the bot QA status.

| Existing signal | Bot signal | Source |
| --- | --- | --- |
| `ci-required` | `ci/gesher-required` | Current `ci.yml` PR run and its `ci-required` aggregate job |
| `arm64-required` | `arm64/gesher-required` | Current `arm64-windows.yml` PR run and its `arm64-required` aggregate job |
| `qa/label-approval` | `qa/gesher-label-approval` | Existing exact-head approval/QA controller |
| `CLA` | `CLA` | Existing CLA App writer |
| Actions `automerge` job | App check `automerge` | Actual per-PR controller outcome, advisory |

`required-checks.yml` executes only default-branch scripts. Its short-lived App
token can read Actions, Checks and PRs and write commit statuses. It cannot
merge, approve, edit PRs, write contents or change protections. Fork code,
artifacts, logs and event-supplied conclusions are never executed or used as
success evidence. Source workflow registration, run ID, attempt, repository,
fork, branch, head, aggregate job and GitHub Actions check-suite identity are
verified against live API data. Missing CI/approval is pending; unsuccessful
workflows or aggregate jobs do not become green. Publication begins pending,
rechecks evidence before writing, and revokes a success if a new attempt is
observed immediately afterwards. API errors fail closed. A schedule and a
reporting-only manual dispatch recover missed events without rerunning CI.

Aggregate discovery uses the named latest check in the canonical workflow
run's check suite. Its GitHub Actions job link locates an individual job, whose
ID, name, run ID, exact `run_attempt`, head and check back-reference must match.
The check's publisher/head/suite/link are verified before discovery and again
on readback. A green check from an older attempt is rejected. This avoids the
job-list endpoints that returned HTTP 502 for successful CI run `46020002278`,
attempt 2, on 2026-11-28. The changed production reader verified that run and
the corresponding ARM64 run using read-only live API calls; authenticated App
publication still requires post-merge validation. No requirement, permission,
aggregate success condition or fail-closed behavior is removed.

The automerge controller records outcomes as local step output. A separate
step publishes an App check with a link to that workflow's logs. Confirmed
protected merges are success, waiting/no merge is neutral, and controller
errors are failure. Reporting cannot retry a merge or alter controller results.
It skips changed heads and inherited native waiting. The controller's protected
expected-head merge remains its final GitHub action; the original raw CI reader
and all approval/QA validation remain in place.

The QA receipt is authenticated from the full `createCommitStatus` response.
GitHub's combined-status API omits `creator`; readback checks the immutable
authenticated receipt ID and its exact head/context/state/URL. A replacement
status gets another ID and is rejected. Any creator supplied in a readback is
also checked. The previous implementation incorrectly required this omitted
field, which was confirmed by live run `45991130949` on 2026-11-27. The QA flag
was restored to unset after that validation failure, pending this correction.

The subsequent activation failed readback again in run `46001151940`, while a
read-only replay accepted a settled live receipt. The immediate failing
response was not logged, so delayed visibility is a hypothesis rather than a
proven diagnosis of that particular response. `GESHER_APP_QA_STATUS` was set
to `false` on 2026-11-27; CI reporting remains enabled and Required rules remain
unchanged.

Immediately after its own POST, the QA writer now permits up to six combined
reads, one second apart, only while the context is missing or has an older
numeric status ID. It requests cache revalidation and never repeats the POST.
The exact authenticated receipt must become current before it proceeds. A newer
ID, conflicting receipt metadata, invalid ID/head/source, or API error still fails
closed. Later pre-release checks do not retry a previously confirmed receipt.
An exhausted/mismatched readback reports the expected and observed ID/state for
diagnosis. Tests simulate lag, exhaustion, replacement during a retry and QA
revocation during publication. A new live rollout is still required after merge.

## Deployment

1. Merge the workflow/scripts through the existing protected PR process. Keep
   `GESHER_APP_REQUIRED_CHECKS` unset until then. Installation `183304638` must
   have Statuses write and Checks write (already granted on 2026-11-27).
2. Enable `GESHER_APP_QA_STATUS=true` and `GESHER_APP_REQUIRED_CHECKS=true`.
   The first flag enables the existing bot QA signal and the advisory automerge
   report. The second enables CI reporting. Dispatch **`required-checks.yml`**
   to backfill current PRs; it only reads evidence and publishes statuses.
   An `automerge.yml` dispatch can actually merge eligible PRs and is not a
   reporting-only operation.
3. Verify live success/pending/failure attribution to the App on current PR
   heads, including both aggregate jobs and a fork. Confirm existing QA and
   CLA results come from this App, not merely a same-name status. Retain the
   existing CI gates until this evidence is recorded.
4. Add `ci/gesher-required`, `arm64/gesher-required` and
   `qa/gesher-label-approval`, each bound to integration `5816428`, to ruleset
   `27482394`. Bind `CLA` to `5816428` too. Read back the live rules. Prove the
   dual gates before removing only the three old Actions contexts. Preserve
   reviews, conversation resolution, bypass policy and all other rules. The
   advisory `automerge` check must not be required: it reports an operation
   which itself depends on required checks completing.
5. Drain older controller runs and reconcile inherited native requests through
   the approved workflow. Confirm the required App contexts remain active
   before treating failed legacy QA writes as best-effort; see `automerge.md`.

The bridge is asynchronous. A GitHub event/API outage can delay revocation or
publication; there is no atomic snapshot across GitHub APIs. Same-head reruns
need the in-progress event or scheduled recovery to replace a previous result.
The automerge controller independently revalidates the original live CI attempts
immediately before release. Local fixtures cannot establish live source binding,
event delivery, native merge enforcement or installation permissions.

Rollback: restore and verify original Actions requirements **before** removing
App requirements or disabling either flag. Reconcile QA as documented in
`automerge.md`; an old legacy success must not become authoritative again.

Validation:

```text
node --test .github/scripts/label-automerge.test.js .github/scripts/app-required-checks.test.js .github/scripts/app-automerge-check.test.js
python .github/scripts/test_automerge_workflows.py
```

Contracts: [workflow jobs](https://docs.github.com/en/rest/actions/workflow-jobs),
[check runs](https://docs.github.com/en/rest/checks/runs),
[commit statuses](https://docs.github.com/en/rest/commits/statuses),
[required status checks](https://docs.github.com/en/repositories/configuring-branches-and-merges-in-your-repository/managing-protected-branches/about-protected-branches#require-status-checks-before-merging).
