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
receipt provenance, installer bytes and PE metadata. Partial reruns are refused;
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

Offline own-byte tests cover red/partial/fork/wrong-ref/wrong-SHA runs, skipped
required steps, mismatched artifacts/receipts and safe existing tag/release
handling. Version selection tests retain normal numbering and reject malformed
release overrides. Full branch CI with the exact 0.1.0 override, successful main
CI, actual promotion and released-byte verification remain pending.
