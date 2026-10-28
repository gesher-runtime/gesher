# 0080. Native3 advisory POSIX and open-file-description record locks

Status: reserved implementation/design work, not verified. Dedicated branch
`wip/native3-locks`, worktree `C:\work\gesher-native3-locks`, starts at
integrationc455f432. Reserved Windows test87. Every compilation and runtime
fixture is CI-only. Root coordinates integration and all release operations.

## Problem and existing behavior

The bionic Fcntl wrapper in posix_bionic.cc reports every requested lock as
granted and every queried range unlocked. The common POSIX fcntl supports
integer commands but does not implement record locks; its argument storage
is host32-bit long and cannot carry a Linux64-bit flock pointer. flock's
separate open-description lock remains implemented and must not change.

## Proposed implementation boundary

Implement both POSIX process-owned and Linux OFD-owned record locks for
regular files. Normalize SEEK_SET/CUR/END and signed start/length with
checked64-bit arithmetic; zero length extends through future EOF, negative
length covers bytes preceding the start. Validate type, access rights,
read/write ownership, pointer/layout and required OFD l_pid zero. Queries
return real conflicting ranges/types/PIDs (OFD owner PID is-1), leaving a
nonconflicting query's other fields intact. Own locks can split/coalesce or
convert; POSIX and OFD locks still conflict with each other, including within
one process. Record locks remain independent of flock.

Use an advisory coordination registry keyed by stable file identity, rather
than Windows mandatory locks over the data. Each update/query is serialized
across processes, and blocking waits release coordination before sleeping.
Registry capacity failures must fail explicitly rather than drop existing
locks. Dead process/PID-reuse cleanup must compare process creation identity,
and abandoned coordination ownership must preserve or recover a consistent
published snapshot. Tests must exercise genuine independent processes.

POSIX locks belong to a process and any close of that file drops its locks.
OFD locks belong to one open description, share through dup/dup2/F_DUPFD and
are dropped only on its final close. Replacing a dup2 target first closes its
old description. Descriptor numbers and native handles may be reused; neither
alone is a durable owner identity. Audit common close/dup paths and stdio
fclose/freopen, retain integer fcntl behavior, and dispatch pointer-sized lock
arguments before reading integer varargs. ARM flock ABI must be converted
through its existing guest wrapper without truncating offsets or pointers.

Fork is not implemented by Gesher and this change does not claim otherwise.
Document the unsupported fork boundary explicitly; prove relevant ownership
and inheritance with supported independently started/duplicated processes.
Signal interruption/deadlock detection and noncooperating Windows programs
must be evaluated against the actual promised POSIX/Linux scope rather than
silently excluded. No API/gap is marked closed by this design.

## Verification required before acceptance

An actual Linux CI reference must run the same operation contract against
native fcntl; a handwritten list of expected successes is insufficient.
Both Windows runners must execute real common-layer and bionic ELF clients
for both ABIs, plus JNI/single/device contexts as appropriate. Require actual
conflicts, blocking/unblocking, read/read sharing, advisory data I/O, query
normalization/PIDs, range splitting/conversion, invalid arguments/errors,
OFD/process distinction, any-close/final-close/dup2/stdio lifetime, descriptor
reuse, independent files and flock, process crash cleanup and contention.
No unchanged red retry, weakened assertion or symbol-only acceptance.

Bounded independent peer review is required for the coordination design and
final pointer/layout/lifetime source before qualifying CI. Full coherent
Windows suites/package/install/run/uninstall/both complete e2e remain the
integration acceptance gate. Native API inventory proof/history is append-only.

## Proposed equivalent evidence for the real stdio contention test

Status: proposal for independent source and actual-evidence review. This does
not amend the current acceptance predicate. Component40689909664 on source
708b9961c2ccda4ac3f55588c6795d6ba1cc0e73 remains FAILURE. No Native3 API,
blocking command or integration requirement is accepted by this proposal.

The test intent is to establish that an actual independent closer is blocked
on the SAME live FILE's real CRT lock owned by the fixture thread, and that
closing another descriptor can finish while that lock remains held. After
the real owner unlocks, the real fclose must finish successfully. A thread
delay, a noncompletion flag, WCT count1 or a nearest-export name alone proves
none of the required lock identity/ownership/call causality.

The original full WCT chain remains one direct Windows observation. On actual
Windows2025 in40689909664, WCT returned321 successful queries but only one
Blocked thread node for the owned closer. Additional actual data now binds
the CRT implementation, FILE, critical section, owner and closer argument:

- Original exact worker4f8 from failed40674284726/successful producer122768663211
  was independently authenticated in diagnostic40684287454. Its actual
  return-site instruction9830 calls exact IAT slotc42a0, whose named import
  is _lock_file. No symbol was invented for the stripped original executable.
