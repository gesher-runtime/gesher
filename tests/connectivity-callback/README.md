# Connectivity callback host checks

`tools/test_connectivity_callback_contracts.py` compiles the changed production
ConnectivityManager and ConnectivityService. `--case timeouts` checks removal of
an Integer.MAX_VALUE-millisecond deadline after unregister and first availability,
including the initial online snapshot. Existing tests cover stale timeout delivery,
callback reuse, loss after satisfaction, reentrant unregister and service teardown.

The controlled Handler uses a real Android legacy MessageQueue linked list;
Handler.removeCallbacks and MessageQueue removal/recycling execute unchanged.
Enqueue/dispatch is deterministic and tracing is disabled at explicit JNI seams.
No native poller, real Looper scheduling, Windows network discovery or cross-process
Binder behavior is claimed by the host run. Windows ART fixtures additionally run
their existing actual HandlerThread and generated Binder/Parcel checks.

On Windows pass `--cc <llvm-mingw C compiler>` and a Windows `--java-home`.
The unrelated notification descriptor read seam returns ENOSYS on that host.
The baseline control is `--source-ref origin/main --case timeouts`; it fails at
`unregister retained a long deadline` before the fix.

## Connectivity observation ownership (JAVA-03 leaf)

The default `all` case also compiles and executes the actual ConnectivityService,
StickyBroadcasts, BroadcastController and JobSchedulerService. Pass
`--apksig-jar <apksig.jar>` for the constructed package metadata. The existing
framework CI job's verified `$RUNNER_TEMP/gesher-apksig.jar` is also recognized,
as is `APKSIG_JAR`; this adds no download or substitute dependency.

`--case demand` covers observer release/reacquisition, remote death-link failure
and cleanup, late death of a retired registration, 10,000 sequential and 2,000
concurrent attach/remove cycles, current sticky-only queries, actual receiver
registration/unregistration/process death, native-event handover to a running
JobService, resume without an interface event, network-loss stop reasons,
pending/periodic/retry/finished/canceled job ownership and callback reentrancy.
Overlapping resume signals keep one dispatcher and one pending refresh bit.
An unchanged-IP cost change still reaches active observers through the retained
15-second fallback boundary.

`--case idle-trace --trace-ms 600000` measures ten real minutes after the last
client leaves and the final wait is drained. The production Java watcher uses
its real 15,000 ms timeout. The JNI boundary waits on a host monitor and supplies
controlled network snapshots. Counts are actual calls to that boundary, not a
model extracted from the implementation. Use
`--source-ref 751d82310f7c4c470c098ccebfd5d2d04699a0cd` for the negative control:
it retains the watcher and produces 40 reads/waits during the interval. The
candidate produces zero reads/waits and releases its watcher. This is not a
Windows network-notification, whole-process CPU, RAM or wakeup measurement.

The fixture supplies local Binder endpoint/death behavior and write-size-only
Parcel accounting for broadcast delivery. It does not simulate persisted job
deserialization. The existing Windows/ART `JobSchedulerConstraintContractTest`
additionally writes a real persisted network job, restores those produced bytes
through a fresh scheduler's `start()/loadPersisted()` path and verifies final
cancellation removes both disk state and network ownership. Local app/services
DEX compilation does not establish that target execution passed.

See `docs/research/perf178-java03-connectivity.md` for the native retirement
boundary, input provenance, results and remaining acceptance.
