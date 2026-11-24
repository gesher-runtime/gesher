# ptrace between real processes (ADR 0093). tests/exec/ptrace_contract.c,
# built by exec.yml for x86_64 and arm64-v8a, runs in gesher-exec and spawns
# copies of itself: a child attaches to its parent (PR_SET_PTRACER_ANY), waits
# for the stop, PEEKs one of the parent's stack slots and POKEs it into
# another, then detaches; a parent attaches to and seizes its own children;
# the permission and error cases Linux with Yama ptrace_scope 1 gives; a
# tracee stopped while it takes record locks in a loop (Gesher's cross-process
# locks); code poked into a translated tracee; and PTRACE_O_EXITKILL.
foreach ($ptraceAbi in 'x86_64', 'arm64-v8a') {
  $ptraceProgram = NoDrive (Resolve-Path -LiteralPath "$Dir/ptrace-contract-$ptraceAbi" -ErrorAction Stop).Path
  CheckTimed "ptrace ($ptraceAbi): attach, wait4 stop, PEEK/POKE, detach between real processes; Yama permissions" "$Dir/gesher-exec.exe" @(
    '--elf', $ptraceProgram, 'ptrace-contract', $ptraceProgram) @(
    "ptrace contract ABI: $ptraceAbi",
    'PASS  ptrace: a child''s ATTACH without PR_SET_PTRACER is EPERM',
    'PASS  ptrace: a child not named by PR_SET_PTRACER is EPERM',
    'PASS  ptrace: a descendant of the process PR_SET_PTRACER named may attach (and detach)',
    'PASS  ptrace: ATTACH to a process that is not dumpable is EPERM',
    'PASS  ptrace: child: the attach stop is 0x137f (WIFSTOPPED, SIGSTOP)',
    'PASS  ptrace: child: PEEKDATA of an unmapped address is EIO',
    'PASS  ptrace: child: another process''s ATTACH to a traced process is EPERM',
    'PASS  ptrace: the child copied the parent''s slot through PEEKDATA/POKEDATA',
    'PASS  ptrace: a tracer child exits without detaching; the parent runs on',
    'PASS  ptrace: attach: wait4 reports the tracee killed by SIGKILL',
    'PASS  ptrace: seize: wait4 reports PTRACE_EVENT_STOP',
    'PASS  ptrace: child: the raw wait4(parent, __WALL) system call reports the tracee',
    'PASS  ptrace: child: the raw ptrace system call instruction (PEEKDATA) reads the slot',
    'PASS  ptrace: uid: a process of another uid cannot attach (EPERM), though PR_SET_PTRACER_ANY allows any',
    'PASS  ptrace: uid: ATTACH to its own child of another uid is EPERM',
    'PASS  ptrace: poke: a POKE into read-only code succeeds (FOLL_FORCE)',
    'PASS  ptrace: pipe: a tracee blocked reading a pipe stops; its writer is not held up',
    'PASS  ptrace: pipe: the reader gets the byte written while it was stopped',
    'PASS  ptrace: spawn: SEIZE right after posix_spawn succeeds (10 of 10)',
    'PASS  ptrace: shared pipe: the tracer''s poll sees the byte (the stopped reader did not take it)',
    'PASS  ptrace: shared pipe: the tracer reads its byte back',
    'PASS  ptrace: shared write: a stopped writer makes no progress beyond what the pipe holds',
    'PASS  ptrace: shared write: the tracer''s write() on the shared description completes at once',
    'PASS  ptrace: shared write: every byte arrives once, the tracer''s 16 between whole pieces',
    'PASS  ptrace: stream attach: every byte written arrives once, in order',
    'PASS  ptrace: stream seize: every byte written arrives once, in order',
    'PASS  ptrace: stream attach: every byte fed is read once, in order',
    'PASS  ptrace: stream seize: no EINTR or error from a stop',
    'PASS  ptrace: shared pipe: the resumed reader''s restarted read takes it',
    'PASS  ptrace: poke: the tracee ran the patched code and read the patched constant',
    'PASS  ptrace: exitkill: PTRACE_O_EXITKILL kills the tracee when the tracer exits',
    'PASS  ptrace: locks: the tracer''s own record lock never waits for the stopped tracee',
    'PASS  ptrace: locks: an unrelated process''s record lock never waits for the stopped tracee',
    'ptrace contract: 0 failed')
}
