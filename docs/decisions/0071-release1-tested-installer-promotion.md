# 0071. Promote the exact installer tested by full main CI

Status: proposed; release1 preparation. No tag or release has been created.
The coordinator owns integration/main merges and final publication under the
owner's advance authorization. This worktree prepares and tests the mechanism.

## Decision

An optional `ci.yml` release_version input selects exactly 0.1.0. Blank input
keeps 0.1.<run_number>. Inno Setup's PE and displayed versions use that same
selection; the stable release filename is gesher-setup-0.1.0.exe.

Preserve the early gesher-setup artifact used by e2e diagnostics. Only after
all package install/run/input/uninstall checks pass, create a separate receipt
artifact with exact source repo/ref/SHA/run/attempt, original artifact id/digest,
installer SHA-256/size/PE metadata and SHA256SUMS. A red run cannot be promoted.

Promotion is explicitly dispatched on main at the expected full SHA. Validate
a fresh complete attempt of ci.yml on main in this repository, every job and
required test steps, documented skips only, both artifact archive digests,
receipt provenance, installer bytes and PE metadata. Compare both artifact
identities and all prepared asset hashes again before writing; validate release
notes before creating a tag. Prepared workspace data is only advisory: fetch and
hash the source receipt ZIP again before any write, retain its authenticated
members as immutable in-process bytes, and derive expected installer/member
hashes from them. Candidate installer execution has read-only job permissions;
publication runs on a fresh runner/checkout, consumes the original artifacts
again and never executes the installer. Partial reruns are refused;
run a fresh full main CI with release_version=0.1.0 for the final candidate.

The publishing runner consumes CI artifacts; this machine never downloads
artifacts. GitHub documents artifact digest/identity and download redirects in
its [artifact API](https://docs.github.com/en/rest/actions/artifacts).
Digest mismatches must fail even though download-artifact itself only warns,
as described in [artifact validation](https://docs.github.com/en/actions/tutorials/store-and-share-data#validating-artifacts).

Never rebuild, move an existing tag or overwrite a differing asset. A matching
tag and matching partial assets may resume; a complete identical release is
idempotent. New releases remain drafts until all expected uploaded bytes are
verified. Verify install/run/uninstall from candidate and released bytes in CI.
The workflow defaults to a dry run; only the coordinator requests publication.

## Verification and open gates

Before candidate execution, preparation records source run/attempt/SHA, both
artifact identities/digests/sizes and member hashes as a GitHub step/job output.
The fresh publishing runner must match that exact pin against its authenticated
source again. A later complete green rerun cannot substitute new unexecuted
candidate bytes; it requires a fresh candidate verification job.

Thirty-four offline own-byte tests cover red/partial/fork/wrong-ref/wrong-SHA runs, skipped
required steps, mismatched artifacts/receipts and safe existing tag/release
handling, both complete archive downloads/extractions and corruption of either
archive, changed prepared artifacts/bytes and missing/unresolved notes before
any tag write. A complete mutation of installer, local manifest/checksums and
prepare evidence cannot override the original API-authenticated receipt; a
corrupt re-fetched receipt also refuses publication. Job-output generation and
a new full-green attempt between candidate execution and promotion are covered.
Version selection tests
retain normal numbering and reject malformed
release overrides. Full branch CI with the exact 0.1.0 override, successful main
CI, actual promotion and released-byte verification remain pending.
