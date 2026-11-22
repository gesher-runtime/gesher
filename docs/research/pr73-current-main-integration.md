# PR 73: current-main integration and runtime repair

## Shared owner and main cache integration, 2026-11-22

This author round starts from published PR73
537936e89a450107821b0bcf4cd40cec0d060f24 and preserves its ancestry. It
integrates the exact prepared PR296 composition
3ad165e47c91ed830c61a3528bd35db801bbf13a, whose parents are main
452372561e0a50c63f072a5ae1e7ca5aa81354b8 and fence repair
8df95e1cc29e74ef2d0853074bdc5cf6548baf0a, followed by main
0d25937fac3548c5e97f07df0e57def7f43ae868. This composition requires its own
independent review; older review results below are historical evidence, not
approval of the new owner/cache implementation.

The first merge has two source conflicts: Bionic stdio and the selected-source
policy. Incoming main's Bionic change is its positive TLS metadata cache.
PR73's codec, conversion error and wide pushback already belong to the actual
shared stream owner, so keeping a second Bionic metadata registry would split
that lifetime again. The integration retains the published Bionic delegation
and places the positive cache at the shared owner's actual lookup.

### One owner and a bounded positive cache

stdio_output.cc adds eight trivial TLS entries pointing to stable std::map
shared-owner slots. A successful lookup copies the existing shared_ptr while
the caller holds the outer CRT FILE lock, or supplies the synchronization
required by an explicitly unlocked API. The TLS entries own no stream,
allocate no state and run no ownership destructor. Missing streams are never
cached.

Thirty-two atomic generation buckets invalidate positive entries before their
map slots are erased. Both ordinary retirement and replacement after native
FILE-token reuse advance the bucket while holding the registry mutex.
Generation zero permanently disables that bucket after wrap; the original
locked lookup remains available. A miss acquires the registry mutex and
records the exact current slot and generation. Insertions and retirement of
other map nodes do not move a live stream's slot.

The original stream lock ordering, cancellation cleanup, flush-all pinning,
descriptor snapshots and retirement rules remain. Buffering, seek/reopen,
conversion state, errors and pushback still share the same owner.
Already-guarded and explicitly unlocked guest calls retain unlocked
orientation; public fwide retains its normal lock. Main's descriptor-backed
wide formatter path and closed-standard-handle spawn assertions are preserved.

Against main0d plus the approved fence repair, PR73 changes the same four
production paths listed in the earlier round below. Of PR73's four production
paths, only stdio_output.cc changes against the published PR73. No separate
socket correction outside the approved incoming main/fence inputs is included.

### Exact authority composition

The current OFD qualification manifest, its patch, all 71 source digests and
the native approval state are byte-for-byte the incoming main authority.
The native-fence manifest and patch are likewise unchanged. The original
PR73 shared-stdio manifest, patch and its 23 negative controls remain unchanged.
The cache integration does not replace these receipts.

A separate shared-stdio-cache-source.json pins two minimal patches and the
literal historical endpoints:

1. Reverse the owner-cache patch to the published PR73 owner bytes at
   537936e; check both reverse and forward endpoints.
2. Run the unchanged original four-file shared-stdio transition against its
   SHA-verified original main751 receipt, read from that immutable Git object.
3. Prove main's separate Bionic cache patch from its literal main751 source to
   literal main452 bytes in both directions.
4. Require all four resulting predecessor files to equal literal main452 and
   the current 71-path qualification before continuing the original checks.

The later main0d refresh changes none of these endpoints or protected paths,
so it requires no pin or baseline update. The 23 additional cache-policy
controls reject changed source/patch bytes, repinned false predecessors,
expanded paths, altered baseline or authority identities, changed endpoints
and invented approval fields. They run alongside the common 33, fence 4+2,
and unchanged original shared-stdio 23 controls: 85 controls in total.
This establishes bounded source preservation, not behavioral approval.

### Actual-source host evidence

The current adapter fixture compiles the complete production owner and the
actual guest Fgetc, unlocked read/write and orientation bodies. Its Linux
envelope declares the Win32 descriptor and CRT lock-lifetime boundaries;
files, descriptors, pipes and byte effects are real. Historical comparisons
read the selected revision's private/public headers as well as its production
bodies.

