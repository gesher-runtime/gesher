# Proposed kernel-completion TLS ownership — NOT activated

Actual source02b882/run410884:28 jobs,26success/2deferred-C++failure.
Published and private-macroOFF preserve identity in all four genuine cases on
both hosts. Deferred owned pthread return changes1→3 and pthread_exit4→5 in
both TU destructors on the original native thread, while completions are1/1,
join kernel sample0 and std::thread/nativeExit61 remain positive. Actual
compiler symbols are __cxa_thread_atexit/__mingw_cxa_thread_atexit. There is no
blanket original-compiler TLS defect and no early-join failure in this run.

The deferred wrapper clears TLS at3102 before _endthreadex; simply removing that
clear would expose a recyclable record to later callbacks. The following is the
whole proposed protocol for the changed deferred-only source, pending code peer.
Old supplier/fault patches and all seven old role expansions remain unchanged.

## Ownership and state

Append three private deferred-only fields after existing x: tls_owner_refs at544,
kernel_finalized at548 and retire_next at552 (calculated size560/align16; actual
CI must print and strictly verify every old/new field). This preserves existing
field offsets. These are not a public ABI or production capability change.

Each owned thread gets tls_owner_refs1 before native Resume/publication. Foreign
attachment gets it before TlsSetValue/public return. The record's cpu_handle is
its stable native completion handle: only final push closes it; bookkeeping h
aliases do not. Existing cleanup_refs and operation_refs keep their distinct
callback/operation ownership; neither substitutes for the new TLS lifetime.

lifecycle1=LIVE; lifecycle3=EXITING/queued; lifecycle2=kernel-completed ENDED.
prepare_exit runs after target-thread user cleanup, under registry→state. It
disables cancellation, consumes any pending count exactly once and changes1→3.
It does NOT publish ended, clear TLS, close events/clock/spin state, deregister x,
or release the TLS owner. Repeated preparation is idempotent; only the first
transition enqueues the record. External cancel rejects lifecycle!=1, while late
self/get/setspecific sees the same still-owned TLS record and resources.

Owned wrapper releases its ordinary cleanup_refs after preparation as before,
but transfers the independent TLS owner to the queue. Foreign pthread_exit and
native TLS cleanup also prepare without clearing TLS. The native hook skips
already-completed user cleanup on lifecycle3; an unprepared native ExitThread
still performs its existing cleanup on that exiting thread first. No user
callback runs on the reaper. OS thread termination discards the TLS slot; the
worker never attempts to clear another thread's TLS.

## Queue and normal startup

One EXE-owned diagnostic worker, explicit normal-main start, BSS-null atomic
incoming head and preallocated retire_next. No allocation, loader lookup,
provider wait or worker startup in enqueue/TLS. Published wake event is retained
across thread-create failure/retry. Worker first drains early records, then
samples each pending cpu_handle with timeout0 outside all provider/state locks.
It never blocks on one unfinished target: pending work uses a finite10ms wake
wait outside locks, empty work waits on the process-lived event. Actual kernel
completion wakes ordinary joins directly; the worker's next finite scan owns
retirement. No callback waits for worker progress. Process termination and
FastExit must not depend on real_main returning.

The queue owns the TLS ref until the node is removed from its incoming/private
pending list. Capture next before any last release. tls_owner_refs prevents the
CPU handle/record being reclaimed even when join finishes before worker scan.
Do NOT reuse existing next: push_pthread_mem returns early for next!=NULL.
New retire_next is exclusively queue-owned after publication.

## Completion and resource helpers

finalize_kernel is idempotent, called under registry→state only after a real
cpu_handle timeout0 result signals. It publishes kernel_finalized+ended2 and
consumes pending accounting if still present. State unlocks BEFORE resource
destruction. Event close and target-clock destroy occur once through that
finalization; operation_refs continue to defer clock destroy. Spin-key state
retires only at the final deregistered pool transition, not early finalization:
external key deletion can still initialize/touch an ended joinable record's
spin lock. Final pool retirement holds the registry, removes x and has no live
TLS/operation/cleanup owner, preventing a later scanner from recreating it.

gesher_close_start_event and gesher_destroy_target_clock must defer while a TLS
owner exists and kernel_finalized is false. The latter keeps clock_destroy_pending.
gesher_target_release must not abort merely because clock retirement still has
the TLS owner: leave it pending until kernel-finalization permits destruction.
push_pthread_mem must not deregister x or recycle a record before actual kernel
completion while tls_owner_refs is nonzero. After kernel completion it may
deregister an API-consumed ID while retaining the queued record/CPU handle until
the final TLS/cleanup/operation owner drops. No generation is reused while a
queue link can still refer to the record.

Worker and join/tryjoin may race to finalize after actual kernel signal. The
registry serializes the exactly-once kernel_finalized transition. Join/tryjoin
invoke that common finalizer, then perform their existing claim/return-value
consume and push; no duplicate close/destroy blocks remain. Detach during held
callbacks changes bookkeeping ownership only and leaves ended0. Detach after
finalization only consumes already-finalized ownership, not a second cleanup.
If join precedes worker release, push sets pending retirement with the queue's
TLS ref intact; worker then removes its node, releases that ref, and completes
pending retirement only when operation/cleanup refs also allow it.

Required full map before freeze: all3 cpu_handle writers/closer, all event-close
and clock-destroy helpers/callers, both push preconditions and x deregistration,
all lifecycle/p_state/ended readers/writers, cleanup refs, join claim/unwind,
tryjoin, detach, target leases and native/foreign/normal finish. No state→registry
edge, native wait, loader call, allocation or user callback under state.

## Actual fixture requirements

