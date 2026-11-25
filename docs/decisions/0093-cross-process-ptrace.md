# 0093 — ptrace between processes

**Status:** proposed, 2026-11-24 (native-libraries track). The coordinator may
renumber; 0092 is the guest fork/clone decision this one pairs with.

Code: `compat/bionic/ptrace_bionic.cc`, `compat/posix/src/ptrace_state.cc`,
`compat/posix/include/gesher/ptrace_state.h`, the wait hook in
`compat/bionic/process_bionic.cc`, `prctl` in `compat/bionic/extra_sys_bionic.cc`
and `compat/posix/src/misc.cc`, `TracerPid` in `compat/posix/src/procfs.cc`,
guest system calls 117 and 260 in `compat/arm64/dll/guest_libc.cc`, the code
epoch in `compat/arm64/dll/translator.cc`, and the app process's dumpable flag
in `app/java/com/gesher/app/ProcessSpecialization.java` (from
`SystemServer`, `GesherAppProcess` and `GesherAppDriver`).
Tests: `tests/exec/ptrace_contract.c` (`tests/windows/tracks/78-ptrace.ps1`).

## Context

Until now nothing traced a Gesher process and it traced nothing: the first
`PTRACE_TRACEME` succeeded and a second failed with `EPERM`, any attach or
peek failed with `EPERM`, any other request with `ESRCH`
([0084](0084-native-libs-self-unpacking-code.md)); `prctl(PR_GET_DUMPABLE)`
was always 1 and `PR_SET_DUMPABLE` did nothing.

The maintainer decided on 2026-11-24 that real fork/clone and real
cross-process ptrace and `wait4` for arm64 guest code are in scope (the same
experience, implemented for Windows). The motivating case is Google Play's
PairIP (`libpairipcore.so`): the parent sets `PR_SET_DUMPABLE` 1 and
`PR_SET_PTRACER_ANY` and clones; the child attaches to its parent, waits for
the stop with `wait4(__WALL)`, `PEEKDATA`s one of the parent's stack slots,
`POKEDATA`s it into another, detaches and exits; the parent waits for the
child. Without the copy the VM later crashes.

This decision is the ptrace half. It does not need fork: it works between any
two Gesher processes, for instance a parent and the child it started with
`posix_spawn` ([0055](0055-exec2-native-processes.md)). The fork/clone half
(0092) gives PairIP's child, and must keep the per-process state below
correct in a cloned process (see "With fork").

## Decision

**ptrace between real processes, with Linux's permissions and errors, and the
tracee really stopped, at a safe point, while it is traced and stopped.**

### The state Linux keeps in the kernel

Each Gesher process publishes a named section, `Local\gesher-ptrace-<pid>`
(`gesher/ptrace_state.h`), next to its signal mailbox (ADR 0045, 0055):

- `dumpable` (`PR_SET_DUMPABLE` 0 or 1; anything else is `EINVAL`);
- `uid` (`getuid()`, the app's uid);
- `ptracer`, Yama's exception (`PR_SET_PTRACER`: 0 clears it, -1 is
  `PR_SET_PTRACER_ANY`, a pid that does not exist is `EINVAL`);
- `tracer`: the tracer's pid and the low 32 bits of its creation time as one
  64-bit value, claimed and released with one compare-exchange each, so that
  two tracers cannot both claim a tracee and a reused pid is not the tracer;
