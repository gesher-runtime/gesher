# Whole-workload performance measurements (PERF-00)

`tools/perf/benchmark.py` runs an alternating baseline/candidate comparison and,
optionally, a third integrated build. It preserves every planned attempt, its
actual artifacts, raw observations, resource samples and failures. It is the
first executable foundation for [issue #178](https://github.com/gesher-runtime/gesher/issues/178),
not a claim that the epic's performance targets have been achieved.

## Run a real application

Use a dedicated Windows 10+ machine with **64-bit Python 3.12+**, Windows
PowerShell, the required WebView2 runtime, and unpacked complete Gesher runtime
directories. Supply a local APK whose bytes match an entry in
`tests/e2e/apps.json`. The tool does not build or download runtimes or APKs.

Save a plan similar to this outside the runtime directories. Replace the paths
and the two placeholder source revisions with the actual build information.
The first variant is always the baseline; IDs need not have these exact names.

```json
{
  "schema_version": 1,
  "workload": {
    "id": "rpn-cold-device-x64",
    "kind": "e2e",
    "package": "com.ath0.rpn",
    "apk": "inputs/com.ath0.rpn_18.apk",
    "manifest": "checkout/tests/e2e/apps.json",
    "scenario": "cold-device",
    "abi": "x86_64"
  },
  "variants": [
    {
      "id": "baseline",
      "source_revision": "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
      "artifacts": { "runtime": "runtimes/baseline", "compiler_receipt": "inputs/baseline-build.json" }
    },
    {
      "id": "candidate",
      "source_revision": "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb",
      "artifacts": { "runtime": "runtimes/candidate", "compiler_receipt": "inputs/candidate-build.json" }
    }
  ],
  "measurement": {
    "rounds": 30,
    "warmups": 1,
    "timeout_seconds": 600,
    "sample_interval_ms": 100,
    "minimum_tail_samples": 100,
    "max_regression_percent": 5,
    "collect_resources": true,
    "collect_residency": false,
    "required_metrics": [
      "command_to_first_frame_observed_ms",
      "first_input_to_change_observed_ms",
      "cpu_ms",
      "private_commit_sampled_peak_bytes"
    ]
  }
}
```

```powershell
python tools/perf/benchmark.py plan.json --out results/rpn-discovery-01
```

Paths in the plan are relative to the plan's directory. Every output directory
must be new: concurrent or subsequent runs cannot overwrite an earlier run.
Start with 20–30 measured rounds to inspect noise, then increase `rounds` to at
least 100 for the configured tail gate. Thirty successful launches intentionally
return **insufficient evidence**, rather than a performance pass.

For three-way comparisons add another variant with its own source revision and
artifacts. Adjacent blocks reverse order; three-way blocks also rotate to balance
position. Warmup attempts are separately recorded and excluded from performance
distributions, but failed warmups still fail the run.

### Isolation and workload boundary

Each attempt uses a new profile below its output directory. The maintained
Gesher launcher roots device/application data there through `LOCALAPPDATA` and
`ANDROID_DATA`. No installer runs and the ordinary user's device/profile is not
shut down or reused. Start-menu/URL registration is disabled only for this
isolated fixture (`GESHER_SHORTCUTS=0`). Runtime/application behavior itself is
unchanged. Profile data, logs and screenshots remain as evidence; they may be
large after many attempts.

The adapter installs the pinned APK as **preparation**. In `cold-device`, it then
stops only that profile's new device. In `warm-device`, it retains that newly
prepared server. Neither scenario claims a cold OS/file cache, an already used
application profile, a resume measurement, or an installer benchmark. Artifact
hashing itself warms file caches before measurement.

The adapter and ordinary E2E runner use the same
`tests/e2e/window-workload.ps1` functions. They check an actual first-frame log,
a live window, nonblank/text pixels, input-induced changes, any configured
content-readiness predicates, and normal close/process completion. A known
failure is a failed benchmark attempt. This is the existing visual/lifecycle
smoke workload, not proof of arbitrary APK compatibility.
Missing or geometrically incompatible before/after captures fail the workload;
the pixel-difference sentinel never supplies successful input-change evidence.

The adapter identifies its three actual device processes and the application
by PID **and creation time**. It also discovers WebView2 processes associated
with its fresh profile. The parent checks real process handles for job
membership. Missing identity, inaccessible required coverage, or attachment to
an outside process fails the attempt. It never adopts or terminates such an
outside process. Before the first measured window interaction, the observer
submits live application/device/browser identities in `participants.json` with
an invocation ID and increasing sequence number. The parent checks actual job
membership and creation times, retains the verified process handles, and writes
the matching `participants-ack.json`. The observer waits for that acknowledgment;
it also registers newly discovered participants before normal close. Every
measured capture, configured input and close checks the current HWND owner
against the acknowledged application process generation. Recycled or foreign
windows fail before receiving input or `WM_CLOSE`, including handles retained
in logs. The parent retains each verified registration in raw attempt evidence.

Registration is independent of optional sampling. With resource collection off,
a normally exited application can still be authenticated by the exact process
identity verified while it was alive; no process name or late unverified PID is
accepted as a substitute. Windows services, compositor/driver work and other shared
system activity are outside the owned-tree accounting and require separate
ETW/GPU measurement.

## What the numbers mean

| Field | Measurement and limitations |
|---|---|
| `command_to_first_frame_observed_ms` | Stopwatch before launching the command to detection of the real first-frame log and live window; an **observer upper bound**, not exact GPU presentation. |
| `first_input_to_change_observed_ms` | First posted input to the existing successful changed-pixel/readiness observation; an **observer upper bound**, not input-to-present latency. |
| `workload_ms` | The shared launch/visual/input/close workload, including its original waits. |
| `cpu_ms` | Measured-phase job user+kernel CPU, including exited descendants, with the E2E observer process subtracted. Uses the conservative upper bound of bracketed observer subtraction; `cpu_ms_lower_bound` and `observer_subtraction_cpu_uncertainty_ms` retain uncertainty. 1,000 CPU ms is one core-second, independent of logical CPU count. |
| `read_bytes`, `write_bytes`, `other_bytes`, `page_faults` | Same measured-phase job accounting and conservative observer subtraction. I/O counters are OS process counters, not necessarily physical disk traffic. |
| `private_commit_sampled_peak_bytes` | Maximum aggregate private commit of complete observed live-process snapshots in the measured window, excluding the E2E observer. Sampling can miss short peaks; this is not the sum of individual lifetime peaks. |
| `private_resident_sampled_peak_bytes` | Optional current private working-set sampling with `collect_residency`; otherwise unavailable. Working-set enumeration adds overhead. |
| `handles_sampled_peak`, `threads_sampled_peak` | Corresponding sampled tree totals, excluding the E2E observer. |
| `working_set_sum_including_shared_duplicates_bytes` | Raw snapshot diagnostic only: **not unique physical RAM**. Shared pages can occur in more than one process. |
| `job_lifetime_peak_commit_bytes` | Raw native job peak including preparation and observer. It is deliberately separate from the measured-window private peak. |
| GPU memory, wakeups/context switches, allocation/copy bytes, GC pauses | Explicitly `null` in this round; never interpreted as zero. |

Existing E2E waits remain intact: first-frame polling is 500 ms, there is a
3-second startup pause, input has a 2-second pause, and settling captures are
1 second apart. These observation bounds cannot establish 16.7 ms frame budgets
or submillisecond input latency. Precise launcher/ART/Binder/onCreate/present
instrumentation remains follow-up work.

Preparation ends with an explicit start handshake. The parent snapshots counters
before acknowledging, then snapshots again before acknowledging stop. Timeouts
cover the **whole attempt**, including preparation; neither hangs nor missing
frames can count as fast successful launches. Raw collection durations and
non-atomic/incomplete snapshot counts are retained. Incomplete snapshots are
excluded from sampled peaks, with their count visible in the evidence.

The Windows collector itself is outside the job. The E2E observer script must be
inside the ownership tree to launch/contain all runtime descendants; its own
CPU/I/O/fault counters are bracketed and subtracted, and its memory/handles/threads
excluded. This does not remove perturbation that screenshot capture or observation
causes **inside** the application. Repeat a suitable comparison with
`collect_resources=false` and resource metrics removed from `required_metrics`
to investigate sampler overhead. Parent sampler CPU alone is not total overhead.

## Evidence and gates

- `frozen.json`: resolved plan, actual SHA-256 inventory of every runtime file,
  APK/manifest/optional compiler or driver receipts, harness hashes, host and
  observed Windows hardware/driver metadata. Directory inventories reject links.
- `runs.json`: the full planned denominator checkpointed before execution, unique
  attempt IDs, statuses and result hashes. Interrupted runs remain incomplete.
- Each attempt: request, start/stop handshakes, stdout/stderr, raw workload result,
  acknowledged live participant registrations, all resource samples and
  collection errors, application logs/screenshots and
  the isolated profile.
- `summary.json`: measured success/failure counts, p50/p95/p99, paired-block
  bootstrap ratio intervals and the final gate. p99 is descriptive only.

Supplied source revisions are explicitly **declarations** unless backed by a
separately verified build receipt. Byte hashes prove artifact identity, not
which source built it. Driver metadata hashes likewise do not pretend to be
driver binary hashes. Add compiler/image/driver provenance files as named
artifacts when available. Missing host collection is recorded as an error,
not replaced by invented hardware data.

Files are rechecked before and after attempts. Changed artifacts, stale/wrong
invocation IDs, missing or failed evidence, nonfinite/negative/out-of-order
timings, missing required metrics, timeout, nonzero exit and cleanup failure
all fail. All attempts, including failures, remain in the denominator.
Successful samples alone enter distributions; failure still prevents the
comparison from passing.

For each required metric, p50 and p95 ratios use 1,000 deterministic paired-block
bootstrap resamples and percentile 95% intervals. Passing requires enough paired
samples and the upper interval within the configured regression allowance.
A lower interval above the allowance is a regression; overlap is inconclusive.
Intervals do not account for systematic drift, thermal throttling or faulty
workload design. Baseline-zero ratios remain unresolved. There is no p99
precision claim and no automatic claim of improvement.

Exit codes: **0** gates passed, **1** failure/regression, **2** insufficient or
inconclusive evidence. A successful small smoke run is therefore normally 2.

## Other workloads and validation

Portable `kind: "command"` workloads use a shared argv array with `{python}`,
`{attempt}`, `{variant}` and `{artifact:ROLE}` substitutions. They receive the
same request and must participate in the start/stop handshake and write the
versioned result. `fixture_workload.py` demonstrates the protocol explicitly as
a fixture; it never supplies application performance evidence. On Linux,
Windows resource metrics remain unavailable. Windows runs still use an owned
Job Object even when collection is disabled.

Each live mailbox (`phase.json`, `ack.json`, `participants.json`, and
`participants-ack.json`) has one writer. Publish a complete temporary file and
replace the mailbox; never update the visible JSON in place. Windows readers
must share deletion/rename access while retaining their opened snapshot. The
provided Python `protocol_io` helper and PowerShell adapter use native
replacement for existing mailboxes, with no delete-then-move interval. They
retry only native Windows access/sharing errors 5 and 32, at most five I/O
attempts with four delays of up to 10 ms, inside the caller's original deadline.
Persistent denial remains a failure; generic permission errors, malformed JSON,
identity/phase/sequence mismatches and ownership failures are not recovered as
successful transport. The message names and payload contracts are unchanged.
Custom workload implementations must provide compatible file sharing, or fail
closed if their transport cannot complete within the existing attempt deadline.

Run portable contracts with:

```sh
python tools/perf/test_contracts.py -v
python tools/perf/test_protocol.py -v
```

On Windows, also run `python tools/perf/test_windows.py -v`. These exercise real
child/grandchild ownership, an exited root, retained accounting, unrelated-process
rejection, startup failure and abrupt controller-death cleanup. The Windows
suite also runs an actual child registration and normal exit with
collection disabled. `tests/e2e/performance-ownership.test.ps1` exercises the
maintained window workflow against explicit foreign/recycled-window boundaries;
the shared readiness test covers missing and incompatible input captures.
The dedicated `performance-contracts` workflow runs portable tests and shared E2E regressions
on Linux/Windows and real job tests on Windows 2022/2025. These fixture results
prove tooling contracts; they are not real Gesher/APK benchmark results.

### Windows API basis

Creation-time job assignment uses documented
[`PROC_THREAD_ATTRIBUTE_JOB_LIST`](https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-updateprocthreadattribute).
The handle inheritance list contains only standard I/O handles; the kill-on-close
job handle is not inherited and breakaway is not enabled. Cleanup uses
[`TerminateJobObject`](https://learn.microsoft.com/en-us/windows/win32/api/jobapi2/nf-jobapi2-terminatejobobject)
and waits on real owned process handles rather than later numeric PIDs.
[`JOBOBJECT_BASIC_ACCOUNTING_INFORMATION`](https://learn.microsoft.com/en-us/windows/win32/api/winnt/ns-winnt-jobobject_basic_accounting_information)
includes CPU and faults from terminated as well as active members;
[`JOBOBJECT_EXTENDED_LIMIT_INFORMATION`](https://learn.microsoft.com/en-us/windows/win32/api/winnt/ns-winnt-jobobject_extended_limit_information)
supplies the distinct native lifetime peak. Process commit/working sets are
collected with `GetProcessMemoryInfo`, optional private residency with
`QueryWorkingSet`.

## Still required for PERF-00 acceptance

Representative actual Windows A/B reports and calibrated budgets; exact pipeline
timestamps and ETW/GPU/GC/copy counters; observer-overhead experiments; controlled
cache/resume/installation scenarios; x64/translated/game/media/provider/background
coverage; 1/5/10 applications and memory pressure; 30-minute activity and 8-hour
soaks. Keep #56 compatibility gates and every existing product gate active.
