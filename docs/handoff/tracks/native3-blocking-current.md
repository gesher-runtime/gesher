# Native blocking finite internal source checkpoint, 2026-10-28

Base is qualified phase4 f1550540186b4ae3f17a5e501a886ec1c4d5d613
(accepted code81fd3eb1, full40731915058, actual38 jobs32success/six skipped).
Exclusive worktree C:/work/gesher-native3-blocking-current, branch
wip/native3-blocking-current. Source checkpoint only: no local native build,
configure, syscall fixture, CI dispatch, download, push, primary merge or
runtime acceptance. The exact code blobs are in
tools/native3-locks/blocking-source.json; the documentation checkpoint follows
the code commit so its full identity can be recorded without self-reference.

Read ADR0080 and Native lock handoff directly from frozen ec3d58a8 because
those documents were not included in the selected integration port. Read the
canonical external remaining-software-work-queue-2026-10-28.md, selected156
manifest and independent407319 selected audit in C:/work/gesher-ci-review.
The original161 raw reference comparisons,13 real duplicate modes, stdio
five-second conjunction, current seven fixture outputs, accepted Exec ordered
spawn, Binder/Media/AOT fixes and existing supplier guards are unedited.

## Implemented boundary

WaitProcessLock is an internal process-owned, finite (1..60000ms) operation
over an immutable validated F_SETLK snapshot. An already opened per-thread
Registry belongs to the caller. Caller retains a real file anchor and a live
manual-reset cancellation event, and must release DescriptorGuard before
entry. There is no fd-number lookup after entry and no shared local registry
buffer. Public dispatch is deliberately unchanged: common command7/38 returns
EINVAL at its existing default; bionic command7/38 explicitly returns ENOSYS.
Neither has public blocking behavior. Current signal.cc queues asynchronous
signals without handler delivery. Cancellation is ECANCELED, never EINTR.

Each conflict registers its actual PID/TID plus process/thread creation
identity, random128-bit token and requested owner/file/range in the existing
validated registry. A secured, noninherited, per-request Global manual-reset
event is named by that random token. Commit publishes first and notifies all
registered requests under the registry mutex. Each waiter resets only its own
event under that same mutex before unlock; notifications cannot be lost in
the unlock-to-wait interval. Both registry and descriptor locks are released
during the kernel wait. Notifications are hints; every wake revalidates the
actual conflict. All existing committed unlock/close/dup2/stdio mutations use
the same Commit path and therefore notify. dup by itself is no unlock;
POSIX any-close is. OFD waiting is explicitly rejected by the primitive.

Actual process/thread creation probes prune confirmed death and PID/TID reuse.
Unknown/access-denied probes retain ownership. A real100ms event wait bounds
rechecking of dead processes, missing notifications and old compatible peers
which do not emit event hints. No interval or noncompletion is treated as lock
acquisition. There is no fairness or public deadlock-detection claim.

Acquisition/removal is one validated commit. Cancellation observed before
publication wins; a later cancellation does not revoke committed ownership.
Waits for the registry mutex accept cancellation and the remaining original
deadline. Registry Open remains a caller prerequisite and is not covered by
the operation deadline. Deadline measures scheduling opportunities, not a
hard real-time guarantee over OS calls or per-transaction liveness probes.
Timeout/error cleanup uses only the same remaining budget. If registry access
or commit fails during cleanup, a request can remain as harmless metadata
(never a granted lock) until confirmed thread death pruning; a live thread
then retains a capacity slot. This failure mode is open, not hidden as success.
No rollback is reported after a successful publication merely because event
notification failed. Event spoofing can cause only revalidation, not grant.

The primitive does not solve an fd-close-during-wait policy: its caller must
pin the original FILE_OBJECT and supply cancellation independent of reusable
fd/handle numbers. The fixture retains its fd until its owned worker joins.
Public POSIX/OFD descriptor integration, OFD token/anchor/last-alias lifetime,
cross-process inherited descriptions and atomic CLOEXEC/spawn remain open.

