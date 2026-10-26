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

This decision does not approve a partial property or package-manager
implementation: getprop/setprop, logcat and pm/am must use the real device
services and have cross-process tests before their roadmap boxes are ticked.

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

## Remaining work

- umask and actual permission behavior; broader shell conformance, including
  special-builtin assignment scopes and persistent-descriptor nesting.
- Signal interruption while blocked reading input, real mailbox traps and
  Process.destroy/destroyForcibly verification.
- Shared properties, logcat formats/filter/clear and real pm/am service calls.
- Executable local-exec TLS and the on-device native-process suite.
- Review the recorded fork-before-exec/static executable limitations against
  the complete brief. This shell's process serialization does not make a
  general native fork clone possible.
