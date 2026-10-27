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

Owner-requested checkpoint: release source0e4304a25251fbfeb4499886e9e955abaa34f2f1
is saved; this checkpoint changes documentation only. No release-version
full, promotion workflow, tag, PR, main merge or release has been run here.
There is no active own release CI/watch log. The exact notice/version/receipt/
promotion code is unchanged. Next session must first import Root's final
accepted coherent runtime and reconcile the English README with that evidence,
then verify full CI with release_version0.1.0 and actual installed notice.
Root alone owns integration/main and publication. Version0.1.0 remains
unpublished and not release-verified. Stop all new work after this checkpoint.

Release preparation now includes accepted Windows4/Web3 integration6297e80a
(mergebf4cf7fe), retaining explicit release_version, early installer upload,
post-smoke receipt and promotion/source/attempt/digest guards. Root accepted
full40466021634/source160 after independently reading every outcome and the
five actual critical logs: both Windows0failed, package23 and both ten-app
e2e sets0unexpected. The earlier exit-freshness prerequisite is included and
verified. The README now reflects that bounded evidence while keeping0.1.0
unpublished and not release-verified.

The reviewed Media OpenSL notice proposal is imported only in this release
worktree (patch SHA256f9c4197cf2200bc012dd2ce7e73b5a7dc70a01c59c3733f1982953dff03429d1).
It ships the pinned canonical interface-table notice under licenses and checks
the actual installed file's exact source SHA256 in package CI, without changing
the23 PASS count or release/version guards. This packaging change still needs
actual package/release-version CI; it is not an artifact/publication claim.
Final source must include the coordinator's accepted coherent remaining tracks
before the release-version full and main promotion. No source from running
Net4/AOT/Surface candidates is imported here; Root alone publishes/releases.

### Earlier preparation and diagnostic history

The owner's latest local-load instruction covers Java as well: all C/C++/Java
compilation and native/JVM fixtures run only in CI, including tiny probes.
The README reflects this. No local compiler or runtime fixture is required for
the held release preparation.

Current preparation includes coordinator integration
f17d03b1a96f4f0f62ad1d1101ec1eb01b93873d (merge2e347eb9), including the
verified Media3/live-environ/deterministic-TLS combination and ten-app e2e
set. Root personally read full40448521754/dd06d296, and its merged non-doc
integration tree matches the verified source. Earlier integration also has
the verified startup profile, native clocks and signed/unsigned width work. The coordinator
personally accepted full40434741055/source
bdfb230fa3d70797caece6568b6ac529e854b4ee; this agent does not claim to have
independently read that whole run. The additive CI merge retains explicit
release_version, early installer upload and post-smoke receipt/promotion guards.
It introduces no change to release promotion code. Release-version branch CI
remains held for verified Web3/Windows4 and the coordinator's final combined
source. Web3 full40444053066 is green before its exit-oracle hardening;
strict full40449137605 is complete green (22 success/six skipped), with both
actual Windows0failed/14Web3PASS/VM0/Launcher0/child0/lifecycle, package and
both e2e logs read. The child-zero freshness property remains unverified.
A coordinator-owned exec4-exit prerequisite must also make cached child exit
results process-generation-aware before a fresh child-zero claim is accepted.
Windows4 full40447661700 proves actual first TSF reconversion, partial caret,
foreground/shared Display and legacy resize on both runners but is red on the
new non-editor fixture observation and external positive SNI. Its focus-gain
fixture correction is prepared; the coordinated HTTP/SNI slice now has actual
both-runner component evidence, but a new combined full run remains required.
No preparation merge supplies release
verification or authorizes this agent to publish.

Explicit release documentation gap: the repository README still described phase
1 with no Java or APK execution. The coordinator authorized this agent on
2026-10-27 to update that shared file in the release worktree. Its English
replacement now describes actual ART/app/installer capabilities and scoped
limitations, while marking 0.1.0 as not yet published or release-verified.
Before main/release completion the coordinator must reconcile this README with
the final combined evidence, remove only pending items that actually pass,
and add the actual released asset URL, digest and source/CI references after
publication and released-byte verification. That final evidence update remains
an open gate; preparing the text does not close it or publish anything.

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
Remaining: full branch CI with release_version=0.1.0 after
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

Current verified integration 117629e (including the AOT3 oatdump foundation,
pinned Unicode parity and Process proc natives)
is merged into the release branch without conflicts. The release input,
explicit PE versions, early installer upload and post-smoke receipt remain
present. All 34 offline guards still pass after this integration update.
Candidate/released-byte verification now also requires the installed compiler
tools and runs installed oatdump against the installed speed-profile framework,
matching the integrated package smoke. The full release branch CI is still
held until the coordinator merges verified Web3 and the latest integration;
the promotion workflow remains undispatched and nothing is tagged/published.

Recovery verification at 7257b5a: all 34 offline guard/promotion tests pass
again (1.536 seconds), and the version-selection fixture passes. Independent
review of 8073a31 is complete as recorded above; it has not been replaced by
an actual candidate/publisher CI run. The branch CI is still held while the
coordinator resolves the separate full-suite boot-profile failure and merges
verified Web3. No installer, artifact, tag or release was downloaded or
published locally.

Recovery update (2026-10-27): current integration a0f32e1, including verified
AOT runtime merge 6dc3f03, is merged without conflicts. This adds the verified
DM/StrictJarFile and compiler-test stage plus portable TLS/media preparation;
the exact-version input, early installer artifact and post-smoke receipt,
candidate pin/reauthentication guards and installed compiler/oatdump checks
remain present. No C/C++ fixture will be compiled or executed locally under
the updated CI-only rule. Branch release-version CI is still held for verified
Web3 integration and the coordinator's final combined source. Neither the
candidate/publisher workflow nor any tag/release/publication has run here.