The sanitizer run passes 269 core, 191 adapter and 71 Linux-reference checks,
including all four cancellation phases, unlocked progress, shutdown ordering
and the existing late socket-registry controls. The other affected suites pass
114 lifetime checks; 446 host, 446 guest and 160 Linux reopen checks; and 229
device-lifetime plus 36 Linux checks. ASan/UBSan are fatal for these runs;
LeakSanitizer is disabled in this host environment.

Twenty-four cache-specific adapter checks cover progress while another thread
holds the actual registry mutex, every byte and EOF, cross-thread registration
after an earlier miss, retirement of another thread's cached owner, codec/error
reset, same-FILE replacement, native token registration, generation wrap and
real descriptor close. The unchanged current fixture rejects published PR73's
uncached owner at the bounded progress assertion. Four isolated generated-source
mutations separately disable the positive lookup, omit retirement invalidation,
omit replacement invalidation and re-enable a wrapped bucket. Each is rejected
by the intended assertion, without sanitizer errors. Failed-invalidation
controls clear only the test's dangling TLS cache after recording failure, so
the negative terminates safely without making a broken lookup pass.

benchmark_stdio_metadata.py now builds that same production adapter twice,
once from immutable published PR73 and once from current source. It no longer
replaces the owner with separate orientation/read stubs. Timing excludes file
creation and thread startup; order alternates, and each sample checks every
byte, EOF and descriptor close. A three-sample, 65,536-byte-per-thread host run
measured median after/before ratios of 0.840 with one thread and 0.787 with four.
These short host-envelope timings are diagnostic, not Windows performance
claims or approval gates.

### Bounded newer-main preservation and target limits

Main452 to main0d adds eight linear merges across 51 paths: PRs277,279,282,281,
307,308,311 and313. The merge is conflict-free. Fifty incoming paths equal
main0d byte-for-byte; the only composed path is ci.yml, which retains all
existing gates and adds exactly main's two NativeStderrChecks.ps1 calls.
Incoming regex optimization and FileIdInfo fallback stay intact. Neither stdio,
the descriptor owner, the 71 protected paths nor qualification inputs change.

The native build uses this worktree's exact AOSP lock and ordered patches,
not PR107's additional ART patches, and its own verified publisher-cancellation
toolchain receipt. x64 stdio/OFD, guest shim, native3 workers and ELF loader
compilation/linking, plus ARM64 compilation, check target ABI/build composition.
Such cross-build results must remain bound to their actual source commit/tree
and supplier receipt.

Actual Windows execution remains required for MinGW wide formatting,
cancellation, native3 lock/owner observations, the libc ELF load and both native
tracks. The genuine-CI identity guard is retained; no local CI identity or
Windows result is synthesized. The reproduced old unlocked-call defect and the
cache counterfactuals do not establish the cause of the complete historical
Windows timeout or the service/Puzzles failures. Those require the new composed
candidate's real CI execution after independent review.

## Main refresh, 2026-11-22

This refresh preserves the previously reviewed PR73 candidate
`3db72444ee486eb68f93e124ec092b9a2cb6dc0a` (tree
`f14267a28c2cae029fe2631fa6d1db2692635d1d`) and merges verified main
`324dc7e41ea13ceedc3d52db3c8f7c2f3590e452`. The earlier review was against
main `751d82310f7c4c470c098ccebfd5d2d04699a0cd`; this composition requires
incremental review and its own exact-head CI results.

Main advances by six merged changes across 49 files. None touches the four
production files described below. Those files, the PR73 fixtures and workflow,
and both shared-stdio qualification files are unchanged from the reviewed
candidate. Of the 49 incoming files, 48 equal main byte-for-byte, including
all incoming production code and `.github/workflows/ci.yml`.

The only conflict is `tools/net-tcp-priority/test-selected-policy.py`. It retains
main's exact memmem include/wrapper reversal, pinned helper and five additional
mutation controls, alongside the unchanged PR73 shared-stdio transition. Removing
only the prior PR73 additions reconstructs the complete new-main policy exactly;
removing only main's memmem additions reconstructs the reviewed PR73 policy
exactly. The memmem normalization applies to `shim.cc`, separately from the four
stdio paths. The original OFD manifest, patch, 71 source digests and approval
state remain unchanged; no existing qualification receipt is repinned.

