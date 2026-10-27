# Track brief: binder2

Binder transport contracts left open by ADR 0015 and the input-channel
restriction in ADR 0027. Reserved ADR: 0068. Branch `wip/binder2` starts from
integration after aot2; use a dedicated worktree under C:\work.
Follow track-rules.md and read compat/binder, compat/posix/src/binder.cc,
the binder UAPI and current FD transfer/Parcel tests before implementation.

Required work:
- Audit BINDER_TYPE_FD/FDA against existing DuplicateHandle transport and
  real close/ownership/error behavior. Surface Bitmap/memfd transfer already
  passes in ci 40307919084; do not call all descriptor transfer absent.
- Close scatter-gather BINDER_TYPE_PTR/HIDL buffer fixup and ownership limits,
  with bounds/error tests and real cross-process transactions.
- Implement the remaining BR_TRANSACTION_SEC_CTX, freeze and transaction
  priority behavior against the Windows process/thread model, with actual
  observable results and a documented identity model.
- Make polling a binder fd obey readiness, cancellation and concurrent waiter
  rules. Coordinate epoll internals with native3.
- Verify an InputChannel/socket descriptor sent across binder to another
  process (ADR 0027), including independent process lifetime and closure;
  coordinate socket handle duplication with web3.

Test real binder clients, broker and service processes on both Windows
runners, retaining package, application lifecycle, descriptor and both e2e
checks. No local AOSP build or WSL. Merge current integration before final
full CI and cite actual PASS lines; protocol support is not established by
accepting a command without implementing its effect.

## Status and next steps

Manager-assigned SG PTR leaf on2026-10-27: dedicated
C:/work/gesher-binder2-sg, wip/binder2-sg from acceptedb323a42c; existing
ADR0068 and test88. Native owns record-lock fd/stdio/spawn hooks and confirms
no concurrent binder.cc/wire/broker/epoll edits. Source candidate adds guarded
snapshot/relative owned payload/independent shape checks/receiver rebasing
and wireversion2, requiring fresh host+broker together. No fixture, compile,
runtime or full proof exists yet. Read ADR0068 for source identity/invariants.
Next: bounded source peer, actual cross-process malformed/lifetime/ABI
fixtures, CI only, then current accepted Libcore union and full gates. Existing
FD/Bitmap/InputChannel behavior must remain; FDA/security/freeze/priority/
readiness stay open. The older reservation below is historical.

Reserved on 2026-10-26 during the post-aot2 gap audit. Unstarted. Existing
descriptor support is verified only in its cited contexts; the remaining
contracts above are open, pending source audit and executable evidence.

Peer review of76a121e8 identified real pre-CI blockers: aggregate wire budget
was checked too late, aligned receiver bytes were not included in the cap,
malicious wire body lengths could allocate before remaining-byte validation,
and receiver copy/allocation/FD failure needed explicit ownership rollback.
The correction is still unverified source: preflight every append/snapshot,
check aligned dimensions and actual remaining frame, guard zero-byte writes,
and use pending delivery claim/finish with guarded caller output copies.
Unpublished delivery failure closes local FDs/allocation, unwinds the real
synchronous caller and broker/oneway buffer; already published commands keep
their read_consumed prefix. PTR allocations obey real mapped capacity without
heap fallback, enabling genuine small-map ENOMEM fixtures. Plain transaction
heap fallback remains separately open. New protocol boundary/death/cancel/
exactly-once/partial-consumption fixtures and second peer remain mandatory.
No CI is dispatched from this checkpoint.

Second peer of3a2efbc3 found a genuine claim-ACK ownership ambiguity and a
preceding plain read-command publication failure that could strand a pending
transaction. Current correction keeps broker-owned staging handles, adopts
separate receiver copies, closes/zeros staging on finish/abort and closes the
thread pipe for every read-stream failure. Common POSIX independent-process
actors and a finite host parent are now source only. Remaining before CI:
raw-wire lost-ACK/disconnect/death/FD-reuse and queue-progress controls,
both ABI/ART/APK wiring, final peer/static checks. No source/runtime closure.

## Ordinary ART/APK mode source (separate continuation)

Active mode wiring branches from compiled7e9549, independently of preserved
interrupted WIPb52. It adds the same common POSIX actor body to both NDK ABI
ELFs/JNI libraries, standalone ART entry and separate server/client APK IDs.
The existing public GESHER_BINDER selection is changed only around the new
actor descriptor's open and restored immediately before mapping/ioctl/wait.
The test descriptor captures its own pipe; established application Binder
objects remain separate. APK code observes the existing package Binder's
identity/descriptor/ping concurrently and checks endpoint restoration. CI
must also audit the SG broker's actual per-APK PID open count for unexpected
concurrent opens; no absence of misrouting is claimed from source alone.
The package builder checks exact APK/native library/dex byte identity.

No new production switch or API is introduced. These source files have not
compiled or executed; build/runtime orchestration and peer remain pending.
Before ARM/ART/APK runtime, select exact verified Native942/Envb497/frozen5587
and current accepted Libcore integration, rebuild host/DLL/jars/images together
and retain original FD/Bitmap/InputChannel/full/package/e2e gates. Complete
public transaction-prefix transport-failure proof remains open on preserved
unverified interrupted WIP; ordinary mode coverage does not substitute.

Mode candidate now includes accepted645 plus exact verified Native942 and
Envb497/frozen5587 (candidate mergecf416). This selects the coherent prerequisite
before any ARM runtime; it is not combined runtime proof. Both real successful
and unavailable-endpoint open paths will check exact environment restoration.
JNI registration now fails cleanly on registration error/pending exception.
A dedicated two-job build-only workflow compiles the selected host sources,
actual JNI/ELF actors, Java entry and both role APKs with source/hash metadata;
no runtime/ART/APK execution or acceptance is part of that workflow.

Bounded source peer found no concrete build blocker in immediate restoration,
JNI shape or packaged-byte checks. The fixture mutex covers fixture opens only:
a concurrent new framework open/getenv could observe the temporary global
selection. Existing-framework ping identity alone cannot prove general absence
of misrouting. Runtime PID/open-count audit and concurrent-new-open requirement
remain open; no broader session/isolation closure is claimed by build evidence.
