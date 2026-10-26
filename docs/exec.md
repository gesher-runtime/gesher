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
  Java (the exit code; 0x80 + signal for a crash), `destroyProcess` sends
  SIGTERM through the signal mailbox so a native handler runs; its default
  action exits 143. Errors are `IOException`s with Linux's
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
    and **`#!...sh` scripts** run in gesher-exec's shell; other `#!`
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
  - `--sh [-c <cmd> | <script> [args]]` uses the shell in ADR 0060: quoting,
    variables and positional/parameter/arithmetic expansion, pipes, ordered
    redirections/here-documents, command substitution, subshells, functions
    and control flow, with common builtins and external programs (above).
    A missing command is 127. Commands come from -c, a script or incremental
    stdin. The status section distinguishes CI-verified cases from pending
    conformance fixes; this is not a claim of complete mksh compatibility.
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
  `getprop`, `setprop`, `logcat`, `kill`, `sleep`, `toybox`, `pm`, `am`, as a bare name or a
  `/system/bin` path; `sh` is the launcher's shell. `su` stays absent (no root),
  getprop/setprop use the shared backend in ADR 0060. Structured logcat and
  the pm/am Binder client are prepared; the new property ABI, log capture and
  command/device assertions still need CI proof.

## Not done / limits

- **ELF TLS in the executable itself** (`__thread` at API 29+): a new
  implementation recognizes linker-resolved negative FS operands using the
  explicitly marked executable's compatible first static PT_TLS block. The
  own-file instruction probe and real NDK/ProcessBuilder checks pass on both
  runners in CI 40338806524; the app variant remains pending. Incompatible layouts, text relocations and unsupported
  instruction forms fail or trap explicitly (ADR 0060). Emulated TLS (API
  <29) and library ELF TLS retain their existing paths.
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

**Current review:** CI 40405306826 is fully green at
87d5adadea4008a9c4340b9ee58fbabf9aedb00f: all 28 jobs completed (22 success,
six intentional skips). Both actual Windows logs end in `0 test(s) failed`.
Every exec3 Check passes, including shell57, signals/processes/tools, the full
shared-property/log suites in three modes, executable TLS, broker readiness and
the installed-device pm/am target/receiver/force-stop checks. The new actual ART
camera request-selection regression and all original track58 single/device
camera, sensor, permission and capture-intent assertions pass. No signal records
were emitted. This is successful combined runtime evidence after the independent
camera ownership corrections; it does not prove the prior ntdll fault's cause.

Actual package logs pass installed oatdump/image/fonts, lifecycle/shortcuts,
UI/input and complete shutdown/uninstall cleanup. Both e2e logs pass all nine
apps including TIC-80 and Puzzles, four transport fixtures and zero unexpected
failures; native Puzzles generators 6156/9424 exit 0. The exec build directly
passes LauncherTailTest and LauncherCommandLineTest's 13 cases. The later
97e63e5 default-tag filter/setter regression and integration f520fd1's CI-only
rules are prepared for final combined CI; no new local compiler will run.
Final CI 40410489251 runs that candidate at
51bbfaccddf1fd7f466cab2a493874fbf0e1cb01. Watch
C:\work\gesher-ci-review\exec3-watch-40410489251.log at 120 seconds; retain
87d5ada and its completed logs as the separately verified combined foundation.

**Previous review:** CI 40393019261 completed at
6a3289d2162359aa4f59d22a39b52d724b61517b. Every exec3 Check passes on both
runners, including all exact installed-device pm/am expectations (actual list/
path, separate resumed target/receiver and force-stop). Windows-2022's entire
suite has zero failures. Windows-2025 has nine devices Checks affected by one
post-steps device-process crash; single-process devices also pass. All build,
media, package and both e2e jobs pass. Both e2e logs show zero unexpected, all
nine apps including TIC-80, four transport fixtures and Puzzles generators
3672/8816 exiting 0. Installed oatdump/image, UI/input and cleanup pass.

