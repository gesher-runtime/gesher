# PR 73: current-main integration and runtime repair

## Event-loop fixture continuation, 2026-11-20

This continuation preserves published PR73 head
`8525b6001b739db477eb9aa161ec73cd3cda03e2` and composes it additively with
PR199 head `72feb34c5f6dce3e9b42bffcb34458b7a7181c99`, which contains actual
main `7a45c2468c270bfe68ee5f854aa9d4feaf2208b3`. These are integration inputs,
not a declaration that PR199 or PR73 has merged or passed every runtime gate.

The required event-loop host must execute production `PollFile`, retaining the
actual pipe object across descriptor replacement and reporting native readiness
errors. Its generated scratch translation unit instead lacked the snapshot,
descriptor guard and pipe-readiness dependencies now used by that body. The
current head reproduces the original compilation failure before its assertions
can execute; replacing those operations with successful stubs would not test the
requested behavior.

The host now extracts the unchanged production `DescriptorGuard`, `PipeAdmission`
and `PollPipeHandle` bodies. Its declared OS boundary uses a real recursive
transaction, an owned Linux descriptor duplicate, and actual Linux pipe queue,
capacity and peer state. The existing poll/epoll, recursion, lifetime, deadline
and allocation assertions are unchanged. Added cases exercise queued bytes,
empty pipes, write readiness, final-writer EOF, actual numeric descriptor reuse,
duplicate closure, and failure propagation. They verify that capture holds the
descriptor transaction and that the native query executes after it is released.

This boundary does not implement or certify Windows OFD publication, named-pipe
scheduling or the Windows ABI. The separate existing OFD and Windows contracts
retain those responsibilities. Local ASan/UBSan execution disables LeakSanitizer
only because it is unavailable under the traced executor; checked-in CI retains
its sanitizer command and all original assertions.

The integration repins exactly three dependency hashes in the OFD qualification:
`unistd.cc` retains main's real `RLIMIT_SIGPENDING` limit, `posix.cmake` retains
the xattr source, and `process_bionic.cc` retains interrupted-wait error
propagation. The qualification records both composition heads. The fd/system
transition patch, socket/native-poll preservation comparisons and negative
controls are unchanged. No other production change is introduced here.

## Earlier reviewed composition

The integration preserves the approved file-status/open-file-description change
at `d329e86f2f77dba4e7a52a76c33b16fb8663568a` and composes it with actual main
`ea1a8feba9e3ec14755789b774fbd135fa8b4432`. Contributor commits remain ancestors.
This document records the reasons for the composition; it is not a claim that
the new combined tree has passed the required Windows runtime workflows.

## Production composition

An application asks public stdio operations to observe the descriptor's access,
append and nonblocking state, retain buffered data and conversion state correctly,
serialize operations on the same stream, and retire ownership on close/reopen.
The approved PR routes host and Android stream I/O through one descriptor-backed
owner. Reverting to CRT reads or writes would bypass that OFD state. Main adds
useful Bionic wide-character formatting, locale conversion, error and lifecycle
behavior; those contracts must survive the merge too.

| Conflict | Composed behavior |
| --- | --- |
| `extra_bionic.cc` | Keep main's move of wide/unlocked entry points into `stdio_bionic.cc`; route remaining unlocked flush/status operations to the shared owner. |
| `stdio_bionic.cc` | Keep main's formatter, UTF-32 Bionic converters, FORTIFY and stream contracts; use the shared owner for all I/O, locks, orientation and seeking. |
| POSIX `stdio.h` | Keep public unlocked/locking declarations and main's internal retirement outcome bridge. |
| `fd.cc` | Keep the pipe I/O and bounded poll-scratch dependencies and both contributions' dispatch behavior. |
| `fsops.cc` | Keep descriptor/OFD retirement and truthfully reject unsupported null-path mode changes; return retirement status only after actual close or reinitialization. |
| Freopen fixture and extractor | Exercise the composed host/guest path, actual file effects, both close-error outcomes, OFD retirement and exclusive/inheritance modes. |
| TCP selected-policy check | Reconstruct the exact current-main predecessor and retain complete socket/poll comparisons, with only explicitly listed registry access substitutions. |

Bionic's decoder state, one-wide-character pushback and conversion error now
belong to the shared stream's lifetime. There is no second FILE-address registry
that a host close could leave behind. Host `ferror`/`clearerr` see Bionic conversion
errors, successful seek resets pending decoder/pushback state, and close or
reinitialization removes the old metadata. Queries and zero-length operations do
not require creating new metadata. Failed pre-close admission retains ownership
and the `pclose` child association; it does not wait on a still-open pipe.

