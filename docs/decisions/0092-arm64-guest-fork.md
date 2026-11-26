# 0092 — A real fork for translated arm64 code

**Status:** accepted, 2026-11-24 (maintainer decision). Amends
[0055](0055-exec2-native-processes.md) and [0086](0086-packer-art-hooks.md) for
arm64 guest code. Companion: gesher-runtime/gesher#461 (ADR 0093), ptrace and
the tracer's wait between such processes.

Code: `compat/posix/src/process.cc` (`gesher::CloneProcess`, gesher/fork.h),
`compat/posix/src/fork_locks.cc` (the fork set, the exit gate),
`compat/posix/src/spawn_broker.cc` (process creation for forked children),
and the `GESHER_FORK_LOCK`
registrations in gesher-base, the shim and the ELF loader,
`compat/bionic/process_bionic.cc` (the shim's fork, children and wait),
`compat/bionic/pthread_bionic.cc` and `signal_bionic.cc` (their state in the
child), `compat/arm64/dll/guest_libc.cc` and `translator.cc` (the guest's
`fork`/`vfork`/`_Fork`, raw `clone`, `wait4`, `exit`, `rt_sigaction`).
Tests (tracks/47-arm64.ps1, built by arm64.yml): `tests/native/arm64_fork_exec.c`
(under gesher-exec, and inside dalvikvm as `gesher.Arm64Fork`, and
`gesher.Arm64ForkChurn`: forks while Java threads keep starting and leaving;
`tests/native/java/gesher/`), `tests/native/arm64_fork_adversarial.c`
and `arm64_fork_ctor_lib.c` (forks under contention on every system call path,
signals right after fork, a fork from a library constructor, from four
threads, nested); the fork + `exec`/`posix_spawn` checks run the test program
itself.

## Context

0055 made `fork()`/`vfork()` fail with `ENOSYS` for native code: Windows has
no `fork()`, and the inline emulation it replaced ran the child's code in the
parent. 0086 made arm64 guest `fork()` fail with `EAGAIN`, because Ijiami's
`libijmdetect-drisk.so` forks a child that sends itself SIGABRT, and the shim's
in-process "child" killed the app; that fork's second return also went back to
the host frame with the child's guest registers.

The maintainer decided on 2026-11-24, under CONTRIBUTING.md's scope ("the same
experience, implemented for Windows"), that a real fork/clone, with a real
separate Windows process, is in scope for arm64 guest code. The motivating
case is Google Play's PairIP (`libpairipcore.so`, Spoke): the parent calls
`clone(220, flags 0)`, and the child attaches to the parent with ptrace (ADR 0093),
copies one stack word and exits; the parent waits for it with `__WALL`. Today
that clone fails, the word is never copied, and the VM crashes later.

ntdll's `RtlCloneUserProcess` makes the process Windows' own fork needs
(probes on Windows Server 2025, 2026-11-24): a copy-on-write copy of the whole
address space with only the calling thread, returning in both processes
(`STATUS_PROCESS_CLONED` in the child); the CRT heap, `thread_local`,
`CreateThread` and `std::thread` work in the child; the exit code reaches the
parent; 300 stress clones ran without a deadlock. The child has no ntdll thread
pool (the parent's worker factory is not its own; `RegisterWaitForSingleObject`
raises `0xC000070A` there).

## Decision

**Guest `fork()`, `vfork()`, `_Fork()` and fork-like raw `clone` make a real
child process.** `x86-64` code's `fork`/`vfork` stay `ENOSYS` (0055): its
loaded code runs on the host's own stack and registers, with no translator to
resume it, and nothing has asked for it.

- **The clone.** The guest's call reaches the shim's fork (`__gesher_fork`)
  on the guest thread, inside the translator's host call. The shim takes the
  fork set (below), then `gesher::CloneProcess` clones. The child's only
  thread is the copy of the calling one: it returns from the same host call
  into the same Jit, at the guest's return address, with `x0 = 0`; the parent
  gets the child's pid. A raw `clone` with a stack runs the child on that
  stack.
- **The fork set** (`gesher/fork.h`). Every lock a system call path of
  gesher-base, the bionic shim or the ELF loader can take is registered
  (`GESHER_FORK_LOCK`), with an order; the translator adds its own through the
  fork's hooks. The fork takes all of them so that no other thread is inside
  any while the process is cloned: the child's copies of what they guard are
  consistent and none is held by a thread the child does not have. Each lock
  is only tried, never waited for: when one is busy everything taken so far is
  released and the round starts again (yields, then 1 ms sleeps; after 10 s
  the fork fails with `EAGAIN`), so no lock order between the fork and other
  threads can deadlock. Registered, outermost first:
  1. the ELF loader's recursive lock and data lock, ELF TLS, the `%fs`
     rewriter's locks;
  2. the descriptor table, the descriptor-operation guard, the socket table;
  3. the process tables: children, popen streams, the children's mailboxes,
     the shim's signal queue and state, gesher-base's signal state, the
     pthread_atfork list, thread registry, descriptor slabs and keys;
  4. subsystems: environment (writer, snapshot), `mmap`'s state, properties
     and the shim's other tables, epoll and eventfd registries, the stdio
     stream registry (not the streams' own locks, below), binder files, Windows `dlopen`,
     memfd seals, revocable files, unix rights, fonts, inotify, DNS queries,
     the JNI bridge's stubs and kinds, `rand48`, `/proc` names and the boot
     id, memfd seal changes, the local IPv4 address cache (`IP_PKTINFO`),
     the identity, file and mapping of `/system/bin/linker64` (ADR 0096; made
     once under registered locks instead of `std::call_once`), the ART
     stand-ins' loads;
  5. leaves: futex buckets (their waiter lists are emptied in the child: the
     waiters were the parent's other threads), `memalign`, logging caches,
     binary128 parsing, resource limits, condition variables (made anew in the
     child only);
  6. the translator (its hooks, last): the Env pool,
     Envs, host-entry stubs, va_list functions, trampolines, guest code pages,
     code regions, host pages, TLS, processor ids, the guest pages'
     protection record and the live Jits with each Jit's page record (guest
     instruction-cache maintenance), and dynarmic's exclusive-monitor spin
     lock. In the child the other threads' Jits stay registered (an
     invalidation still reaches them, harmlessly), with their page records'
     locks made anew.

  At the start of each round, before any lock of the set, the fork takes
  the translator's guest mapping lock, which a guest mmap/mprotect/munmap/
  mremap holds across the host call and the translator's record of it: it is
  the outermost lock of such a call, and those calls keep it while they wait
  for gesher-base's mapping lock, so a fork that tried it only once would
  seldom find it free. It is tried for a slice of up to 50 ms (yields, then
  1 ms sleeps); a round that does not get it fails like one that found a lock
  of the set busy, and the fork set's 10 s deadline bounds the whole wait
  (`EAGAIN`), so a thread that holds it while waiting for something the
  forking thread holds (a fork from a `dl_iterate_phdr` callback while
  another thread unmaps that object) is never waited for without bound. A
  round that fails gives it back before the fork yields, so a library
  constructor that maps memory under the ELF loader's lock (which the fork
  only tries) goes on between rounds. It is taken after the
  `pthread_atfork` prepare handlers (which may map memory under their own
  locks: on Linux they run before any kernel lock), and given back before the
  parent and child handlers.

  Before them (order 50) comes the helper lock: Gesher's own helper threads
  (a forked child's mailbox thread and child watchers, the spawn broker) hold
  it while they work and are started only once their loader initialisation is
  over, so the clone, which takes the process heap's lock, never copies one in
  the middle of an allocation.

  `dl_iterate_phdr` callbacks are not locks but are waited for: an unmap of a
  caller's reservation (`android_dlopen_ext`) waits until no callback runs
  for an object loaded there. Each thread lists the objects its running
  callbacks are for; in the child each object's count becomes the forking
  thread's own (the other threads' callbacks never end there), so the
  child's unmap does not wait for ever.

  Per-object locks (one descriptor's, one binder file's, one epoll's, one
  directory stream's) are not in the set: an object another thread is using
  at the moment of the fork may be unusable in the child. **stdio streams**
  are not waited for either: another thread may hold a stream across a
  blocking read (a thread reading stdin), and Linux's fork waits for no
  stream. In the child each stream's lock is made anew, free, unless the
  forking thread itself held it (`flockfile` around `fork`), which keeps it at
  that depth; a stream another thread was writing at that moment may hold part
  of that write in the child's copy of its buffer.
- **ntdll's loader lock** is not taken. `RtlCloneUserProcess` first waits
  for the loader's work in progress (a thread that is leaving holds it while
  it runs its TLS and FLS callbacks under the loader lock), then takes the
  loader lock itself, and makes the child's free. Holding the loader lock
  across the clone deadlocked with a leaving thread (stopped between the two);
  holding Gesher locks across it deadlocked when a leaving thread's callback
  wanted one (the translator's `thread_local` Env, ELF TLS arenas, the
  signal state's per-thread record), and when a thread left while the fork
  was trying the set. Two rules make the exit path safe:
  - **The exit gate.** Every thread Gesher starts (`pthread_create` in
    gesher-base, which ART's threads use, and the shim's guest threads) calls
    `gesher::ThreadLeaving` as its start routine returns or it calls
    `pthread_exit` (gesher-base's threads through a winpthreads key whose
    destructor runs on both ways out, before the thread's exit callbacks),
    before ntdll runs its exit callbacks: it passes a gate (shared) and
    records its thread id and creation time (not a handle: a process keeps
    no handle of a thread that left, and one that never forks holds exactly
    the handles it held before). A fork closes the gate (exclusive), waits for the
    threads already past it to be gone, and only then tries the set; it opens
    the gate after the clone. If one of them is still there after 5 s (stuck
    in another library's exit callback), the fork opens the gate again and
    fails with `EAGAIN`: it never takes the set while such a thread may be
    inside the loader lock. So while a fork tries or holds the set, no thread
    Gesher started is in its exit callbacks; one that is leaving waits at the
    gate, outside the loader lock. Threads Gesher does not start
    (`CreateThread` and `std::thread` in host code, the thread pool's
    workers, other libraries' threads) do not pass the gate; the second rule
    covers their exit callbacks.
  - **The exit paths never wait for the set.** For threads Gesher did not
    start, and other libraries' callbacks, the callbacks Gesher runs as a
    thread leaves take their locks with `gesher::LockUnlessForkHolds`: they
    wait as usual while a fork only tries the set (the lock comes free at
    once), and give up only while the fork holds the whole set
    (`gesher::ForkSetHeld`, set from a successful round to the release).
    Then the translator retires the Env on a lock-free list (released before
    the next fork takes its locks, or by the next thread that takes an Env),
    ELF TLS defers its arena the same way, the signal state's per-thread
    record (heap-allocated, not a `thread_local` destructor) is marked dead
    for the next registration to sweep, and the shim's key destructor pass
    reads the keys without their lock (the fork holds it, so they cannot
    change).

  A guest TLS destructor that a host thread runs as it leaves is guest code
  and may still call into a locked path; that remains a limit.
- **In the child each lock is made anew** (placement), because releasing a
  lock a thread of the parent waited for would try to wake a thread that does
  not exist in the child. A recursive lock the forking thread itself held in
  outer frames (a fork from a library constructor, under the loader's lock) is
  rebuilt owned by the child's thread at that depth, so those frames' guards
  release it normally.
- **Handles.** Every handle of the parent is inherited, at the same value,
  for the clone, and then given back its own inheritance flag in both
  processes: descriptors keep their close-on-exec flag, and the child has the
  handles behind every descriptor (fork keeps close-on-exec descriptors) and
  behind the runtime's own state (pipe gates, events). Not inherited: the
  thread pool's worker factory, completion ports and wait packets, ALPC
  ports, ETW registrations and graphics kernel objects, which would tie the
  child's work to the parent. A flag is given back only if the value still
  names an object of the same type and access (another thread may have closed
  it and opened another, inheritable on purpose). Gesher's spawns hold a spawn
  lock (shared) around `CreateProcess`; the fork holds it exclusively from the
  inherit step to the restore step. A process another component starts in
  that window with `bInheritHandles` and no handle list would inherit every
  handle of the process: Gesher's own spawns pass explicit handle lists and
  are excluded by the lock; a third-party library that spawns that way is a
  documented risk.
- **Processes a forked child starts.** A clone cannot call `CreateProcess`:
  kernelbase reads the CSR shared section csrss maps into a process it
  starts, and a clone has no copy of it (an access violation). The first
  process that forks (started normally: the root) serves a named pipe, the
  spawn broker; `exec`, `posix_spawn`, `system` and `popen` in a forked child
  (and in its forked children) send it the `CreateProcess` request, and it
  creates the process with `PROC_THREAD_ATTRIBUTE_PARENT_PROCESS` set to the
  requester and the requester's handle list: the new process is the
  requester's child (its parent pid, its inherited handles and standard
  handles, interpreted in the requester's handle table), as if the requester
  had created it. The process and thread handles are duplicated into the
  requester.
  - *Who may ask.* The security boundaries are Windows': the user, the
    AppContainer and the integrity level. The pipe
    (`\\.\pipe\LOCAL\gesher-spawn-<root pid>-<random>`, in the namespace an
    AppContainer may create pipes in, so the sandboxed and the ordinary
    process use the same) has an explicit DACL granting only the process's
    user, and its AppContainer in the sandbox, so code outside those cannot
    connect. Code of the same user, container and integrity level could open
    the root itself with full access (`OpenProcess`), so the broker gives it
    nothing it lacks; what the broker's own checks keep out is a request
    from the wrong process, not a stronger attacker. The root serves only a
    requester that is the pipe's client and a forked descendant of the root
    (each fork in the root's tree records the child's pid and creation time
    in a table the tree shares, a section whose view stays shared in clones;
    a reaped child is taken out; the root opens the requester first and then
    compares its creation time, so a reused pid is another process) and that
    sends a 128-bit secret the root drew (`BCryptGenRandom`), which its
    clones inherit in memory. The pipe name has its own random value,
    independent of the secret (names can be listed). Another process cannot
    serve the name: the root makes the pipe with `FILE_FLAG_FIRST_PIPE_INSTANCE`
    under a name no one can predict, with a limit of eight instances, and makes
    all eight at once, so no instance is left for anyone else to add. (The
    root then also takes the right to add instances out of the DACL, but that
    is not the barrier: the pipe's owner keeps `WRITE_DAC` and could grant
    it again, and so could any process of the same user and level, which
    could as well act on the root directly.) The client checks that the
    pipe's server is the root (`GetNamedPipeServerProcessId`, while it holds
    a handle to the root, so that pid cannot be another process's). A
    same-user process can keep the instances busy (a denial of service);
    that is outside this model. The parent records a child in the table
    just after the clone, while the child already runs: the child's first
    request waits (up to 5 s) for its entry, and if the table had no room
    (8192 live descendants) the spawn fails with `EAGAIN`.
  - *Load.* Eight instances serve at once, each on its own thread; a client
    has 5 s to send its request and 5 s to take the reply, so a client that
    stalls holds one instance for at most that. A request over 1 MiB fails
    with `E2BIG`.
  - *When the root ends.* Clones do not inherit the broker's pipe instances
    (`CloneProcess` leaves them out), so the pipe ends with the root. A
    client waits for its reply and for the root's process together, and a
    spawn in a forked child after the root ended fails at once with `ENOTSUP`
    (`EOPNOTSUPP`): process creation is not available to an orphaned clone,
    and retrying would not help. If the broker cannot start or stops (it
    should not), its spawns fail the same way and Gesher says why once on
    standard error.
  - *`exec` in a forked child* is 0055's exec: the child starts the program,
    waits for it and ends with its status. The program runs in a job that
    ends with the child (`JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE`, assigned before
    it runs), so `kill(SIGKILL)` of the child's pid ends the program too. The
    job also has `JOB_OBJECT_LIMIT_SILENT_BREAKAWAY_OK`: only the program
    itself is in it, and the processes it starts outlive it, a SIGKILL of
    the child's pid included, as on Linux. Other signals sent to the child's
    pid are forwarded to the program. Unlike Linux, the program has its own pid,
    and its `getppid()` is the forked child's pid, not the forked child's
    parent.
- **The child's state.** In the child, after the clone:
  - the threads: the pthread registry keeps only the forking thread (its
    pthread_t, with the child's thread id and pid in its descriptor); the
    parent's other threads' descriptors report no thread (tid 0, `ESRCH`);
  - signals: only the forking thread's state, no shim signal thread, and no
    pending signals, as on Linux;
  - children: none (the parent's children are not the child's);
  - the signal mailbox other processes' `kill()` reaches: the parent makes
    the child's (named by its pid) before `fork` returns and keeps it until it
    reaps the child, so a signal sent at once waits there; the child opens the
    same mailbox and watches it with a thread of its own (no thread pool), and
    marks it as a forked child's. A mailbox records its owner's process
    creation time: a sender skips a mailbox whose owner is not the process
    now using that pid, and a new owner of a reused pid resets what was left
    in it;
  - ntdll's own clone locks: `RtlCloneUserProcess` takes some of ntdll's
    SRW locks around the clone (the protected-policies lock, the `.mrdata`
    lock that `LdrForkMrdata` takes, the TLS and fork locks) and gives them
    back in both processes, but not on every Windows build: on Windows
    Server 2022 (build 20348) the child keeps `RtlpProtectedPoliciesSRWLock`
    and `LdrpMrdataLock` held (each reads 1 in the child), so the child's
    first `RtlAddFunctionTable` (the translator's unwind data for new code:
    a signal handler's first run, a new thread's code) and any fork of its
    own wait for ever. That was every Server 2022 failure: signals sent
    right after fork were never handled, nested forks, `contend
    thread`/`exit` and `fork` in dalvikvm hung (found with a clone probe and
    debugger stacks on the CI's windows-2022 runner). When the clone returns
    in the child its only thread is outside those functions, so each of
    these locks must be free; one still held (or recording waiters) is
    stale and is made free, first thing in the child, before any other
    thread exists. Which locks are made free is found in ntdll's code, on
    x86-64 only: in `RtlCloneUserProcess` (its `.pdata` extent) and in each
    function it calls with a `call rel32` whose target starts a `.pdata`
    function or lies in none (a leaf), within that callee's first 0x40
    bytes, each `lea rcx,[rip+X]` followed, after at most three
    instructions that leave rcx alone (stack adjustments, pops, `xor
    edx,edx`, `xor r8d,r8d`, `mov edx,imm32`), by a call or jump to the
    exported `RtlAcquireSRWLockExclusive`/`Shared`, with X inside ntdll's
    image on an 8-byte boundary. On build 26100 that finds the TLS,
    protected-policies, `.mrdata` (through `LdrForkMrdata`), the two
    feature-configuration (`RtlpFcProcessManager`), critical-section-list
    and fork locks; it does not find locks taken through a register computed
    otherwise (the 16 `LdrpDelayedTlsReclaimTable` locks, `mov rcx,rbx` in
    a loop) or deeper in the call tree. The scan is the repair; the proof is
    a check that does what a forked child does: a throwaway clone makes the
    locks free, adds and removes a function table and makes a clone of its
    own, which takes every lock `RtlCloneUserProcess` takes (those the scan
    does not find included), so a lock left held anywhere there makes it
    hang. The throwaway clone ends as soon as its own clone returns, and
    that one ends at once, so two copies exist together only while the
    second is made; each commits as much memory as the forking process, so
    the check needs room for two copies besides it, and two more processes.
    It runs once per build of ntdll and of Gesher on a machine: a working
    result is kept in the user's local application data (the profile's
    folder, read once before the first fork, outside the fork set, so the
    app's environment cannot move it), in `Gesher\clone-check`, named by
    ntdll's link time and size, the link time of the image that carries
    gesher-base, and the architecture; a forked child has its parent's
    result. When the first copy cannot be made (memory, a job's process
    limit) the fork's own copy could not be either: that fork fails, with
    `ENOMEM` for memory and `EAGAIN` for a process limit (as Linux's
    `RLIMIT_NPROC`), and nothing is kept. When only the second copy cannot
    be made (it needs room for one more process than the fork does), the
    first level is proven (a function table added in a clone) and the fork
    goes on without the second level, which is checked again after 30 s;
    nothing is kept either, so a limit never turns fork off. Server 2022's
    stale locks are repaired in every child whether or not the check ran;
    only an unknown build whose clone leaves a lock the scan does not find
    could then make a child whose own fork hangs, and only while such a
    limit stops the check. Only a step that returned an error, or one that
    did not end within 15 s (a lock never given back; a probe that ended by
    itself right at the bound counts with its own result), turns fork off
    in the process: `EAGAIN`, said once on standard error. The probe
    confirmed it on 20348 (without the reset the child's
    `RtlAddFunctionTable` and nested clone hang every time; with it, both
    work) and that build 26100 leaves the locks free;
  - the child's start: the child opens its signal mailbox and starts its
    watcher thread and its ptrace guard before `fork` returns in it, and
    tells the parent so once the watcher thread and the guard both run
    their own code (the guard's thread id is published then); the parent,
    after it releases the fork set, waits for that (up to 15 s; in a forked
    child each helper thread's start is bounded at 5 s and logged if it
    does not come, and such a thread never runs its work; in a process that
    is not a forked child it is waited for without a bound, as the mailbox
    thread and the broker's always were, and the spawn broker reports a
    helper that did not start as stopped). The guard now starts through the
    same function everywhere, so a process that is not a forked child waits
    without a bound for its guard to run its own code: in the executable's
    static initialisation, run by the C runtime's entry point after the
    loader has finished, never under the loader lock. Only the executable's
    copy of gesher-base starts a guard; a DLL that links gesher-base (a test
    module) does not, so no thread is started or waited for in a `DllMain`. A child that cannot start them, or that ends first,
    has run no guest code yet: it is ended and `fork` fails with `EAGAIN`,
    rather than return a child that loses signals or cannot be traced;
  - ntdll's address-wait table: `WaitOnAddress` keeps its waiters in the
    PEB (`PEB.WaitOnAddressHashTable`, 128 buckets, each a list of wait
    blocks on the waiting threads' stacks), and every wait that goes through
    it uses it: Gesher's futexes, the guest's mutexes and condition
    variables, ntdll's `CRITICAL_SECTION` waits. The clone copies the PEB, so
    the child's table still lists the parent's threads that were waiting
    then, threads the child does not have, and a wake in the child could go
    to one of them and be lost: the child's own waiter would sleep for ever
    (ntdll locks every heap for the clone, so a parent's thread that
    allocates is exactly one that waits then). The child's only thread waits
    for nothing at that point, so the table is emptied first thing in the
    child, before any other thread exists. The field is in the public PEB
    symbols, at 0x3A0 in the 64-bit PEB (x64 and ARM64 share the layout)
    since Windows 8; `RtlpWaitOnAddress` indexes it so on x64 build 26100.
    So that an offset that moved never makes the clear write over other
    fields, it is used only when the layout checks out: Windows 8 or later
    (`RtlGetNtVersionNumbers`, which compatibility modes do not change), and
    every non-empty bucket in the parent, just before the clone, points to
    committed private read-write memory (a stack) on an 8-byte boundary;
    otherwise fork fails with `EAGAIN` rather than make a child whose wakes
    may be lost. In the child only buckets that pass the same check are
    cleared. A 32-bit build has another layout and fails fork the same way;
  - the ptrace state (ADR 0093): `ReinitializeAfterFork` calls the
    executable's copy of gesher-base's `gesher::ptrace::Self()`, so before
    fork returns in the child it publishes its own `Local\gesher-ptrace-<pid>`
    section and runs its own guard: a tracer can stop it at once. The section
    keeps the dumpable flag the parent had at the clone (recorded while the
    fork holds `ptrace_state`'s `g_lock`, which is in the fork set and which
    `PR_SET_DUMPABLE` takes, so the parent cannot change it in between), as a
    Linux child keeps its `mm`'s, and has no tracer, no Yama ptracer
    exception, no stop request and no parked threads (the parent's are the
    parent's threads'). The child's view of the parent's section is
    unmapped, never written, and its inherited copies of the parent's
    handles (section, guard events, the tracer it watched, threads the
    guard had suspended) are closed: only values the clone really brought
    (the child records its handle values when the clone returns, before it
    opens any; the guard may have opened one after the parent made its
    handles inheritable, and that value may later name another handle of
    the child's). The cached `SelfTracerId` is cleared, so the child's is its own
    pid and creation time. The shim's tracee table (its lock is in the fork
    set too) is emptied in the child, closing the inherited handles and views
    of the parent's tracees without touching their state: a forked child
    traces nothing, as on Linux;
  - memfd write leases: a writable shared mapping of a memfd the child
    inherited gets its own lease in the child (the parent's byte-range lock
    is the parent's), so `F_SEAL_WRITE` stays `EBUSY` while the child maps
    the file, as on Linux. When there are such mappings, the parent waits for
    the child to have taken them before it releases the fork set, so a seal
    right after `fork` returns is already `EBUSY`. The wait is short (up to
    2 s; the child only renews byte-range locks there, and every other
    thread of the parent that makes a system call waits meanwhile). The
    child then waits for the parent's word before it goes on (or for the
    parent's end): a child that has not taken them by then, or that ended
    first, has run no guest code: it is ended, `fork` fails with `EAGAIN`
    (as fork(2) may), and a warning is logged (`gesher-fork`), rather than
    return a child that a seal could miss;
  - the translator: the same Env (guest stack, TLS block, Jits and their
    translated code) goes on; the parent's other threads' TLS blocks and
    profiler entries are dropped; the exclusive monitor is cleared (a
    store-exclusive spanning the fork fails and is retried, as after any
    monitor clearing);
  - `pthread_atfork` handlers (registered by the guest; the shim's
    `pthread_atfork` used to drop them) run as bionic runs them: prepare
    handlers last registered first, then parent or child handlers in order.
    `_Fork` and the raw `clone` system call run none, as on Linux.
- **Process semantics.**
  - `getpid()` is the child's own pid; `getppid()` the parent's.
  - `wait4`/`waitpid`/`wait` (and raw `wait4`, 260) report the child's real
    status, honour `WNOHANG`, `__WCLONE` and `__WALL` as Linux's
    `eligible_child` does (a child whose exit signal is not SIGCHLD is waited
    for only with `__WCLONE` or `__WALL`), accept `__WNOTHREAD`, reject
    unknown options with `EINVAL`, and fill `rusage` from the child's times
    and peak working set. A child is reported as soon as its process ended:
    the wait blocks on the children's process handles, waking to run signal
    handlers. A handler that runs during the wait interrupts it with `EINTR`,
    unless it was installed with `SA_RESTART`, which restarts the wait, as on
    Linux; an interrupted wait first looks at the children once more and
    reports one that has ended (the `SIGCHLD` of the awaited child itself
    does not make the wait fail), and a child's exit signal still pending
    when its wait returns is handled before the wait returns, as Linux runs
    it on the wait's return, rather than interrupting the next wait. `WUNTRACED` and `WCONTINUED` are accepted; this
    model has no job-control stops, so they report nothing (the tracer's
    stops are 0093's).
  - With SIGCHLD ignored, or `SA_NOCLDWAIT`, a child is reaped at its exit,
    as Linux does: `wait` then blocks until the children are gone and fails
    with `ECHILD`.
  - **Exit status.** A forked child's death by a signal ends it with a
    Windows exit code of its own (`0x0F5E0000 | signal`, gesher/fork.h); its
    ordinary exit codes are 0..255. So `exit(130)` is an exit and SIGINT is
    a death by SIGINT. A `kill(SIGKILL)` from another Gesher process uses the
    same code when the target's mailbox says it is a forked child, and so do
    a guest fault (SIGSEGV, SIGBUS...) and `abort()` in a forked child. A
    process whose parent uses these codes uses them too, and decodes its own
    children with them: a program a forked child `exec`s (which replaces it:
    the child ends with the program's status) or spawns. A process learns it
    at startup from its parent's mailbox (its forked flag, for the process
    that now has the parent's pid and is older than it); nothing is added to
    the program's environment. Other Gesher processes keep 128 + signal
    (Windows callers of a program read it), and a parent that was not forked
    still reads its spawned programs' codes that way. `kill()` of a pid
    whose mailbox an earlier process with that pid left (another creation
    time) treats the target as a process with no mailbox: a default-ending
    signal ends it with 128 + signal.
  - The exit signal (SIGCHLD, or a raw clone's `CSIGNAL`, none for 0) reaches
    the parent through the shim's external-signal path with `si_pid`, and
    `CLD_EXITED`/`CLD_KILLED` with the code or signal.
  - `exit()` in the child runs the program's exit handlers and flushes stdio;
    `_exit`, raw `exit` (93), raw `exit_group` (94) and a death by a signal
    flush nothing, as on Linux (the child's stdio buffers are copies of what
    the parent had not written). None runs the host runtime's static
    destructors or DLL detach routines in a child. Raw `exit` ends the calling
    thread, and the process with the last of the program's threads.
- **Raw `clone` (220).** Fork-like flags make the child as above: no
  `CLONE_VM`/`CLONE_THREAD`, any exit signal up to 64, optionally a child
  stack, `CLONE_PARENT_SETTID` (the child's pid in the parent's word),
  `CLONE_CHILD_SETTID` and `CLONE_CHILD_CLEARTID` (bionic's own fork uses
  both: the child's thread id, as its `gettid()` returns it, is stored in the
  child's copy of the word, and cleared with a futex wake when that thread
  leaves by raw `exit`), `CLONE_DETACHED` and `CLONE_UNTRACED` (no effect, as
  on Linux). Linux's own invalid combinations are `EINVAL`. Thread-style
  clones and flags this model cannot honour (shared memory, descriptor table
  or fs information, a new TLS, vfork suspension, `CLONE_PTRACE`, namespaces)
  keep `ENOSYS`.
- **Raw `rt_sigaction` (134)** converts the kernel's arm64 struct to the
  shim's `sigaction`, so a check that compares the two sees the same answer.

### What the child cannot do (documented limits)

- **Binder.** A binder descriptor the parent opened is not usable in the
  child: its ioctls and `mmap` fail with `EINVAL` (the child would share the
  parent's broker connections and has no copy of its binder buffer; Linux's
  binder refuses a mapping from another process, and Android's libbinder
  refuses binder after fork). A new `open` of /dev/binder in the child is its
  own connection.
- **The thread pool.** The child has none. The paths of the shim and
  gesher-base that used it take another way there (children are watched by a
  thread of their own; the signal mailbox has its own thread; signal state is
  not retired; pipe transfers block without asynchronous cancellation), and
  `timerfd_create` and `inotify_init1` fail with `ENOSYS` in a forked child.
- **Names.** `getaddrinfo` of a name and `getnameinfo` of a host name fail
  with `EAI_SYSTEM` and `errno` `ENOSYS` in a forked child: Windows' resolver
  (dnsapi) faults loading in a clone, as IMM32's thread attach does (the
  clone does not map the session's win32k shared memory; IMM32's thread
  callbacks are switched off in the child), and the shared resolver runs on a
  thread pool. Numeric addresses still parse.
- **Other locks.** Per-object locks, guest-level locks and locks of code
  outside Gesher (ART's in dalvikvm, a stdio stream of the CRT's own) that
  another of the parent's threads held at the fork stay held in the child,
  as POSIX allows for a multithreaded fork. Windows SRW locks and condition
  variables keep their waiters in the lock word itself (wait blocks on the
  waiters' stacks), not in the address-wait table: the fork set makes the
  child's copy of each of its own locks anew, but one outside it (a Windows
  DLL's, the app's own host code's) that a parent's thread waited on at the
  clone keeps that stale waiter in the child, and a release there may wake
  no one. A `CRITICAL_SECTION` outside the fork set (the UCRT's own locks,
  say) can likewise carry a stale "waiter woken" bit in its `LockCount`
  into the child: ntdll repairs only its heap and loader critical sections
  in a clone.
- **ART.** A fork in dalvikvm copies ART's process with one thread; the
  child can run guest code and make system calls, not Java.
- **Orphans.** As on Linux, a forked child is not ended with its parent: a
  test harness that kills a parent must also end the parent's children
  (tracks/47-arm64.ps1 ends the process tree of each run). An orphaned
  forked child cannot start processes (`ENOTSUP`, above).
- `alarm()` is not implemented by the shim (independent of fork).
- `CLONE_CHILD_SETTID` stores the child thread's id, which is not its pid
  (a Windows thread id); `gettid()` in the child returns the same value.
- **Write-through files** (an unresolved observation, see Consequences).

## Ijiami (0086)

0086 chose `EAGAIN` because the shim's vfork-style child ran Ijiami's checks
in the app's own process: the child that sends itself SIGABRT, whose handler
SIGKILLs itself, killed the app. With a real child both checks run where
Linux runs them:

- the SIGABRT child: its handler's raw `kill(getpid(), SIGKILL)` ends the
  child only, and the parent's `wait` sees it killed by SIGKILL;
- the `sigaction` child: libc's `sigaction` and the raw `rt_sigaction` (now
  provided, 134) agree, and its raw `exit` (now 93) ends the child with its
  status.

The app's process is untouched. `arm64_fork_exec` runs both shapes, with the
same calls (the shim's `sigaction`, raw `kill`, raw `rt_sigaction`, raw
`exit`), and checks the statuses the parent sees. The ARM64 smoke test's
assertion that guest `fork`/`vfork` fail with `EAGAIN` is replaced by these
checks.

## Consequences

- PairIP's sequence (dumpable, `PR_SET_PTRACER_ANY`, raw `clone`, the
  child's `ATTACH` to its parent, `wait4(__WALL)`, `PEEKDATA`/`POKEDATA`,
  `DETACH`, `exit`) and Ijiami's checks run as on a device, in real child
  processes; `arm64_fork_exec` runs PairIP's sequence, a parent tracing its
  forked child, and the waits and heap of a parent's busy threads across the
  clone.
- Cost: none until a guest forks. A fork copies the process copy-on-write and
  holds the fork set briefly (a few milliseconds for gesher-exec: 120 forks
  with four busy threads take 3 to 8 s); making every handle inheritable for
  the clone and back is proportional to the process's handle count.
- `O_SYNC`/`O_DSYNC` and write-through files are not changed by a fork.
  During this work, before a restart of the test machine, a write-through file
  object that clones inherited wedged both processes reproducibly (a
  standalone `RtlCloneUserProcess` loop, 200 clones that each write such a
  file once, hung within 30-75 clones; files without write-through did not).
  After the restart the same probe ran 3 × 200 clones cleanly, as did 300
  forks writing PowerShell `Start-Process` redirection files (write-through)
  and the full test under that redirection; an independent probe did not
  reproduce it either. No cause was found, so no workaround is kept (one that
  took files out of write-through mode changed the file object other
  processes share and could not keep the `O_SYNC` contract in the child). If
  it recurs, the probe is the reproduction to start from.
- 0055's x86-64 `ENOSYS` and its note that fork is unsupported stay for
  x86-64 code; docs/exec.md says so.