The preserved native record now identifies Windows-2025 thread6624, address
0xffffffffffffffff, RIP ntdll.dll+0x9a7e7 and RSP0x18e92ff070. RCX and RDI
contain UTF-16-like path fragments. Access direction and the calling product
code are not present in that record, so no cause is inferred. A prepared narrow
extension records the original access kind and at most eight candidate code
addresses from a safe 512-byte stack snapshot. These are labelled stack words,
not validated frames; at most eight records/96 published lines are allowed.
Own-file -Werror and enabled/disabled fatal-child/publication contracts pass,
with the same exit7. The crash remains open pending an actual runtime fix and
combined CI. Verified Native3 integration117 and media's independently proven
callback/handle/request ownership corrections are now combined. The source-only
camera transfer uses ordered asynchronous callbacks, capture-thread-owned native
handle release and atomic repeating-request selection. Its actual-class 1000
cancel/flush race regression passes locally on JDK21 and is invoked under ART
from track58 in the next run. This independently fixes demonstrated ownership
and request races; it does not establish the cause of the ntdll crash.

The combined tree includes verified integration117629e and a quote-aware Windows
argument scanner for Zygote.vmCommandLine. That scanner fixes Windows4's actual
startup failure when the Launcher main-class token is quoted; thirteen host
cases pass, with direct execution wired into the exec workflow. New ART camera
regression, quoted Launcher startup and caller-word records await combined CI.
CI40405306826 now runs that complete source at87d5ada. Its watch file is
C:\work\gesher-ci-review\exec3-watch-40405306826.log (120-second interval).
Latest integration7826cbe is already included; it adds documentation atop117.
A subsequent liblog correction restores pinned null/zero-length default-tag
filtering for host calls and ELF writes, and adds the ELF default-tag setter.
The NDK log-abi test now exercises those actual exports and hidden/visible
writes. This follow-up is outside404053 and awaits a later combined run. A local
own host-wrapper contract with mocked upstream tag access passes; it does not
compile AOSP or establish the full liblog ABI.

**Previous review:** CI 40385411337 completed at
80bf3947bee2dabf428b2c5c07b7c672dec0a947. All build/media/package and both
required e2e jobs pass. Both e2e logs have zero unexpected failures, all nine
apps including TIC-80, four transport fixtures and Puzzles generators 4076/7544
exiting 0. Actual package proof includes installed oatdump/boot image, device
apps and input, and full uninstall cleanup.

Both Windows suites pass shell57, signals/tools/TLS, binder readiness, the ten
native host assertions, full standalone Java properties/logcat and the complete
single-process APK. The installed-device APK also passes its native nine-process
suite, shell/signals/TLS, properties and logcat before pm starts. Java/ELF/tool
properties now prove scratch-root inheritance, readonly/long values, serial ABI,
foreach, waits/wait_any, explicit reportSyspropChanged and boot/persist behavior
in all three modes. The readonly fixture collision is corrected without any
write-policy relaxation. Native3 Process, libartbase233/libdexfile120, windows3
and AOT2/oatdump checks pass on both runners.

Remaining actual failures: Windows-2025 has ten Checks, Windows-2022 fourteen.
Each has one installed exec3 failure: pm exits126 before contacting Binder
because DeviceCommand checks the logical framework/boot.art as a physical file.
The real image is in framework/x86_64/boot.art; ART resolves that subdirectory
from the logical -Ximage option. The prepared resolver now accepts that layout
while passing the logical option, with five native launch-contract checks for
root/flat/fallback/missing images. Those checks use an own child executable,
not ART/Binder; actual pm/am success remains for the next combined CI.