The Android 16 r4 implementation uses a separate single wchar pushback slot.
Its `ftello`/`fgetpos` path adjusts ordinary buffered bytes and does not encode
that slot back into byte pushback. The composition retains that existing Bionic
boundary rather than inventing a new position adjustment:

- [Pinned Bionic ungetwc](https://android.googlesource.com/platform/bionic/+/refs/tags/android-16.0.0_r4/libc/upstream-openbsd/lib/libc/stdio/ungetwc.c)
- [Pinned Bionic stdio and position operations](https://android.googlesource.com/platform/bionic/+/refs/tags/android-16.0.0_r4/libc/stdio/stdio.cpp)

## Buffered output during process shutdown

The stdio owner deliberately registers final flushing before ordinary application
exit handlers, so their later output is not lost. Previously, the socket table
and mutex were destructible C++ globals. An application exiting with live sockets
and buffered output could therefore call `write` from final stdio flushing after
the socket-dispatch registry had already been destroyed. This is a lifetime
violation even when the output stream itself is a regular file or pipe: dispatch
checks the socket registry before selecting that stream's backend.

The socket registry now has fixed, lazily initialized process storage with no
static destructor. This adds no allocation for the registry object itself and
keeps the existing zero-socket fast path. Explicit descriptor close still removes
entries and retires native resources normally. Resources deliberately left open
at process exit remain available to late callbacks and are reclaimed with the
process.

The regression test executes the actual stdio owner and its real late flush,
plus production socket-registry construction, insertion and lookup. Linux socket
representation and descriptor dispatch are declared boundaries; output uses a
real file. It requires `earlylate` after ordinary static destructors. A negative
variant changes only the registry lifetime back to a destructible static and
requires a precise `SOCKET_EXIT_RETIRED` diagnostic before invoking undefined
access to the destroyed map. This establishes the source lifetime defect. It
does not substitute for a Windows stack or prove that this was the sole cause
of the historical quarantine-socket timeout.

## Strict command transport

The blocking worker's protocol is LF-delimited bytes. Python text-mode stdin
otherwise translates LF to CRLF on Windows. The new descriptor-backed stdio
correctly preserves those bytes, exposing the old oracle's transport assumption:
the worker rejects `observe\r\n` instead of treating it as `observe\n`.

The oracle now selects LF explicitly. It also uses the real worker to accept
LF and reject an intentionally CRLF-translated command, on either host. Product
byte semantics and the worker's strict parser are unchanged. This failure is
distinct from the deadline-cleanup test repaired in PR 189.

## Validation and limits

The public-lock fixture now holds `flockfile`, the API an application actually
uses, while the closer calls public `fclose`. Independent `ftrylockfile` probes
check the target and a separate live stream before and during ownership, including
recursive acquisition by the owner. The existing bounded thread, wait-chain,
native stack, independent-close, retirement and negative controls remain. The
stack witness binds the real FILE argument and public close call to the observed
critical-section wait; it does not read undocumented CRT FILE layouts. Synthetic
parser controls cover both accepted evidence routes and 48 corruptions, and are
explicitly distinct from Windows runtime evidence.

The active producer inventories all tracked sources, including the new
`public_lock_probe.h` and parser controls. The consumer compares those source
hashes with its current checkout and retains exact producer/source/run/attempt,
output-member, compiler and role checks. The parser controls also run in the
blocking workflow's preflight. Frozen historical source envelopes in the retired
preflight mode are not rewritten as current approval receipts.

Focused local checks include:

- Real pinned llvm-mingw x86-64/ARM64 compilation of the affected production
  translation units with the project's build definitions and warnings as errors.
- Production stdio core, adapter and Linux reference, including actual owner
  metadata reset/error/retirement, cancellation, normal exit ordering and the
  shutdown-lifetime negative above, under ASan/UBSan.
- Actual close/reopen effects and Bionic failure propagation, including pclose
  association retention. In this fixture the shared owner and wide codec are
  explicit boundaries; the real owner executes in the separate stdio corpus.
- Freopen host/guest mode tests against real filesystem effects and a Linux
  reference, and stream-device retirement/reuse tests.
- Actual Linux blocking rows and strict LF/CRLF transport controls.
- Exact TCP source-transition reconstruction and mutation rejection. The
  qualification includes the new shared Bionic-state header; supplier roles,
  original native operations, deadlines and artifact checks are not relaxed.

LeakSanitizer cannot run under this local traced executor; local sanitizer runs
disable LSan only for that environment. The checked-in CI commands retain their
sanitizer requirements. Real Windows shared-owner behavior, Bionic locale/ABI
execution, public-lock evidence, virtual-resource shutdown and full supplier
provenance remain required runtime gates. A subsequent independent QA must bind
its result to the frozen combined tree, including the public-lock fixture repair.
