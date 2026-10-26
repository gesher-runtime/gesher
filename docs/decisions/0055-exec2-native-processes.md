# 0055 — Native processes from native code

**Status:** accepted, 2026-10-26 (exec2 track; see docs/exec.md for the CI runs
that verify it). The coordinator may renumber.

## Context

[ADR 0035](0035-exec-elf-processes.md) ran Android ELF executables as Windows
processes for Java's `ProcessBuilder`/`Runtime.exec` (libcore's `UNIXProcess`
natives over `CreateProcessW`, and `gesher-exec.exe` as the dynamic linker's
stand-in). It left the native-code side open: an app's `.so` that calls
`fork()`, `exec*()`, `posix_spawn()`, `system()` or `popen()` hit the bionic
shim's gaps (docs/native-libs.md), signals to other processes were `ESRCH`
(docs/exec.md), `Process.destroy()` terminated without running the program's
SIGTERM handler, and Android's `/system/bin` programs did not exist.

Windows has no `fork()`, and ART's heap and threads rule out copying the address
space (0035, option 2). What apps' native code actually does with these calls is
start another program, so the work is to map each call onto `exec_spawn` (0035's
program finder) and `CreateProcessW`, and to route signals through gesher-base's
signal mailbox (ADR 0045), which already delivers `kill()` between Gesher
processes.

## Decision

A new `compat/bionic/process_bionic.cc`, registered last in the shim so it wraps
the fd/env/dir entries the other files already provide.

- **`system()`, `popen()`/`pclose()`, `posix_spawn()`/`posix_spawnp()`** are
  implemented directly, without fork: each builds the child's fd set (and, for
  `posix_spawn`, applies its file actions — a linked list the shim's own
  `posix_spawn_file_actions_*` build, since the type is an opaque pointer in
  bionic), environment and working directory, and spawns through `exec_spawn`.
  `system`/`popen` run the launcher's shell (`sh -c`).
- **`fork()`/`vfork()`** are a **vfork-style child** on the calling thread:
  `fork()` records the process state and returns 0; the fd, environment and
  working-directory calls the child then makes are buffered, not applied to this
  process; its `exec*()` materialises them and spawns the program, and `fork()`
  returns the child's pid to the "parent"; its `exit()`/`_exit()` ends the child
  without spawning. `__builtin_setjmp`/`__builtin_longjmp` (a register
  save/restore with no SEH unwinding, unlike the UCRT's `longjmp`) carry control
  back to `fork()` across the abandoned child frames, as `vfork` abandons the
  child's stack. A real `fork()` that then does work other than `exec`/`_exit`
  is **not supported**, as `vfork` never allowed it.
- **`execve`/`execv`/`execvp`/`execvpe`** outside a fork, where Linux replaces
  the image: the shim spawns the program inheriting this process's fds,
  environment and working directory, waits, and exits with its status (the pid
  changes — documented).
- **`waitpid`/`wait`/`wait4`** over the shim's children, returning Linux's status
  word (`WIFEXITED`/`WEXITSTATUS`, `WIFSIGNALED`/`WTERMSIG`); a child's exit
  raises `SIGCHLD`.
- **Signals between processes.** `kill(pid, sig)` to another process (any Gesher
  process, a child included) goes through gesher-base's signal mailbox
  (`gesher_kill_other`, compat/posix/src/process.cc). The bionic shim installs a
  hook there (`gesher_signal_hook`): a signal another process sends is delivered
  as Linux delivers a process-directed signal — to a thread that does not block
  it (pending if all do, to one waiting in `sigsuspend`/`pause`/a sleep which
  then returns `EINTR`, else on the shim's own signal thread at once). The hook
  is **authoritative** in `gesher-exec` (the program is the whole process, so
  `SIG_DFL` acts as Linux's default) and non-authoritative in dalvikvm (only
  signals a `.so` set an action for; the rest stay ART's). Sleeps
  (`nanosleep`/`usleep`/`sleep`) end early with `EINTR` when a handler runs.
- **`Process.destroy()`/`destroyForcibly()`.** `destroy()` sends SIGTERM through
  the mailbox, so a Gesher child runs the launched program's own SIGTERM handler
  (and a child with none is terminated with 143 by the default action);
  `destroyForcibly()` is a new `UNIXProcess` native (patch
  `patches/libcore/0003-exec2-unixprocess-destroy-forcibly.patch`) that
  `TerminateProcess`es (137).
- **`/system/bin` programs** (`gesher-exec --tool`, compat/elf/exec/gesher_tools.cc):
  `ls`, `cat`, `id`, `ps`, `getprop`, `setprop`, `logcat`, `kill`, `sleep` and a
  `toybox` multiplexer, as a bare name or a `/system/bin` path. `getprop` reads
  `$ANDROID_ROOT/build.prop`; there is no shared property store, so `setprop`
  cannot persist; `pm`/`am` decline (no binder from a bare process); `su` stays
  absent (no root). `sh` is the launcher's shell, extended where apps need it.
- **Static executables** (`ET_EXEC`, no `PT_INTERP`): `ENOEXEC`. They make Linux
  system calls directly, which the loader and shim do not provide.

## Consequences

- `system`/`popen`/`posix_spawn` are faithful; raw `fork()` is only the
  vfork-style subset (fork immediately followed by `exec`/`_exit`), which is what
  NDK code does in practice. Its child runs on the parent's thread until exec, so
  it must not touch the parent's memory, as `vfork` requires.
- An `exec*()` that replaces the image keeps the program's behaviour but changes
  the Windows pid; nothing in Gesher observes a native process's own pid across
  an exec.
- Signals from another process reach a `.so`'s handlers only through `sigaction`
  (the shim's model, 0045); `signal()` dispositions and synchronous fault signals
  are unchanged from docs/native-libs.md.

## Not done

- **ELF TLS in the executable itself** (local-exec `%fs:-N`, API 29+): still
  unsupported. The `%fs` rewrite runs before relocations, so a local-exec site
  (its displacement filled by an `R_X86_64_TPOFF` relocation) cannot be told
  apart from a thread-pointer access at rewrite time; it still traps. Emulated
  TLS (API < 29) and ELF TLS in the libraries an executable loads work (0035,
  docs/native-libs.md).
- Static executables and non-NDK runtimes (Go), as above.