The other nine/thirteen Checks follow actual devices process crashes after all
expected steps: device mode on both runners, plus single-process sensors/camera
on 2022, at fault address -1 and exit1. The native trace did trigger, but my test
helper used a tracks-relative ../../tools path and then filtered out its signal
context. The prepared correction uses ../../../tools and prints the bounded
context before the category/200-line filter. An own fatal-child probe verifies
publication beyond 300 preceding lines, symbolizer invocation and original
exit7 preservation. No new native diagnostic or runtime cause is claimed.
The camera callback work remains separately owned/coordinated by media3_next.
CI 40393019261 is now running at 6a3289d2162359aa4f59d22a39b52d724b61517b
(prepared code e064969 plus docs-only integration cd0b849). It tests the actual
pm/am correction and visibility of the already captured sentinel context,
without importing the separate camera callback patch or changing native signal
diagnostics. Watch C:\work\gesher-ci-review\exec3-watch-40393019261.log at
120-second intervals. No green final tree or shutdown root cause is claimed.

**Previous review:** CI 40372171824 completed at
7ae515fdd4a7dd84af4a65f15e19392d6517a437. All build/package and both e2e jobs
pass; both e2e logs have zero unexpected failures, nine apps including TIC-80,
four transport fixtures and Puzzles generator pids 2560/8992 exiting 0. Both
Windows logs pass the previous loggability canary, libartbase's 233 tests,
libdexfile's 120 tests, the host callback/root regression, standalone Java/NDK
logcat (nine formats, filters, clear and follow), the complete native3 Process
proc regression, 57 shell cases and unchanged windows3 UI assertions.

Windows-2025 has three failing Checks, all from a test-name collision: Java
first writes ro.exec3.<suffix>.long with 1000 bytes; the later NDK ABI fixture
tries to write 180 bytes to that same readonly name. The backend correctly
refuses. The NDK fixture now uses ro.exec3.<suffix>.abi.long; a native local
fixture proves both long values coexist and both still reject overwrites.
Explicit Java reports, later waits and pm/am remain blocked by that earlier
assertion and need the next combined run, including both complete APK modes.

Windows-2022 has those three failures and thirteen devices Checks affected by
two actual process crashes after all expected steps, at fault address -1.
Single-process and device sensor/camera output is complete, but the exit is 1;
the assertion is retained and the shutdown cause remains open. A coordinator-
authorized, env-gated pre-handler trace now captures that sentinel fault's
RIP/module/registers before ART can exit, with at most eight records per process.
Only devices children enable its narrow environment flag; existing signal
disposition is unchanged. Own-file syntax and a fatal-handler native contract
probe pass (same exit with diagnostics enabled/disabled). The devices helper
symbolizes captured addresses with the runner's existing map on failure.
Combined diagnostic/full CI 40382268730 was dispatched at
4568c71db6ee033bcd7e4c4014e9f36cae8e71b5, including integration 634a273,
the fixture namespace correction, inherited boot-root/ART regressions and the
narrow sentinel trace. Watch log:
C:\work\gesher-ci-review\exec3-watch-40382268730.log (120-second interval).
Its builds passed, but media generation did not finish before coordinator-
directed supersession: job 121784230431's Generate step began 19:02:46Z and was
still in_progress after 909 seconds at observation. It was canceled once before
any Windows job started; there is no discarded Windows/runtime failure evidence
and no inferred apt error or cause. Partial build proof and metadata are kept.
A final combined run must follow actual runtime fixes and latest integration.
The coordinator/media3 source-only workflow patch from 1361200 is now included:
media generation reuses an installed
ffmpeg, otherwise bounds apt retries/timeouts, prints the actual version and
has a ten-minute step limit. Fixtures and their assertions are unchanged. The
same source has actual media-job proof in media3 CI; this branch's YAML/Bash
parsing passes. One replacement diagnostic/full run, 40385411337, is now active
at 80bf3947bee2dabf428b2c5c07b7c672dec0a947. It keeps all assertions and the
sentinel diagnosis; watch C:\work\gesher-ci-review\exec3-watch-40385411337.log
at 120-second intervals. Neither runtime root cause nor a green final tree is
claimed from the canceled preparation run.

