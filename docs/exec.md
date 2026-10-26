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
- 466f6b1: merged origin/phase4/integration (arm64; exec_spawn keeps both the
  EM_AARCH64 acceptance and the ET_EXEC refusal). **ci 40260836601: fully green**
  — build, package, e2e (`0 unexpected failures`, Puzzles' generator through
  gesher-exec), test on windows-2022 and windows-2025 (440 PASS / 0 FAIL each),
  including
  `PASS  exec2: native processes from native code (fork+exec, system, popen, posix_spawn, signals, waitpid)`,
  `PASS  exec2: execv() outside a fork replaces the image (exit 17)`,
  `PASS  exec2: /system/bin/ls lists a directory`,
  `PASS  exec2: /system/bin/id prints the uid`,
  `PASS  exec2: /system/bin/getprop returns a default for an unset property`.
  Roadmap Phase 6 box ticked for what CI checked; the rest stays in an open box.
- Next (not done): a fuller sh; CI checks for destroy/destroyForcibly, logcat,
  ps, cat; ELF TLS in the executable.
- Not done: ELF TLS in the executable (ADR 0055 explains why); `pm`/`am`
  decline; `setprop` does not persist; a real fork that does work before exec.

_exec3 track (branch `wip/exec3`, ADR 0060 reserved). Opus 5.5 successor; the
first agent ran on an older model and was replaced per the all-Opus rule._

- Current continuation (2026-10-26): the shell executor, builtin dispatch and
  RunShell entry are now linked into gesher-exec, replacing the minimal shell.
  Subshells/pipeline stages/command substitutions use child processes and
  serialized state on inherited fd 63 (not a potentially oversized Windows
  command line); stdin stays available to commands. Redirections, incremental
  input, functions/local scopes, control flow, positional fields, parameter
  operators, getopts, traps and process status handling are implemented.
  Small local compilation/linking of Gesher's own files passes with
  `-Wall -Wextra -Werror`; 47 exact-output/status cases pass, including a
  pipeline larger than a pipe buffer and 40 KiB of serialized state. The new
  Exec3Test also passes locally through a small launcher driver and JDK 21's
  ProcessBuilder, including read from the still-open stdin. The local driver
  has no Gesher signal mailbox, so it does not verify signal delivery or ART.
  CI checks are in 45-exec3 and exec.yml's existing Java-source glob builds
  Exec3Test. No AOSP build was run locally. No exec3 work is CI-verified yet.
- Running: ci 40311734733 at 8c90ac2, including integration 91c195d/native2.
  Check both shell suites through the real launcher/ART, all existing track
  checks, package and e2e. The implementation below still has open scope.
- Follow-up prepared while that run is in progress: ExecTest now expects a
  working pipe instead of the removed shell's unsupported-operation error.
  Exec3ProcessTest uses the existing NDK signal-loop child to assert that
  Java destroy runs its SIGTERM handler (43), destroyForcibly returns 137,
  ps includes its real pid, and the kill tool reaches its handler. Binary cat
  and elapsed sleep checks are included. Java --release 11 compilation and
  PowerShell parsing pass locally; these new checks are not CI-verified yet.
- A shared-property implementation now routes libbase/Java, ELF bionic and
  getprop/setprop through one Win32 mapped area per data directory and device
  boot, or an inherited session in single-process mode. It includes build.prop
  initialization, write-once ro.*, persisted persist.*, enumeration and shared
  property serials. Own-file C++ builds pass with -Werror; a native-only local
  driver passes cross-process, boot-isolation and 32-writer checks. The shell's
  47 local regression cases still pass. Exec3PropertiesTest covers the real
  Java/tool/NDK paths for CI; no property roadmap box is checked. The backend
  has no Android SELinux property permissions or change-wait notifications.
- Merged integration 4f99afd (verified surfaces) in cfba36a before the next
  combined run. First CI 40311734733 has build/package/e2e green (e2e reports
  `0 unexpected failures`, including Puzzles' generator exiting 0); both
  Windows test jobs are still running. Review their logs before dispatching
  the follow-up containing the pipe expectation, signal/tool checks and shared
  property implementation. No artifact was downloaded for this review.
- First Windows-2022 job 121551537293 completed with three failing Check
  groups: ExecTest's stale pipe/diagnostic expectations (also reused for
  arm64) and the shell Python harness omitting --sh, which sent its scripts
  to launcher usage rather than the shell. The pipe assertion is updated;
  the harness now selects --sh explicitly and missing commands print the
  expected "not found" with status 127. The actual ART incremental-stdin
  Exec3Test and all exec2 checks passed on both runners. Windows-2025 has the
  same three failures; all remaining required jobs succeeded (probe jobs are
  skipped by design). None of the 47 Python cases is claimed CI-verified from
  this run. The corrected harness passes all 47 cases in the small local
  launcher build before the combined follow-up dispatch.
