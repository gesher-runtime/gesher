# Publishing Gesher 0.1.0

The coordinator owns these final operations under the owner's advance
authorization. Track agents prepare and test the mechanism on their branch.
Every known gap and the integration/main CI gates must be closed first.

The installer version is an explicit input. Ordinary CI keeps
`0.1.<run_number>`; the final candidate must use `release_version=0.1.0`.
The publishing workflow never rebuilds an installer or replaces an existing
asset. Local artifact downloads are not part of this procedure.

1. Merge the verified track trees, inspect the final complete integration run
   and merge the reviewed integration PR to main. Record the full main SHA.
2. Dispatch a **fresh complete** run, then watch at the required interval:

   ```powershell
   gh workflow run ci.yml -R gesher-runtime/gesher --ref main -f release_version=0.1.0
   gh run watch <ci-run-id> -R gesher-runtime/gesher --exit-status --interval 120
   ```

   Read every job, both zero-failure Windows summaries, the required track
   PASS lines, actual package install/run/input/uninstall cleanup and both e2e
   zero-unexpected-failure summaries, including required TIC-80 and Puzzles.
   Do not promote a partial rerun: all required jobs must belong to the same
   complete attempt. The post-smoke package receipt must exist.
3. Review `docs/releases/0.1.0.md` against the final commit. Its source SHA and
   CI run placeholders are filled from the validated inputs. Dispatch the
   preparation and candidate-install dry run at that same main SHA:

   ```powershell
   gh workflow run release.yml -R gesher-runtime/gesher --ref main -f source_ci_run_id=<ci-run-id> -f expected_sha=<full-main-sha> -f publish=false
   gh run watch <dry-run-id> -R gesher-runtime/gesher --exit-status --interval 120
   ```

   Confirm `PASS release prepare`, both candidate-byte install/runtime and
   cleanup PASS lines and `PASS release publish` with `dry_run: true`. This
   path creates no tag/release/assets. If main moves, produce a fresh complete
   CI run for the new expected SHA.
4. Dispatch publication with the **same** validated main SHA and source run:

   ```powershell
   gh workflow run release.yml -R gesher-runtime/gesher --ref main -f source_ci_run_id=<ci-run-id> -f expected_sha=<full-main-sha> -f publish=true
   gh run watch <release-run-id> -R gesher-runtime/gesher --exit-status --interval 120
   ```

   Candidate execution has read-only job permissions. Publication uses a fresh
   runner/checkout, downloads the original artifacts again and runs no installer
   with write credentials. The workflow verifies source provenance, all job conclusions/critical
   steps and the pre-execution candidate job's immutable output pin (source
   attempt, artifact identities/digests/sizes and member hashes). A new green
   source attempt requires candidate verification again. It also verifies both
   artifact ZIP digests, the manifest/checksums, installer PE
   metadata and candidate install/runtime/cleanup. Immediately before any write
   it re-fetches/hashes the source receipt ZIP and derives expected hashes from
   its authenticated in-memory members, without trusting mutable prepare data.
   It creates `v0.1.0` at the
   expected commit, uploads the original bytes to a draft, downloads and
   hashes each asset, rechecks source CI and only then publishes. An existing
   matching tag/draft can resume; a different tag or asset fails without being
   moved or overwritten. Identical published assets are read-only.
5. Inspect both `verify-released` jobs. They check the public download URLs and
   digests against the original CI candidate, validate actual installer PE
   metadata, install/run/uninstall the released bytes, run the pinned real-app
   e2e set and verify cleanup again. Require both e2e summaries to report zero
   unexpected failures. Record the release URL, source SHA, source CI run,
   publication run and installer SHA-256 in the final report. Publication is
   not considered verified until these jobs pass.

Evidence is uploaded by the workflow even after a failure. Read logs through
`gh run view --log` or the jobs log API; do not download artifacts locally.
An API 403 requires approximately ten minutes of backoff. A failed released
verification requires investigation; never silently replace published bytes.