## Prepared real fixtures and future gate

blocking_worker.cc uses actual Linux fcntl in Linux mode; Windows links the
real common POSIX implementation and internal primitive. blocking_oracle.py
starts independent holder/waiter/third-conflict processes. Linux must expose
the genuine queued POSIX request in /proc/locks; Windows must expose the real
registered conflict. Success additionally requires the actual acquisition
result and exclusion of a third real process while the waiter remains alive.
Cancellation/timeout must preserve the holder and remove the registered wait.
All owned processes/readers must exit and join under bounded waits.

Shared strict rows cover unlock, any-close, duplicate-alias close and holder
crash. Linux separately exercises real EINTR, SA_RESTART with confirmed
handler delivery, OFD-holder unlock and nonfinal OFD alias close preservation.
Windows separately exercises actual cancellation, absolute timeout and the
unchanged common EINVAL public-command negatives. The controller compares
the four actual shared Linux rows exactly and labels distinct semantics
explicitly. It never normalizes ECANCELED into EINTR or claims OFD blocking.

The new manual-only native3-locks-blocking workflow is PREPARED, UNDISPATCHED.
It retains all original strict helper controls, producer/API/archive/source
receipts,161 reference comparison and13-mode assertions, then adds the actual
Linux blocking reference and finite Windows controls on2022/2025. This future
component changes its own explicit artifact inventory to include the new
worker/imports/reference; the accepted seven-output supplier is unchanged.
Exact source guard requires clean source, qualified ancestry, a closed set of
code paths and exact reviewed blobs. Existing full supplier pins intentionally
reject this new delta; integration must explicitly review/requalify it.

Required before dispatch: independent bounded source peer per ADR0080. Review
event-reset/publication ordering, post-commit errors, stale live-thread request
cleanup under deadline, cancellation vs grant linearization, registry mutex
cancellation, process death/unknown probes and proof strength of registered
Windows request versus actual event wait. This checkpoint does not claim a
WCT/kernel-blocked Windows thread witness. Also review portable worker errno
and Linux /proc visibility on actual CI. No peer is fabricated in this task.

Remaining: public7/38, targeted signal disposition/masking/delivery, EINTR and
SA_RESTART including real negative cases, POSIX/mixed wait-graph EDEADLK,
OFD blocking/final-alias and inherited ownership, fd reuse/close cancellation,
fault/permission/abandonment event tests, capacity/cancel contention, liveness
failure proofs, bionic/guest/ART layouts and cross-ABI execution, hardware
contexts, coherent full/package/two-e2e. Old red results remain red; no Native
coverage inventory closure or hardware waiver is made.

## Saved exact local checkpoint

Code/real-fixture checkpoint4711c5066a10fbb00c4f763b7ba83a620a9e742f
contains ten pinned changed/new code blobs plus the pin manifest and this
handoff. Local read-only validation: Python source AST parsing passed for
the controller and guard; git diff --check passed; exact source guard passed
at that clean commit with pinned_blobs=10/public_blocking_closed=false. No
C/C++ compiler/configuration, native/Java/C# fixture or Linux syscall ran.
This final documentation-only commit records that checkpoint; its parent
remains the exact code identity. Independent peer and fresh actual component
evidence are prerequisites for qualification, not permission to merge this
unverified source into primary or main/release.

## Unverified stale-event checkpoint and witness research

Source checkpoint dc8d2ac7 preserves the inherited event-absence reclamation
change. No failure injection, source peer, runtime test or updated exact-blob
pin accompanies it; the guard deliberately rejects the changed source until
review and requalification. It is not integration-ready. Reclamation relies on
an event remaining open throughout request publication, and removes a slot
only on ERROR_FILE_NOT_FOUND; all other OpenEvent errors retain it. A live
observer handle may delay reclamation. Test cleanup Begin and Commit failures
under both cancellation and original timeout, preserve the original result,
then prove a later waiter reclaims the abandoned slot while the original
thread remains alive; also exercise unknown/access-denied behavior.