- Combined CI 40319876518 at ad6f7bb failed its exec artifact build early:
  __system_property_serial is a platform libc export absent from the public
  NDK header/stub. The test now resolves that export with dlsym (and links
  libdl) instead of declaring an unavailable NDK API. Supersede that run with
  the correction; the shared-property implementation remains unverified in CI.
- Running: corrected combined CI 40320290754 at 1189dc4. Watch log is
  C:\work\gesher-ci-review\exec3-watch-40320290754.log (120-second interval).
  Verify the NDK artifact first, then the real shell/property/signal checks on
  both Windows runners, package and e2e before recording any new completion.
- The corrected NDK exec artifact now builds in 40320290754. The follow-up
  property ABI work remains explicit: shared counters currently change across
  processes, but bionic's length-byte/clean dirty-bit encoding and wait/wait_any
  semantics are still open. ADR 0060 now records the fork-before-exec and
  ET_EXEC feasibility review and the exec builtin's changed-pid limitation.
- Prepared for the next independent CI step: exec3.apk packages the NDK child
  and a JNI wrapper around the existing nine-check exec2 driver. Its Activity
  calls that driver inside the real ART app process (not a separate test
  executable), then runs the Java shell/signal/tool/property suites. 45-exec3
  starts its own binderd/servicemanager/system server and verifies the APK's
  output. The APK's Java sources, PowerShell and packaging script pass local
  compilation/parsing only; neither its native JNI build nor on-device run is
  verified. The current CI 40320290754 predates this prepared test step.
- Four more local shell cases now cover special-builtin prefix assignments,
  ordinary temporary assignments, repeated prefix-name restoration and prefix
  assignment with readonly. Special builtins retain their assignments and
  other commands unwind repeated names in reverse order. All 51 exact local
  cases pass with an own-file -Werror build. This follow-up also awaits the
  next CI; the active run contains the earlier 47-case suite.
- The nested persistent-descriptor issue is also fixed for the next run:
  command I/O tracks the persistent generation it inherited, so temporary
  group/function redirects take precedence; leaving a redirected compound
  command restores the descriptors it scoped, including nested exec changes.
  Six added exact cases cover groups, functions, pipelines, extra descriptors
  and close/restore. All 57 local cases pass, and those six outputs/statuses
  also match Bash in POSIX mode. Real-launcher CI for these fixes is pending.
- Pipe input now checks queued mailbox traps while waiting for bytes, both at
  the shell command loop and inside read. An interrupted read returns
  128+signal; command-boundary traps use restored command I/O. A new ART test
  sends SIGUSR1 from the real kill tool with stdin left open, then during a
  blocked read, and expects immediate trap output/read status 138. Own-file
  -Werror build, 57 local shell cases, Java compilation and the local JDK
  incremental-stdin test pass. Real mailbox delivery and this test's on-device
  variant remain pending CI. Native console input is not covered by the pipe
  polling path. Active CI 40320290754 predates this change.
- Still open: umask (reports unsupported rather than claiming permissions),
  broader shell conformance (including special-builtin assignment scopes and
  persistent-descriptor nesting), caught signals during a blocked input read,
  real mailbox/Process.destroy tests, shared properties, logcat, pm/am,
  executable local-exec TLS, the on-device native-process checks and the
  feasibility limits in the original brief. Complete these after reading the
  first CI result; do not mark the whole track or shell complete.
- Previous WIP (superseded by the current implementation above): fixes nested
  substitution/quote scanning, incomplete-input reporting, quoted assignment
  parsing and empty-field expansion. `shell_execute.inc` now contains draft
  AST execution, process-isolated subshells/pipeline stages, redirections and
  expression/printf helpers. It is not included in the build yet: simple
  commands, builtin dispatch and `RunShell` are still missing. None of these
  changes has been compiled or CI-verified. The earlier syntax-only result
  below applies only to the original front end; it did not establish shell
  correctness. Process isolation replaces the earlier proposed stage threads
  so subprocess environment/current-directory changes cannot race.

- **STOPPED by the coordinator mid-task (2026-10-26)**, project handed to a new
  team. No exec3 CI run was ever started; nothing exec3 is verified yet. Last
  fully-green exec/exec2 CI was `40260836601` on the merge base; exec3's own work
  is all uncommitted-then-WIP-committed, unbuilt and unverified.
