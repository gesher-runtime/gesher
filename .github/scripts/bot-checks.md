# Bot identity for required checks and automerge

The required signals are published by `gesher-runtime-automation` (App
`5816428`, installation `183304638`), so GitHub displays its avatar. The
underlying Actions jobs retain the GitHub Actions identity.

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
