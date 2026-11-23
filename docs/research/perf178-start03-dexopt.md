# Issue #178 START-03: profile-driven background dexopt

This round implements the scheduler, cancellation and publication part of
[START-03](https://github.com/gesher-runtime/gesher/issues/178). The original
baseline is `8b316aebf09ab46d5407e955bf86ed748a1e739f`. The workstream remains
open until the Windows execution and representative foreground latency gates
below are verified. This document does not claim completion of the 42-workstream
epic or its aggregate CPU, RAM and soak targets.

## Observed baseline defects

`Dexopt.startBackground` scanned and sorted the complete installed-package map
every second while idle. It recorded a profile timestamp **before** running
profman/dex2oat. A failed compilation of an unchanged profile was therefore
suppressed indefinitely. Profman changed the live reference profile before
compilation succeeded, and successful compilation truncated the current profile
even if the profile saver had appended newer data meanwhile. Tool completion
used repeated 50 ms exit-code polling. Static synchronized compile methods held
the class monitor through all process execution.

## Behavior and ownership

`DexoptProfileWatch` owns one recursive native `ReadDirectoryChangesW` stream.
Native I/O is armed before publishing the initial snapshot. Profile writes are
coalesced by package and a monotonically increasing profile revision, including
same-size/same-timestamp writes. Membership changes, explicit overflow and
invalidated watches cause reconciliation. Registration failures are logged and
retry with a 1–60 second backoff; a working, clean watcher has no scan timer.

The queue contains at most one entry per installed package. There is one compiler
worker and one watcher consumer; the native directory stream owns three handles
and one 32 KiB buffer, independent of the package count. The host test exercises
100,000 notifications across 500 packages while retaining only 500 pending
entries. The directory stream waits directly on Windows events, avoiding the
existing inotify/socketpair mixed-descriptor 10 ms polling fallback identified in
NATIVE-01. Independent review caught that regression in the first WatchService
design; that design was replaced before publication. Native I/O is rearmed before
returning each batch, and close cancels/drains pending I/O before freeing buffers.

Activity/launch and foreground-service changes publish a notification and signal
the compiler cancellation event. They never wait for a compiler while holding an
activity/service lock. A single fair compiler lock serializes tools, including
install-time compilation. The queue/publication monitor is not held during
parsing or compiler execution. Installer withdrawal invalidates an in-flight
installation generation, and an older registry snapshot cannot resurrect it.
Profile revisions are separate: a saver write during compilation keeps the
captured snapshot valid and queues the newer revision after successful publication.
Continuous writers go to the queue tail so other packages continue to progress.

Background profman receives private copies of the current and reference profiles.
Only a successful compiler publication installs the new reference profile. The
current profile is retained: a concurrent saver write is never discarded by
truncation. A compiler error, cancellation or failed publication remains pending
and retries on a bounded deadline. Installation withdrawal rejects stale output;
its eventual replacement receives a new identity even at the same code path.
No-data/empty profman results acknowledge only the captured profile revision. The existing JIT,
profile saver, app images, compiler filters and APK execution fallback remain.

## Windows compiler policy

Background children start suspended. Before resuming, production spawning applies:

| Resource | Policy |
| --- | --- |
| Compiler workers | One process, `-j1`; install-time `-j4` is unchanged |
| ART compiler heap | Existing `-Xmx512m` ceiling preserved |
| Child committed memory | 1,536 MiB hard process limit in a private kill-on-close job |
| CPU | Below-normal process priority and a 20% job CPU ceiling |
| Page priority | Windows `MEMORY_PRIORITY_LOW` |
| Admission | Defer background tools below 1,024 MiB available physical memory |
| Running pressure | Wait on the real Windows low-memory notification and cancel/reap |
| Deadline | Existing 10-minute tool deadline, with blocking completion wait |

The CPU ceiling is relative to the parent job's allocation if an ancestor job
already limits CPU, as documented for
[nested CPU rate control](https://learn.microsoft.com/en-us/windows/win32/api/winnt/ns-winnt-jobobject_cpu_rate_control_information).
Committed-memory limits reject excess commit; they are not working-set/RSS
measurements. Priority is supplied through `CreateProcess`, avoiding the elevated
base-priority privilege required by
[`JOB_OBJECT_LIMIT_PRIORITY_CLASS`](https://learn.microsoft.com/en-us/windows/win32/api/winnt/ns-winnt-jobobject_basic_limit_information).
If Windows refuses a mandatory resource configuration, the child is reaped and
the work retries; it is not silently run without the budget.

`WaitChild` duplicates the retained child handle after checking its creation
identity, then waits outside the child registry lock on completion, cancellation,
memory pressure or the actual deadline. Cancellation and timeout terminate and
reap that pinned child before releasing the compiler slot. Handle state, rather
than the numeric exit value 259, decides whether a completed process is reaped.

## Validation and remaining acceptance

Local validation:

- Full `app/build.sh` compilation and DEX generation against Android core and
  framework classes completed. Cached framework inputs are compile dependencies,
  not evidence of candidate Windows execution.
- `tools/test_dexopt_work.py --java-home <JDK21>` passed 45 main checks and 12
  concurrent profile-progress checks through the
  unchanged production scheduler, watcher and profile/artifact transactions.
  Android/Windows tool processes, directory notification input and APK parsing
  are explicit host boundaries. The notification adapter uses real host
  filesystem events; it is not evidence of the new Windows native backend.
  Checks cover registration failure recovery, deletion/recreation, writes and
  unchanged clean state. Independent QA reproduced a progress defect in the
  first candidate: three successful tools were discarded when each overlapped a
  profile saver write. The added scheduler regression requires every captured
  snapshot to publish, newer data to stay pending, and eventual publication of
  the latest data. It also checks that staging/reference/artifact/log output
  outside `misc/profiles/cur/0` cannot feed the current-profile watch.
  The new progress regression rejects the unchanged first candidate
  `796d44110a093633276bc3fb3cd98a21b43558c1` at the installed-identity assertion.
- The same transactional test with `--source-ref 8b316aebf09ab46d5407e955bf86ed748a1e739f`
  fails on the baseline's modified reference profile after compiler failure.
- Existing child-lifetime tests passed all 24 checks with ASan/UBSan. Local
  LeakSanitizer is unavailable under the executor's tracing environment; only its
  leak detector was disabled for that run.
- The complete Windows process implementation plus
  `tests/native/dexopt_child_wait_test.cc` cross-compiled with warnings as errors.
  Cross-compilation is not Windows execution.

Both `windows-host-contracts` jobs now build and run that native fixture against
real children. It checks exact exit/identity, queued and active cancellation,
timeout reaping, job CPU/commit/page-priority configuration, actual excess-commit
rejection and stable handles over repeated children. It also checks recursive
directory notifications, explicit refresh, clean blocked-wait CPU, close/read
races and repeated watch retirement. The existing Windows ART
`61-aot2` path continues to cover actual profman/dex2oat and the next app-image
launch. Fresh candidate CI must verify these gates.

No agreed foreground-launch p95 limit or representative 1/5/10-app Windows
benchmark has been supplied or measured here. The 20%/1.5 GiB budgets are explicit
initial policy bounds, not measured optimal tuning. Large-app throughput,
representative peak memory, launch-tail latency under compilation and sustained
pressure/soak remain required evidence before marking START-03 fully complete.