- Diagnostic40686447043 inspected both actual named CRT exports using guarded
  loaded-code/file-byte equality and owned snapshots. Windows2025 module
  SHA2569601850e2ede2cd86b441223557cae0fa7f5e33b1dd9d481c58c8cc2d688bdc3
  matches the original failed run's actual module. Both Windows implementations
  add0x30 to the FILE argument, then tail-jump through their own IAT.
- Actual708 worker verifies its public lock/unlock functions are the named
  retained CRT exports, decodes their exact instruction shape and requires
  their IAT targets equal the actual named Enter/LeaveCriticalSection and
  their decoded offsets match. Unsupported code fails; no FILE ABI is guessed.
- Actual2025 FILE1a4148964c0 yields CS1a4148964f0. Before lock: owner0/recursion0.
  Held and waiting: owner8384/birth134376650401996076/recursion1. Guarded reads
  use checked Windows-header structure offsets, alignment and same allocation.
- The captured actual closer748/birth134376650406837328 has one exact worker
  return site atPC7ff6b466a376. Its bytes are movRSI,RCX then call[IATrel32]
  resolving the actual _lock_file; that frame's restored nonvolatile RSI is
  1a4148964c0, the SAME FILE. Capture requires ownPID/birth, checked balanced
  suspend0/resume1, and complete guarded reads. This is direct call-site and
  argument evidence, not a nearest-export or generic register interpretation.
- The closer had not completed before independent-fd close0 or before unlock.
  Actual returned result0/stream_result0 proves real close completion after
  unlock; contended=false still failed the unchanged original assertion.
  No FILE/CS memory was read after unlock permits fclose/free or slot reuse.
- Windows2022 observed the same before/held/waiting transitions in all four
  cycles together with four original actual full WCT chains. Linux still
  produced161 real kernel-reference rows. This supplies a comparison platform;
  it does not automatically qualify the proposed alternative.

### Candidate rule, not implemented

Retain the original full WCT path and all raw WCT output. A narrowly reviewed
alternative could require ALL of the following conjunctive evidence, rather
than treating missing WCT ownership as success:

1. The runtime-decoded named CRT implementation and matching lock/unlock IAT
   target establish one checked critical-section address for the exact live
   FILE. Unknown layout, unreadable memory, different allocation, inconsistent
   offsets, failed structure checks or different public function fail.
2. Before/held/waiting snapshots establish owner0/recursion0 -> actual current
   ownerTID/nonzero birth/recursion1, on the same FILE and CS. The owner is
   this live fixture thread. The closer's exact process/thread/birth is held
   through capture; no PID/TID/descriptor or virtual-address reuse is accepted.
3. The actual owned closer has a real Blocked WCT thread node matching its
   PID/TID, and a checked captured call site resolving _lock_file with its
   restored actual FILE argument equal to that same FILE. Mere count1, API
   failure, wall time, a sleeping thread or an inferred export cannot pass.
   A captured frame must also fall within the actual named
   EnterCriticalSection's RUNTIME_FUNCTION bounds. This condition is mandatory;
   a nearby export name or an optional unobserved frame cannot satisfy it.
4. Independent close returns0 while the owner still holds that CS; closer
   completion remains false before unlock. A contradictory completed closer,
   owner, identity, decoded call or actual WCT chain fails even if another
   witness appears positive. A reported cycle or explicit other owner is not
   hidden by an alternative branch. LockCount/DebugInfo are recorded only;
   their version-specific numeric meanings are not presumed.
5. The real matching unlock happens once and real fclose returns0 with actual
   kernel-signaled thread exit and join before the SAME existing five-second
   absolute deadline, recorded with monotonic per-phase timestamps.
   The external10-second command and180-second root bounds remain unchanged.
   No new deadline starts after sampling, close, unlock or cleanup. No FILE/CS
   read is permitted after unlock allows fclose to invalidate the stream.

For each of the four real contention cycles, log which complete evidence
route was used and retain actual WCT, code/CS/thread/call-site and completion
records. Preserve all original161 Linux-row comparisons, semantic assertions,
13 real duplicate modes, raw-publisher/capacity/quarantine/FD-reuse/Binder/job
cleanup gates and actual process exit checks. A positive assertion count or
an optional unexercised branch cannot substitute for a required case.

Before adopting any alternative, independent review must examine both exact
source and saved actual evidence, verify the mandatory Enter frame bound in
all four cycles on both hosts, and require meaningful rejection controls (different live
FILE/CS, wrong owned thread birth/argument and conflicting ownership) without
mocking a successful wait or adding guest/environment switches. Then freeze
the reviewed test-intent/source change and run fresh CI on both Windows with
all required cycles and actual Linux rows. Old red runs remain red. Source
peer, static diagnostics or this proposal cannot qualify the runtime contract.

This proposal only concerns evidence for the stdio lock-order test. Inherited
OFD adoption, atomic CLOEXEC/spawn, blocking7/38, targeted signals/EINTR/restart,
ABI/bionic/ART, concurrent protection/copy, and full/package/e2e remain open.