- **State of the work (all WIP, not built in the real toolchain):**
  - A new real `/system/bin/sh` is being written from scratch in
    `compat/elf/exec/gesher_shell.{cc,h}` (ADR 0001: original code, not copied
    from mksh/toybox — both are MIT/BSD-permissive and could be vendored instead,
    but a self-contained shell avoids an AOSP fetch and the `fork` a real shell
    expects). Design: lexer keeps words raw; recursive-descent parser builds an
    AST (simple/pipeline/and-or/list/if/for/while/until/case/group/subshell/
    funcdef); expansion does tilde, `${...}` (`:-` `:=` `:+` `:?` `#` `##` `%`
    `%%` `/` `//` `:off:len` `${#x}`), `$(...)`/`` `...` ``, `$((...))`
    arithmetic, IFS field splitting, pathname globbing, quote removal; pipelines
    are meant to run as concurrent threads joined by real Win32 pipes (no fork:
    subshells/stages/command-subs are shell-state copies on threads); signals via
    gesher-base's mailbox with a `ShellSignalHook` (traps, foreground-pass).
    **DONE and clean under `g++ -std=c++20 -fsyntax-only`:** the whole front end
    — lexer, parser, variables/specials, arithmetic, glob matching, and the full
    word-expansion engine. **NOT yet written:** redirection application
    (files/here-docs/fd-dups/pipes), the executor (`ExecNode` and the per-kind
    exec functions — currently only forward-declared), all builtins
    (`cd pwd echo printf export unset set shift read test [ : true false exit
    return break continue . source eval local trap command type getopts let
    umask wait jobs kill exec`), and the real `RunShell` entry (arg parsing,
    incremental stdin reading, EXIT trap). The file ends at the
    `// Defined after the parser and executor` placeholder; `ExecNode` /
    `RunProgramString` are declared but undefined, so it compiles to a syntax-OK
    unit that does NOT link yet. `exec.cmake` still builds `gesher-exec` from
    `gesher_exec.cc` (the old minimal `--sh`); it is NOT yet switched to
    `gesher_shell.cc`, and `RunShell` is not yet wired into `main()`.
  - `exec_spawn.{h,cc}` already extended (committed WIP): `PrepareLaunchInPath`
    (honour the shell's own `$PATH`), `FindInPath`, and `SpawnWithFds`'
    `extra_flags`. Tools/property-store/logcat/pm-am/ELF-TLS: NOT started.
- **Exact next steps for a successor:**
  1. Finish `gesher_shell.cc`: redirections (incl. here-docs via temp file and
     `N>&M`), `ExecNode` dispatch, pipeline with real pipes + stage threads
     (each stage a Shell copy; last stage's status, or pipefail), simple-command
     exec (assignments-only sets vars; builtin vs function vs external via
     `PrepareLaunchInPath` + `SpawnWithFds` with `ChildEnv`), the builtins list
     above, and `RunShell` (parse `-c`/`-s`/script/stdin; read stdin one complete
     command at a time so a Java process piping commands sees each result; run
     the EXIT trap; install `gesher_signal_hook = ShellSignalHook`). Compile it
     locally with `g++` (free, fast) and a tiny driver `int main(){return
     gesher::exec::RunShell(args);}` before any CI.
  2. Point `exec.cmake` at `gesher_shell.cc` and call `RunShell` from
     `gesher_exec.cc`'s `--sh` branch (delete the old in-file minimal shell).
  3. Shared property store + persistent `setprop`/`getprop`, device log ring for
     `logcat -d/-s/-v/-c` (the system server sets `GESHER_LOG_FILE`; see
     `gesher_tools.cc` and `app/java/.../SystemServer.java`), `pm`/`am` through
     the system server, `ps`/`cat`/`kill`/`destroy(Forcibly)` CI checks.
  4. ELF TLS local-exec in `compat/elf/fs_rewrite.cc` (the executable's own
     `R_X86_64_TPOFF` sites): the `%fs` rewrite runs before relocations, so
     recognise the executable's PT_TLS and rewrite local-exec `%fs:-N` forms, or
     record precisely why not (ADR 0055 "Not done" explains the current trap).
  5. Tests `tests/windows/tracks/45-exec3.ps1` + NDK programs + `.github/
     workflows/exec.yml` additions; on-device variant of 43-exec2; keep 42/43/47
     and e2e green; before final CI merge `origin/phase4/integration`.
  6. Write ADR `docs/decisions/0060-exec3-shell-and-tools.md`; tick roadmap only
     for what CI verifies.
- **Pitfalls:** real llvm-mingw build is `-Werror` and MS x64 ABI (local `g++`
  is only a syntax/logic smoke test; it will miss `sysv_abi`/JNI issues, but the
  shell links no JNI so it is a fair check). NetFree may block some text. Never
  build AOSP locally; CI only. `WindowsPath()` maps Linux paths; the shell keeps
  its own cwd via `SetCurrentDirectoryW`/`GetCurrentDirectoryW`.