**Previous review:** completed CI 40352529279 at
dc19ee29190961bb6238081d1978d0882ab77f44 includes integration 46bc9e5.
All build jobs, package and both e2e jobs pass; both e2e summaries have zero
unexpected failures, required TIC-80 and Puzzles generator pids 5244/8604
exiting 0. Both Windows suites retain 57 shell cases with zero failures,
incremental ART stdin, real idle/read signal traps, native Java process tools
and API29 executable TLS passes. The ten-case broker readiness probe passes,
and JNI's nine native-process checks pass in both single-process and installed
device APKs. NDK serial/readonly-pointer/long-value/foreach/wait/wait_any
assertions pass in standalone ART and both APK modes.

Each Windows suite has seven failed Checks: the framework loggability canary,
libartbase/libdexfile scratch cleanup, explicit Java property callbacks,
Java log capture and the two complete APK suites blocked by the callback.
Their exact causes are fixed in the prepared tree: ANDROID_LOG_DEFAULT is 1,
not 0; the boot property mapping must survive scratch ANDROID_DATA changes;
Win32 libutils had discarded callbacks; and ART replaces liblog's logger
after constructor registration. The logger setter now keeps structured capture
around the selected destination. Nine native host assertions pass the data-root
and callback regression; an own mock upstream-call contract passes logger
replacement/capture forwarding. These local results do not replace the next
actual ART/Java/e2e run. Both completed Windows logs are retained under
C:\work\gesher-ci-review\exec3-40352529279-windows-*.log.

The prepared tree also merges verified native3 integration f677944 (merge
333b1d6). Its separate green CI does not verify this combination. Combined CI
40372171824 ran at 7ae515fdd4a7dd84af4a65f15e19392d6517a437 after
the complete Process follow-up and integration d68ec795 (docs on f677944).
Watch log C:\work\gesher-ci-review\exec3-watch-40372171824.log uses 120 seconds.
After dispatch, merge 2ba2375 incorporates integration 634a273 (documentation
above the verified f5ce6f6 AOT3 oatdump stage). It is retained for the final
combined CI; the completed diagnostic is at the SHA stated above.
Its main build and package job completed successfully;
the package log confirms actual install, Hello, device UI/input and uninstall.
One additional post-dispatch property regression is corrected: a child started
while ART temporarily changes ANDROID_DATA now inherits the boot's original
property root, bound to its session. A new session still chooses its own root.
The bounded native child assertion failed before the correction (missing value
and a nonempty scratch directory) and now passes with ten host assertions and
zero failures. This source and the updated CI expectation need the final run;
the completed diagnostic contains the earlier nine-assertion probe.
The Java suite also now launches both getprop and the NDK child with a scratch
ANDROID_DATA override, requires both to read the original boot value and then
requires the scratch directory to be empty. Java source compilation passes;
this ART/ProcessBuilder assertion runs standalone and in both APK modes in
the final CI, rather than treating the native CreateProcess probe as its proof.
The full track, pm/am and umask remain open until actual completed evidence.
Applied the coordinator's complete four-file Process follow-up (source 644e7f8):
readProcLines/readProcFile/parseProcLine, matching lightweight JNI declarations,
the full-width/procfs parser regression and native3 test wiring. The coordinator
has compiled/linked its C++ in CI, and local Java/script checks pass here; the
combined runtime test passes both Windows logs in CI 40372171824.

