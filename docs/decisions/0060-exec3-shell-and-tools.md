# 0060 — Windows shell execution and Android process tools

**Status:** proposed (exec3 track). The first shell implementation is ready
for CI; the complete track remains open.

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
The exec builtin starts a child, waits and exits with its status; it preserves
the launcher architecture's process behavior but changes the program's pid.

This decision does not approve a partial property or package-manager
implementation: getprop/setprop, logcat and pm/am must use the real device
services and have cross-process tests before their roadmap boxes are ticked.

The property implementation now has one Win32 mapped area shared by libbase
(including Java SystemProperties), loaded ELF bionic and the command-line
tools. Its namespace combines ANDROID_DATA with the unique GESHER_BINDER boot
name. Single-process execution publishes an inherited GESHER_PROPERTY_SESSION
before children copy their environment. A named mutex serializes changes and
the area contains both per-property and global serials. Readers refresh cached
values when the generation changes; prop_info identities remain stable in a
process and callbacks retrieve the latest value. build.prop and platform
defaults seed each new area. ro.* values are write-once; persist.* changes also
use a locked, flushed replacement file and reload on a new boot. Closed areas
from old boots are removed when a new area opens; live mappings deny deletion.
This is a shared property service backend, without Android SELinux property
contexts or caller-specific write permissions. Those permissions and property
wait/change-notification APIs remain outside the current implementation. The
first draft's shared serials are monotonic counters; the complete bionic
length-byte/clean-dirty-bit serial representation still needs implementing
and checking with the wait APIs.

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

## Verification

Local checks compile/link only Gesher's own C++ with -Werror, using a small
launcher driver without the real signal mailbox. Forty-seven exact output
and status assertions cover quoting, positional/parameter expansion, control
flow, functions, pipes, subshell isolation, redirects, here-documents, read,
getopts, background waits, EXIT status/traps and state above 32 KiB. A Java
ProcessBuilder test covers incremental stdin and a read builtin consuming
the next input line. These local results establish neither ART integration
nor real signal delivery.

CI must run these same tests through the real launcher on both Windows
runners, with the existing native system/popen/exec checks, package and e2e.
No new roadmap box is checked before that evidence exists.

The property backend passes a native-only local driver: build.prop defaults,
separate-process writes/reads, callbacks and changing serials, empty and long
read-only values, write-once/invalid-name rejection, boot isolation, persistent
reload and 32 concurrent persistent writers. The real ART/NDK tests are added
to 45-exec3 and remain pending CI.

An additional APK runs the nine exec2 native checks through JNI inside the
app's ART process, with real device services, then repeats the Java shell,
signal, tool and property suites. This is prepared but not yet CI-verified;
its Java/package/script checks alone do not establish on-device behavior.

## Remaining work

- umask and actual permission behavior; broader shell conformance and
  persistent-descriptor nesting. Special-builtin prefix persistence and
  repeated temporary-assignment restoration have four added local checks;
  their real-launcher CI remains pending.
- Signal interruption while blocked reading input, real mailbox traps and
  Process.destroy/destroyForcibly verification.
- Shared-property ART/NDK CI proof, complete bionic serial encoding and waits,
  property permissions/notifications, logcat
  formats/filter/clear and real pm/am service calls.
- Executable local-exec TLS and the on-device native-process suite.
- General fork-before-exec and static executables remain unsupported for the
  architectural reasons reviewed above.