Keep every original21 gate and the four genuine C++ cases, strict last-action
TU completion, identity, join-return versus kernel observations and original+8
handle bound. Explicit bootstrap/drain is diagnostic, separately attributed.
Add a genuine TU destructor-held crossing (no manufactured callback order):
actual callback is held while worker scans see its kernel unsignaled and main
performs validated lookup/tryjoin and concurrent join/detach ownership attempts.
Before release original x/resources remain valid, no kernel-finalized/retired
record; afterward actual kernel termination, exactly-once finalization and final
owner release. Separate successful detach case if required to avoid joining a
detached target. Do not replace the real callback-held crossing with a counter.

Canonical supported-entrypoint bootstrap, standalone DLL owner adoption, module
pin failure, general async entry and complete supplier/consumer adoption remain
open. This source proposal does not activate the process-owned worker in product.

## Concrete writer, ownership and lock map

Only the separate deferred delta changes thread.c/thread.h. The supplier/fault
patches and all7 old role expansions remain byte equal for Linux LF inputs.
The new fields are absent from every other role's actual layout; four new hooks
must be absent from their static/import archives and DLL exports as well.

* cpu_handle writes: pop zero/init, foreign attachment under registry, successful
  owned create while native thread is suspended; sole close is final pool push.
  The TLS owner pins it while the worker samples outside locks. CPU generation
  is assigned by pop and never changes before actual pool/memset.
* evStart: create before owned publication or foreign attachment; all destruction
  passes through close_start_event. Before kernel it defers for TLS owner. The
  one common finalizer closes it after real kernel signal. An operation owns
  its independently duplicated event and can finish after original event close.
* p_clock: wrapper initializes/uses before body. All destruction passes through
  destroy_target_clock; TLS owner before kernel or operation_refs defers it.
  target_release performs pending destruction only after both conditions allow.
  No finalizer acquires a target clock while holding state. The diagnostic
  snapshot unlocks registry/state before actually locking its pinned clock.
* lifecycle/ended: initialization1/0 before start; prepare under state changes
  only1 to3 and consumes pending request; common kernel finalizer under state
  changes3 to2/ended1 exactly once. Existing cancellation checks lifecycle1.
  Join/tryjoin use actual native signal, not ended as substitute. Detach uses
  state-held ended only to select the already-completed consume branch.
* join_claimed_by: registry then state at capture/claim, commit and cleanup
  unwind. Queue release/finalization also holds registry. The queue's TLS owner
  prevents API consumption from recycling a queued or popped record. Each
  final push has no cleanup/operation/TLS owner and deregisters before reclaim.
* key arrays and Spin: live callbacks use their original record; key deletion
  holds the existing key lock then registry/scanner Spin. Worker holds registry
  for final deregistration and Spin destruction, without holding state or the
  key lock. It cannot race a scanner which has not acquired registry. No user
  callback is called by the worker. Original owned cleanup remains on its
  original target-thread path before preparation, with one platform256-pass
  budget, never another budget under loader teardown.

The only state nesting is registry to state. State holders perform finite field
operations and event signal/reset; they never acquire registry, run callbacks,
allocate, load/unload modules, or wait for target/worker completion. Worker
WaitForSingleObject CPU samples and event waits occur outside provider locks;
join target/actor waits retain independent leases and cleanup/unclaim ownership.
The existing registry is recursive; helper re-entry does not add a new lock.

The held genuine callback tests original self/get/setspecific, real worker
unsignaled scans, actual concurrent join claim or detach, failed extra consume
attempts with unchanged outputs, existing kill-before-detach semantics, external
key deletion of the residual256-pass early key, and actual clock/event/Spin/CPU
resource witnesses. After release/kernel signal, watched generation counters
must finalize/release/clock/event/Spin/pool exactly1 and queue0. No counter reads
recycled target storage. Process owner handles2 are counted inside the original
total+8 limit, rather than adding a new allowance.

## Explicit limits still requiring work

The new owner starts only by an EXE normal-main diagnostic hook. It does not
solve canonical supported-entrypoint bootstrap, standalone DLL owner adoption,
module callback lease startup or general async entry safety. No worker stop or
process shutdown wait is added. Real FastExit/process termination remains a
separate entrypoint contract.

The owned phase3 held callback additionally enables cancellation, requires
self-cancel to return ESRCH without pending work, calls testcancel successfully,
and restores the original disabled state. Self/external cancel share lifecycle1
admission; testcancel/shallcancel/state/type delivery also require lifecycle1.
Direct invoke aborts before any counter mutation if no longer LIVE. Recursive
pthread_exit checks state BEFORE ret_arg/TSD cleanup/longjmp and aborts at an
explicit diagnostic-only unqualified boundary; it never recursively terminates
the CRT. These are not new product error-return policies. Full late-control
semantics remain open. In particular foreign std/native C++ callbacks may run
BEFORE the provider hook, so their lifecycle1 does not establish body context.
The owned phase3 guard cannot be generalized to those callbacks. An earlier
marker needs actual callback-order evidence and preserves the original owned
versus foreign cleanup location/budget. The POSIX pthread_exit destructor
recursion rule is not silently extended to arbitrary C++ interoperability. Likewise the observed
late-created TSD callback count is not a conformance result. The candidate does
not promote DEFERRED_ONLY or change the original async/product contract.

Source-only checks reconstruct all8 ordered patch stacks from the exact11 pinned
upstream files, with git core.autocrlf=false to match Linux CI. Seven old source
expansions and original21 YAML job definitions remain unchanged. The trace gate
has183 metadata ablation/conflict/isolation negatives; old11 provenance parser
negatives and both actual410871 archive/member replays remain passing. None of
these source checks is a substitute for actual560 layout or Windows execution.
