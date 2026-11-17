# N20 real-time signals: blocked intermediate repair

**Merge status: BLOCKED.** This checkpoint improves pending storage and adds
ordinary public-API gates. It does not establish Android signal compatibility.
The remaining transport, thread, job-control, frame, and accounting failures
below must be repaired and genuinely exercised before approval. Passing only
the queue or Linux/model checks is not sufficient.

The starting PR head is `bdf01a1f28f45faab3fc94cab1bcb901785db428` (PR 79).
The integrated main is `e80cd1854163bd18e4ea5392c25f423184048804`. The CI sparse
checkout correction is the minimal change from
`a0f415eb6ac9e01ec57ec758b67dec1f4133de69`. No changes from the unqualified PR 73
branch were imported. Existing CI gates remain required.

## Contract sources

The implementation was written independently from these interface contracts;
no Linux/GPL implementation was copied into Gesher.

| Source | Relevant contract |
| --- | --- |
| [Android common kernel, pinned signal.c](https://android.googlesource.com/kernel/common/+/5228562af4fa87e987989e0a55f5c01d71f2aae1/kernel/signal.c) | `sig_ignored`, `__sigqueue_alloc`, `__send_signal_locked`, `collect_signal`, `do_sigaction`, `do_sigtimedwait`, default stop/continue handling. Standard coalescing; RT FIFO; receiver real-UID-wide quota; kill allocation fallback; disposition flush. |
| [Android common kernel, pinned x86 signal.c](https://android.googlesource.com/kernel/common/+/5228562af4fa87e987989e0a55f5c01d71f2aae1/arch/x86/kernel/signal.c) | Signal frame, actual register state, alternate stack, and restored signal mask. |
| [Bionic Android 16 r4 signal.cpp](https://android.googlesource.com/platform/bionic/+/refs/tags/android-16.0.0_r4/libc/bionic/signal.cpp) | `sigqueue` fills real sender identity and value; waits forward to the kernel; reserved signal filtering. |
| [Bionic Android 16 r4 signal types](https://android.googlesource.com/platform/bionic/+/refs/tags/android-16.0.0_r4/libc/include/bits/signal_types.h) | LP64 sigaction and signal-set ABI. |
| [Bionic Android 16 r4 ucontext](https://android.googlesource.com/platform/bionic/+/refs/tags/android-16.0.0_r4/libc/include/sys/ucontext.h) | Architecture-specific guest frame layouts. |
| [GNU libc upstream history](https://sourceware.org/pipermail/glibc-cvs/2018q4/065840.html) | The glibc sigtimedwait wrapper normalizes SI_TKILL to SI_USER; Android's pinned wrapper does not. The reference fixture accounts only for this documented libc difference. |

Blocked ignored signals are still eligible for synchronous waiting. Installing
SIG_IGN discards already pending instances even when blocked. These are distinct
operations; dropping every ignored signal unconditionally would be incorrect.
Linux kill(RT) attempts a queued record but may fall back to a pending bit on
allocation or quota failure. The fallback has zeroed SI_USER information, not
an invented sender or payload. A failing RT sigqueue reports EAGAIN instead.

## Implemented at this checkpoint

`Pending<Info>` in `compat/posix/include/gesher/pending_signals.h` now supplies
both POSIX and bionic pending storage. It coalesces standard signals while
preserving their first record, queues RT records FIFO per number, and frees
records on consume, disposition discard, and container destruction. The shared
storage code does not make the adapters' action tables or delivery engines one
coherent signal service; that remains unfinished.

Queue records use nonthrowing allocation. Allocation and quota failure follow
the standard-signal/RT-sigqueue/RT-kill distinctions above. All allocated records
in the linked process use one atomic local charge counter. The current soft
RLIMIT_SIGPENDING is read for each allocation; reducing it affects subsequent
sends, and `_SC_SIGQUEUE_MAX` reports that configured limit. The invented fixed
SIGQUEUE_MAX=32 cap is removed. **This local charge counter is not Linux's
cross-process, real-UID-wide accounting.** It is not a claim that the resource
contract is complete.

The bionic adapter uses this queue at ordinary send, wait, pending-delivery and
sleep boundaries, including repeated thread-targeted RT sends. Local siginfo
uses getuid(), not a fixed uid or zero. Installing an ignoring disposition
flushes stored pending records in the relevant adapter. The mask never retains
SIGKILL or SIGSTOP. Negative/invalid sigtimedwait intervals return EINVAL.
Ignored deliveries do not finish bionic sigsuspend.

The bionic pthread send now resolves a known live pthread through its existing
registry and retains the kernel thread object. Unknown identifiers no longer
receive fabricated success or create arbitrary signal-state entries. Retained
objects allow later reclamation of exited-thread pending state. This does not
yet provide immediate retirement for every host-created thread or complete
mask inheritance. The host's non-MinGW pthread_kill success stub is removed.

## Unresolved merge blockers

1. **Guest/host authority and thread lifetime.** Host tgkill and pthread_kill
   still put signals on a process queue. Host and guest mask inheritance and
   action ownership remain separate. Guest state can be reclaimed after a
   later registry observation rather than at every thread's actual exit.
   External handlers may run on the shim's dedicated signal thread. A correct
   design must preserve the intended recipient, its lifetime, pending state,
   mask, and execution context across host, x64 guest and translated ARM64 paths.
2. **Cross-process payload, identity and permission.** process.cc still uses a
   bit mailbox and sender-pid slots. It loses repeated RT instances and queued
   values; hook signatures do not carry full siginfo. It does not establish
   Android real/effective/saved UID permission decisions from authoritative
   identities. The guest Sigqueue remote path still delegates to kill. A
   checked, quota-accounted transport needs admission and lifetime semantics,
   and must preserve sender, value, order and target state.
3. **Stop/continue and wait status.** Host sigsuspend still routes a default
   stop to ExitProcess. Guest default stops and external stops also lack the
   correct complete lifecycle. Children have no full stopped/continued status
   stream. Existing WaitStatus confuses a normal exit143 with SIGTERM. Default
   terminate, core, ignore, stop and continue must remain distinct operations.
4. **Actual handler frames and alternate stack.** SA_ONSTACK is not acted on
   by these asynchronous paths. Host and guest sigsuspend still give handlers
   a zero/fabricated context, without the actual interrupted mask, PC, or
   architecture frame and restore behavior. This is a known failing contract,
   not missing test evidence alone. Registering a stack is not proof of using it.
5. **Real-UID resource ownership.** The local counter does not share charging
   between processes of the same real UID, cannot atomically coordinate IPC
   admission, and does not prove one authority across every DLL supplier.
   Immediate reclamation, queue allocation failure, and process exit need to
   be validated through the complete delivery architecture.
6. **Windows and translated execution evidence.** Local cross compilation is
   not runtime evidence. No real Windows, ART, native ARM64 or translated ARM64
   execution is claimed here. The mandatory live gates must execute, expose
   these failures, and remain red while contract violations remain.

No unsupported-error return was inserted to hide a missing frame, permission,
stop, or transport implementation. No application-specific exception was added.

## Verification and actual gates

The original independent `qa79-validation` source/models/results remain outside
this branch unchanged. Their seven original assertions are preserved. At this
checkpoint the host adaptation passes FIFO/sender UID, ignored-send resource
use, SIG_IGN retirement, capacity over 32, and kill fallback (5/7 groups). It
continues to fail alternate-stack/context and stop (2/7). The independent Linux
reference passes all seven. The adaptation still replaces only platform headers,
symbol names and Windows exit; it is not a Windows scheduler or guest ABI test.

The original 58-check test used a fixed-32 overflow oracle that disagreed with
Linux. The revised bounded test uses 40 sends and a temporary zero soft quota,
restores the quota, and verifies kill's no-info fallback. Both the host
adaptation and real Linux pass its 62 checks. This cannot substitute for the
new adversarial contract fixture.

`tests/exec/realtime_signals_contract.c` runs ordinary public calls without
production replacements. Its ten Linux groups pass: FIFO/identity, ignore and
flush, quota/fallback, priority, timeout errors, thread targeting and inherited
mask, actual handler stack/context/mask, cross-process FIFO/identity, real
stop/continue/wait status, and normal-exit versus signal status. All children
are created by the fixture; no external targets are discovered. A glibc-only
expectation accounts for the SI_TKILL normalization described above, while
both Gesher roles must retain Android's SI_TKILL.

The fixture is also a CMake native Windows target linked to `gesher_posix`.
The exec workflow builds its NDK x64 and ARM64 ELF versions with NDK r27c,
records source revision and SHA-256 values, and runs the Linux reference.
`tests/windows/realtime-signals-contracts.ps1` is called unconditionally by the
existing POSIX and exec test tracks. It requires the native binaries and both
NDK artifacts, verifies guest provenance, runs each ordinary case under a
20-second owned-process deadline, and fails on missing artifacts, timeout,
nonzero exit, missing success sentinel or any failed assertion. There are no
expected-failure allowances. Per-case JSON includes stdout, stderr, executable
hash, source revision and elapsed time, and the existing shard artifact upload
retains it. Context, thread and job-control failures are deliberately exercised.

For a focused run after producing matching runtime and exec artifacts:

```powershell
pwsh -NoProfile -File tests/windows/realtime-signals-contracts.ps1 -Dir dist
```

Run the independently executable Linux and isolated allocation checks with:

```sh
python3 tools/test_realtime_signals_contract.py --out /tmp/realtime-reference
```

The isolated storage test uses the exact production container with a labeled
allocator-fault boundary and a test charge counter. It covers allocation/quota
failure, fallback, FIFO, standard coalescing, flush and destruction without
claiming process permissions or Windows delivery. A queue has 1,048 bytes of
empty storage on the tested 64-bit host; each 128-byte siginfo record uses a
136-byte node, excluding allocator overhead. There are no eager per-signal
deques. At 63,438 allocated records, node storage alone is 8,627,568 bytes. A
bounded local exercise measured 65,536 enqueue/dequeue pairs; its timing is in
isolated-storage.json and is not a Windows performance or memory-footprint result.

The existing controlled sleep fixture has only a storage-interface adapter
change; its timing, interruption and payload assertions are retained. Relevant
sleep and pthread attribute regression checks, both signal translation units
for Windows x64/ARM64, and guest fixture compilation are recorded separately.
