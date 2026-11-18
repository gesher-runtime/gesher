# N20 local real-time pending queues and explicit delivery boundaries

This PR is a **partial local-queue improvement**, not completion of Android's
asynchronous signal architecture. It preserves FIFO payloads, standard-signal
coalescing, actual local sender information, configured local queue limits,
process/thread queue ownership, synchronous waiting, and ordinary one-argument
handlers. Unsupported asynchronous frames, active alternate stacks, local
default-stop delivery, remote RT transport, and host non-self thread delivery
must fail explicitly where the adapter receives that request.

The repaired input is PR 79 head
`937473b81173173b0de0ad1b35305b6f32a57619`. The review distinguished defects
introduced by that queue checkpoint from broader pre-existing limitations. The
Windows CI workflow is composed with main
`704595a550f5f0abac9ad183d5196da82e2623ba`, retaining main's routing and validation
changes and the PR's signal-evidence upload. Publication must preserve both
parents' changes if main advances again.

**Qualification is pending independent QA and actual Windows/guest execution.**
Cross-compilation and Linux tests do not establish those runtime results.
The full Android/Linux reference corpus is retained and executed separately;
its unresolved failures are not represented as passing compatibility tests.

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

## Supported operations and exact failure boundaries

| Application request | Implemented behavior | Qualification |
| --- | --- | --- |
| Queue blocked local RT records and take them with sigwaitinfo/sigtimedwait | Per-number FIFO, lowest-number priority, original value/code/local pid/uid, one consumption per record | Required public-API FIFO, priority, timeout and capability cases |
| Queue a blocked signal whose installed handler uses SA_SIGINFO or SA_ONSTACK | Enqueue and synchronous waiting remain valid; an unused handler does not invalidate that operation | Required capability case retrieves the original record |
| Deliver an asynchronous SA_SIGINFO handler | ENOSYS before consuming its record or applying SA_RESETHAND; the prior mask is restored | No zeroed or invented ucontext is passed |
| Deliver SA_ONSTACK with an enabled alternate stack | ENOSYS with record/action/mask preserved | Merely recording a stack does not claim execution on it |
| Deliver SA_ONSTACK while its alternate stack is disabled | Ordinary-stack delivery remains available for a one-argument handler | Required positive capability case |
| Run an ordinary one-argument handler at a supported delivery point | Real call with the applicable handler mask, deferral and SA_RESETHAND; sigsuspend returns EINTR and restores its mask | Required positive capability case |
| Stop this process through an unsupported local default-stop action | ENOSYS; a pending blocked stop remains available to synchronous waiting | It never becomes ExitProcess |
| Terminate through a terminating default action | Existing actual termination remains; no core-file or wait-status claim is added | Complete core/status semantics remain a separate reference gap |
| Host pthread_kill(pthread_self()) or tgkill of the current process/thread | Private thread queue; another waiter cannot take its record | Required self-target case |
| Host pthread_kill of a different live thread | Nonzero delivery returns ENOSYS after liveness validation; signal zero retains the existing actual liveness check | No conversion to process delivery |
| Guest pthread_kill of a registered live thread | Existing retained-identity routing and its private pending queue remain | Required self-target case includes a blocked other-thread send |
| Remote RT kill or nonzero remote sigqueue | ENOSYS before entering the legacy bitmap mailbox | RT multiplicity or queued payload is never silently discarded by these sends |
| Ordinary remote kill, including signal zero | Existing Windows backend remains available | Required owned-child existence and termination case |

Synchronous Windows exception handling in `fault_signals.cc` is unchanged. It
has an actual Windows CONTEXT and is a distinct path from asynchronous pending
queue delivery. This partial repair does not disable that functionality.

Guest action validation and claiming occur under the same state mutex as the
pending-record removal. The claimed action is then dispatched outside the lock.
A concurrent disposition change cannot replace that action with an unsupported
frame after the record was removed. Unsupported records remain pending and can
still be consumed by synchronous waits. Sleep wrappers propagate resource or
unsupported-delivery failures instead of claiming that a wait completed.

## Storage, lifetime, performance, and memory