Microsoft's WCT documentation does not list event waits as supported, and
GetThreadWaitChain explicitly says a single node means either not blocked or
blocked on an unsupported element. It cannot prove this event wait. Documented
PerfProc thread state reports Wait and User request, but it is a sampled state,
not the identity of the waited object. A credible fixture must correlate the
actual waiting thread and its creation identity with a tightly synchronized
wait phase and prove the observed state belongs to the event-wait path; account
for registry mutex waits, stale samples, 100ms periodic wakes and bounded
failure. See https://learn.microsoft.com/en-us/windows/win32/debug/wait-chain-traversal
and https://learn.microsoft.com/en-us/windows/win32/api/wct/nf-wct-getthreadwaitchain
and https://learn.microsoft.com/en-us/previous-versions/aa394279(v=vs.85).
No such witness is implemented or claimed in this checkpoint.


## Cleanup failure control source, 2026-10-29 (unverified)

The blocking worker now compiles the real registry/wait translation units
with GESHER_RECORD_WAIT_TEST; gesher_posix itself has no test definition.
No wire command or environment variable can activate hooks in a production
binary. CI checks the actual archive lacks the hook symbol and the fixture
contains it. Commit injection occurs after the inactive bank CRC, before the
sole active-bank exchange. Begin injection applies only to final cleanup.
A published request whose original deadline expired enters final cleanup
directly, checking cancellation first and retaining the original budget.

Four Windows controls cover cancellation/timeout times final Begin/Commit
failure. Each observes the actual request, retains its event, checks the
actual owner thread creation identity, and keeps that thread alive after
WaitProcessLock returns. An authoritative read must retain the failed-cleanup
request. Real named events and live thread identities fill all256 slots;
a later conflicting waiter must fail ENOLCK while the observer retains the
original event. The observer closes before checking actual FILE_NOT_FOUND.
Targeted ACCESS_DENIED and INVALID_DATA event-open errors must each retain
all256 slots and fail ENOLCK. With injection removed, a fresh real finite
waiter must reclaim exactly the original slot, register, time out, and leave
255 fixture entries. The actual holder must remain a conflict throughout;
a separate process checks it after each control. Only then are owned filler
entries removed,260 explicitly owned handles closed/invalidated, the original
live owner released and joined, and the original cancellation handle closed.
These are runtime assertions awaiting CI, not evidence that they passed.

The controller requires four exact CLEANUP_CONTROL rows plus the prior eight
rows, with BLOCKING_SUMMARY rows=12. Source manifest now pins12 concrete
blobs. Local verification is source inspection, Python AST parsing and Git
whitespace checks only. Independent source peer is required before any push
or CI. No local native/JVM/C# compile or fixture ran. No run ID exists for
this source. Event-wait kernel witness, public7/38, signals, OFD/inheritance,
full integration and release remain open; WCT is not an event-wait witness.


### Peer-reviewed source and pending component

Exact source1037c8f103a341b5b733a78c064efd1db2b54e72 passed the independent
integration worker's bounded source review on2026-10-29. A follow-up review
confirmed the checked noninitial result's release/acquire establishes C++
happens-before before the main thread reads hook counters; the Win32 event
is only the bounded wakeup. The actual fixture object is inspected before
normal executable stripping, while the production archive must lack hooks.
Read-only local guard passes12 blobs and public_blocking_closed=false;
Python AST and Git whitespace checks pass. No local compiled fixture ran.

After checking no active same-ref run and checking gh's immediate account
was palmoni5, gh-authenticated HTTPS pushed the branch. The narrow workflow
push started component40782744623 on exact1037c8f1. Integration owns the
120-second-interval watch; its result is pending. This docs-only checkpoint
does not change the source under test. There is no runtime qualification,
full acceptance, main/PR/tag/release or gap-checkbox change in this handoff.