Focused checks pass: the new-main policy and its 20 mutation controls in a
separate checkout; the combined policy with those same 20 controls plus the 23
shared-stdio controls; and 426,273 production memmem contract/work checks under
ASan/UBSan, with the comparison source verified against its original Git object.
The changed shim compiles for x64 and ARM64 Windows, and the x64 ELF loader
fixture relinks successfully; the unchanged stdio/OFD target remains up to date.
The earlier stdio implementation and fixture checks are preserved evidence,
not newly claimed executions. Windows/native3 runtime execution and the final
tested merge identity remain CI requirements after incremental review.

## Earlier frozen composition, 2026-11-22

This round starts from published PR73 head
`de1e43c391275137c7b7b25dc170892684d55b5e` and merges verified main
`751d82310f7c4c470c098ccebfd5d2d04699a0cd`. Their merge base is
`7a45c2468c270bfe68ee5f854aa9d4feaf2208b3`. All 57 conflicting paths were
examined against those inputs. Earlier dated sections below describe previous
checkpoints; their source qualification and validation claims are historical.
This section describes the new composition, which requires its own independent
scope, production-source and qualification review before publication.

Main already contains much of the older PR through subsequent integrations.
The resulting production delta against main is exactly four files:

| File | Behavior retained from PR73 and composed with main |
| --- | --- |
| `compat/posix/src/stdio_output.cc` | Store Bionic conversion error, decoder and wide pushback in the shared stream owner; retire/reset them with the real host lifecycle. Preserve main's outer CRT lock, cancellation cleanup, unlocked orientation API and wide formatter dispatch. |
| `compat/bionic/stdio_bionic.cc` | Use that owner for guest locks and metadata, close/reopen, seek and errors. Already-guarded and explicitly unlocked operations call orientation with its unlocked flag; public `fwide` retains ordinary locking. |
| `compat/posix/include/gesher/stdio.h` | Declare the Bionic metadata and its required caller synchronization while retaining main's orientation signature. |
| `compat/posix/include/stdio.h` | Document shared-owner retirement and the existing close/reopen retirement result; the declarations remain unchanged. |

Every other `app/` and `compat/` file equals main. In particular this preserves
the OFD v3 transfer/revocation format, memfd seals and descriptor adoption,
environment ownership, signal behavior, ARM support, and current permission and
service implementations. Main's two relevant OFD corrections remain intact:
wide formatting uses the descriptor-backed FILE callback path, and a native
child with no inherited standard handles uses the detached-process creation
behavior. Their strengthened Windows assertions remain unchanged. Overlapping
history had introduced duplicate signal exports and duplicate executable target
definitions; the composition keeps main's single definitions.

### Reproduced unlocked-call defect and its limits

The old PR's Bionic `FreadUnlocked` and `FwriteUnlocked` call an orientation helper
that takes the shared stream lock. In the native libc fixture, another thread
can own `flockfile` while an unlocked operation is expected to proceed. A bounded
regression now compiles the exact production owner, orientation and Bionic
unlocked function bodies. The lock holder remains idle while the other thread
performs real file I/O; the fixture always releases the lock and joins the worker
after the bounded observation, including when testing the defective source.

The unchanged fixture against exact head `de1e43c...` reports two failures out of
twelve assertions: both unlocked operations fail to complete before unlock.
Actual byte effects and normal close still succeed afterward. The composed
source passes those twelve assertions as part of the 167-check production
adapter corpus. This establishes the source-level lock defect. It does **not**
establish that this defect caused the entire historical Windows timeout: the
last failed native jobs reached the libc ELF invocation, but no target stack or
fresh composed Windows execution is available locally.

The event-loop fixture additionally preserves main's capture/query error
assertions while executing PR73's exact `DescriptorGuard`, `PipeAdmission` and
`PollPipeHandle` bodies over the declared Linux pipe boundary. Descriptor reuse
occurs after the real descriptor transaction releases; failed capture cannot
query a pipe, and failed native queries retire their snapshot. Main's real
revocation, seals, allocation-failure and OFD-transfer cases are retained.

### Exact source qualification

The original `qualified/ofd-stream-source.json`, its transition patch and the
`OFD_QUALIFICATION_SHA256` constant are byte-for-byte main. All 71 original
source digests, supplier roles, provenance rules, deadlines and native ownership
comparisons remain. A separate
`tools/net-tcp-priority/qualified/shared-stdio-source.json` and `.patch` bind only
the four production files above to exact main and PR input identities.

