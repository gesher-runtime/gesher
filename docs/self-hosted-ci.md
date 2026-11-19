# Framework build routing and the local Linux runner

The required `ci / framework` pipeline retains its existing `host-tools` and
`framework` jobs. The current CI caller requests GitHub-hosted `ubuntu-24.04`
execution with `local-runner: false` for the new immutable policy described below.
Their producer IDs, artifact names, current-run host-tool receipts, compiler
flags, Android contract checks and Windows consumers are preserved. No
application behavior is changed.

The previously authorized policy can run these jobs on `gesher-wsl-trial`.
There is one physical runner, so it executes one job at a time; other PRs can
queue. The new policy has not been authorized for that restricted runner group.

The root native Gesher `build` job still runs on GitHub. This rollout first
validates these two framework producer jobs before moving more required work.

## Capacity and simultaneous PRs

Keep a single runner registration on this computer. GitHub assigns it one job
at a time, so several eligible PRs wait rather than start concurrent builds on
the machine. Scheduling is not guaranteed to follow PR creation order. A newer
commit cancels the older CI run for the same PR; different PRs do not cancel
one another.

Local native compilation uses at most eight Ninja processes. This is a build
parallelism limit, not a whole-machine CPU or RAM quota. The framework's javac
heap remains 10 GiB; native tools and the operating system use additional RAM.
The existing 8 GiB ccache and 10 GiB snapshot budgets also do not cap checkout,
toolchain or temporary-build disk usage. Check free space and actual load
before expanding the workload. Do not register a second runner to clear a
queue without revisiting these limits.

If the computer sleeps, disconnects or shuts down, assigned jobs cannot finish
and queued local jobs wait; there is no automatic hosted fallback after routing.
Use the rollback flag below and cancel/rerun affected CI if a long outage is
expected. Windows-only execution tests continue on GitHub-hosted Windows.

## Trust and rollout

The current CI caller pins `framework.yml` to
`3c40de3e81c55ca867779024f82e4b728dcfd45c`. That workflow pins the runner-route
action to `5d76cae649c2257c376aaf87a5283099c99f111c`; its local-cache actions
remain pinned to `b9510513a538d80669895521d1061b54fbe11aff`.
Changes to PR workflow files cannot change the policy authorized by the runner
group. Publishing a replacement requires reviewing/pinning a new immutable
policy and explicitly updating the group allowlist before enabling local use.
This change does not update that allowlist or any permission. The current caller
sets `local-runner: false`, so the pinned action selects the existing hosted
runner without making local-authorization API requests. Both required Linux
producer jobs and all Windows execution jobs remain required.

The pinned action also handles confirmed GitHub quota responses if local routing
is later enabled after separate authorization. Matching HTTP 403/429 status and
the API response's rate-limit message can select only `ubuntu-24.04`; it does not
retry the API, grant restricted-runner access, skip work or log the response body.
Permission denials, malformed quota evidence, invalid merge identity and invalid
ancestry retain their existing failure behavior.

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

The existing documented `gesher-build-trial` allowlist remains restricted to this
repository and the following older policies; the new framework policy is not
added by this change:

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

## Return to GitHub-hosted builds

Run on the management computer, where `gh` is already authenticated:

```powershell
gh variable set GESHER_SELF_HOSTED_PRS --repo gesher-runtime/gesher --body false
gh variable set GESHER_SELF_HOSTED_TRIAL --repo gesher-runtime/gesher --body false
```

This routes both required framework producer jobs to `ubuntu-24.04` and keeps
automatic duplicate ARM64 diagnostics disabled. New CI runs use the existing
hosted dependency installation, JDK setup and Actions caches. No commit,
workflow edit, runner unregistration or local-cache transfer is required. A
hosted cache may be cold on the first run, but no build depends on local files.

Routing is decided once by each run's `runner-route` job. Changing the flag does
not relocate a job that has already been routed to the local runner. For an
affected run, copy its numeric ID from the Actions URL and run:

```powershell
$ciRunId = Read-Host 'CI run ID to return to GitHub'
if ($ciRunId -notmatch '^[1-9][0-9]*$') { throw 'A numeric run ID is required.' }
gh run cancel $ciRunId --repo gesher-runtime/gesher
gh run watch $ciRunId --repo gesher-runtime/gesher --interval 5
# Once cancellation has completed, rerun the entire workflow so runner-route
# is evaluated again; rerunning only failed jobs can retain the old route.
gh run rerun $ciRunId --repo gesher-runtime/gesher
```

The cancelled run's `watch` command can report a nonzero exit status. Confirm
that the run is completed before rerunning. Keep the local runner task running
until active local jobs finish or cancellation completes. If cancellation gets
stuck, use `gh api --method POST repos/gesher-runtime/gesher/actions/runs/$ciRunId/force-cancel`
and wait for completion before rerunning. Do not bypass required checks.

For the previously authorized policy, `GESHER_SELF_HOSTED_PRS=true` enables local
routing. For the new policy, a separate reviewed rollout must first authorize its
exact workflow SHA in the group and change the caller's `local-runner` input to
true. Leave the trial flag false. The repository switch remains the rollback
path for any policy that has been authorized and explicitly opted in.

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
wsl -d Ubuntu-24.04 -u root -- apt-get install -y --no-install-recommends gh clang bison flex libc++-18-dev libc++abi-18-dev unzip zip zstd openjdk-21-jdk-headless
```

The existing `Gesher WSL Runner` scheduled task and its registration stay in
place. New installations receive these dependencies from `setup-wsl-runner.ps1`.
`gh` must be installed, but must not be logged into a personal account: artifact
reads use only the current job's read-only `GH_TOKEN`.
`prepare-wsl-runner.ps1` verifies the exact group policy, refuses unknown
workflow grants, and can add the newly approved pinned workflow to the old
single-workflow group. It does not weaken repository or workflow restrictions.

## Validation

```powershell
python -m unittest discover -s .github/actions/local-build-cache -p test_cache.py
node --test .github/actions/runner-route/route.test.cjs
python .github/scripts/test_automerge_workflows.py
```

Cache fixture tests cover real snapshot I/O, corruption, immutable keys,
fallback reporting, path traversal, symlinks, interruption and size limits.
Routing tests exercise the production route function, including author/fork
exclusion, permissions, stale heads and merge ancestry. These do not establish
real runner execution: verify both required producer jobs on the local runner,
their fresh artifact receipts, and downstream Windows consumers in CI before
claiming an end-to-end pass or a speed improvement.
