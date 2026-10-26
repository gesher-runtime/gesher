# Track brief: release1

Final published Gesher 0.1.0 Windows installer, explicitly requested and
authorized by the owner in chat on 2026-10-26. Main merges were preauthorized
in the same instruction; no additional owner approval pause is required.
Reserved ADR: 0071. Branch `wip/release1`, dedicated worktree
`C:\work\gesher-release`, from current phase4/integration.

## Required work

- Give the final installer the exact version 0.1.0 and a stable release
  filename. CI/nightly numbering must not silently become the release version.
- Promote the exact installer produced and tested by a successful full CI
  run on main. Verify repository, workflow, ref, SHA, all job conclusions,
  installer metadata and SHA-256; never rebuild an untested replacement.
- Publish the v0.1.0 tag/release with that installer, checksums and concise
  English release notes: what works, verified Windows environments, exact
  CI runs/commit and accurately scoped hardware/API limitations.
- Keep publication in CI so NetFree's blocked local artifact download does
  not require fetching CI artifacts on this machine. Reading logs is allowed;
  the publishing runner may consume the tested installer artifact.
- Prevent publication from pull requests, forks, a red/partial/cancelled run,
  an unexpected commit or an asset whose digest differs from the tested one.
  Check for an existing tag/release before creating one; never overwrite a
  different published artifact or move an existing release tag silently.
- Verify the published asset's metadata/digest and download link, and repeat
  install/run/uninstall cleanup from the released bytes in CI. Confirm the
  supported app/native/runtime smoke tests and both full Windows suites.

## Completion gates

Every known gap remains in scope. Merge verified tracks to integration;
run the final full integration CI, inspect both zero-failure summaries,
all new PASS lines, package install/run/input/uninstall cleanup and both
e2e zero-unexpected-failure summaries. Open the final integration-to-main PR
with per-track verified SHAs/run IDs, checks and real limits, attach the PR
to the current task, merge under the owner's advance authorization, then
finish the main/release verification and publish the exact tested asset.

Domain: CI/release promotion, package version/metadata, installer validation
and release docs. Keep shared workflow/installer edits additive and coordinate
with current tracks. No local AOSP builds or WSL; absolute worktree paths.

## Status and next steps

Preparation is owned by the web3 agent in this separate worktree. The root
coordinator alone merges integration/main, creates tags and publishes; do not
interpret the broad completion gates above as authorization for this agent to
perform those final actions.

Implemented locally, not yet CI verified: exact 0.1.0 selection with ordinary
CI numbering retained, explicit PE metadata, post-smoke manifest/checksum
receipts, promotion guards and GitHub transport, a dispatch-only main release
workflow, candidate and released-byte install/run/uninstall verification,
released-byte real-app e2e on both Windows runners, and release-note source.
Thirty-four offline test methods (including negative subcases) and the version
selection fixture pass. ADR 0071 records the mechanism; [the release
runbook](../../releases/README.md) gives the coordinator's final commands.
Remaining: independent review, full branch CI with release_version=0.1.0 after
verified web3 integration, successful full main CI, promotion dry run and the
coordinator's eventual publication/released-byte verification. The publication
workflow itself has not run. No tag, release or local artifact download has
occurred.

Independent review found that mutable local candidate and prepare evidence
could be changed together after preparation. Fixed before CI: publication now
downloads/hashes the source receipt again before any write, keeps authenticated
members in memory and derives expected asset hashes from those bytes. The full
own-byte reproducer now refuses before tag creation. Candidate execution and
write-capable publication also use separate runners/checkouts and permissions;
the publication runner never executes an installer. Re-review and actual CI
remain pending.

The follow-up review's full-green-rerun race is also guarded: pre-execution
prepare writes an exact source attempt/artifact/member pin to a GitHub job
output. The publisher's reauthenticated source must match that pin or refuse
before any write. Both pin emission and an intervening new green attempt have
offline fixtures. The exec3 agent independently reviewed 8073a31 on 2026-10-26:
all 34 offline tests passed in its snapshot (2.473 s), both concrete P1/P2
reproducers were blocked before writes, and it verified pre-execution pin
transport, API/receipt/member authentication and safe tag/asset idempotence.
It found no further actionable issue in that reviewed scope. This does not
replace actual full CI or publication verification; neither has run yet.