`Pending<Info>` in `compat/posix/include/gesher/pending_signals.h` retains the
contributor's shared nonthrowing queue implementation. It coalesces standard
signals while preserving the first record, stores RT records FIFO, and frees
records on consume, disposition discard, and destruction. It does not unify
host and guest action ownership or implement remote transport.

All linked queue records use one atomic **process-local** charge counter. Each
allocation reads the current soft RLIMIT_SIGPENDING. This is useful local
resource enforcement; it is not Android's cross-process, real-UID-wide quota.
The existing configured-limit report and removal of the arbitrary 32-record cap
are retained without claiming complete Linux resource authority.

Host self-directed queues are thread-local objects in an intrusive registry.
Their actual thread-local destruction unlinks the queue and frees its records;
SIG_IGN can flush every registered queue under the same state lock. No extra
kernel lifetime handle is needed for this host self-only path.

Each guest ThreadState owns a retained kernel thread handle and a one-shot
Windows thread-exit wait. Registration must succeed before state is published;
resource failure returns EAGAIN. Actual kernel-thread exit invokes a callback
that removes only the exact stored identity, unregisters its own wait without
blocking, and destroys the queue and retained handle. No later pthread_kill,
Self registration, or unrelated signal operation is required to release its
records and charges. The state mutex serializes callback publication/removal
against queue observers. This covers guest-created and host-created threads
that register signal state, including a target created before its first Self
call.

