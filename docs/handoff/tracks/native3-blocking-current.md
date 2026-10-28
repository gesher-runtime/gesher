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
