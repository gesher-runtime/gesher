# Private admission lifetime corrections: source only, unqualified

Exact source: `3c1c066de20c4a91516f21402f0f96f9ebbd8fea`. Clean parent: `0e1f7bec05d85ea85ee3f6d68dd90cbc704779ac`;
prior active source `cc13e24c288f550bb9134b3062bf6b886061e38c`.
Exclusive checkout: `C:/work/gesher-binder-peer-identity-current`.
Independent peer read: `C:/work/gesher-ci-review/binder-private-admission-peer-high-cc13e24c.md`
and JSON; report SHA256 independently matches
`bc004331ecc7c2ed9b270cf70ef67446547b83f74e495bab2802073620adbf6f`.

Disposition: two concrete P2 source corrections submitted for new independent
review. No original P1, global CLI/platform closure, runtime acceptance, AOT
qualification or Windows sandbox claim. Exact rebuilt supported-device AND
mandatory legacy route regressions, followed by a fresh full-after, remain owed.

## Exit completion and cleanup

ExitStatus uses only the retained actual CreateProcess HANDLE under the native
child-map lock. WAIT_TIMEOUT is running (-2); WAIT_OBJECT_0 permits a subsequent
GetExitCodeProcess query and removal/close; any other wait or query failure is
unknown (-1) and retains the instance for retry. Terminal values are nonnegative
64-bit unsigned DWORD values, including259 and4294967295. Neither is a running
or unknown sentinel. The JNI signature is (I)J. All current internal pollers
(ATM preattach watcher, postdeath observer, Dexopt, both new fixtures) use this
status. The old public ExitCode/nativeExitCode/exitCode APIs remain ambiguous for
external binary compatibility; no current internal poller calls them.

ATM publishes only nonnegative terminal status. Unknown before attach fails the
pending start closed without inventing a terminal status; the later observer
retries unknown/running for its existing bounded interval. ATM/ProcessExitStatus
and tool result APIs still store signed32-bit code bit patterns; 0xffffffff is
represented as int -1 at those legacy result boundaries, not as an internal
liveness decision. Expanding the external result protocol is outside this fix.
Dexopt changes only its two polling lines; execution, timeout, command generation
and all existing Exec/AOT assertions/drivers/workflows remain unchanged.

## Registry/transaction ownership

Control-worker maintenance calls Registry::PruneDead under g_lock -> Registry
lock. It erases only actual WAIT_OBJECT_0 records, including never-connected
published children; unknown API results do not authorize admission or prove death.
Pruning marks the record revoked before erasing the map owner. Existing local
shared leases, including an outstanding Challenge, keep that HANDLE valid until
their last use. There is no PID reopen or UID-startup authority fallback.

ProcReleaseLocked retains its lease through thread pending-buffer rollback,
transaction failed/dead replies, node/ref/work cleanup and all remaining buffer
FD cleanup. Remaining buffers now release objects while proc is_dead skips dead
Binder node/ref updates; FD staging cleanup still uses the retained lease. Only
after this cleanup and map removal does proc->launch.reset run. Raw Proc/Thread
objects remain allocated for pre-existing cross-thread transaction references.
ServeProc/ServeThread admission-local leases are reset after proof/installation,
so a blocked or still-open accepted pipe cannot independently pin a revoked
process indefinitely. Outstanding proof leases still last through proof return.

Publication rules, exact package/generation revoke, cancellation tombstones,
private writer death invalidation, challenge event duplication/ack/final liveness,
64-proof budget and existing lock direction remain intact. Unknown API failures
stay fail closed. Tombstone memory/proof budget policy limitations remain open.

## Real-process negative controls added, not executed

The native fixture source now contains actual owned ExitProcess(259) and
ExitProcess(0xffffffff), normal pending controls, actual retained-child kill137,
StartTime/OwnedInstall dead rejection, exact terminal observation and subsequent
unknown-after-consumption. Existing exit0 controls remain. It also contains48
production owned spawn/exit cycles for one unchanged install, alternating real
Binder proc/thread connections and no-Binder-open children. The writer queries
its actual inherited broker HANDLE via GetProcessHandleCount; barriers and bounded
settling check broker and writer kernel handle counts return to baseline.

An explicitly cancelled still-live suspended child's exact birth cannot republish.
A separate real process tests Registry prune while an outstanding shared lease
keeps the signaled HANDLE valid, then checks the last lease drops the one process
HANDLE. The existing generation-revoke control additionally holds accepted pipes
open in a live child while checking broker handle count drops after revoke.
Per-process cooperative proof-event opened/closed counters and private process
transfer counts are emitted without locators. Kernel totals include process,
event and pipe capabilities. These are aggregate counts, not an exact typed
broker allocation/close trace or proof that every production error path balances.