Included in this dispatch: pm/am now invoke a boot-classpath Binder client
for actual package list/path, activity start, force-stop and asynchronous
broadcast. The device test installs a second target APK, observes its resumed
Activity pid and receiver output, then verifies force-stop removes that pid.
Local Java/native/script checks pass; this behavior has no runtime CI proof yet.
The packaged one-shot device path also now uses the same protocol handshake
through RuntimeCommands and fails before servicemanager when it cannot connect;
its old pipe-name polling loop had proceeded after timeout. Its package job
and both e2e jobs pass in CI 40372171824.
Property UTF-8 validation is included in this dispatch: local native
assertions reject overlong/surrogate/out-of-range sequences and preserve a
valid four-byte value. Initial build/persistent values follow the same policy.
An additional Java check reads a 1000-byte ro value through the framework and
verifies the same property's legacy NDK get returns the Android error value;
the Java and legacy NDK reads pass in CI 40372171824 before the later fixture
name collision. The new UTF-8 ABI assertions remain after that collision.
The coordinator's windows3 diagnosis also exposed Launcher forwarding partial
log chunks: its own status println could split a child's first-frame window
title. Tail now frames raw UTF-8 bytes into complete atomic lines, resets the
pending fragment on truncation and emits an unterminated final fragment once,
with a separating newline, only when finishing. The focused JDK regression
passes in the CI build; unchanged real windows3 UI assertions pass both
Windows jobs in CI 40372171824.
Applied the coordinator/web3 completed-geocoder termination follow-up after
the 30/45-second bounds: the isolated probe prints and flushes its genuine
MapLocationFinder result, then terminates only itself to bypass a CLR/WinRT
teardown hang. Errors exit 1 and incomplete answers still fail. PowerShell
the actual query returns InvalidCredentials on both runners in CI 40372171824.

Continuation history and exact evidence:

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
  Exec3Test. No AOSP build was run locally. No exec3 work was CI-verified at
  that first dispatch; later evidence is recorded above and below.
- Initial CI 40311734733 at 8c90ac2, including integration 91c195d/native2.
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
- Earlier corrected combined CI 40320290754 at 1189dc4. Watch log is
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
- CI 40320290754 Windows-2025 job 121578589424: the real launcher passes all
  47 exact shell cases, ART incremental stdin, and the Java signal/tool suite
  (destroy executes the native SIGTERM handler, destroyForcibly kills, ps/kill/
  cat/sleep pass). Its sole failing Check is the property test's bootstrap:
  $bcp contained only core jars, so android.os.SystemProperties was absent.
  The test now uses $bcpApp and explicitly loads android_runtime when run as
  standalone dex; the APK already bootstraps that library in its normal app
  startup. No property assertion ran in that failed Check. Both e2e runners
  have zero unexpected failures, including required TIC-80 and Puzzles with
  generator status 0; package passed. Windows-2022 is still pending.
- Merged the coordinator's verified AOT integration 1d49642 in e86aae while
  retaining the current Windows-2022 watch. The merge is clean and preserves
  the boot-image/CI artifacts, test 61, app/window/service changes and the
  existing libbase ScopedFlock work. Next combined CI must verify this tree;
  AOT's separate green run does not verify it together with exec3 changes.
- Completed current-run review: Windows-2022's log also has
  `exec3 shell: 47 cases, 0 failed` and the native-handler Java destroy/tool
  PASS, plus the existing exec, exec2 and arm64 ProcessBuilder checks. Its
  property test has the same pre-assertion ClassNotFoundException. A separate
  media check reported 29 sounds instead of 28; the job ended cancelled at its
  60-minute limit after the final devsvc output. These outside-domain findings
  were sent to the coordinator with the full job log; they are not hidden by
  the exec3 passes.
- Executable TLS is now prepared independently: an explicit main-executable
  option reserves a compatible first static PT_TLS block and enables negative
  absolute FS replay only within it. Eleven own-file native Windows assertions
  pass for integer/SSE operations, flags/registers, thread initialization and
  rejected forms. The API29 NDK program uses explicit local-exec assembly and
  is checked through the real launcher, ProcessBuilder and the device APK in
  the next CI. Its full ELF startup is not locally verified. No AOSP build was
  run locally; loader syntax checking used a host-only pread declaration.
- Applied the coordinator's source-only CI reliability patch 50f16a9 for the
  next run: the devices geocoder wait now has completion and parent-process
  limits, and e2e APK downloads use pinned atomic cache files plus an official
  mirror fallback. Its four own-byte transport fixture checks pass locally.
  These changes preserve failures on timeout/hash mismatch; the active run
  40338806524 predates them. No APK or CI artifact was downloaded locally.
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