Windows specifies that a one-shot registered wait still needs explicit
unregistration. A callback must not synchronously wait for its own completion;
the nonblocking unregister can report ERROR_IO_PENDING while that callback is
running. The one-shot flag prevents a second callback from retaining this
context. See [RegisterWaitForSingleObject](https://learn.microsoft.com/windows/win32/api/winbase/nf-winbase-registerwaitforsingleobject)
and [UnregisterWaitEx](https://learn.microsoft.com/windows/win32/sync/unregisterwaitex).

Enqueue and dequeue remain constant-time within each signal number. Selection
scans the fixed 64-signal namespace. Removing an ignoring disposition and thread
retirement free only their owned nodes. No eager per-signal deque is added. On
the tested 64-bit host the shared empty queue is 1,048 bytes and each 128-byte
siginfo record uses a 136-byte node before allocator overhead. Guest state adds
one native wait registration per participating thread; actual Windows lifetime
and handle evidence is required before approval.

## Remaining Android reference gaps

These are visible limitations of the broader existing signal service, not
claims completed by this local FIFO PR:

- Host/guest action authority and mask inheritance remain separate. Host
  non-self delivery has an explicit unsupported boundary. Guest external
  ordinary handlers can still run on the shim's dedicated signal thread;
  this is not a new claim of application-thread asynchronous ABI support.
  Unsupported frame/alternate-stack records stay pending instead of receiving
  an invented context on that internal worker.
- The interprocess mailbox cannot preserve RT multiplicity or queued values;
  new RT/queued sends reject that transport. A future transport must preserve
  admission, identity, payload, order, lifetime and authoritative permissions.
  Existing ordinary Windows kill authorization is not Android UID permission
  authority.
- Actual local asynchronous frames, architecture-specific register restoration
  and active alternate-stack delivery are not implemented by these adapters.
  Explicit failure is part of this partial capability boundary, not successful
  execution of those operations.
- Complete local/external stop and continue behavior and stopped/continued
  child events remain unfinished. Existing numeric WaitStatus interpretation
  can confuse normal exit143 with SIGTERM. This repair neither invents a
  stopped event nor claims that numeric termination metadata fixes waitpid.
- The resource counter is process-local, not real-UID-wide across processes.
  Full signal permissions and process lifetime accounting need a unified
  authority.
- Real Windows, ART, native ARM64 and translated ARM64 results must be recorded
  separately. Compiling both Windows architectures is not runtime evidence.

No application-specific exception or fabricated machine state is introduced.
A complete N20 implementation must pass the full reference contracts; merging
a qualified local-queue improvement does not close those remaining tasks.

## Verification and mandatory qualification

`tests/exec/realtime_signals_contract.c` keeps all ten ordinary Linux reference
groups: FIFO/identity, ignore/flush, quota/fallback, priority, timeout, targeted
threads and inherited mask, actual handler stack/context, cross-process FIFO,
stop/continue status, and normal exit versus signal termination. The original
assertions in those groups are unchanged. The glibc-only SI_TKILL normalization
remains limited to that reference libc; Gesher's guest path still expects the
Android code.

The same source adds four **Windows capability qualification** groups:
`capabilities`, `self-target`, `retirement`, and `remote-capabilities`. They
exercise the positive supported operations and the exact explicit failures in
the table above. The retirement case uses one exiting thread with one queued
record and a one-record limit in its isolated fixture process, then checks
bounded reuse without a lazy-reaper trigger. The remote case creates only its
own child, checks rejection before unsupported transport admission, and proves
that ordinary remote existence and termination still work. All children have
bounded waits and owned cleanup.

`tests/windows/realtime-signals-contracts.ps1` requires FIFO, ignore, quota,
priority, timeout and all four new qualification groups, plus the 62-check
native regression binary. Its Windows-specific legacy frame checks now require
truthful ENOSYS and preservation of the original waitable signal/value; the
Linux version retains its actual handler checks. The full Android thread/frame
and guest cross/stop/status groups still execute and retain their raw pass/fail
results as `unfinished-full-android-reference` observations. They are never
relabeled as passing. `-RequireFullAndroidContracts` makes those observations
mandatory as well; it is expected to fail until their implementations exist.
Missing binaries, provenance mismatches, process timeouts and harness errors
always fail qualification, including reference observations.

Every process has a 20-second deadline, followed by a bounded kill of only its
owned process tree. Receipts include source revision, fixture/executable
SHA-256 values, scope, whether the contract is required, stdout/stderr, exit,
timeout and elapsed time. CI retains those JSON records. No Windows result is
claimed by the local cross-compilation or source-adapted tests.

The CMake targets link the production POSIX implementation. With the normal
AOSP checkout and LLVM-MinGW toolchain prepared:

```powershell
cmake -S . -B build-signals -G Ninja `
  -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-llvm-mingw.cmake `
  -DLLVM_MINGW=$env:LLVM_MINGW -DAOSP_DIR=$env:AOSP_DIR
cmake --build build-signals --target realtime_signals_test realtime_signal_contract_test
pwsh -NoProfile -File tests/windows/realtime-signals-contracts.ps1 `
  -Dir build-signals -HostOnly -EvidenceDirectory build-signals/realtime-signals-evidence
```

The host qualification executes 12 processes: one legacy case, nine required
public-API cases, and two full-reference observations. Required cases must have
exit zero, no failed assertions, and the expected success sentinel. The report
must say zero required failures; nonzero full-reference observations remain
explicit and cannot establish complete Android compatibility.

The existing exec workflow builds x64 and ARM64 fixtures with NDK r27c, binds
them to the exact source revision and fixture digest, and supplies the manifest.
Run the guest qualification only with matching `gesher-exec.exe`, runtime DLLs,
NDK artifacts and manifest in the same artifact directory:

```powershell
pwsh -NoProfile -File tests/windows/realtime-signals-contracts.ps1 `
  -Dir dist -GuestOnly -EvidenceDirectory dist/realtime-signals-evidence/guest
```

The guest entry points use the production bionic and ELF components in
`gesher-exec.exe`; the host-only target is not a substitute for guest execution.
For complete Android qualification add `-RequireFullAndroidContracts` to either
command. Run all unchanged Linux reference groups and the isolated production
storage allocation checks with:

```sh
python3 tools/test_realtime_signals_contract.py --out /tmp/realtime-reference
python3 tools/test_sleep_contracts.py
python3 tools/test_boottime_resume.py
```

Local repair evidence records 12 strict Windows cross-compiled objects across
x64/ARM64 (host signal, guest signal, pthread, shim, extra_sys and process), the
ten Linux reference groups, the shared allocation/cleanup checks, 107 controlled
host sleep checks plus 30 real Linux checks, and 35 boottime checks. Independent
re-QA and actual Windows/guest receipts remain required before automerge.
