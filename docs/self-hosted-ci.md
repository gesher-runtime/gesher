# Required PR builds on the local Linux runner

The required `ci / framework` pipeline can run its existing `host-tools` and
`framework` jobs on `gesher-wsl-trial`. Their producer IDs, artifact names,
current-run host-tool receipts, compiler flags, Android contract checks and
Windows consumers are preserved. No application behavior is changed. There is
one physical runner, so it executes one job at a time; other PRs can queue.

## Trust and rollout

The CI caller pins `framework.yml` to
`93239cc6ea2702ff5f97c7bbe7c8dd64ea6110c7`. That policy pins the runner-route
and local-cache actions to `b9510513a538d80669895521d1061b54fbe11aff`.
Changes to PR workflow files cannot change the policy authorized by the runner
group. Publishing a replacement requires reviewing/pinning a new immutable
policy and explicitly updating the group allowlist.

The route job runs on GitHub, without checking out PR code. It permits local
execution only when all of these hold:

- The caller requests it and `GESHER_SELF_HOSTED_PRS=true`.
- It is an open PR to `main` in `gesher-runtime/gesher`, authored by `palmoni5`.
- The head repository is this repository, not a fork, and the author currently
  has write, maintain or admin permission.
- The PR head still matches the event. The checkout is its two-parent merge,
  and the tested base belongs to the current `main` history.

Other authors, forks (including the owner's forks), pushes, dispatches and
merge-queue events use GitHub-hosted runners. A stale head also falls back to
hosted CI. Invalid merge identity fails closed. No GitHub write token or account
login is installed on the build computer.

The organization group `gesher-build-trial` permits only this repository and:

```
gesher-runtime/gesher/.github/workflows/arm64-windows-self-hosted.yml@refs/heads/main
gesher-runtime/gesher/.github/workflows/framework.yml@93239cc6ea2702ff5f97c7bbe7c8dd64ea6110c7
```

The first entry remains for explicit ARM64 diagnostics. Turn off its automatic
duplicate builds once required framework execution is enabled:

```powershell
gh variable set GESHER_SELF_HOSTED_TRIAL --repo gesher-runtime/gesher --body false
gh variable set GESHER_SELF_HOSTED_PRS --repo gesher-runtime/gesher --body true
```

For rollback, set `GESHER_SELF_HOSTED_PRS=false`; new required builds return to
GitHub. A job already queued for an offline local runner must be cancelled and
its parent CI rerun after changing the flag. Do not bypass required checks.

## Local caches

Self-hosted input snapshots live at `~/.cache/gesher-ci/v1`, outside the checkout.
They use uncompressed local archives, not Actions cache uploads/downloads.
The code retains real directories at `workspace/third_party/aosp`, so the AOSP
cleanup/sparse-set guards retain their original boundary checks.

- Snapshots are isolated by PR ref, preserving the original branch boundary.
  A different PR cannot restore the same key. The original input hashes, cache
  keys, ordered fallback prefixes and exact-hit outputs are retained. An inexact fallback is not reported as an exact hit.
- AOSP fetching still validates the pinned source/patch recipe. Host tools are
  freshly invoked for the current run and attempt and publish a fresh archive.
- Coremod, framework resources, build tools, public SDK and JVM tools use the
  same key/invalidation logic as hosted CI. The final framework is rebuilt from
  the current PR; test results and final build directories are not cached.
- Compiler output stays in ordinary local ccache, bounded to 8 GiB for these
  jobs. Ninja uses at most eight processes; the framework retains its 10 GiB
  default javac heap. Snapshot storage is bounded to 10 GiB, with oldest entries
  evicted across PR namespaces under a process lock. Oversized snapshots are skipped.
- Snapshots are immutable and published atomically with a SHA-256 manifest.
  Restoring validates archive identity, digest, paths and links before replacing
  cache destinations. Interrupted staging directories are removed under the
  cache lock. A failed snapshot does not become a cache hit.

The optional ARM64 workflow also uses local AOSP/Android-build-tool snapshots,
keeps the versioned toolchain already on disk, and retains local ccache. It has
no large cache upload at teardown. Manual dispatch can run while its automatic
trial flag is off.

The cache contains build inputs and compiler output. It remains accessible to
the Linux runner user; it is not a security sandbox for untrusted external PRs.
Continue excluding forks from the self-hosted pool. Monitor C: free space and
queue delay before routing more pipelines to the single machine.

## Machine prerequisites

Run on the runner computer in PowerShell if updating an older installation:

```powershell
wsl -d Ubuntu-24.04 -u root -- apt-get update
wsl -d Ubuntu-24.04 -u root -- apt-get install -y --no-install-recommends clang bison flex libc++-18-dev libc++abi-18-dev unzip zip zstd openjdk-21-jdk-headless
```

The existing `Gesher WSL Runner` scheduled task and its registration stay in
place. New installations receive these dependencies from `setup-wsl-runner.ps1`.
`prepare-wsl-runner.ps1` verifies the exact group policy, refuses unknown
workflow grants, and can add the newly approved pinned workflow to the old
single-workflow group. It does not weaken repository or workflow restrictions.

## Validation

```powershell
python -m unittest discover -s .github/actions/local-build-cache -p test_cache.py
node --test .github/actions/runner-route/route.test.cjs
python .github/scripts/test_merge_queue_workflows.py
```

Cache fixture tests cover real snapshot I/O, corruption, immutable keys,
fallback reporting, path traversal, symlinks, interruption and size limits.
Routing tests exercise the production route function, including author/fork
exclusion, permissions, stale heads and merge ancestry. These do not establish
real runner execution: verify both required producer jobs on the local runner,
their fresh artifact receipts, and downstream Windows consumers in CI before
claiming an end-to-end pass or a speed improvement.
