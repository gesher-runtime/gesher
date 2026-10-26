# 0060 — Windows shell execution and Android process tools

**Status:** proposed (exec3 track). Shell, process and compatible executable
TLS checks pass CI; the combined tree and complete track remain open.

## Context

ADR 0035's minimal shell rejects pipelines, redirections and control flow.
ADR 0055 routes native system/popen/exec calls through that same shell but
leaves shared properties, device tools and executable TLS work outstanding.
Windows cannot fork ART's multithreaded address space. Shell stages need
isolated variables and working directories while communicating concurrently.

## Decision

Implement the shell in Gesher's own launcher. The lexer/parser retain raw
words and compound-command trees; expansion runs at execution time.
`gesher_shell.cc` includes the executor, builtin and entry-point sources in
dependency order. `gesher_exec.cc` delegates --sh to this implementation.

Simple external commands use exec_spawn's normal program search, environment
block and explicit inherited descriptor list. A shell keeps its current
directory/environment in its own state. A subshell, pipeline stage or command
substitution runs in another launcher process with a serialized copy of that
state. A private inherited fd 63 carries the script so large variables and
functions do not exceed Windows' command-line limit or consume the child's
stdin. Pipeline stages start concurrently before any wait. Here-documents
and command-substitution output use temporary files, with handles owned by
RAII objects; command substitution strips trailing newlines.

The shell supports control flow, functions/local scopes, parameter/arithmetic
expansion, quoted positional fields, pathname expansion, ordered redirects,
fd duplication, background jobs/wait, common builtins and getopts. Exit traps
run with the original status. Caught mailbox signals are queued without
calling the parser from the mailbox thread and dispatched between commands.
Default signals are forwarded to foreground children before shell exit.
For synchronous pipe input, the command loop and read builtin check pending
traps while waiting for bytes. A caught signal interrupts read with 128+signal
and the main command loop keeps accepting input. Native console input does
not use this pipe polling path.
Persistent descriptors carry a generation in command I/O: an inherited exec
redirect does not override a newer temporary group/function redirect. Scoped
redirections restore their descriptors after the command, including nested
exec changes, while changes to other descriptors remain in the shell.
The exec builtin starts a child, waits and exits with its status; it preserves
the launcher architecture's process behavior but changes the program's pid.

This decision does not approve a partial property or package-manager
implementation: getprop/setprop, logcat and pm/am must use the real device
services and have cross-process tests before their roadmap boxes are ticked.

The property implementation now has one Win32 mapped area shared by libbase
(including Java SystemProperties), loaded ELF bionic and the command-line
tools. Its namespace combines ANDROID_DATA with the unique GESHER_BINDER boot
name. Single-process execution publishes an inherited GESHER_PROPERTY_SESSION
before children copy their environment. The original data root is inherited
with its session binding, so a temporary ART scratch ANDROID_DATA cannot give
a child a different property service in the same boot. A new session ignores
the previous root binding. A named mutex serializes changes and
the area contains both per-property and global serials. Readers refresh cached
values when the generation changes; prop_info identities remain stable in a
process and callbacks retrieve the latest value. build.prop and platform
defaults seed each new area. ro.* values are write-once; persist.* changes also
use a locked, flushed replacement file and reload on a new boot. Closed areas
from old boots are removed when a new area opens; live mappings deny deletion.
This is a shared property service backend, without Android SELinux property
contexts or caller-specific write permissions. Per-property serials now encode
the value length and an even revision incremented by two on every update,
including a same-value write. Long ro values retain the bionic long flag and
legacy error value; read_callback returns the complete immutable value from a
process-lifetime allocation. Specific-property and area waits release locks
between observations, use a monotonic relative timeout and leave the output
serial untouched on timeout. wait_any waits indefinitely for an area change.
Polling currently bounds notification latency to approximately ten milliseconds.
These ABI assertions pass on both runners in CI 40352529279, in standalone ART
and both APK modes. The boot mapping now retains its initial data root when
ART temporarily changes ANDROID_DATA for scratch files; log startup initializes
that mapping before fixtures can change the environment. A native regression
checks that the scratch directory remains empty and removable.
Caller-specific permissions and the device's report/poke notification path
remain open.
Property values now reject malformed UTF-8, including overlong encodings,
surrogates and out-of-range code points; initial build/persistent values use
the same validation. Native host assertions pass invalid sequences and a valid
four-byte value. This correction was introduced after CI 40352529279 and its
NDK assertions pass CI 40385411337 in all three execution modes.

