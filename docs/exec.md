# ELF executables as processes

Some Android apps run native programs as separate processes: an executable
shipped in the APK's `lib/x86_64` (named `lib*.so` so the package manager
extracts it into `nativeLibraryDir`), started with `ProcessBuilder` or
`Runtime.exec`. Simon Tatham's Puzzles generates every new game that way
(`libpuzzlesgen.so`). On Android, libcore's `UNIXProcess` forks and execs the
file and the kernel hands it to bionic's linker. Gesher has neither, so this
track provides both halves on Windows ([0035](decisions/0035-exec-elf-processes.md)).

## Pieces

- **UNIXProcess natives** (`compat/libcore/process.cc`, in dalvikvm in place
  of ojluni's `UNIXProcess_md.c`): `forkAndExec` finds the program as `execvp`
  would and starts it with `CreateProcessW`; stdin/stdout/stderr are pipes
  whose parent ends are gesher-base fds (CRT fds over the pipe handles), so
  `FileInputStream`/`FileOutputStream` and the process reaper work unchanged;
  `Redirect.INHERIT` and file redirects pass the fd's handle; the environment
  block and working directory are the child's. The pid is the Windows process
  id. `waitForProcessExit` returns the status as Linux's `waitpid` gives it to
  Java (the exit code; 0x80 + signal for a crash), `destroyProcess` terminates
  with 0x80 + SIGTERM (143). Errors are `IOException`s with Linux's
  `error=<errno>, <text>`.
- **Finding and starting programs** (`compat/elf/exec/exec_spawn.{h,cc}`,
  library `gesher_exec_spawn`): a name with a `/` is a path (relative to the
  new working directory, as after the child's `chdir`); a bare name is looked
  up in `PATH` (Windows' `;` list, or a Linux `:` list), with `.exe`, `.com`,
  `.cmd`, `.bat` tried for names without an extension. By the file's contents:
  - an **ELF x86-64** file runs in `gesher-exec.exe --elf` (below); other ELF
    machines (arm64) fail with `ENOEXEC`;
  - a **PE** image (`MZ`) runs as itself; a **`.cmd`/`.bat`** through
    `cmd.exe /d /s /c`;
  - **`sh`, `/system/bin/sh`, `/bin/sh`** (also `/system/xbin`, `/vendor/bin`)
    and **`#!...sh` scripts** run in gesher-exec's minimal shell; other `#!`
    interpreters are looked up like programs; a text file without `#!` runs
    with the shell, as the JDK's `execvp` fallback does;
  - anything else is `ENOEXEC`; a directory is `EACCES`; nothing found is
    `ENOENT`.
- **The launcher** (`compat/elf/exec/gesher_exec.cc`, `gesher-exec.exe` next
  to dalvikvm.exe, shipped by the installer):
  - `--elf <file> <argv0> [args]` loads the executable with the ELF loader
    (`compat/elf`) and the bionic shim as the main program, as bionic's linker
    would: `DT_NEEDED` libraries from `$LD_LIBRARY_PATH`, then the program's
    own directory, then the shim; `DT_PREINIT_ARRAY`/`DT_INIT`/
    `DT_INIT_ARRAY` called with `(argc, argv, envp)`; then the entry point
    (the NDK's `_start`) on a stack laid out as Linux's (argc, argv, envp,
    auxv with `AT_PHDR`, `AT_PHNUM`, `AT_ENTRY`, `AT_PAGESZ`, `AT_RANDOM`,
    `AT_EXECFN`...), on a thread with an 8 MiB committed stack (NDK code does
    not probe the stack as Windows code does). The launcher provides bionic's
    `__libc_init` (the executable's `.fini_array` registered, `main` called,
    `exit` with its result), `exit` (atexit/`__cxa_atexit` handlers, stdio
    flushed, the low 8 bits as the exit code), `_exit`, `getauxval` (from the
    auxv), `environ` and `__progname`. stdin/stdout/stderr are the inherited
    handles in binary mode. `abort()` ends with 134, a fault with 128 + the
    signal (139 for an access violation) after gesher-base's crash report; a
    program that cannot be linked prints `CANNOT LINK EXECUTABLE` and exits 1.
  - `--sh [-c <cmd> | <script> [args]]` is a minimal `/system/bin/sh`: words
    with `'`/`"`/`\` quoting, `$NAME`/`${NAME}`/`$?`/`$1` expansion, `;`,
    `&&`, `||`, the builtins `echo`, `exit`, `true`, `false`, `:`, `cd`,
    `pwd`, `export`, `exec` and `NAME=value` prefixes; other commands run as
    programs (above) with the shell's stdio, `not found` is 127. Commands come
    from `-c`, a script file or stdin. Pipes, redirection, subshells,
    command substitution and control structures are refused with status 2.
- **Loader additions** (`gesher/elf_loader.h`, additive): `GetExecutableInfo`
  (entry point, program headers, `PT_INTERP`), `SetInitArguments` (what init
  functions are called with; dalvikvm never sets it, so `.so` files there get
  argc 0 and null argv/envp, where they used to get whatever was in the
  registers), and `DT_PREINIT_ARRAY`.

## Native processes from native code (exec2 track, ADR 0055)

An app's `.so` starting other programs is supported too, over `exec_spawn` and
gesher-base's signal mailbox (`compat/bionic/process_bionic.cc`):

- **`system()`, `popen()`/`pclose()`, `posix_spawn()`/`posix_spawnp()`** (with
  its file actions: open, close, dup2, chdir) run programs directly, no fork
  needed; `system`/`popen` through the launcher's `sh -c`.
- **`fork()`/`vfork()`** are a **vfork-style** child on the calling thread:
  `fork()` returns 0, the child's fd (`dup2`, `close`, `open`), environment and
  `chdir` calls are recorded, and its `exec*()` spawns the program with them
  while `fork()` returns the child's pid to the parent; its `exit`/`_exit` ends
  the child. The child must only prepare and `exec`/`_exit`, as `vfork` requires.
- **`exec*()` outside a fork** spawns the program with this process's fds,
  environment and working directory, waits and exits with its status (the pid
  changes).
- **`waitpid`/`wait`/`wait4`** return Linux's status word
  (`WIFEXITED`/`WIFSIGNALED`); a child's exit raises `SIGCHLD`.
- **`kill(pid, sig)` to another process** (any Gesher process, children
  included) goes through the signal mailbox and is delivered to the target's
  `sigaction` handlers (SIGTERM, SIGKILL, SIGINT, SIGUSR1/2, SIGHUP; signal 0
  for existence), with the sleeps interruptible (`EINTR`).
- **`Process.destroy()`** now sends SIGTERM (the program's handler runs; a
  program with none is terminated with 143); **`destroyForcibly()`** kills
  (137), through a new `UNIXProcess` native.
- **`/system/bin` programs** (`gesher-exec --tool`): `ls`, `cat`, `id`, `ps`,
  `getprop`, `setprop`, `logcat`, `kill`, `sleep`, `toybox`, as a bare name or a
  `/system/bin` path; `sh` is the launcher's shell. `su` stays absent (no root),
  `pm`/`am` decline (no binder from a bare process), `setprop` cannot persist
  (no shared property store).

## Not done / limits

- **ELF TLS in the executable itself** (`__thread` at API 29+): still
  unsupported. The linker resolves an executable's own thread-locals as
  local-exec offsets from the thread pointer (`%fs:-N`), filled by
  `R_X86_64_TPOFF` relocations; the loader's `%fs` rewrite runs before
  relocations, so it cannot tell a local-exec site from a thread-pointer access,
  and such forms trap. Emulated TLS (API < 29), and ELF TLS in the libraries the
  executable loads, work.
- A real `fork()` that does work other than `exec`/`_exit` in the child is not
  supported (vfork-style only, ADR 0055).
- Exit-status convention: a child that dies of a signal carries a Windows
  exit code of 128 + signal (the posix layer's `kill` and the launcher's
  fault mapping both use it), and `waitForProcessExit` passes such codes
  through unchanged, so Java's `waitFor()` sees the same 128 + N that
  Linux's `Process` reports.
- Static executables (`ET_EXEC`, no `PT_INTERP`) and non-NDK runtimes (Go)
  are not supported (`ENOEXEC`).
- Windows programs get the command line quoted for the CRT's rules; a
  Windows program's exit code is passed through unchanged (it may exceed 255).

## Status and next steps

_Updated by the exec track (branch `wip/exec`)._

- ci `40114487210` (39227be): everything built; on both runners
  `PASS  exec: gesher-exec runs an NDK-built ELF executable as its main program`,
  `PASS  exec: gesher-exec exit statuses: exit(3) is 3, abort() is 134, a fault is 139`,
  `PASS  exec: gesher-exec --sh -c runs simple commands with &&, || and exit`;
  ExecTest failed only where the test program printed its destructor's line in
  every mode (fixed in the test). The other five e2e apps still pass.
- ci `40128223507` (305965c): **test jobs green on windows-2022 and
  windows-2025**, including
  `PASS  exec: ProcessBuilder runs an ELF executable (stdin/stdout/stderr pipes, environment, working dir, exit status, destroy), sh and Windows programs`.
  e2e: Puzzles never ran its generator: on a first launch its GamePlay goes up
  to its chooser with `NavUtils.navigateUpFromSameTask`, and the system
  server's `navigateUpTo` stand-in answered false, so the app sat on an empty
  GamePlay (no exec at all in its log).
- 588c83d: `navigateUpTo` in the system server (finishes the activities above
  the destination; starts the destination in the task when `startActivities`
  left it out); the e2e runner's `waitLog` (apps.json) is checked after the
  actions. ci `40133940460` failed only in e2e: the runner kept watching the
  chooser window after GamePlay replaced it (fixed in cd27227, which follows
  the app's newest full-size window).
- ci `40135101983` (cd27227): **fully green** — build, package, test on
  windows-2022 and windows-2025, and e2e with
  `PASS  e2e: name.boyle.chris.sgtpuzzles 2025-09-12-1919-23762278-fdroid (Simon Tatham's Puzzles) installs, draws its first frame (not blank), reacts to input, closes and its process ends`
  (`waitLog` matched: `gesher: exec pid 8172 exited with status 0` — the
  generator ran through gesher-exec), `0 unexpected failures`. Roadmap box
  ticked.
- Next: final CI on the branch merged with origin/phase4/integration.

_exec2 track (branch `wip/exec2`, ADR 0055):_

- d63005d: process_bionic.cc (fork/vfork, exec*, posix_spawn, system, popen,
  wait*), signals between processes through the mailbox hook,
  destroy/destroyForcibly, `gesher-exec --tool`, tests (43-exec2.ps1,
  exec2_test.c/exec2_child.c). ci 40238884918: the libcore patch was malformed
  and the test had a `-Wnonnull`; ci 40239339448: process_bionic.cc lacked
  `<fcntl.h>` (fixed in fa01b01).
- 784c47b: merged origin/phase4/integration (hwsvc2). ci 40240910969: system,
  popen, posix_spawn, exec-replace and the tools passed; the driver died at the
  fork test (exit 127): `__builtin_longjmp` re-entered fork's own frame, which
  the child's calls had overwritten.
- 995eef1: fork is now an asm trampoline (`gesher_fork`) that saves the
  caller's context and returns to the caller a second time
  (`gesher_fork_return`). ci 40244652480 (after a transient curl failure in the
  build job, rerun): every exec2 check passes on windows-2022 and windows-2025;
  the test's expected SUMMARY count was off by one (9 checks, fixed). package
  and e2e green.
- Not done: ELF TLS in the executable (ADR 0055 explains why); `pm`/`am`
  decline; `setprop` does not persist; a real fork that does work before exec.