Existing private wrong birth/system UID, forged public UID/birth/PID, unregistered
self, wrong-generation revoke, revoke wake/dead dispatch, live reinstall refusal,
terminated publication rejection and private-writer death controls are preserved.
No fixture, compile, configure, test, CI or rebuilt artifact was executed or
created. Outer watchdog still required. Actual ATM preattach failure/exit259
publication, FD rollback/dead-reply under concurrent revoke, delayed challenge,
parallel cancel/revoke, failed Win32 API and handle-count controls remain required
runtime evidence; this source fixture alone does not qualify them.

## Mandatory compatibility gap and peer questions

Fresh unrelated CLI Launcher VMs, toast activation, old cohost Main/Exec/install/e2e
routes and direct legacy binderd/SystemServer/SG/Native harnesses have no inherited
registration authority and still fail closed. Secure host-command/harness
bootstrap adaptation and their complete regressions are mandatory future functional
work. Do not drop these routes or assertions to claim P1 closure. No filtered b52
ACK-loss or public WriteRead-prefix relay case is added or run.

New independent exact-source review must check:

- shared challenge versus prune/revoke/ProcRelease concurrency and lock ordering;
- all FD staging rollback and transaction buffer references before lease reset,
  including live revoke with accepted open pipes and rejected proof paths;
- Win32 wait/query failures, true259 and all-ones terminal code across JNI/ATM;
- fixture handle baseline stability and whether additional typed production
  allocation/close instrumentation is needed before lifetime qualification;
- unchanged private publish/handoff/install-generation/tombstone/death authority
  and bounded proof policy, plus unsupported CLI/legacy adaptation scope.

Source-only validation: git diff --check passed; all ten changed source paths
are focused lifetime/fixture paths. Eight other prior declared source blobs match
exactly. Dexopt.java is the one additional path, limited to polling migration.
Original SG9/malformed/FD rollback and Native13 assertion actors/drivers/workflows,
Exec sources and AOT behavior/assertions remain unchanged. Existing source17 plus
Dexopt18 are recorded below. No local native/Java/C# compile/configure, fixtures,
CI, push, GitHub write, downloads, artifacts, other-tree edits, cleanup or agents.

## Exact source blobs

| Path | Blob |
| --- | --- |
| app/java/com/gesher/app/Launcher.java | 97e4c9a2ed205653078deddf1f9ade32b9fba753 |
| app/java/com/gesher/server/ActivityTaskManagerService.java | 07250c4b7daba2efc47a16db9c97f3ac9886d6e2 |
| app/java/com/gesher/server/SystemServer.java | 2f032a7bd4128932c400ebbc856c1848fae60fd1 |
| app/java/com/gesher/server/Zygote.java | 6e9ac24346d6ee8fdc3c49522b4121727456c76a |
| cmake/aosp/servicemanager.cmake | 9a5e82b08e6821ada0716ae550987d19d74c093a |
| compat/binder/binderd.cc | 8484b5ba7f5b70a21b18c97eafe7f631ac3ee986 |
| compat/binder/launch_control.h | 7c1be3f6e5fc0b2a8dcf9ee5a985554aa5669d7f |
| compat/binder/launch_registry.h | 345114afe0d923ea6f4c22ff1973f953020c8414 |
| compat/framework/services_natives.cc | b774de7c19b5af200c5b7621cb9472c62bddee60 |
| compat/framework/services_zygote.cc | 69258265fd5cbecda82139d5cae707d83d1b788c |
| compat/framework/services_zygote.h | 28662ec825bf37f508ae18032a44c7101ed7b22f |
| compat/posix/src/binder.cc | 91982ec527f1567b72f396c9c914c5d25d7b37f5 |
| compat/posix/src/binder_wire.h | 13a0d985396998ea9caff67706909f7e6232fa28 |
| compat/posix/src/launch_context.h | b6fe600aaae6d2f2f60be4294278c2412cd5ad67 |
| tests/dex-package-identity/com/gesher/server/PackageIdentityFixture.java | 5fbecbcbe8981b5047d8759b07965609a8666a38 |
| tests/dex-package-identity/com/gesher/server/PackageUidChild.java | 3df906657f3bf81302647a60b646ae9def837626 |
| tests/windows/binder-private-admission-fixture.cc | e70e0160d574d0333187da50e271521f18408d5c |
| app/java/com/gesher/server/Dexopt.java | 79f22e41a5e2f1555ba0b75e75d067588fca267d |