Java change callbacks retain the existing framework/libutils report path:
[SystemProperties JNI](https://android.googlesource.com/platform/frameworks/base/+/refs/tags/android-16.0.0_r4/core/jni/android_os_SystemProperties.cpp)
registers callbacks with libutils and reports them on reportSyspropChanged;
[SystemPropPoker](https://android.googlesource.com/platform/frameworks/base/+/refs/tags/android-16.0.0_r4/packages/SettingsLib/src/com/android/settingslib/development/SystemPropPoker.java)
uses Binder's SYSPROPS_TRANSACTION to notify services. Ordinary property
writes do not replace that report/poke API. Verification of this existing
callback path failed CI because pinned libutils/misc.cpp discards its registry
on Windows. An own host implementation now preserves Android's priority order,
copies the registry under its lock and invokes callbacks after unlocking.
The native report/priority/reentrant-registration regression passes locally;
actual Java report callbacks pass CI 40385411337 in standalone and both APK modes.

Logcat now has prepared real structured records from Java's pinned liblog
logger and the NDK shim, while stderr keeps its existing human-readable output.
SystemServer passes each app a separate GESHER_LOG_FILE sidecar; single-process
Main sets one before starting the APK. A named mutex protects whole records,
reads and actual clear operations. Storage rolls over at 16 MiB. The tool dumps
or follows these records, applies tag/priority filters and renders brief,
process, tag, thread, raw, time, threadtime, long and epoch formats. Shared
properties and liblog's process minimum priority control loggability, including
length-limited NDK tags. Binary event decoding, logd sockets and other logcat
options are outside this text-log implementation. Local backend/tool checks
pass. CI 40352529279 captured NDK records but missed Java records because ART
replaced the constructor's logger. The prepared setter interposer now records
around whichever logger the runtime selects, preserving its forwarding behavior.
It also uses the actual DEFAULT priority sentinel (1). An own mock-call contract
checks replacement and forwarding; actual combined Java/NDK capture awaits CI.

CI exposed a real broker startup race in Launcher: servicemanager could open
/dev/binder before binderd created its first endpoint. Launcher now waits for
a connection to the expected broker pid and a valid HelloProc response, using
overlapped I/O bounded by a monotonic deadline and broker lifetime. Only then
does it start servicemanager. The ten-case native probe includes delayed,
malformed, stalled, exited and wrong-pid brokers. RTL assertions stay intact.

The next pm/am slice now has a real Binder client on the boot class path.
Native tools start this helper with the same device environment and runtime
jars/image. pm list packages/path query IPackageManager; am start uses the
activity-task service, force-stop delegates through IActivityManager to its
existing process termination implementation, and broadcast calls the actual
broadcast controller. Broadcast submission is asynchronous, with no fabricated
ordered result. A second installed APK lets the device test observe Activity
resume, receiver delivery and actual pid disappearance after force-stop.
User 0 and the implemented intent options are explicit; unsupported commands
fail. Java and native source checks pass, but this slice is absent from active
CI 40352529279 and still requires real device evidence.
The packaged one-shot startup reuses this helper's internal wait-binder entry
point before starting servicemanager, so reaching the old pipe polling deadline
cannot silently proceed. Its runtime/package verification is also pending.

Launcher log forwarding now buffers raw bytes until newline before a single
atomic PrintStream write. This prevents the launcher's status line from
splitting a child window title across 16 KiB reads, which had hidden a genuine
first-frame HWND from the windows3 test. Truncation discards pending old bytes;
finishing emits one remaining fragment with a separating newline. A focused
plain-Java regression passes UTF-8 chunking, concurrent parent/child lines and
reset/final-fragment behavior. Existing UI assertions remain intact.

## Fork and static-executable feasibility review

The native fork trampoline saves the calling thread's register/stack return
context and records selected descriptor, environment and directory operations.
It creates a Windows process only when exec is reached. It never clones the
native heap, ART heap, TLS, running threads or synchronization objects, so
arbitrary writes or library calls in the child affect the original process.
Saving more registers or serializing shell variables cannot provide fork's
independent copy of these objects. A general child that performs work before
exec therefore remains unsupported under the current architecture. The
supported subset is the documented vfork-style preparation followed by
exec/_exit; this review does not expand it.

Fixed-address allocation alone would not enable an ET_EXEC static program.
exec_spawn currently rejects that ELF type, and the loader maps ET_DYN objects
whose imported libc operations can resolve to the bionic shim. A static NDK
libc embeds Linux syscall instructions instead of those imports; Windows
interprets those instructions with a different syscall ABI and kernel object
model. Supporting it requires syscall translation and process/runtime state
beyond the existing loader. Keep ENOEXEC for ET_EXEC rather than mapping it
and allowing its first kernel operation to execute incorrectly. Static PIE
and non-NDK runtime compatibility are not established by the dynamic NDK
executable tests.

## Executable local-exec TLS

The launcher now marks the initial dynamic executable explicitly in
LoadOptions. The loader clears this option for dependencies and reserves the
executable's compatible variant-II TLS block before rewriting its code. It
must occupy the first static block, fit the static reserve, have alignment at
most a page and no alignment skew or text relocations. Incompatible layouts
fail explicitly. A linker-resolved negative absolute FS displacement is
eligible only within that executable block; the normal thread-pointer and
stack-guard rewrites retain their existing meaning.

Absolute local-exec instructions use a preserved scratch GPR to load the
thread pointer from its GS slot, then replay the original memory operation
without FS and return. Scratch storage stays below the System V red zone;
the setup/teardown preserves flags and legacy high-byte register encodings.
Integer, scalar SSE, SIMD moves and x87 memory operations have supported
replay paths. VEX/EVEX instructions, stack-pointer operands and unsupported
control-flow/opcode forms retain explicit traps. This does not extend static
executable support or the arm64 translator's separate TLS model.

An own-file Windows probe executes eleven assertions, including registers,
carry flags, AH, scalar/vector SSE, per-thread initialization and rejected
out-of-block/stack-pointer cases. Those checks pass locally with -Werror.
The NDK API29 executable has explicit FS local-exec integer/SSE assembly and
checks constructors, initialized data, tbss, alignment and pthread isolation.
Its loader and ProcessBuilder runs pass both runners in CI 40338806524;
the same checks also pass inside both APK modes in CI 40352529279 before the
later callback failure. CI 40385411337 passes the complete single-process APK
and those checks inside the installed APK before pm's image probe fails.
Local instruction replay alone was
not used as proof that the complete ELF startup works.

## Verification

CI 40405306826 at 87d5adadea4008a9c4340b9ee58fbabf9aedb00f is fully green:
both Windows whole-suite zero summaries, all exec3 and new ART camera-race
checks, package and both nine-app e2e runs. The prior post-steps fault does not
recur in this combined tree; its exact cause is not inferred. The later
default-tag loggability/setter correction awaits final combined CI with f520fd1.

CI40393019261 passes all exec3 Checks on both runners, including real installed
pm/am list/path/start/broadcast/force-stop, and the whole Windows-2022 suite.
Only a Windows-2025 device-mode post-steps crash remains in the combined run;
all build/package/e2e jobs pass. Its preserved record locates RIP in ntdll, so
the product caller and root cause remain open. Media's ownership/race fixes
have independent comparison proof and are combined with verified Native3
integration117. Track58 invokes their actual-class1000 cancel/flush selection
regression under ART in the next run; local JDK execution passes. This is not
proof of the ntdll fault's cause. Zygote's VM-prefix extraction also now decodes
complete Windows argument tokens, fixing quoted Launcher main-class startup;
thirteen host cases pass and actual combined runtime proof remains pending.

CI 40385411337 verifies the complete shared property and log suites in
standalone ART and both APK modes, including scratch-root ProcessBuilder,
explicit Java reports, serial/wait ABI, boot/persist and live logcat clear/follow.
The complete single-process APK passes. Installed pm stops at an incorrect
logical-image existence probe, now corrected locally; pm/am runtime proof and
the post-steps devices SIGSEGV cause remain open. The existing native context
was captured but hidden by the test helper's path/filter error; a publication-
only correction is prepared and locally verifies the original fatal exit.

CI 40372171824 passes the 57 shell cases, process/TLS suites, readiness,
standalone Java/NDK logcat, unchanged windows3 and all build/package/e2e jobs.
Its complete property/APK/pm-am checks stop at a readonly fixture-name collision,
now corrected without changing write-once semantics. Java report and later
property assertions remain pending. Windows-2022 also has two real post-steps
devices process crashes at address -1; the shutdown cause remains open and a
bounded, env-gated pre-handler context trace is prepared for the next run.

Local checks compile/link only Gesher's own C++ with -Werror, using a small
launcher driver without the real signal mailbox. Forty-seven exact output
and status assertions cover quoting, positional/parameter expansion, control
flow, functions, pipes, subshell isolation, redirects, here-documents, read,
getopts, background waits, EXIT status/traps and state above 32 KiB. A Java
ProcessBuilder test covers incremental stdin and a read builtin consuming
the next input line. The expanded 57-case suite and the real ART/mailbox tests
pass both runners in CI 40338806524; local driver results alone establish
neither ART integration nor real signal delivery.

CI must run these same tests through the real launcher on both Windows
runners, with the existing native system/popen/exec checks, package and e2e.
No new roadmap box is checked before that evidence exists.

The property backend passes a native-only local driver: build.prop defaults,
separate-process writes/reads, callbacks and changing serials, empty and long
read-only values, write-once/invalid-name rejection, boot isolation, persistent
reload and 32 concurrent persistent writers. Further host checks pass encoded
serials, stable readonly callback pointers, long-value legacy reads, reentrant
enumeration, zero/finite timeout, unrelated writes, same-value wakeups and
cross-process specific/global/wait_any waits. The same property ABI assertions
are in the NDK child, with Java coordinating actual cross-process writers and
checking explicit reportSyspropChanged callbacks. The NDK ABI assertions pass
CI 40352529279 and again CI 40385411337, including the real Java report callbacks.

An additional APK runs the nine exec2 native checks through JNI inside the
app's ART process, with real device services, then repeats the Java shell,
signal, tool and property suites. The nine JNI checks pass both APK modes on
both runners in CI 40352529279 and CI 40385411337. The latter also passes the
complete single-process APK and installed properties/logcat. CI40393019261
verifies installed pm/am, and CI40405306826 verifies the complete combined run.

## Remaining work

- umask and actual permission behavior; broader shell conformance.
  Special-builtin prefix persistence and repeated temporary-assignment
  restoration have four added local checks; persistent descriptor scopes
  have six, also compared with a POSIX shell. All 57 real-launcher cases pass
  both runners in CI 40385411337.
- CI 40338806524 verifies idle pipe/read mailbox traps and native-handler
  Process.destroy/destroyForcibly on both runners. Native console interruption
  remains unverified. CI40405306826 verifies the combined runtime tree; the
  separate default-tag follow-up still needs final combined CI.
- Property permissions and Binder report/poke notifications. Properties,
  explicit callbacks and real logcat capture pass CI 40385411337 in all modes.
- CI 40338806524 verifies the API29 NDK executable local-exec TLS program and
  ProcessBuilder invocation on both runners. The installed APK native-process
  suite and broker startup fix also pass CI 40385411337; installed pm/am pass
  CI40393019261. Unsupported TLS layouts and opcode forms remain explicit.
- General fork-before-exec and static executables remain unsupported for the
  architectural reasons reviewed above.