- `options` (`PTRACE_O_EXITKILL` acts in the tracee);
- `want_stop` (the tracer's request), `stopped` and `stop_epoch` (the
  tracee's answer, also signalled on `Local\gesher-ptrace-done-<pid>`),
  `guard_tid`;
- `code_epoch` and a ring of the last 64 ranges a tracer wrote (below);
- the threads parked in a Gesher blocking wait while owning a Gesher mutex,
  and whether a stop interrupted that wait (below).

gesher-base's copy in the executable creates it when the process starts, with
the guard thread, and publishes it (its version) only once the guard's id is
in it, so a process that never called anything ptrace-related can be traced
(by its ancestor). A tracer that finds a live process without a published
state or guard waits until 10 s after its creation for its start-up to
publish them (a process older than that without a state is answered at once) (a process
just started with `posix_spawn` is attached to at once); after that, a process
whose gesher-base lives only in a DLL (a state but no guard), or that is not a
Gesher process, cannot be traced (`EPERM`).

The section is not a security boundary. Any code running as the same Windows
user can open and write it, as it can open the process itself and read or
write its memory; it only carries what Linux keeps for the kernel's checks
between cooperating Gesher processes of one user.
Both `prctl`s (bionic's for loaded code, gesher-base's for the runtime's own)
and `/proc/self/status`'s `TracerPid` read and write it. A tracer that exited
no longer counts: whoever reads `tracer` checks that the process is still the
one recorded, and the guard watches it, as Linux detaches a dead tracer's
tracees.

### Dumpable: the zygote's specialisation

Linux's default is dumpable 1, and a native process Gesher starts keeps it.
An Android app process is not dumpable: the zygote's change to the app's uid
clears the flag, `SpecializeCommon` clears what is left for an app uid, and
only `DEBUG_ENABLE_PTRACE` (a debuggable app: `EnableDebugger`, dumpable and
`PR_SET_PTRACER_ANY`) or `PROFILE_FROM_SHELL` (profileable from the shell:
dumpable) sets it again (`com_android_internal_os_Zygote.cpp`). Gesher's
zygote is `SystemServer` starting `GesherAppProcess`; it now passes the
runtime flags ProcessList would (`--runtime-flags`, from the app's
`FLAG_DEBUGGABLE` and `isProfileableByShell()`; `ro.debuggable` is 0), and
the app process applies them before any of the app's code runs
(`ProcessSpecialization`). The single-process driver (`GesherAppDriver`) and
`Main --system-server` (which runs the app in its own process through
`GesherAppProcess.run`) do the same from the app's `ApplicationInfo`. An app's own native code may
change the flag with `prctl`, as PairIP does (it sets 1 itself). Nothing else
in Gesher, ART or the framework stand-ins reads the flag (the GraphicsEnvironment
stand-in's `setInjectLayersPrSetDumpable` is a no-op returning false).

### Permissions

`PTRACE_ATTACH` and `PTRACE_SEIZE` (`kernel/ptrace.c`, `ptrace_attach`;
`__ptrace_may_access`; `security/yama/yama_lsm.c`), decided by `MayAttach`:

- a pid that does not exist, or a process that has exited: `ESRCH`;
- the caller itself: `EPERM`;
- a tracee of another uid: `EPERM` (that needs `CAP_SYS_PTRACE`, which no app
  has), also for the tracee's own parent;
- the tracee is not dumpable: `EPERM`;
- the tracer is neither an ancestor of the tracee, nor the process the tracee
  named with `PR_SET_PTRACER` or a descendant of it, and the tracee did not
  allow any: `EPERM` (Yama `ptrace_scope` 1). Ancestry follows Windows'
  parent pid while the parent runs and is older than the child; an orphan's
  parent is init (as `getppid` already did);
- the tracee is already traced (one tracer per tracee): `EPERM`;
- a process that publishes no ptrace state or has no guard (not a Gesher
  process, or gesher-base only in a DLL) cannot be stopped as a tracee and is
  `EPERM`; so is a process Windows does not let this one open.

`PTRACE_TRACEME` makes the parent the tracer, and fails with `EPERM` if the
process is traced already (a second `TRACEME` keeps failing as before).
Linux's `ptrace_traceme` makes no uid or dumpable check (only the security
modules', which Yama scope 1 passes), and neither does Gesher. Linux does not
stop the process for it, and neither does Gesher; its `TracerPid` is now the
parent's pid, as on a device. A tracer adopts such a child only if the child
names this very process (pid and creation time).

### Stopping the tracee, at a safe point

Linux's attach stop is a group stop: every thread of the tracee stops until
the tracer continues or detaches it, and a thread stops where it can take a
signal: in user code, or in an interruptible wait such as a `read()` of an
empty pipe, never inside the kernel holding a lock. Gesher's system calls run
in user mode, and some hold one of Gesher's named Windows mutexes, shared with
other processes, for a moment: `Global\Gesher.RecordLocks.v1` (every
process's record locks), the log, system properties, open file descriptions,
extended attributes, the CA store. A thread suspended there would block every
process that needs that mutex, the tracer included (an independent review
showed the tracer's own `fcntl(F_SETLK)` and an unrelated process's blocking
for seconds, sometimes for good). One holds it for longer: a pipe
description's gate (`Local\gesher-pipe-...`) is held for a whole read or write
of that description, including while it blocks for data or room.

Each Gesher process has a guard thread (gesher-base, `ptrace_state.cc`) that
waits on `Local\gesher-ptrace-request-<pid>` and on its tracer's process. To
stop, it:

1. suspends every other thread (`NtGetNextThread`, `SuspendThread`, and
   `GetThreadContext` to wait until each has really stopped), repeating until
   a pass finds no thread left running. A thread it cannot suspend (other
   than one that is exiting), or a list it cannot read to its end, means no
   stop yet: it resumes them and retries 20 ms later;
2. lists its process's handles (`ProcessHandleInformation`) and, for each
   mutex, reads its name first (`NtQueryObject`): only Gesher's own
   mutexes count (their name's last component starts with "gesher", in any
   case); a private mutex of a Windows DLL or of the app is neither
   duplicated nor queried, as no other process waits for it. For a Gesher
   mutex it asks for the owner (`NtQueryMutant`). A pipe description's gate
   is held for a whole transfer, also while it blocks (`ParkedScope`
   records the blocked thread and its request). Such a thread is
   interrupted, as a Linux stop interrupts a blocked `read()` or `write()`:
   the guard marks it, resumes the threads and cancels that very request
   (`CancelIoEx` with its handle and `OVERLAPPED`, identified while the
   thread was suspended; cancelling a thread's synchronous I/O waits for
   the thread, so it cannot be done while it is suspended). The transfer
   gives the gate back, waits holding no Gesher mutex until the stop ends,
   and goes on (a write with what it already wrote). A transfer aborted
   with neither of its caller's cancellation events set goes on in the same
   way, so a cancellation that reaches a later request of the same thread
   restarts it too. Nothing is lost: a blocked read has taken no data, and a
   blocked write has at most one byte pending (below). For any other owner
   it resumes them all.

   The guard then waits up to 20 ms for the mutex; with it, it suspends
   them all again while holding it (no thread of the process can take it
   again), releases it and checks again; without it (a thread that took it
   again, or entered its read only after the interruption), it redoes the
   pass. A mutex whose owner died
   (`WAIT_ABANDONED`) is taken and released the same way, so its next owner
   sees an ordinary acquisition; every Gesher use of these mutexes treats
   the two results alike. A handle list it cannot read or a Gesher mutex it
   cannot examine means no stop yet, as in 1;
3. publishes `stopped` once every other thread is suspended and none owns a
   Gesher mutex.

None of this spins: every wait is on the mutex, the tracer's request and the
tracer's process, bounded at 20 ms before the next pass. A stop is delayed
while a thread of the tracee holds a Gesher mutex outside a blocking wait,
for as long as that thread's critical section lasts, as Linux delays a stop
while a task is in an uninterruptible kernel section; it is never published
with a Gesher mutex held.

A blocking pipe write never leaves more than one byte pending in the
kernel. A pending write larger than the room in the pipe is partly read
while it waits, and a cancelled one reports that it moved none although some
of its bytes were read (restarting it sent them twice: an independent review
measured megabytes duplicated over a few hundred stops); and while it is
pending its thread holds the synchronous handle's file object, so every
other operation on that description waits for it, also while the thread is
stopped. So a blocking write is written only in ways that complete at once,
with the description's gate keeping every other Gesher user out:
- when the free buffer space the pipe reports (`WriteQuotaAvailable`) holds
  all of the write, it is written: it is buffered at once, whole;
- otherwise the reported room says little: a read pending on the other end
  reserves its own size from it (a pending read of R bytes leaves
  65536 - R, none from 65536 on), although a write into that read completes
  at once. The endpoint is switched to `PIPE_NOWAIT`, which Windows allows
  only while the pipe holds no data (it refuses with `ERROR_PIPE_BUSY`
  otherwise). When the switch succeeds the pipe is empty: the write goes in
  at once, all or nothing, into the pending read and the buffer, which
  together take about R + 65536 bytes (beyond `PIPE_BUF`, a piece of that
  size first, halved until it fits), and the endpoint is switched back. A
  write into a pending read is therefore neither delayed nor split, as on
  Linux, whatever its size;
- when the switch is refused, data waits for a reader that is not reading,
  and the reported room is real. Beyond `PIPE_BUF`, a piece that fits it is
  written; with no room at all one byte waits in a blocking write, which
  completes when the reader frees room and which a stop or a signal cancels
  without ambiguity (it was read or it was not). A write of at most
  `PIPE_BUF` bytes is never split: it waits with nothing pending in the
  kernel, where a stop can take it, and tries again: a few yields, then
  high-resolution waitable-timer waits of 50 us that grow to 1 ms while the
  wait goes on (`Sleep(1)` lasts a whole timer tick, about 16 ms, and would
  bound such a writer to a few hundred kilobytes a second).

A stopped writer therefore makes no progress beyond what it had written into
the pipe, and a signal or a thread's cancellation of a blocking write returns
the bytes it wrote (a short count) or `EINTR`, as on Linux. Against a reader
that keeps up, writes go through at full speed; a `PIPE_BUF` writer that
waits for a slow reader to free room resumes within the timer waits above,
not at once as a Linux writer woken by the reader does.

While stopped it rescans every 20 ms. A thread the system started meanwhile
(a thread pool worker) is suspended and checked as in 2; if it is not at a
safe point (or cannot be suspended), the guard withdraws `stopped` and runs
the procedure again before publishing the stop anew. While the others are
suspended the guard makes only system calls (no heap, loader lock or thread
pool: a suspended thread may hold them); its buffers are static or
`VirtualAlloc`ed.

A stopped tracee therefore holds no pipe description's gate and has no
transfer pending: the tracer or any other process sharing a description
(after fork or inheritance) can poll, read, write and change its flags while
the tracee is stopped, data written meanwhile stays in the pipe for whoever
reads it, and the tracee's interrupted transfer goes on when it is resumed,
as on Linux. (While the tracee runs and blocks in a transfer, it holds the
gate, and another process's `poll()` of that description fails with
`EOPNOTSUPP` and its blocking transfer waits; and `F_SETFL` on a pipe fails
with `EOPNOTSUPP` while the pipe holds data, because Windows refuses the
mode change then: that is Gesher's pipe model, with or without ptrace.) A
stopped tracee holds no Gesher mutex, so it does not make another process
wait for one; other processes wait for what they would wait for on Linux (a
record lock it holds, data it would write).

`CONT` or `DETACH` resumes the process; so does the tracer's exit, which
releases the tracee as Linux does, after killing it under
`PTRACE_O_EXITKILL`.

The tracer's `wait4` reports the stop once the guard has published it
(`0x137f`, `WIFSTOPPED` with `SIGSTOP`, for `ATTACH`; `PTRACE_EVENT_STOP`,
`0x80057f`, for `PTRACE_INTERRUPT` on a seized tracee), and a tracee's exit
(unless the tracer is also its parent, whose ordinary `wait4` reports it
once). `__WALL` is honoured; `__WCLONE` without `__WALL` does not select a
tracee (it is not a clone); `WNOHANG` and `WNOWAIT` work. This is a small
hook, `PtraceWaitPoll`, at the top of the shim's `CollectOne` (0055), which
otherwise keeps reporting the shim's children as before. Raw `wait4` from
arm64 code (`svc #0`, 260) reaches the same function.

### Requests

| Request | Behaviour |
|---|---|
| `TRACEME`, `ATTACH`, `SEIZE` | as above; `SEIZE` with a nonzero `addr` is `EIO` |
| `SEIZE` options, `SETOPTIONS` | `PTRACE_O_TRACESYSGOOD` (no syscall stops are produced, so it changes nothing) and `PTRACE_O_EXITKILL`; others `EINVAL` |
| `INTERRUPT` | stops a seized tracee; `EIO` for one attached with `ATTACH` |
| `PEEKTEXT`, `PEEKDATA` | `ReadProcessMemory` of the stopped tracee; a word that is not wholly mapped is `EIO`, as is a tracee that is no longer dumpable (`ptrace_access_vm`) |
| `POKETEXT`, `POKEDATA` | the same, as on Linux: `WriteProcessMemory`; a mapped page without write access is made writable for the write (copy-on-write for an image's or a file's pages) and protected again, as Linux's `FOLL_FORCE`; unmapped or unwritable memory is `EIO`; the range is recorded for the translator (below) |
| `CONT`, `DETACH` | resume (and release); a signal in `data` is then sent to the tracee, except after an event stop (`INTERRUPT`), where Linux ignores it; a signal number above 64 is `EIO` |
| `KILL` | `SIGKILL` |
| anything else | `EIO`, Linux's answer to a request it does not support |

Every request but `TRACEME`, `ATTACH`, `SEIZE`, `INTERRUPT` and `KILL` needs a
tracee of the caller's that is stopped, else `ESRCH` (`ptrace_check_attach`).

bionic's `ptrace()` returns a peeked word; the system call stores it at
`data` (an unwritable `data` is `EFAULT`) and returns 0. Raw system calls
take the second form: x86-64 `syscall` 101 and syscall(2) (the shim's raw
table, 0084), and arm64 `svc #0` / syscall(2) 117 (`kSyscalls`). The shim
exports it under a private name, `__gesher_ptrace_syscall`, not as a libc
symbol.

### Addresses

A tracer passes the tracee's virtual address, and the tracee's memory is read
there. That is exact for Gesher: the ELF loader maps loaded code at the
addresses the code sees, and the arm64 translator's guest addresses are host
addresses (dynarmic's fastmem with a zero base, and without fastmem its
callbacks dereference the guest address itself; docs/arm64.md). A guest stack
slot, heap block or global is therefore read and written by an x86-64 or an
arm64 tracer the same way. Arm64 pointers carry no tag here (no TBI or MTE).

### Code a tracer writes into a translated tracee

`POKETEXT` and `POKEDATA` are the same request on Linux, and either may write
code, so every POKE records its range in the tracee's state (`code_epoch` and
a ring of 64 ranges). Before its process runs again, with every other thread
still suspended, the guard publishes the ranges where the arm64 translator
reads them (`__gesher_ptrace_code_writes`), then calls the translator's halt
hook (`__gesher_ptrace_set_halt_hook`): it asks every running `Jit` to stop
(`HaltExecution`, which takes no lock). Each thread then returns to its
dispatcher, which drops what it translated from each written range
(`InvalidateCacheRange`; everything, if more writes were made than the ring
holds) and runs on; a thread in host code does the same at its next dispatch.
A thread spinning in translated code with no host call therefore runs the new
code too. The dispatcher publishes the `Jit` it is about to run (a full
barrier) before it clears old halt requests and refreshes, so a thread
suspended anywhere in its dispatcher either sees the ranges in its refresh
or is halted before running a block.

### What is not provided

- **Registers.** `GETREGS`, `GETREGSET`, `PEEKUSER` and the rest are `EIO`.
  An arm64 guest's live registers belong to the translator, not to the
  suspended Windows thread's context, so the thread context would be wrong.
- **Signal-delivery, syscall, exec, fork and exit stops**, single-stepping,
  `PTRACE_LISTEN`, siginfo: not produced. The options that would ask for them
  (`TRACEFORK`, `TRACEVFORK`, `TRACECLONE`, `TRACEEXEC`, `TRACEVFORKDONE`,
  `TRACEEXIT`, `TRACESECCOMP`, `SUSPEND_SECCOMP`) fail with `EINVAL` rather
  than being accepted and never reported.
- **Per-thread tracing.** A tracee is a process (its thread-group leader);
  attaching to another thread id is `ESRCH`.
- **exec.** A tracee that calls `execve` starts a new Windows process (0055)
  that is not traced; Linux keeps tracing across exec.
- **`kill(pid, SIGSTOP)`** of a Gesher process suspends the whole process
  (`NtSuspendProcess`), its guard included: a tracer that then attaches gets
  no stop until it is continued.
- **`State:` in `/proc/<pid>/status`.** Only `/proc/self` exists, and a
  stopped process cannot read its own; `TracerPid` is reported.
- A thread the system starts while the tracee is stopped can run until the
  next rescan (at most 20 ms) before it is suspended and checked.
- More threads than the guard's list holds (8192): it never publishes a stop.
- A process that publishes no ptrace state (a Windows program, or a Gesher
  executable without gesher-base) cannot be traced.

## With fork (0092)

A process cloned with `RtlCloneUserProcess` maps the parent's section through
its inherited view. Every accessor (`gesher::ptrace::Self()`) checks the
current pid; in a clone it creates the clone's own section and guard, keeps the
dumpable flag (a forked child keeps its `mm`'s) and drops the parent's tracer
and ptracer exception, as Linux. The shim's tracee map is emptied in a clone
(a forked child traces nothing), unmapping the views it copied; none of its
handles is inheritable. The clone should call `Self()` from the executable's
copy of gesher-base early, so its guard runs before a tracer asks; the guard
is a dedicated thread, not the thread pool (which does not work in a clone).
The translator's code-epoch counter is at the same address in the clone, and
the clone's guard updates it. A tracer's own identity (`SelfTracerId()`: pid and creation
time) is cached per pid, so a clone computes its own.

## Relation to 0055 and 0086

- 0055's `wait4` keeps its contract for the shim's children; tracees are
  added through one hook, and a tracee that is also a child is reported once.
- 0086's Ijiami checks do not use ptrace across processes. Its guest
  `fork`/`vfork` stay `EAGAIN` here (0092 decides that). `TRACEME` still
  succeeds once and then fails with `EPERM`; `TracerPid` now shows the parent
  after it, as on a device.

## Consequences

- PairIP's sequence works between real processes once its child exists
  (0092). `tests/exec/ptrace_contract.c` runs the same sequence between a
  parent and a spawned child, for x86_64 and translated arm64, and the
  record-lock case the review found.
- Every Gesher process has one more thread (the guard: a 64 KiB stack
  reservation, blocked in one wait) and a page-sized named section: measured
  at 35-190 µs and about 28 KiB of private memory per process.
- A stopped tracee is really stopped: its timers and watchdogs do not run
  until it is resumed, as under Linux. Stopping it waits until its threads
  have left Gesher's cross-process critical sections, and interrupts (then
  continues) its blocking pipe transfers.
- Gesher's media audio state takes a named mutex
  (`Local\gesher-<device>-audio-lock`) before the spin lock in its shared
  mapping, and holds the spin lock only inside it, so that a stopped tracee
  never holds the spin lock; a process that cannot take the mutex (an older
  binary, a failed creation) still excludes the others through the spin
  lock.