The check validates the manifest schema and its independent digest, exact path
list, patch digest and headers, main policy digest, current endpoints and original
receipt digests. It reverses the minimal patch in an isolated directory and
compares every result to literal `git show 751d823...:path` bytes, then applies
the patch forward and compares every result to the current file. Only those
verified predecessor bytes enter the original 71-digest check. An unrelated
source change remains an error. No execution-approval field is added or changed.

The original main policy was run in a separate checkout and passed all 15
original negative controls. The composed policy passes the same controls plus
23 new controls. These reject unqualified admitted-source/header changes,
repinned endpoints with undeclared includes, expanded paths, modified patch
before-hunks, changed baseline identities, invented approval fields, changed
original receipts and unrelated protected-source edits. Run the complete set
with `python3 tools/net-tcp-priority/test-selected-policy.py --selftest`.
This is source preservation evidence, not a substitute for behavioral review.

### Windows witness composition and available validation

The native3 worker retains main's native CRT layout/owner observations, exact
`_lock_file` argument instruction and import checks, owned-thread birth checks,
WCT rejection controls, actual completion/join checks and original five-second
absolute deadline. PR73's public `ftrylockfile` observations and exact public
`fclose` call witness are required **in addition** on both accepted wait routes.
Four cycles alternate public and native ownership. A released/reacquired sibling
gets a fresh native-owner observation. The wrong-live-owner negative binds the
still-live blocked closer; an exited probe is not described as a live owner.
The descriptor must be actually retired after close. Unsupported call shapes
fail the witness rather than falling back to an inferred argument.

Synthetic parser controls accept two complete transcripts and reject 70
corruptions across the two routes. They do not constitute Windows evidence.
The genuine-CI restriction in the command oracle, actual evidence validator and
artifact adversarial runner remains enforced. No CI identity is supplied to run
an actual worker or to qualify actual execution. Parser controls use only their
explicitly synthetic test context and transcripts.
The producer inventories all tracked sources, including both public witness
files, and the consumer's source/run/attempt/compiler/member checks remain.
Historical source-envelope modes and approval receipts are unchanged.

Eleven relevant host suites pass with ASan/UBSan: stdio buffering/adapter/Linux
reference, Bionic stream lifetime, event-loop contracts, OFD/exec transfer,
scalar and positioned file I/O, pipe I/O, revocable files, freopen modes,
stream-device retirement and memfd contracts. They include main's cancellation
and CRT-lock composition assertions and PR73's shared metadata and actual
socket-registry shutdown negative. LeakSanitizer is disabled only for the traced
local executor; the checked-in sanitizer commands remain unchanged. Linux
native3 workers compile with warnings as errors, the instruction-pattern corpus
passes 45 checks, and the command cleanup suite passes seven tests.

The build uses an isolated Android 16 r4 tree refreshed by this checkout's
unchanged fetch script: all 123 project heads match their pinned tags and every
sparse/ordered-patch stamp matches these sources. PR107-only patches and sparse
inputs are absent. The llvm-mingw 20261006 publisher archive and WebView2 SDK
digests are verified. The x64 build and link pass for `stdio_ofd_test`,
`ofd_spawn_test`, `pipe_native_process_test`, all three native3 workers and
`elf_load_test`, including the real `gesher-exec` and native-child dependencies.
The standalone font input proof passes through the existing
`GESHER_VERIFY_STANDALONE_LOCAL=ON` mode, retaining its actual map and archive
member checks and recording a local execution context with no workflow source.
Both changed stdio translation units also compile with the ARM64 Windows
toolchain and the project's warnings-as-errors settings.

Inspection of the linked x64 worker confirms the supported generated public
R12-to-RCX `fclose` call and native RDX-to-RDI-to-RCX `_lock_file` import-call
patterns. The three native3 PE import sets pass the existing system-import
policy, and the production POSIX archive contains no wait-test hooks while the
private blocking fixture does. These are compilation, link and binary-inspection
results, not Windows runtime observations or qualification of the separate CI
publisher-cancel toolchain overlay.

Actual Windows 2022/2025 native libc, stdio/OFD, native3, services and Puzzles
process-exit checks remain required on the reviewed new head. Old AppOps,
document-dialog and process-exit failures are not assigned speculative causes
or patched independently here. A new target failure must be assessed against
this exact composition and its actual evidence.

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
