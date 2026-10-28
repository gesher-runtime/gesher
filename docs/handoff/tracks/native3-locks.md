# Native3 record locks: initial source/design checkpoint

Dedicated worktree/branch `C:\work\gesher-native3-locks` / `wip/native3-locks`
starts at integrationc455f432. Root reserved ADR0080 and Windows test87 after
checking canonical files for collisions. The source audit and proposed
contract are in [ADR0080](../../decisions/0080-native3-advisory-record-locks.md).

Current stage is design/source audit only: no production change, fixture,
compiler, runtime or CI result. Known shared seams are common fcntl's32-bit
integer argument versus record pointers, bionic's fake success, common fd
close/dup2/F_DUPFD lifecycle, and stdio's CRT close paths. A correct advisory
registry needs process creation identity/crash cleanup and stable file/open
description identity, without mandatory file-data locks. Independent design
and source review precedes qualifying CI. Preserve existing integer fcntl,
flock, x86 and all established tests. Unsupported fork remains explicit.

Coherent ARM API3 prerequisite remains independently active at40549806536 /
c7a43653 in the varargs worktree. Reviewed environment85 source is saved at
`wip/arm64-env-review-checkpoint`70beaaa5, production54cccc22/provenancee543d8ea,
and has no CI dispatch until coherent runtime proof. Neither is accepted as
a full product. No broad full is dispatched until relevant prerequisites and
Web3 gate pass. Next lock action: peer review the actual coordination design,
then implement and verify its bounded native/common and NDK contracts in CI.

Prepared Linux reference source838e90a0 executes actual libc fcntl through
independent CI workers, including POSIX/OFD owner distinction, ranges/errors,
real blocking/EINTR/SA_RESTART, cross-file POSIX deadlock versus OFD waiting,
hardlink/dup/stdio/failed-freopen lifetime, crash cleanup and exec descriptor
transfer. Peer review found lifecycle/query-normalization issues; source now
cleans a failed worker construction, attempts every worker cleanup with
bounded reader EOF, preserves nonquery/error PID evidence and asserts
unlocked-query input fields unchanged. AST parse only ran locally; no syscall
fixture ran. Reference scope still explicitly excludes injected close-EINTR,
simultaneous mutation contention and PID-reuse/denied-query controls. This is
not production record-lock implementation or final acceptance.

Independent actual Linux reference40555739745 at exact
4de5b1d38c28bba95120bcc3c7e3dd83d246e81b is green. Both final metadata and
plain runtime log are saved under `C:\work\gesher-ci-review\native3-locks-oracle-40555739745*`.
The actual summary is `rows=114 failures=0`; evidence includes EINTR4,
SA_RESTART successful completion after unlock, POSIX cross-file EDEADLK35,
an OFD cycle remaining blocked until EINTR, failed-freopen ENOTDIR20 still
releasing the old file, process crash cleanup and inherited OFD ownership.
This establishes Linux reference semantics only.

The first production interval core is now prepared in
compat/posix/src/record_lock_ranges.h: checked signed64-bit normalization,
process/OFD owner distinction, conflict/query, split/replace/merge and
capacity-safe staged replacement, plus POSIX wait-graph cycle helper. It is
not wired into runtime yet; shared coordination, liveness, signal/descriptor/
stdio/spawn ownership remain mandatory open work. A new CI-only native
kernel comparator prepares10000 actual OFD mutations and20000 queries over
independent descriptions using the same core, requiring actual Linux result/
errno/range/type/PID agreement. No compiler or fixture ran locally; bounded
source review precedes CI wiring/dispatch of this new core.

Follow-up pure-core source now uses fixed512-record snapshot and514-record
scratch storage instead of dynamic interval allocations, with explicit
normalized-range/non-Unlock invariants. Seven zero/full/split/conversion/
merge capacity outcomes require byte-identical old snapshots after ENOLCK.
The independent kernel comparator additionally prepares17 signed-limit/EOF/
relative boundary rows and deterministic seed0x47657368 with exact replay
inputs on mismatch. Windows peer review found no bounded pure-interval source
blocker; CI now compiles/runs it on Linux only. The native engine is still
unwired, so no Windows record-lock acceptance follows. Mixed POSIX/OFD wait
cycles are separately observed by real Linux worker requests before any wait
graph implementation can be qualified. Shared registry, unknown-owner/PID
reuse, close/stdio/spawn lifetimes, signal integration and full runtime gates
remain mandatory open scope.

Actual pure-core qualification40559255447/a70682a3 is green: both jobs and
actual logs were read. Capacity seven outcomes/byte preservation all1,
17 signed/EOF boundaries agree,10000 kernel mutations and20000 queries
have0 failures. Actual process reference now has126 rows/0 failures. Mixed
same-file discovery: an OFD requester remains blocked until EINTR; the
reverse POSIX requester gets EDEADLK35 while traversing an OFD waiter edge.
Do not drop all OFD graph edges. This is still Linux/pure-core evidence.

Shared Windows registry source foundation is now prepared, not compiled or
wired: record_lock_registry.{h,cc} uses the same canonical ProgramData path
in standalone/installed/all sessions, an explicitly secured Global mutex and
unnamed file-backed mapping (no Global section privilege or per-session
fallback). Persistent metadata uses version/size checks, two banks with
generation+CRC64, complete inactive publication followed by one interlocked
active index; invalid active state never falls back to an older bank. Checked
Windows heap allocation stages fixed-capacity state; failed candidates cannot
mutate the active snapshot. Process/thread birth, descriptor reference and
wait-request fields are storage only until actual liveness/lifetime code is
implemented. ACL/nonadministrator/cross-session behavior, abandonment/crash/
corruption/heap-failure tests and bounded source peer review precede its
Windows CI qualification. No production fcntl/close hook is installed yet.

Liveness/identity source foundation is additionally prepared in
record_lock_liveness.{h,cc}, still uncompiled/unwired. Real probes compare
PID/TID plus creation FILETIME and signaled process/thread handles; access/
query failures are Unknown and never treated as dead. Staged pruning retains
all unknown references/owners/waits and only removes confirmed dead owners;
OFD locks require surviving description references. Registry validation now
rejects inconsistent POSIX PID/creation identity and orphan OFD records before
pruning. File keys use authoritative64-bit volume+128-bit FileIdInfo, and
documented NtQueryObject basic GrantedAccess supplies actual handle rights
without assuming F_GETFL's old O_RDWR fallback. See Microsoft's
[NtQueryObject contract](https://learn.microsoft.com/en-us/windows/win32/api/winternl/nf-winternl-ntqueryobject)
and [file identity notes](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/ns-fileapi-by_handle_file_information).
Deterministic injected query denial/PID reuse and actual cross-process death/
thread cancellation tests remain required. No native code ran locally.

Prepared actual Windows storage/identity fixture registry_windows_test.cc
uses real hardlinks/128-bit IDs, read-only/read-write handle rights, current
PID with mismatched birth and an actual exited thread. Real owned children
exit23 while holding the named mutex before/after publication; the parent
requires exact old bytes/generation or the complete new bank, then actual
dead-owner pruning. Explicit paired allocator dependency returns null to
check staging ENOMEM without mutating an existing active bank. Invalid
candidate count is rejected with unchanged bytes; injected Unknown probes
retain both process/OFD metadata. These are storage/dependency checks, not
actual fcntl/FD ENOMEM or OS-denied-query acceptance. Child wait/termination
is bounded and failure stops before another mutex wait. Source is pending
peer review/CI; no local compiler or fixture ran. Actual denied owner query,
cross-session/nonadministrator ACLs, corruption/concurrent publication and
all runtime fcntl/lifetime/signal integration remain open.

Metadata namespace source hardening now opens the authoritative ProgramData
directory and both Gesher/RecordLocks-v1 parents without delete sharing,
rejects directory reparses/non-directories, and retains all three handles
through metadata-file open and validation. The file's final normalized DOS
path must equal the path derived from the retained canonical parent handle;
the metadata file itself rejects reparse/directory attributes and stays open
without delete sharing. This closes the earlier parent check/open rename
window in the intended namespace. No native compiler or fixture ran locally;
bounded source peer review and actual Windows namespace/identity tests remain
pending. This does not qualify malicious metadata writers, cross-session
ACLs or descriptor/lock runtime lifetime behavior.

Prepared native3-locks-registry.yml is source-only until bounded independent
peer review. It compiles the actual registry/liveness/internal sources and
existing Windows fixture using the pinned, archive-hash-verified LLVM-mingw
toolchain, no AOSP/host build. Both Windows consumers require the same exact
repository/run/attempt/source artifact, all source/header and executable byte
hashes, an explicit system-DLL import allowlist and an external90s process-tree
bound with failure output preserved. Local YAML/Python/PowerShell source
parsing passed; no native compiler/fixture ran. Do not push the new narrow
trigger or dispatch until the source peer is complete. Its intended scope is
storage/identities only: partial inactive-bank-copy interruption, raw metadata
corruption, OS-denied queries, actual PID reuse, cross-session/nonadministrator
ACLs and every actual fcntl/descriptor/stdio/spawn/signal path remain open.

Storage/identity-only qualification is now actual green on both Windows at
exact ee71c7c5165717c1d431d802ad40967842d4127c, run40579579092 attempt1,
success3/3. Independent Windows peer reviewed the complete source and final
fixture child cleanup before push. Compile122455027650 built the actual
registry/liveness/internal sources with the production POSIX header overlay
and-Wall-Wextra-Werror; pinned archive hash and the explicit14-system-DLL
import allowlist passed. Windows2022 job122455211147 and Windows2025
job122455211119 each executed22 assertions with0 failures. Both verified
artifact12768333815, ZIPsha256
c51220c4010a1d7f36d1414c627f6f2ea2b64f2736b8360e6befae9ff0e0daaa,
and identical executable SHA256
50046d004168633cfa0fcac81c77585d72fb8e7d703f631d0716283a2ac574f8.
Final metadata and all three actual plain job logs are preserved under
`C:\work\gesher-ci-review\native3-registry-40579579092-*`; the required
watch exited0 and is no longer live.

Actual evidence includes hardlink/file identity, read-only GrantedAccess,
exited thread, real pre/postcommit child death while holding the named mutex,
authoritative old/new bank retention, confirmed dead-process pruning, invalid
candidate byte preservation and explicit allocator/Unknown-probe dependencies.
Fixture children are created suspended, assigned to a noninherited owned
kill-on-close job, then resumed; termination outcomes and finite final waits
are checked before handles are released. The Windows job contract is described
in [Job Objects](https://learn.microsoft.com/en-us/windows/win32/procthread/job-objects)
and [AssignProcessToJobObject](https://learn.microsoft.com/en-us/windows/win32/api/jobapi2/nf-jobapi2-assignprocesstojobobject).
No real OS allocation/permission denial or actual PID reuse is implied by the
explicit dependency controls. There is still no wired fcntl/close/dup/stdio/
spawn/signal implementation or runtime-lock acceptance; partial initialization/
inactive-copy corruption, contention, nonadministrator/cross-session behavior
and nonblocking metadata-acquisition policy remain required open work.

Nonblocking runtime integration is now SOURCE WIP, not compiled or executed:
record_lock_operation.{h,cc} snapshots the explicit32-byte LP64 flock through
checked process-memory copies, validates ranges and actual descriptor rights
before allocating ownership, then applies the kernel-compared interval core.
record_lock_descriptors.{h,cc} stages protected/noninherited FILE_OBJECT
anchors, local descriptor serialization and real post-CRT close/replacement
checks. Zero-user accounting compares actual kernel objects, counts exact
anchor/probe handles and rechecks EVERY anchor owner's creation/liveness after
the HandleCount snapshot; uncertainty retains ownership. File identity alone
never identifies an open description. Initial SameDescription controls and
these new modules remain unverified until independent source peer and CI.

Current routing WIP connects the four nonblocking commands to this engine,
consumes pointer varargs before the unchanged integer-command path, and hooks
close/dup/dup2, CRT descriptor publication and fclose/freopen. F_DUPFD's private
staging-alias close is explicitly distinguished from an application close;
another actual user alias must survive, so its cleanup cannot erase existing
POSIX locks. Stdio uses the CRT stream lock and documented _fclose_nolock,
then inspects whether the captured object was actually closed even on error.
No compilation or fixture has qualified these changes yet.

This WIP is incomplete: blocking bionic commands now return ENOSYS rather than
the old fake grant, and real waiter/deadlock/targeted-handler/SA_RESTART work
remains mandatory. Cross-process inherited-description adoption and atomic
suspended-spawn anchor registration are not implemented; do not qualify
inherited OFD lifetimes from the present source. Exec00c7/fec66 ordered-spawn
semantics must be preserved after coordination on the selected union; no
exec/spawn source was edited here. Common POSIX signal.cc limitations differ
from the existing bionic per-thread signal machinery; the latter should be
extended, not replaced by timeouts or a duplicate signal subsystem. Overall87
and coherent full/package/e2e acceptance remain open.

## 2026-10-28 source continuation (not runtime proof)

Local 21af6b3bb7ae8c871aae2ee6aa3fd2cb9081159b addresses three concrete
independent source-review findings: successful dup2 replacement between two
different descriptor numbers releases POSIX any-close locks even when both
handles name the same FILE_OBJECT; fclose/freopen acquire the real stream lock
before the descriptor/registry transaction; early failed freopen destination
or mode conversion closes the original stream through that single transaction.
Null-path freopen mode changes remain an explicitly unqualified existing case.
Final independent recheck and actual CI are pending.

Local 4fa2a6a63f4c4049ed278ac19b42a08a275276d3 includes the portable
command_worker.cc and command_oracle.py plus native3-locks-commands.yml.
The identical worker source calls actual Linux fcntl in the producer and the
actual common POSIX library in both Windows consumers. Two real independent
processes exercise same-description dup2, OFD alias retention, F_DUPFD temporary
cleanup, hardlink identity, advisory I/O, access/range errors, three failed
reopen destinations and four actual contended stream-close cycles. Actual
high-address mappings and READONLY/cross-page/no-access pointers check errno,
canaries and post-fault lock retention. Linux's unspecified partially copied
EFAULT structure fields are not treated as output values; valid/no-conflict
fields and real conflict owners are compared. Stable page checks before
WriteProcessMemory do not qualify concurrent protection-change races.

The proposed narrow workflow verifies the fresh toolchain archive hash, builds
the actual common library with warnings as errors, requires a nonempty explicit
system import set, and pins the Linux result/executable/source bytes to the
same successful repository/run/attempt artifact. Both Windows suites have an
external120s owned-process-tree bound, per-command10s controller bounds and
cleanup for every registered worker. All source-only Python/YAML/PowerShell AST
checks passed. Nothing in this continuation has been pushed, compiled or run;
there is no new run/watch. Windows owns the required final bounded source peer.
Do not use these planned controls as proof before actual logs exist.

Inherited OFD/atomic ordered-spawn integration, blocking waits, deadlock graphs,
targeted signals and handler restart semantics remain mandatory subsequent
work. The active integration full4a47 was untouched; its provenance-capture
failure is separate from this source branch. Integration/release ownership is
now /root/integration_next; prior integration agents are retired. Existing
2026-10-27 proof snapshots retain their historical date and identities.

Source self-review after this checkpoint removed the F_DUPFD staging alias
entirely: under DescriptorGuard it finds the lowest vacant descriptor at or
above the requested minimum, then uses the actual dup2 path directly. There
is no hidden temporary alias whose failed close can leak an OFD pin, and the
unused application-close bypass was removed. Success restores entry errno
after vacant CRT fd probes; failures retain the actual duplication error.
The portable oracle compares those results with actual Linux and checks a
negative minimum leaves existing POSIX locks intact. This delta is still
uncompiled and requires the same final source peer before any CI dispatch.

### Known pre-CI atomicity blocker (source68d26f1c)

Do not push/dispatch the prepared command workflow yet. Direct vacant-fd
search followed by dup2 is not atomic against publishers outside the current
DescriptorGuard. Source audit found unguarded common ReserveFd (fd.cc), pipe2
(unistd.cc), memfd_create (system.cc), Binder ReceiveFds, libcore/process.cc,
bionic/process_bionic.cc, raw _wfopen in ELF tools/media/native-font/FLAC code,
and bionic Tmpfile paths. This is a concrete destructive-replacement race,
not an accepted limitation. The final allocation/publication design and its
actual contention controls must resolve it before qualifying the slice.

Source review is considering a shared publication boundary across controlled
callers or linker wrapping with actual COFF reference proof. Simple function
wrapping alone cannot be assumed to catch dllimport references: canonical
[mingw-w64 io.h](https://raw.githubusercontent.com/mingw-w64/mingw-w64/master/mingw-w64-headers/crt/io.h)
marks raw fd publishers _CRTIMP, and [GNU ld wrapping](https://www.sourceware.org/binutils/docs/ld/Options.html#index-_002d_002dwrap_003dsymbol)
only replaces undefined references to the exact symbol. The frozen compiler
archive's actual headers/objects must decide any toolchain-specific claim.
No cross-edits to Binder/Exec/Media were made. Coordinate Binder changes with
/root/integration_next and preserve the selected ordered-spawn Exec leaf;
Windows owns the bounded independent peer before any production CI.

### 2026-10-28 resumed compile-only checkpoint

Resumed on actual branch wip/native3-locks at54f4604f. Three preserved dirty
files (same01:46:26 write time) continue the interrupted native worker's
pending raw-publisher/EMFILE fixture scope; their source was reviewed and
retained, not discarded. Internal reserved(fd) dependency pauses after a real
occupied NUL target exists; an actual unguarded CRT publisher must retain its
FILE_OBJECT witness and sentinel bytes. A further1000 actual F_DUPFD calls
race repeated raw publications. A real low-I/O table fill requires EMFILE,
all8192 slots occupied, retained source and complete cleanup. These fixtures
remain source-only. Main installs an explicit CRT invalid-parameter handler
so invalid-fd controls return errno rather than aborting. Microsoft's
[_get_osfhandle](https://learn.microsoft.com/en-us/cpp/c-runtime-library/reference/get-osfhandle)
requires this for invalid descriptors; the documented8192 low-I/O ceiling in
[_setmaxstdio](https://learn.microsoft.com/en-us/cpp/c-runtime-library/reference/setmaxstdio)
is independent of its512 default FILE-stream ceiling.

Bounded Windows peer found no concrete blocker in the new compile-only gate;
this is not final semantic peer approval. New native3-locks-compile.yml builds
the actual common POSIX and bionic sources and both native fixture targets
with warnings as errors and the pinned archive hash. No native fixture runs
and no artifact is downloaded locally. Old command and registry workflows
retain explicit manual dispatch but their push triggers are removed to avoid
running unreviewed runtime source or unrelated storage qualification when
pushing this compile-only checkpoint. Every existing runtime assertion is
preserved. The new compile push trigger is branch/path restricted. Local
Python/YAML source parsing and git diff --check passed; no local compilation
or native/JVM/C# fixture execution occurred.

No qualified nonblocking fcntl, descriptor, stdio or spawn evidence exists yet.
Historical interval40559255447 and storage40579579092 remain the only relevant
qualified slices. Atomic F_DUPFD_CLOEXEC versus CreateProcess, concurrent
page-protection TOCTOU, inherited OFD adoption, ordered suspended spawn,
blocking7/38, deadlock, targeted signal/EINTR/SA_RESTART, bionic cross-ABI,
ART and all full/package/e2e gates remain open. Source-only compile success
will not close any of them. Existing ordered Exec82d41 production semantics
must be preserved. Integration and release remain owned by the integrator.

Compile-only40655921479 at exact813cffa9c454d71105a78e0890b5a1b5721f32e2
failed in job122706814363. Required watch exited1; actual final metadata and
plain compile log are saved at C:\work\gesher-ci-review\native3-compile-40655921479-*.
Archive SHA256 validation succeeded; actual common POSIX/bionic source and
duplicate fixture objects compiled, but command_worker.cc70/71 could not find
WCT_MAX_NODE_COUNT in the pinned mingw header. Therefore the aggregate compile
gate failed and no runtime assertion executed. This is a concrete source
failure, not infrastructure retry authorization or completed compile proof.

Correction uses the canonical Microsoft WinSDK16-node constant from
https://github.com/microsoft/win32metadata/blob/main/generation/WinSDK/RecompiledIdlHeaders/um/wct.h
with a static_assert wherever the SDK macro exists. Real WCT success and the
closer->critical-section->owner assertion remain mandatory; oversized counts,
ERROR_MORE_DATA and ERROR_TOO_MANY_THREADS are diagnosed and cannot qualify a
truncated chain. The next compile uses this changed worker source. The narrow
compile push paths now include native3 fixture and record-lock source paths.

Prepared controlled runtime orchestration additionally builds both native
executables, checks each nonempty import allowlist and exactly five produced
output names/byte hashes. An optional portable controller argument then runs
thirteen fresh Windows duplicate fixture processes, each with a60s limit and
owned cleanup; actual mode completion/assertion counts and raw1000-duplicate
witness result are required. Windows parent has an external180s process-tree
bound and requires both actual suite summaries. Bounded Windows source peer
found no concrete false-positive blocker;180s is deliberately an overall
failure bound, not13 times60s. This orchestration is source-only and has not
been dispatched; final runtime peer and the corrected compile gate are still
prerequisites. No runtime assertions or mandatory open gates were removed.

### Corrected compile-only gate: actual success, no runtime proof

Exact7b61deab111c6eeb5108e19d2aed2b1acc135ba5 passed run40656771343,
job122709582665, success1/1 and required watch exit0. Actual plain logs show
330 steps completing the common POSIX and bionic libraries and linking both
fixture executables with the pinned toolchain archive checksum verified.
Actual command executable SHA256:
3e46a0b8f741568003a868229871bc8460a066a1f3aa16a8a496f33b9ccf44c2.
Actual duplicate executable SHA256:
bdc872de2751ea70cf5455d686c025573d0bbebfb91b13801ba8123e32157dbc.
The actual completion line is
`COMPILE_ONLY source=7b61deab111c6eeb5108e19d2aed2b1acc135ba5 runtime=NOT_EXECUTED semantics=UNQUALIFIED`.
Final metadata, watch and actual compile log are preserved in
C:\work\gesher-ci-review\native3-compile-40656771343-*; no CI artifact was
downloaded locally. The prior40655921479 red result remains documented above.

Independent final pointer/lifetime source review is assigned to the sole
integrator after its separate Binder checkpoint. Earlier Windows checks were
only bounded compile-workflow and orchestration reviews. Runtime dispatch
still waits for final production-path review and resolution of any concrete
blocker. No common fcntl/dup/stdio semantics have yet been runtime-qualified.

Prepared runtime gate requires exactly13 fresh processes/modes:
regular/device/event/socket/binder, retained-nul, quarantine of each of those
five resource kinds, raw-race and capacity. Every actual mode must exit0,
emit exactly one completion with assertions>0/failures0 and preserve real
resource checks. Per-mode actual counts must be retained and audited; the
capacity allocation count and explicit raw fd-reuse staging branch can vary
between hosts. The raw publisher result separately requires1000 duplicates,
at least one actual raw publication and zero errors. These are explicit
internal failure dependencies using actual resources, not proof of real OS
permission/close denial. Same-source Linux/Windows command rows are compared
exactly using genuine independent processes. Controller bounds are60s/mode,
10s/command,5s owned cleanup,180s Windows suite/process-tree bound. Every
mandatory gate and copy-limit caveat listed above remains open.

### Independent peer complete; controlled runtime gate prepared

Sole integrator completed the final selected pointer/layout/lifetime review
at exact7b61deab, independently matching14 Git blobs and SHA256. No new
definite blocker was found in the bounded source paths. Review and hashes:
C:\work\gesher-ci-review\native3-final-pointer-lifetime-peer-7b61deab-2026-10-28.md
and companion -hashes.json. This satisfies the ADR0080 source-peer gate only.
All source/runtime limitations above remain explicit.

A subsequent bounded Python/YAML delta peer matched controller blob
5c7a976217303dc80a782df9e3b9cb1e96912050 and workflow blob
1f749b1186c2818cf1b0d83639759ad5d2fa5e6c. The controller now requires unique
actual PASS labels for mandatory mode-specific case IDs, including actual
FD reuse, quarantine/live virtual resources, genuine8192-slot EMFILE and
cleanup; positive assertion counts alone cannot qualify a missing case.
Naturally reused versus explicitly staged reuse both exercise the actual
same target fd, and the preserved real sentinel read must pass. The raw
publisher still requires1000 duplicates, positive publications and zero
errors. The Windows180s external deadline does not reset for each60s mode.

Only the command workflow's own branch/file push trigger is re-enabled for
this controlled component; it builds fresh Linux and Windows executables,
executes the actual Linux reference, pins the new exact source/run/attempt
and five output hashes, and requires actual execution on both Windows.
No compile-only artifact is reused as runtime proof. The native C/C++ files
remain identical to reviewed/compiled7b61deab. Component results must still
be audited from actual final plain logs before any bounded qualification.
No blocker/source requirement is waived and no inventory gap is closed.

### First actual command component: Linux reference timeout, Windows skipped

Exact688ef531daf4bfe7a59a8f04b1b0d59b246ec8f5 ran component40658079598.
Required watch exited1. Producer job122713924879 failed during the actual
Linux syscall reference before toolchain fetching or Windows compilation;
Windows was skipped, so there is no new Windows runtime proof. Actual plain
log contains124 command rows, then the first contended_stdio0/1 exceeded the
unchanged10s command bound. Cleanup attempted every registered worker and
killed the owned hung worker; final metadata/plain log/watch are preserved
at C:\work\gesher-ci-review\native3-commands-40658079598-*.
The separately queued compile-only40658079560 was cancelled as redundant
with this component's intended fresh producer compilation; it is not proof.

Current fixture hypothesis, not a proven PC/stack diagnosis: the Linux wchan
observer's fopen/fclose may itself wait on libc's global FILE-list lock while
the other thread's fclose waits for our held stream. Prepared correction uses
raw open/read/close for that proc read, still requiring the actual closer's
kernel futex wait. It adds actual closer TID/wchan/raw-observer and phase
output before/after independent close and stream unlock. The held-stream
ordering, actual close/query assertions,5s internal and10s external command
deadlines remain unchanged; no timeout or missing contention becomes PASS.
Windows WCT behavior and production code are unchanged. Focused independent
fixture peer is required before a changed-source component run. The next
actual phase evidence must determine how far the observer proceeds; the
previous timeout alone does not establish its root cause.

Focused independent observer peer passed source blob33266879f0d64a4bdbbdb40eee33066c9c095267: actual futex/state evidence, checked NUL-terminated raw read, held-stream order and unchanged deadlines/errno capture preserved. Changed-source retry is justified; no prior timeout root-cause certainty follows.

### Second actual component: Linux completed, two concrete Windows failures

Exact2b244f7fe7424dac35145f2e8abce93e070c211e ran40659198571. Required
watch exited1; final metadata and all three actual plain logs are preserved
at C:\work\gesher-ci-review\native3-commands-40659198571-*.
Producer122717662593 succeeded: actualLinux150 rows/mismatches0 and four
real closer futex_do_wait observations/raw-observer+close/unlock phases,
fresh Windows executables, checksum, nonempty allowlisted imports and output
provenance all completed. This supports the corrected observer proceeding;
it is not a stack-level proof of the previous timeout's precise cause.
Artifact12802693004 ZIPsha256
a8606ceec4971a976f8a21fc081d7690327a5fb18bcb9f5a9cdbb682d677f006
was verified/consumed within CI, never downloaded locally.

Windows2025 job122718193447 passed provenance then failed before process
start: Get-Command python returned multiple applications, so Source was an
Object[] rejected by Start-Process FilePath. Windows2022 job122718193550
actually executed150 rows and four successful actual WCT contention cycles.
Independent local parsing of the two actual plain logs finds exactly one
row difference, index109 freopen0/3: Linux ENOTDIR20 versus Windows ENOENT2
for a child below an existing regular file. The strict comparator failed.
Neither Windows reached the13 duplicate modes and the component is failed;
there is no bounded Windows semantic qualification or full acceptance.

Prepared production fix is deliberately narrow: only after a real failed
_wfreopen with CRT ENOENT, examine the already-mapped UTF-16 ancestors. A
confirmed existing non-directory ancestor maps to ENOTDIR; an existing
directory or failure to establish such an ancestor leaves ENOENT intact.
The actual CRT reopen, original stream close and transaction.Finish ordering
are unchanged. Added reference controls cover an absent disk parent retaining
ENOENT and an actual successful Unicode same-path reopen/any-close. Main file
has Hebrew/CJK characters; the Windows fixture obtains its true wide command
line and converts paths to UTF-8 instead of trusting ANSI argv replacement.
The workflow selects one actual Python Application, validates string/.exe/
leaf and runs its version check before Start-Process. Existing deadlines,
all previous actual rows and13-mode requirements remain unchanged. This new
production/fixture/workflow delta needs its own focused independent source
peer before any changed-source CI; the earlier7b61 peer does not cover it.
The auto compile-only40658999459 was explicitly cancelled as superseded by
the fresh actual component producer; it is not qualification evidence.

### Third command component: real Windows evidence, suite still failed

Exact1dc2e24587130997d562054b16c2edd6a5b30ed0 ran40660954505; required
watch exited1. Final metadata/plain producer122723551074 and Windows2022
122724226864 / Windows2025 122724226871 logs are saved under
C:\work\gesher-ci-review\native3-commands-40660954505-*.
Actual Linux completed161 rows/mismatches0, including the Unicode successful
reopen/any-close and genuine missing-disk control. Windows2022 completed the
same161 rows with zero actual row differences, then real resource duplicate
modes regular/device/event/socket passed23/17/17/21 assertions respectively.
Binder mode then correctly failed to open an actual source: this isolated
component had not started/configured its own real broker. Later duplicate
modes were not reached. Windows2025 selected one real Python executable and
its first135 rows matched the Linux prefix, then the first actual stdio
contention request returned contended=false despite successful actual closes.
The strict assertion failed. Neither suite is accepted and no mandatory gap
is closed; the previous partial observations remain scoped evidence only.

Prepared continuation changes fixture/controller/workflow only; production
C/C++ remains identical to focused-peer1dc2e245. The actual WCT criterion is
unchanged: full closer->critical-section->owner chain is still required.
New diagnostics snapshot the last real query success/error/count/cycle and
node type/status/PID/TID, plus actual FILE pointer/fd/HANDLE/owner/closer and
completed-before-unlock. Five-second internal/ten-second command budgets and
held-stream/independent-close/unlock ordering remain unchanged. Missing WCT
is still failure; no sleep, timeout or inferred wall-time contention is PASS.

The fresh producer additionally builds/import-checks/hashes actual binderd.exe
and its import list (seven exact output members). The duplicate controller
creates an isolated UUID endpoint and child-only legitimate GESHER_BINDER
selection, requires the actual pipe-created broker message, existing real
Binder ioctls and exactly two actual broker opens matching the two known
Binder fixture PIDs. All13 direct client handles must report normal exit0
before a healthy broker's intentional owned termination/five-second reap.
Binderd is designed to serve until killed: its teardown exit is reported
explicitly and never called normal broker exit0. An unnamed noninherited
KILL_ON_JOB_CLOSE job owns exact Popen kernel process handles and performs
shared-five-second reap of owned children on close; failure teardown cannot
qualify client/broker success. No runner-wide name kill is used. The fixtures
spawn no children before job assignment; general atomic spawn remains open.
Command failure diagnostics are preserved while the independent duplicate
suite is also attempted; aggregate failure remains nonzero and the absolute
external180s root/process-tree bound is unchanged.

Strict archive utilities are byte-identical imports from reviewed21441571:
Git blobs c12018f78db23e7696298abc864ce1c4d9d788e8 (download),
8d01e5084c8b0aac7f06bc7eae75cf14e431f5b2 (assembly),
a97840fd9760ed65f8b317621266e67e8092f10f (negative controls), under
tools/native3-locks/artifact/. A cheap CI preflight checks these canonical
blobs and nine adversarial controls before producer compilation. Windows
validates actual successful current producer/run/attempt/repository/SHA and
artifact creation time first, then authenticates raw ZIP SHA256 before any
extraction and verifies exact source/output members before execution. No
native fixture or these CI-only controls ran locally; Python/YAML/embedded
PowerShell source parsing and diff checks passed. This continuation requires
its own independent fixture/ownership/workflow source peer before dispatch.
All blocking7/38, inherited-OFD/atomic-spawn, targeted signals, ABI/ART,
concurrent copy/protection, full/package/e2e and wider requirements remain open.

### Diagnostic/owned-broker component dispatched after independent source peer

Exact sourcef6b1fbfa4f452efb18595b2314045f3ce0b822b0 is pushed and running
component40667374462. Required120s watch is active; its local output is
C:\work\gesher-ci-review\native3-commands-40667374462.watch.log. No current
runtime result is claimed. Before pushing, all prior own-ref runs were
terminal. Redundant auto compile-only40667374463 was cancelled because the
component freshly compiles its own actual consumers and broker.

Independent selected fixture/ownership/workflow review passed31fa4e68, with
a concrete healthy-stop race found: Popen.terminate can hide ACCESS_DENIED
when a process exits concurrently. Final f6b1fbfa replaces that call with a
typed BOOL-returning TerminateProcess on the exact owned Popen kernel handle;
the intentional-kill flag is set only after actual success and the actual
reaped exit must be211. The sole final delta received focused peer approval.
This remains intentional broker teardown, never normal broker exit0 proof.
All13 actual client normal exits/PID and broker-open audits remain mandatory.
Production C/C++ is unchanged from independently reviewed1dc2e245. WCT
criterion,5s/10s/180s deadlines and all original assertions remain strict.

The imported21441571 helper source has a newly documented selected validation
limit: its ASCII reserved-device regex misses Windows' superscript COM/LPT
names (Microsoft Naming Files conventions). Terra owns the canonical fix and
its new pre-extraction control. The current actual producer emits the known
ASCII consumers/imports/reference/provenance members; strict approved raw ZIP
bytes and exact seven output names/hashes are still required before use.
The nine existing negative controls do not qualify every reserved Unicode
alias. Do not claim universal ZIP alias validation from this component or
silently replace the pinned helper blobs without a new source/peer record.
All prior failed runs, partial observations and mandatory open runtime/full
requirements remain preserved above. No inventory checkbox changed.

### Actual f6b1fbfa component audit: both resource suites pass; Windows2025 WCT remains red

Run40667374462 is terminal FAILURE, required watch exit1. Actual metadata
and all four plain logs plus an independent parsed audit JSON are retained
under C:\work\gesher-ci-review\native3-commands-40667374462-*.
Preflight122745173486 succeeded with all three canonical214 helper blobs
and nine actual negative controls. Producer122745251955 succeeded with actual
Linux161 rows/mismatches0 and fresh consumers/broker/import gates.
Both Windows verified actual raw artifact12805869341 ZIPsha256
ce7c9f179f4f50998636eb40040162c4dacc1abfae787986071205f27478015c
before extraction, then source/run/attempt/seven output hashes. Actual worker
SHA256713d79cdc46204bf1c6dcfd42f21af0812508152786db07a07721b52e60d75ff
and Linux reference SHA2563080bbfab5e57a197194217e8900527c6bef527fa30da39f2366d1b9e58ad0f1
were logged identically on both hosts. No artifact was downloaded locally.

Windows2022 job122745893505 succeeded: all161 actual rows match Linux and
four actual WCT chains have count3, blocked closer->owned critical section->
running owner, with completed-before-unlock0. Windows2025 job122745893366
failed the original strict command assertion after135 matching prefix rows.
Its final WCT query after341 samples succeeded/error0 but returned count1,
only type8 Thread/status3 Blocked for closer7404; owner3628 and actual FILE/
fd/HANDLE were recorded, completed-before-unlock0. Independent close, unlock
and closer completion returned0. The provider did not expose a critical
section/owner. These facts neither prove the required chain nor establish
a production close bug or its PC/stack. Actual closer stack/PC/CRT module
version is the meaningful next diagnostic; unchanged reruns or a weakened
wait-chain predicate are not authorized by this result.

Both hosts nevertheless completed the independently attempted actual13-mode
duplicate suite with0 failures. Exact assertion counts in mode order
regular/device/event/socket/binder/retained-nul/quarantine-regular/device/
event/socket/binder/raw-race/capacity are
23/17/17/21/17/15/16/16/17/17/17/9/8197 (8399 each host).
Both require real FD reuse/resource checks/quarantine and actual8192-slot
EMFILE/capacity cleanup. Each completed1000 duplicate operations/errors0;
actual simultaneous raw publications were2574 (2022) and3550 (2025).
Both actual isolated brokers recorded exactly two opens matching the two
known Binder fixture PIDs, thirteen normal client exits0, intentional checked
broker termination/exit211 and fourteen job children reaped. Required case
IDs and actual control completion lines were independently parsed from plain
logs; this is bounded resource-control evidence, not real OS denial or all
fcntl/spawn acceptance. The overall component remains UNQUALIFIED because
Windows2025's required actual chain is missing; no inventory gap is closed.
All inherited-OFD/atomic-spawn/blocking/signal/ABI/ART/copy-race/full/package/
e2e requirements and the214 helper's known reserved-alias limit remain open.

### Prepared owned closer PC/unwind diagnostic (source only)

The next diagnostic reuses the established ThreadWatch Unwind/Describe
pattern from qualified shared source blob
e65d1789f8ca8be3a72e2dfffdc9376a78d0a532, copied only into the private
native3 fixture helper. It opens only the closer TID and requires this live
process plus the closer's actual published creation FILETIME before suspend.
While suspended it collects a fixed32-PC CONTEXT/RtlLookupFunctionEntry/
RtlVirtualUnwind trace, using guarded process reads for leaf return addresses.
Resume is checked before any module/symbol/stdio reporting; resume failure
reports through raw Win32 output and terminates this owned worker with95.
Suspend/context/identity/handle failures fail this diagnostic, never produce
a fabricated frame. Addresses are labeled nearest-export hints plus actual
PC/module RVAs and actual module paths, not presumed exact private symbols.

The probe runs at most once per contention request after three unsuccessful
actual WCT samples, while the same original owner-held five-second deadline
keeps running. It does not reset a timer or replace the full actual WCT
criterion. The main fixture still requires the original real chain; an
unproved Windows2025 chain remains failure regardless of captured PCs.
System32 ucrtbase/kernelbase/ntdll file versions and byte hashes are collected
in the CI parent before starting the worker, outside the suspended section
and stream-lock deadline, for comparison with actual captured module paths.
All seven authenticated output hashes will also be printed after validation.
No new runtime pass, equivalent-proof exception or ADR acceptance is claimed.

Two helper files are updated by exact byte import from reviewed canonical
977be820/1ac73c5e source: download blob
cd5cc415e53102c664edac22b1604df639c8804b and negative-control blob
af7324581ae011f39aacf87d81ae1f44ad4c85f6. Assembly remains8d01e508.
The cheap canonical helper preflight now requires the six additional COM/LPT
superscript-name controls (15 total), including a valid first member and no
destination creation when the later reserved member is rejected. This is
prepared source, not retroactive expansion of f6b1fbfa's nine-control proof.
No production C/C++ changed from1dc2e245 and no fixture ran locally. Python,
YAML, embedded PowerShell source parsing and diff checks precede independent
review of this exact diagnostic/source delta before any CI dispatch.

### Actual 4f8ad228 stack diagnostic: component remains failed

Run40674284726 is terminal FAILURE (required120s watch exit1). Actual metadata,
preflight, producer and both Windows plain logs are retained at
C:/work/gesher-ci-review/native3-commands-40674284726-*; no local artifact.
Preflight122768615427 passed canonical helper hashes and all15 actual controls.
Producer122768663211 passed with Linux161 rows/mismatches0. Windows2022
122769230303 passed all161 rows and four actual full WCT chains. Windows2025
122769230252 failed the original chain assertion after the matching135-row
prefix:324 successful WCT queries still expose only one blocked thread node.
The actual closer did not complete before owner unlock. Both Windows still
completed all13 duplicate modes/8399 assertions/0 failures,1000 actual duplicate
operations/0 errors with2565 (2022) and3580 (2025) concurrent raw publications;
real broker clients/controlled211 stop/job reap remain bounded passing slices.
None substitutes for the missing required Windows2025 actual owner chain.

The checked suspend/resume diagnostic captured the actual owned closer3752,
pid6116, birth134376578797745013, FILE0000019450b75d40, fd3, nine PCs, original
suspend count0 and resume count1. Its exact worker return RVAs are0x9836 and
0x3a02. The System32 ntdll/ucrtbase module addresses, paths, versions and byte
hashes are recorded in the actual job log. Nearest-export strings are hints,
not private-symbol, critical-section-identity or ownership proof. All runtime
qualification and inherited-OFD/atomic-spawn/blocking/signals/ABI/ART/copy-race/
full/package/e2e requirements remain open; no inventory checkbox changed.

### Prepared exact-original-binary symbolization, no runtime retry

The next separate narrow workflow will decode the existing archived worker,
without compiling or executing it. It binds original source4f8ad228, failed
run40674284726/attempt1 but successful compile job122768663211, artifact12809029365,
ZIPsha2563c91e9cb37f3d6ac5df3cb7c572a5a45559d774eeb02df91250a16c71f3c1e4b
and exact workerSHA25610c93728553e8ed90cb5d89450c8b090ad392e98761f26a1ee273ad70af29734.
Current API repository/run/source/attempt/job/upload/artifact timestamps are
checked before the reviewed strict downloader authenticates the raw archive.
The exact eight-member set, original whole tracked-source manifest, original
compiler archive digest and all seven prior output byte hashes are mandatory.
The checksum-pinned original compiler tools then read PE image base, inline
DWARF and nearby actual instructions at both observed RVAs and return-PC minus1.
Unknown mappings fail this diagnostic. No locally downloaded binary/map is used.

This is a new static diagnostic of the SAME produced binary, not an unchanged
runtime retry or acceptance. Only its actual output can support the next
lock-identity/owner/same-held-FILE causal diagnostic. The original WCT criterion
and5s/10s/180s budgets remain unchanged. Source parsing/diff checks only ran
locally; this workflow and script require independent focused review before push.

### Original-binary symbolization40683188044: stripped DWARF, preserved failure

Sourcea6671d73/run40683188044 is terminal FAILURE (watch exit1). Exact prior
producer/archive/whole-source/eight-member/seven-output gates all passed; the
actual archived worker has PointerToSymbolTable0, SymbolCount0 and DebugRVA0.
CMake's existing GESHER_STRIP defaultON removes executable symbols. All four
DWARF lookups returned unknown and the explicit unresolved gate failed.
Plain log and final metadata: C:/work/gesher-ci-review/native3-symbolize-40683188044*.
No private symbol claim is made. Original exact disassembly nevertheless shows
RVA0x39fd direct call to0x9820 returning0x3a02, and RVA0x9830 indirect call through
IAT slot0xc42a0 returning0x9836. Neither fact alone identifies lock owner/data.

The next changed static diagnostic retains original DWARF-failure mode and
adds a separately labeled IAT mode. It authenticates the SAME old artifact,
reads its PE section table and actual two call encodings, then requires one
named import-table entry at the exact computed IAT slot. It makes no private
symbol/lock-owner/runtime acceptance claim and executes/rebuilds no worker.
Only the original source/ZIP/output bytes are consumed in CI. A later map
recovery, if needed, must require full rebuilt executable SHA equality to the
original before consuming any map; the original PE also embeds a link timestamp,
so default current-time relinking is not byte-identical. No such map is prepared
or accepted now. No original runtime requirement/deadline has changed.

### Actual original IAT diagnostic40684287454: named CRT call only

Sourcec2ae549c/run40684287454 succeeded1/1 (required watch exit0). All original
source4f8/failed run40674284726/successful compile122768663211 provenance, raw ZIP,
whole original source and seven byte-hash gates passed again. Exact archived
worker instruction bytes e81e5e0000 atRVA0x39fd call0x9820/return0x3a02;
ff156aaa0b00 atRVA0x9830 callIAT0xc42a0/return0x9836. That exact import-table
slot is api-ms-win-crt-filesystem-l1-1-0.dll!_lock_file. This is direct named
import evidence from the SAME worker, not nearest-export inference or private
symbol/critical-section address/owner proof. Source peer required all Symbol
lines fullmatch1:1 before IAT indexing and AMD64/PE32+ checks before stride8.
Plain logs/metadata: C:/work/gesher-ci-review/native3-symbolize-40684287454*.
OriginalDWARF40683188044 remains failed; actual runtime40674284726 remains failed.

The next separate CI-only diagnostic resolves named _lock_file/_unlock_file
from an explicitly System32-loaded CRT on each real Windows runner, retains
its module lifetime, resolves each actual owning module by address and records
file version/SHA256. Guarded ReadProcessMemory must match the exact file-backed
128 bytes at each export RVA before checksum-pinned llvm-objdump prints its
actual implementation. The Windows LLVM release archive317492c456aa27ee607a5919f1d2d38dcdc1112516a24d0bf4b00d078f52d17a
was obtained from official20261006 release metadata, not an unverified tool.
No private FILE/critical-section offset is assumed, no lock is acquired and no
owner/lifetime acceptance is claimed. This collects implementation evidence
before designing actual CS/OwningThread/same-FILE causal observation. A changed
module hash cannot be relabeled as the prior2025 module. No Native component
criterion/deadline changed. Local checks were source AST/YAML/diff parsing only;
independent focused peer is required before this separate diagnostic CI push.

### Actual named CRT implementation40686447043: dynamic layout basis only

Source73f27c91/run40686447043 succeeded2/2: Windows2025 job122809898579,
Windows2022 job122809898830; required watch exit0. Actual modules match the
prior runtime versions/hashes:2025 ucrtbase10.0.26100.33648/
9601850e2ede2cd86b441223557cae0fa7f5e33b1dd9d481c58c8cc2d688bdc3,
2022 ucrtbase10.0.20348.5756/
d06d0a421398fb663c6c49586e09774143460fdb1dce489aa9844846b9398555.
Guarded loaded128-byte reads matched exact file bytes and owned disassembly
snapshots. The actual named exports on BOTH versions consist first of
48 83 c1 30 (add rcx,0x30), then48 ff25 +signedrel32 (indirect tail jump).
Actual2025 lockRVA0x6e610/IAT0xfcad8; unlockRVA0x715d0/IAT0xfcaf0.
Actual2022 lockRVA0x24280/IAT0xc7a60; unlockRVA0x4770/IAT0xc7a58.
No lock owner or Native runtime acceptance follows from this static evidence.
Plain logs/metadata: C:/work/gesher-ci-review/native3-crt-40686447043*.

Prepared next fixture-only data diagnostic decodes the observed exact eleven
instruction bytes at the ACTUAL public worker _lock_file/_unlock_file exports,
checks same retained CRT module/IAT allocation and actual resolved jump targets
against named EnterCriticalSection/LeaveCriticalSection, and requires matching
positive decoded offsets. Unsupported code fails rather than guessing FILE ABI.
CRITICAL_SECTION size/field offsets are checked against the compiler's Windows
header. Guarded reads require aligned same-allocation committed FILE/lock range.
The exact same live FILE is sampled before owner lock (owner0/recursion0), after
actual _lock_file (actual current ownerTID/creation-time/recursion1), and while the
owned actual closer is waiting (sameCS/sameowner/recursion1/notcompleted). LockCount
and DebugInfo are recorded only, without assuming modern-version wait-count or
debug-list semantics. No FILE/CS read occurs after owner unlock permits fclose.
Existing actual closer birth/stack and independent close/unlock/completion
outputs remain. The actual WCT criterion and5s/10s/180s deadlines are unchanged,
so a Windows2025 one-node chain remains failure even if all CS data is obtained.

Production code remains1dc2e245; no acceptance change or equivalent-proof ADR
exception is made. This new source must receive focused pointer/lifetime/decode
peer before fresh component CI. Local activity was source/YAML/diff checking
only, with no C++ build or native fixture execution. All broader open requirements
remain unchanged, including both ABI/bionic/ART and full/package/e2e acceptance.

The final selected stack witness additionally retains fixed32 unwound RSI values
beside PCs while suspended. After the already checked balanced resume, it accepts
one actual worker-image return site only when guarded bytes match the observed
movRSI,RCX followed immediately by call[IATrel32], the checked worker-image IAT
resolves the actual public _lock_file, and that frame's restored nonvolatile RSI
is the exact same FILE pointer. The preceding original4f8 disassembly established
this instruction shape; unsupported new code or missing/ambiguous witness fails
the diagnostic. No generic RSI/nearest-export inference is used. Code inspection
and reporting happen after resume; no additional API/logging runs suspended.
This ties the actual closer's call path to its FILE argument, alongside the new
liveCS snapshots. It still does not replace the mandatory original WCT criterion.

### Actual708 CS/call-argument component40689909664 remains FAILURE

Required watch exit1, preflight122821765909/producer122821816441/Windows2022
122822731394 success, Windows2025 122822731309 failure. Actual Linux161 rows,
2022 all161/mismatches0 and four original full WCT chains. Windows2025 has
135 matching Linux prefix rows, then the unchanged actual chain assertion
fails:321 successful WCT samples return only its one Blocked thread node.
Both actual duplicate suites nevertheless finish all13 modes/8399 assertions/
0 failures,1000 duplicate operations/errors0 and concurrent raw publications
2690 (2022)/3249 (2025). Actual broker/client211-stop/job cleanup remains checked.

Actual producer artifact12817220700 ZIPsha256
ba34a2440d86ef1db68cd8d0c7f7f660b44528fb86b46ab8d25fe4ae49296135,
workerSHA25616c759b251012a5b5e67abcd25fb28a316f79a666ce3c98fe4f4e242b440c16a,
referenceSHA2563080bbfab5e57a197194217e8900527c6bef527fa30da39f2366d1b9e58ad0f1.
Strict rawZIP/currentproducer/source/attempt/eightmembers/sevenoutputs and15
negative helper controls pass. Saved final metadata/four plain job logs and
independent raw-row/data audit JSON: C:/work/gesher-ci-review/native3-commands-40689909664-*.
Some parallel gh run view log reads returned localcache EOF/invalidZIP; those
plain-log reads were recovered directly with the job-logs API. No CI rerun
or local artifact download occurred.

New actual2025 CS and closer-argument evidence is detailed in ADR0080's proposed
equivalence section. Actual FILE1a4148964c0, CS1a4148964f0, owner8384 with nonzero
birth and recursion1 held/waiting; actual owned closer748 has one checked
worker call site to actual _lock_file with restored RSI==sameFILE. Independent
close0 while closerfalse, real unlock then actualfclose0. The WCT-only boolean
is still false and causes the correct current red result. Windows2022 records
12 before/held/waiting CS snapshots alongside its four full chains. No unknown
LockCount/DebugInfo meaning or provider root cause is inferred.

A DOCS-ONLY ADR/test-intent proposal is prepared for independent source+actual
evidence review AFTER the coordinator's full acceptance/merge audit. It requires
same liveFILE/decodedactualCS, owned ownerTID+birth/recursion, actualcloser call+
restoredargument, actualBlockedThread, preunlocknoncompletion/independentclose0,
realunlock/fclose0, exact lifetimes, contradiction failure and unchanged clocks.
No acceptance predicate has changed; a further exact Enter frame-bound witness
and rejection controls are explicitly proposed for review rather than claimed.
All prior red runs and all broader Native3/integration requirements remain open.

### Mandatory all-four-cycle witness and rejection controls prepared (diagnostic only)

Independent e25 proposal+actual406899 review confirmed the recorded CS/owner/
closer/argument/release evidence but did not approve an alternative predicate.
Its required next evidence is now prepared: actual named EnterCriticalSection
RUNTIME_FUNCTION-bound captured frame; complete strong witness on all four real
cycles on BOTH Windows; every phase through real thread exit/join under the
SAME five-second absolute deadline; meaningful rejection controls and sticky
failure for any observed WCT API error, cycle or contradictory identity/owner.

The captured frame must lie inside the actual named Enter's checked unwind
range, alongside the existing exact _lock_file call/restored-FILE witness.
Sampling occurs early after three real queries even on Windows2022 full-WCT
success, then independent close/unlock/real kernel-signaled closer exit and
join finish before the original deadline. Monotonic start/held/sample/close/
unlock/exit/control-cleanup timestamps retain one deadline per real cycle.
No additional API or logging runs suspended. FILE/CS are never read after
unlock permits fclose/free. Owned thread exit handles verify ownPID/birth/live
identity before use and remain held until actual signaled exit/CloseHandle.

Real per-cycle controls create a distinct live sibling FILE held by the SAME
owner/recursion; its actual CS cannot satisfy the target FILE/CS or captured
argument. Wrong-owner uses the actual live closer TID; wrong-birth uses the
actual owner's different birth for the owned closer. A separate real temporary
FILE closer is birth-verified, released, actually fclose0/kernel-signaled/joined
before the target contention; its genuine completed flag must be rejected by
the completion gate. This negative record is explicitly a different control,
never a claim that the target closer completed while its lock was held.
Unsupported-prefix rejection mutates only an owned copy of the actual export
bytes. WCT error/cycle/wrong-thread copied-metadata rejection is labeled as
copied metadata, NOT an exercised OS deadlock/cycle or successful mocked wait.
Every actual WCT error/cycle/contradiction is sticky and fails the worker.

An independent CI stderr validator requires four complete groups, each with
all native semantic case IDs, decoded layout, before/held/wait/sibling fields,
owned thread/call/Enter frames and all seven monotonic phase records before
the unchanged deadline. It crosschecks actual values, not just positive counts.
Its marker explicitly says runtime_acceptance=false. Native controls and this
CI orchestration have not run locally; only AST/YAML/embeddedPowerShell/diff
source parsing ran. A fresh exact source peer is mandatory before CI push.

The ORIGINAL acceptance boolean remains full-WCT-only. The command controller
now defers (rather than removes) contention failure so all four actual cycles
and all161 Linux contract rows can be saved. It requires161 rows, compares each
row strictly, prints every mismatch, preserves contention failures, and cannot
print successful completion or exit0 with any such failure. Windows2025's four
false WCT contention rows are therefore expected to remain RED while collecting
all required evidence. Linux reference failures also remain failures. The13
actual duplicate modes, broker/job cleanup, raw publisher/capacity gates and
10s/180s bounds remain. No acceptance predicate/conjunction is adopted yet.
Production stays1dc2e245; all previous red runs and wider gaps remain open.

### Actual mandatory witness run40711407869: all groups observed, WCT-only failure preserved

Exact source65c83f7e86d384e372b7a652c390144f7c8d9f35 was pushed through a
per-command gh credential helper after immediate palmoni5 identity verification.
No global credential configuration, other stored identity or SSH was used.
The redundant own compile40711407824 was cancelled after the same account check;
the actual component freshly built its consumers. Run40711407869 is terminal
FAILURE (120s watch exit1): preflight122897374770/compile122897460747/Windows2022
122898293547 succeeded, Windows2025 122898294341 failed. All plain logs, final
metadata and independent raw-log audit JSON are saved at
C:/work/gesher-ci-review/native3-commands-40711407869-*; no local artifact.

Both hosts actually completed four mandatory evidence groups, with exact named
EnterCriticalSection RUNTIME_FUNCTION-bound frames, matching _lock_file return
instruction/IAT/restored FILE argument, decoded live CS/ownerTID/birth/recursion,
held same-owner sibling FILE, six real-resource/identity rejection controls,
explicitly labeled copied-WCT metadata negatives, independentclose0/preunlock
noncompletion, real unlock/fclose0 and kernel-signaled thread exit/join. All
seven phase timestamps used one original5000ms deadline per cycle. Actual total
cycle milliseconds:2022 1172/1000/1016/953;2025 641/375/375/375. Both independent
CI validators completed cycles4/runtime_acceptance=false. No OS deadlock/cycle
or real unsupported system-code modification is claimed by copied negatives.

The actual Linux reference has161 rows. Windows2022 matches all161 with four
full WCT chains. Windows2025 now saves all161 rows and differs ONLY at indices
135/140/145/150: contended=false because the unchanged original WCT-only rule
still sees one actual Blocked closer node rather than its owner's full chain.
All actual close and stream results are0; every other row matches the actual
Linux result. The strict controller prints four contention failures/four row
mismatches and exits nonzero. No missing-chain acceptance predicate was adopted.

Both independently attempted duplicate suites completed13 actual modes/8399
assertions/0 failures; actual1000 duplicate operations/errors0 with concurrent
raw publications2604 (2022)/5494 (2025). Checked actual broker/client211-stop/job
cleanup remains passing within its bounded resource-control scope. Producer
artifact12825768421 ZIPsha256f06b9fd243b5afb71f96f7dd729606bee5ecfbd598a3ddff45401ed6d6ae5de3;
actual workerSHA256a135a930826dd504133ddb3e0ce917c5dd900957289a985076585e6cbc5812ac.
Current source/producer/attempt/rawZIP/wholemanifest/sevenoutputs and all15 helper
negative controls passed. Exact65 source peer is saved separately as
native3-65c83f7e-selected-source-peer.md and hashes JSON.

Independent review of this actual evidence and the ADR test-intent proposal is
requested BEFORE any acceptance predicate change. Actual all-four-cycle evidence
is now available for that review, but this failed run is not retroactively green.
Production remains1dc2e245 and all inheritedOFD/atomicspawn/blocking/signals/ABI/
bionic/ART/copy-race/full/package/e2e and physical context gaps remain open.
Cleanup recovery manifests/files are preserved; removed historical clones and
runtime duplicates were neither recreated nor used by this fresh CI run.

### Narrow conjunctive evidence route prepared after actual65 independent review

Integrator independently verified all four actual witness groups on both
hosts in40711407869, including raw161 rows/identities/one absolute5s. Its saved
native3-40711407869-equivalent-evidence-review.md permits implementing a narrow
alternative, pending exact-source peer and fresh CI. That original run remains
FAILURE and production remains1dc2e245.

The candidate changes only command-worker test acceptance and its independent
stderr validator. Every existing strong decoded CRT/FILE/CS/ownerbirth/call/
named-Enter/negative/control/lifetime/completion witness stays mandatory in
all four cycles on BOTH hosts. Full WCT remains a route; an alternative needs
the current successful exact single Blocked closer node PLUS every strong
witness. Sticky errors/cycles/contradictions cannot pass. Only actual close0,
preunlockfalse, unlock/fclose0, kernel-signaled exit/join and control cleanup
under the original5s permit an explicit selected route/contended=true record.
The validator crosschecks route against raw node shape and all evidence, and
also requires labeled copied-metadata running/sticky rejection controls.

Workflow changes only the validation failure description; its own narrow push
path thereby runs the frozen changed source. The original controller,161 strict
Linux JSON comparisons,13 duplicate modes, actual broker/job checks, compiler,
strict suppliers and10s/180s bounds are unchanged. No local native/JVM/C# compile
or fixture execution, no artifact download and no cleanup-state mutation.
Source/ADR peer and actual two-host runtime remain pending; no Native3/global
acceptance, full-WCT-provider claim or primary integration is implied.

### Actual route component40714975474: both hosts complete, selected-port review pending

Exact15640777a5aa3065b514fd9998d8f0bc9c955819 received independent narrow
source/ADR peer (native3-15640777-route-source-peer.md plus hashes) before the
own branch push. Fresh active-ref guard was clear; immediate gh palmoni5
identity check preceded Git transport through the per-command gh helper.
Redundant own compile40714975508 was cancelled with a separate identity check.
Actual component40714975474/attempt1 is terminal SUCCESS: preflight122910070549,
fresh producer122910135005, Windows2022 122911152343 and Windows2025 122911152246.
Required120s watch session26391 ended exit0. No local compile/native execution,
artifact download or regeneration of removed cleanup caches was used.

Saved plain logs and final API metadata are native3-commands-40714975474-* in
C:/work/gesher-ci-review. The independent saved plain-log parser/report are
native3-40714975474-native-independent-audit.py/.json. Actual Linux161 rows
match ALL161 actual rows on BOTH Windows without normalization or mismatch.
Both hosts observed every strong FILE/CS/ownerbirth/call/named-Enter witness,
all six real-resource/identity negatives and labeled WCT/route metadata-copy
negatives in ALL four cycles. Actual2022 chooses full-wct with three nodes in
all four cycles;2025 chooses decoded-crt-lock with the exact one Blocked node
AND complete strong conjunction in all four. Actual cycle totals2022 are
578/406/422/422ms;2025 1188/1032/985/1032ms under each original absolute5000ms.
Windows2025 full-WCT owner reporting remains unobserved, not retroactively
supplied by the alternative evidence. All previous failed runs remain RED.

Both hosts independently complete13 modes/8399 assertions/0 failures, including
required actual retained-fd reuse/foreign-sentinel survival, all quarantine
cases, true8192 occupied slots/EMFILE cleanup and1000 raw-race duplicates with
0 errors. Concurrent raw publications3305/3441. Real isolated broker audit
records13 normal client exits/two actual opens; checked intentional owned
broker stop211 and actual job14-child reap complete. All15 canonical helper
negative controls and strict current-producer/API/source/attempt/timestamp,
rawZIP-before-extract, whole source manifest and seven output checks pass.

Producer artifact12828630529 ZIPsha256
118db34af03365057232b2769cc2eca91d105804f1e9dab57f40788490117949.
Both hosts verify the exact seven produced outputs:

```
native3_lock_command_worker.exe d4210858ac20e74159168c703682f864aaa328c85af6e08f5e6bc4c4b141913b
native3_lock_duplicate_test.exe 83d6bebb0d1c9528419d6be3970dbfe6d4341e0e609f3702a47e891c8b70444e
binderd.exe ba71e55f4ed1974930562f7a7bf576005c397beafc1d47c65bd0c0a0c8ad07a3
linux-reference.json 3080bbfab5e57a197194217e8900527c6bef527fa30da39f2366d1b9e58ad0f1
native3_lock_command_worker.imports.txt 7311f60b6733bf3f96cc146887b31970400683c9ee2e40d6c4586c80dc8ab611
native3_lock_duplicate_test.imports.txt c2e3d8516034c6f18558cb2a01a9fe9e517c76e893d3a7886a285604403eb4a7
binderd.imports.txt e9c93ca16e9f5221639ac6d6d7e38b9ce97a237ee97b0b7383b33ac111db4598
```

Independent integrator actual-evidence audit is requested before any selected
port. Source/patch boundaries are saved externally as
native3-15640777-selected-port-manifest.json and selected-production.patch.
True fork is c455f432, not current qualified7bb. Never replace the whole leaf:
accepted Media/Binder/reader differences must remain, including qualified
posix_bionic Chdir->chdir. The exact candidate explicitly includes internal
DuplicateOperations callbacks from54 lineage for real-resource unit failures;
they are not selected by guest/environment/public fcntl. Do not claim those
bytes excluded while porting their exact helper/test dependency.

This is bounded common-layer/stdio/resource evidence. Production is still1dc,
and inheritedOFD, atomic CLOEXEC/spawn, blocking7/38, targeted signals,
bionic/ABI/ART, protection/copy races and physical contexts remain open. Any
selected integration requires independent port review and fresh coherent
canonical full/package/two-e2e with these exact161/four-route/13-mode gates and
strict supplier checks. No primary merge follows from this component alone.

### Isolated raw CreateProcess temporal diagnostic source WIP

The next fixture is separate from immutable156 and its accepted13-mode
supplier. No qualified production implementation or ordered Exec source is
changed. New EXCLUDE_FROM_ALL target native3_cloexec_window_probe invokes the
actual common DuplicateAtLeast helper with the existing direct internal
set_inherit dependency to observe its real native alias BEFORE final flags.
That test dependency is never guest/environment/public-fcntl selected.

A real same-binary child is created through raw broad-inheritance CreateProcess
without a HANDLE_LIST. A JOB_LIST attribute assigns the owned kill-on-close
job atomically at creation; actual membership is checked while still suspended,
then exact ResumeThread previous1 is required before the child runs. The child
checks the exact candidate HANDLE against actual file ID/volume and a random
nonce, alongside intended-inherited and always-noninherited controls on the
same real file. It reports actual accessibility and exits23 for a leaked
candidate,0 only for no leak with passing controls. Parent verifies exact
output/code, actual one-process job exit/reap, final CLOEXEC and resource/file
cleanup. A genuine leaked alias makes this diagnostic return failure even if
the final flag state is correct; it is not another accepted resource mode.

This intentionally pauses the internal unit dependency while the parent owns
its process-local descriptor guard. The child performs only raw kernel file
probes, not a parent registry callback or a wait on that parent's guard. No
production callback, generic lock-wait proof, OFD adoption or ordered-spawn
atomicity is inferred. Its own new absolute10s probe bound, at most5s child
wait and separate at most5s failed-child cleanup do not alter any accepted
stdio5s/command10s/root180s gate.

Source/ownership/compile/API peer is required before any diagnostic CI. No
workflow or dispatch is prepared yet; no local native compile/fixture runs.
Native leaf retains old pre-qualified ordered-spawn files, so any later
Exec work must use current protected7bb/union blobs rather than copying this
leaf's old versions. The independent source-only plan and protected blob IDs
are saved as native3-next-cloexec-inheritance-plan-ec3d58a8.md in ci-review.

### Isolated temporal diagnostic workflow prepared, peer pending

Exact3f52b3f3 fixture received independent source/lifetime/API peer for preparing
narrow CI only (native3-3f52b3f3-cloexec-source-peer.md and hashes). No runtime
or compiler proof follows from that peer. Its frozen CMake/probe Git blobs
are pinned by the new independent native3-cloexec-window workflow. The source
guard limits the entire change from accepted docs checkpoint ec3d58a8 to
those two files, this workflow and the track; qualified production, original
controllers and ordered Exec are unchanged.

The own-branch workflow first checks exact reviewed three artifact helper
blobs and runs all15 canonical rejection controls, then builds only the new
EXCLUDE_FROM_ALL probe and common library with the pinned toolchain/archive
SHA and warnings-as-errors. It captures actual configured compiler path/hash,
underlying clang hash/version, actual compile command, all tracked source
hashes and the exact executable/import outputs. It makes no production-mode,
Linux-reference, ordered-spawn or full Native qualification claim.

Both Windows consume only the unique actual successful same-source/run/attempt
producer/upload. API repository identities, upload/job timestamps, raw ZIP SHA
before extraction and exact three-member payload/source/output inventories
are mandatory before execution. Runtime module versions/hashes, actual result
code and all stdout/stderr survive failure. The new probe keeps its source10s
bound; workflow outer30s kills its owned tree and bounds reaping5s if needed.
A real leaked candidate/exit1 remains a failed Windows job despite final flags
or successful cleanup. Exit0 additionally needs the exact nonleaking control/
child-job/reap verdict. No failure is relabeled a successful diagnostic gate.

YAML/embedded Python and PowerShell AST parsing and diff checks pass locally;
no fixture or compilation ran. Workflow/provenance/watchdog peer is required
before authenticated guarded push/CI. This isolated diagnostic is NOT a new
supplier for the in-flight canonical candidate, which stays pinned to156.

The first workflow peer found three repeated CaseSensitive switches from a
shell-expanded edit; AST syntax parsing alone had not detected parameter
binding failure. The correction uses one switch per inventory comparison
and disables Git path quoting for the whole source-name comparison. Additional
AST duplicate-parameter scanning and a simple Compare-Object binding check
pass; no native fixture ran. Corrected exact workflow peer is still required
before push. Rejected3153 was corrected before branch publication; no CI run
used it as its input HEAD (its ancestry is retained in Git).

### First isolated diagnostic run: actual compile, metadata failure, runtime skipped

Fixture3f52 source peer and corrected774a4a4b workflow peer passed before the
guarded palmoni5 push. Actual40729229702/attempt1/source774 is terminal FAILURE
(required120s watch25879 exit1). Preflight122960685775 passes in6s; producer
122960765635 actually compiles/links the48-step probe/common graph under Clang
23.1.3 and validates explicit imports, but provenance extraction fails before
upload because CMakeCache has no CMAKE_CXX_COMPILER entry. The toolchain sets
that as a normal variable. Windows is SKIPPED; there is NO inheritance proof
or published probe artifact from this failed producer. Plain logs and final
metadata are native3-cloexec-40729229702-* in ci-review.

The changed workflow now requires exactly one actual generated
CMakeFiles/*/CMakeCXXCompiler.cmake record and its one absolute compiler path,
crosschecks the actual unique probe compile-command driver and pinned archive
containment, and records generated-record/cache/compiler/clang SHA and version.
An optional compiler cache entry may only corroborate that record, never be
required or contradict it. Compiler launcher is explicitly empty and checked.
This follows the qualified current host_roles compiler() principle and official
CMake template, without importing its unrelated current-union assumptions.
It is a provenance correction, not a native behavior change or unchanged rerun.
Exact changed workflow peer is required before another guarded CI push. The
first red run and immutable156 supplier remain unchanged.

### Changed diagnostic40730886908: compiler provenance passes, upload service fails

Exact4457279ba0904cc450ba773cb15329ece2deb108 received focused independent
automation peer before the fresh account/ref-guarded push; report
native-cloexec-774-445-automation-peer.md records both earlier corrections.
Actual40730886908/attempt1 is terminal FAILURE (120s watch16927 exit1).
Preflight122966557469 passes in9s, including all15 archive negatives. Producer
122966705846 actually builds/links the probe and common library, validates
imports and PASSES authoritative generated-compiler/source/output provenance.
It then uploads440043 bytes before the artifact service returns
"The server is busy" (request08e46124-e01e-004b-7afd-6667b9000000).
The upload step and producer job FAIL. Final artifact API count is0; Windows
is SKIPPED. No candidate-handle inheritance result, successful producer or
approved runtime supplier exists from this run.

Saved native3-cloexec-40730886908-* plain logs, job/run/artifact API metadata
and independent audit JSON preserve exact scope and SHA. Actual configured
compiler is Clang23.1.3, with generated record
out/CMakeFiles/3.31.6/CMakeCXXCompiler.cmake SHA
8afd29127374ec1e5597394c229921dfb0e99380019d8ee5c7b5ea12ab34de93,
driverSHAc9b86311ade81d53235c93fafabdf98328d094de344ea9a9038e0dab6695ee9f,
underlyingclangSHA5996bd5db24162e3767bdf7c75f7dcc0bf2bc3d1957433aecf176e62e153b40a,
empty configured launcher and the exact warnings-as-errors probe compile
command. Produced-but-not-approved-for-runtime outputs:

```
native3_cloexec_window_probe.exe 9e03a497507630c7a69c74d4b61ccf84c2057360f5f96f5c05366bae2d190a25
native3_cloexec_window_probe.imports.txt aa3750f5cbbc3e88799d308eef97f5989670e55bd414f2c63f94533f88c54201
```

No unchanged rerun, artifact transport bypass or fake successful upload is
performed. There is no evidence that changing concurrency/compression or an
unrelated action-version update fixes this completed-upload service error.
Any retry/transport implementation would need a meaningful separately reviewed
source contract, typed transient-error versus quota/auth/content rejection,
bounded timing and unchanged rawZIP/producer identity gates. Both diagnostic
reds remain red. Immutable156/ec3 remains the sole accepted bounded supplier;
the isolated raw-inheritance contract is still unobserved and open. Local
cleanup recovery data and all qualified production/Exec/controller bytes are
unchanged; no local build or artifact download occurred.

### Attempt2 invalid child/job control: source-only evidence checkpoint

The coordinator reconciled the recognized external Azure busy failure and
authorized exactly one whole-attempt retry. Actual40730886908/attempt2 at
4457279ba0904cc450ba773cb15329ece2deb108 completed FAILURE. Preflight and fresh
compile/provenance/upload passed; successful producer123004667030 supplied
artifact12839443140, ZIP SHA256
ca379e979537c474c716c54e4bcb1fc3b8815390049c31360321714f89fecca7.
Both Windows verified that exact producer/source/run/attempt/artifact before
execution. Saved native3-40730886908-attempt2.log, -jobs.json and -artifacts.json
in ci-review are the evidence; no local artifact download was performed.

Both hosts actually return95 with observed=0, transient-native-inherit=1,
candidate-leaked=0, final-cloexec=0, cleanup=1. Both report TotalProcesses=2,
ActiveProcesses=1 at the strict expected-one-exited-child assertion. This
assertion is reached only after the exact created process handle signaled and
GetExitCodeProcess/QueryInformationJobObject succeeded. The old source returns
before printing that exit code or draining child output. Consequently neither
the second process's identity nor the child's semantic result is established.
This is invalid fixture control, not evidence for or against atomic CLOEXEC.
The file/reservation cleanup=1 does not establish that additional process's
normal exit/reap: the old failure branch merely destructs the kill-on-close
job handle. New failure diagnostics explicitly log its CloseHandle result and
retain extra-member-reap=unproven; no successful process cleanup is invented.
The zero final-cloexec field also follows DuplicateAtLeast rollback after the
observer returns EIO; it is not proof that final flag clearing failed.

The new isolated source preserves evidence before the same strict assertion:
actual created PID/TID/handles/thread birth, process birth/image/membership and
zero-time signaled status, job accounting/PID list while suspended and after
the child signals, exact ResumeThread previous count/wait result, actual child
exit/output and candidate/control handles plus file ID/nonce/native flags.
The fixture parent must be outside its new job and the created child inside it;
these actual membership controls complement the inherited/noninherited file
controls. Additional job PIDs are opened only for query/synchronize, with
birth/image/membership recorded; they never authorize PID-based termination.
Incomplete/overflow/denied observations fail and cannot qualify a result.

Concrete next hypothesis: an additional process is associated with the owned
job either during CreateProcess before ResumeThread, or during child startup/
exit. The two phase snapshots distinguish those cases and identify any live
additional member. A console helper or a descendant is only a possibility,
not an established cause. A stdout-mode failure now records errno/PID rather
than silently returning94. No launch flags, accepted process count, child code,
raw broad inheritance, exact output/control requirements, leak verdict or time
bounds are relaxed. All extra diagnostic waits are zero-time; no polling,
sleep, extended deadline, production change or generic handle bypass is added.

This checkpoint is instrumentation, not a behavioral fix. Local checks are
source/diff inspection, workflow YAML/embedded Python/PowerShell parsing and
pin/delta checks only. No native/JVM/C# build or fixture execution, push or CI
dispatch is authorized in this bounded task. Fresh independent source/lifetime/
automation peer and a separately authorized fresh CI producer/two-Windows run
must establish actual process roles, child semantic exit and complete cleanup
before choosing a causal fix. Keep the strict one-child assertion until that
evidence supports a reviewed change; no unchanged retry remains authorized.
Immutable156/407149 and the current canonical integration/full candidate remain
separate and unchanged. Atomic CLOEXEC/spawn remains open.

### Changed diagnostic40745358560: lifetime history and current snapshots diverge

Actual40745358560/attempt1/source dce639018dc1ff122ce7501923061cd19f9161e8
is red: both Windows fixtures exit95. Producer/upload/strict provenance passed;
artifact12840903093 ZIP SHA256
e0214439cb88a0166e8f782cc8bd5aa3b1a871a93d83b101e88e0cc5b69f8acd
is diagnostic evidence only. Completed plain logs were saved (without artifact
download) to ci-review/native3-cloexec-40745358560-attempt1-plain.log.

Both hosts have suspended total1/active1/assigned1/listed1, with the parent
outside the job and the created child inside. ResumeThread reports previous
count1; the exact created handle signals with exit23. The first accounting
query after that wait reports total2/active1 (the failure assertion's stored
snapshot). The later JobEvidence query reports total2/active0/assigned0/listed0.
Those sequential observations are not contradictory or an atomic history.
They establish that the lifetime total changed after the suspended snapshot;
they do not identify a second process, its creation time or executable. Neither
retaining the exact child handle nor a later active0 explains away total2.

Raw child output reports inherited1/excluded1/candidate-visible1 with exact
file ID/nonce and117 bytes, but strict process evidence fails. These bytes and
exit23 do not qualify an inheritance result. ProcessEvidence error31 is from
the combined GetProcessTimes / QueryFullProcessImageNameW / GetExitCodeProcess /
IsProcessInJob condition on the signaled created handle; short circuiting hid
which API failed and prevented subsequent calls and the zero-time wait record.
Image-query or membership behavior after exit is a hypothesis, not an actual
cause established by that collapsed message. Kill-on-close CloseHandle succeeds;
extra-member-reap remains unproven. observed0/final-cloexec0 follow rejection
and duplicate rollback; cleanup1 only covers file/reservation controls.

The new source is a focused diagnostic correction, not a lifetime behavior
fix. Each process API now captures its own immediate error with phase, role
and exact handle, continues collecting the other independent observations,
and prints explicit validity bits even on failure. Unknown zero-initialized
fields cannot qualify evidence. All original required API successes remain
required; GetProcessId success is now additionally required. The first
acceptance accounting snapshot is printed explicitly before later observations,
and the two job-query failures have distinct operation names. No cached live
image/membership, accepted count, delay, retry, larger timeout, launch flag or
output/exit/leak relaxation is introduced. Only the isolated workflow probe
source pin changes along with this handoff.

Microsoft's primary API contracts distinguish lifetime TotalProcesses,
current accounting and current PID-list observations; IsProcessInJob tests
current membership. An owned signaled process object remains valid while its
handle is retained, but this does not promise every process query succeeds.
Sources: https://learn.microsoft.com/en-us/windows/win32/api/winnt/ns-winnt-jobobject_basic_accounting_information,
https://learn.microsoft.com/en-us/windows/win32/api/winnt/ns-winnt-jobobject_basic_process_id_list,
https://learn.microsoft.com/en-us/windows/win32/api/jobapi/nf-jobapi-isprocessinjob,
https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-queryfullprocessimagenamew,
https://learn.microsoft.com/en-us/windows/win32/procthread/terminating-a-process.

Freeze and obtain independent source/lifetime/automation peer before any fresh
authorized producer/two-Windows run. That run can localize error31; it cannot
guarantee identity of a transient process missing from both PID snapshots.
If history remains unexplained, separately review bounded event capture before
launch. Job completion-port notifications can supply historical PIDs, but are
not guaranteed delivery and PID reuse still requires handle identity; they
cannot replace strict onechild/active0/exit/output/leak/bounds or authorize
PID-based termination. No such event machinery is added in this checkpoint.
Official notification caveats:
https://learn.microsoft.com/en-us/windows/win32/api/winnt/ns-winnt-jobobject_associate_completion_port.
No conhost identification, allowed extra process, full cleanup proof or causal
runtime fix is asserted. No local native/JVM/C# build, fixture, artifact download,
push or CI dispatch occurred. Qualified156/ec3, production/Exec/controller and
candidate81fd/full407319 remain unchanged; global Native/atomic spawn/OFD
closure remains unqualified.

### Terminated image-query cause: suspended identity pinned on the owned handle

Actual40747753709/attempt1/source1439a58938ec538fe03b6caa38bfef03fd2bb30e
remains red (both Windows exit95). Plain evidence is retained externally at
ci-review/native3-1439-run40747753709.log. Both hosts now identify error31
specifically as QueryFullProcessImageNameW on the signaled created-child handle;
PID, creation time, membership, zero-time wait and exit queries succeed. The
exact child exits23 with inherited/excluded controls and candidate-visible1,
matching file identity/nonce. The first post-wait accounting remains total2 /
active1; the later current snapshot is active0 / listed0. The additional
lifetime process's identity, role and complete reaping remain UNKNOWN. Raw
visible candidate bytes cannot qualify inheritance while job ownership fails.

The source correction captures PID, birth, nonempty image and membership while
the child is suspended on the exact CreateProcess process handle. Capture must
also establish WAIT_TIMEOUT and STILL_ACTIVE; PID must match PROCESS_INFORMATION.
Failure prevents resume and cannot create a valid pin. The same retained handle
is checked after termination for current PID, matching birth, actual signaled
wait and actual exit (matching the first exit query). Current membership is
still queried and its API success required, without assuming a terminated
process must remain a member. The image and suspended membership in the new
completion record are explicitly historical, never reported as current query
success. No terminated image query or PID-based reconstruction of the created
child is used. Unclassified PID-list entries remain diagnostic only; opened
query/synchronize handles require matching PID, actual birth/image/membership
and wait evidence, with errors/overflow/incomplete lists failing closed.

The first acceptance accounting query now passes its unchanged snapshot directly
to JobEvidence, whose next API operation queries the PID list before any process
identity/parent queries or diagnostic output. Listed PIDs are retained in logs
even if OpenProcess fails. Unclassified member observation precedes parent
controls. These are sequential snapshots, not an atomic history; an already
vanished transient member can still be missed and remains unknown. There is
no polling, retry, new event machinery or added nonzero wait. The later snapshot
remains separate. No total2/conhost exception is accepted. Strict total1/active0,
exact output/exit/native flag agreement, leak failure, cleanup and original
clocks remain unchanged, as do launch flags and broad inheritance.

Microsoft documents that created process handles remain valid after termination
until closed, that termination signals the process object and preserves exit
status, and that GetProcessTimes returns creation time. The executable-image
API contract has no post-termination success guarantee. Error31 is the observed
API failure, not a documented universal outcome for every terminated process.
Primary references:
https://learn.microsoft.com/en-us/windows/win32/procthread/process-handles-and-identifiers
https://learn.microsoft.com/en-us/windows/win32/procthread/terminating-a-process
https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-queryfullprocessimagenamew
https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-getprocesstimes
https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-getexitcodeprocess
https://learn.microsoft.com/en-us/windows/win32/api/jobapi/nf-jobapi-isprocessinjob
https://learn.microsoft.com/en-us/windows/win32/api/winnt/ns-winnt-jobobject_basic_accounting_information
https://learn.microsoft.com/en-us/windows/win32/api/winnt/ns-winnt-jobobject_basic_process_id_list

This fixes the demonstrated fixture lifetime precondition; it does not explain
the lifetime total2 or establish successful runtime behavior. A startup-created
helper, descendant, or failed association is still only a hypothesis. Fresh
independent source/lifetime/workflow peer must review the frozen commit before
a separately authorized fresh producer/two-Windows run. The expected next
proof is successful owned completion identity plus immediate listed-member
evidence; if the transient member still disappears, its identity remains
unknown and strict rejection remains. Historical event capture, if needed,
requires separate bounded review and cannot grant PID-based kill authority.
Only source/diff and workflow YAML/embedded Python/PowerShell syntax and pins
were checked locally. No local C++/Java/C# compile, fixture, artifact download,
clone/cleanup, push or CI dispatch occurred. Production, Exec, controller,
common156 and canonical81fd remain untouched. Global Native/atomic spawn/OFD
closure remains unqualified.


### Raw HANDLE oracle isolation after actual 40749991233 / source 8816bf89

Source-only checkpoint, based on the exact audit/plain log in
C:/work/gesher-ci-review/native3-8816-actual-{audit.md,audit.json,plain.log}.
The actual run is 40749991233 attempt1, not a successful inheritance verdict.
Both hosts retain candidate-visible1 with identity/nonce and actual child23;
probe95 rejects first total2/active1. Later active0 does not replace that gate.
Extra PIDs6488 (2022) and7048 (2025) have distinct queried births and actual
member1 on opened query/synchronize handles. Image error5 leaves origin and
complete extra-member reaping UNKNOWN. These are not PID4/System.

Established source facts and excluded explanations:
- JobEvidence uses the SDK JOBOBJECT_BASIC_PROCESS_ID_LIST, ULONG_PTR elements,
  array indices0..NumberOfProcessIdsInList-1, and storage for eight entries.
  This matches Microsoft's ABI; there is no demonstrated PID parser bug.
- TotalProcesses records lifetime associations, including some failed
  associations; it is not a distinct-PID counter. ActiveProcesses and the
  separate PID-list query are current sequential observations. The opened
  distinct extra PID/birth/member evidence independently demonstrates another
  member here. No total2 allowance or accounting reinterpretation is added.
- The old child is a self-reexec of the common-linked probe. posix.cmake forces
  crash and signal anchors into every linked executable. crash.cc's Installer
  and process.cc's g_mailbox_ready execute before main's --child branch.
  Therefore --child does not isolate the raw oracle from common startup.
  The inspected POSIX constructors do not explicitly CreateProcess; no source
  evidence identifies their work as the creator of the observed extra member.
  A bionic/sharedKernel bootstrap is not established by these target sources.

The new EXCLUDE_FROM_ALL cloexec_raw_child.cc has a custom raw PE entry and
-nostdlib, no CRT/Gesher libraries, no shadow includes, no dynamic static
initializers, no descriptor conversion, no process creation and no console
allocation/attachment calls. LINK_LIBRARIES and INCLUDE_DIRECTORIES explicitly
reset posix.cmake's directory-wide defaults. It uses Windows GUI subsystem:
Microsoft documents GUI processes have no console attachment at creation.
CREATE_NO_WINDOW remains in the parent flags and is ignored for GUI images;
the no-console property comes from the actual PE subsystem and child source,
not an assumption that the old extra process was conhost. GUI programs may
use inherited redirected standard handles; only WriteFile on the explicitly
supplied STARTF_USESTDHANDLES output pipe is used. It opens no pathname.

The parent selects the exact sibling image via lpApplicationName, quotes its
command path, and checks suspended owned-handle image against that path. The
child strictly parses three numeric HANDLE values, nonce, volume and file ID,
checks actual GetFileInformationByHandleEx identity and ReadFile nonce on the
positive, excluded-negative and candidate HANDLEs, and emits the same exact
record. It exits23 for a visible candidate,0 for absent,94 for failed controls
or output,92 for malformed input. A GUI image is not a visible UI here.

The raw CreateProcess TRUE inheritance call remains inside AtFinalFlags,
BEFORE actual final HANDLE_FLAG_INHERIT clearing. No HANDLE_LIST, delayed
observation, fake transaction ACK, public blocking relay, production change,
extra-process exception, ignored denied query, or relaxed timeout exists.
Owned suspended PID/birth/image/member pin, thread identity, checked resume,
first post-wait total1/active0, exact child output/exit/native-flag agreement,
retained handle wait/reap, cleanup and leak1 failure remain required. Unknown
history still fails closed; successful GUI isolation has not yet been run.

Workflow adds the child EXE, actual imports/PE headers and Ninja commands to
the exact output inventory and hashes. It requires actual GUI subsystem,
only kernel32.dll with exactly ExitProcess/GetCommandLineW/
GetFileInformationByHandleEx/SetFilePointerEx/ReadFile/WriteFile/GetStdHandle,
raw entry/-nostdlib link flags, and the same verified configured compiler
for the isolated child compile command with no compat/AOSP includes. Whole
tracked source, producer run/attempt/job/upload interval, exact raw ZIP digest
and pre-extraction member guards are retained. Source pins include the child.

Next controlled fixture, only after separate peer/runtime authorization:
use the frozen source's real producer and both Windows consumers, no local
native build or surrogate. Require actual child-only compiler/link/import/GUI
proof and the suspended image/path/pid/birth pin. The intended positive and
negative controls must pass on the actual HANDLEs. Require first total1/
active0 and one owned unique child's lifetime, actual wait/reap and exact
output before accepting any raw observation. Candidate23 must yield probe1
and a red workflow; candidate0 requires final-cleared flag/clean resources
before probe0. If total2/unknown helper persists, retain strict rejection and
all actual evidence; isolation alone cannot name/reap the historical helper.

Peer questions: Does target property reset truly exclude all directory link
and include contamination in actual commands? Do actual PE entry/imports prove
no CRT or console/process-startup path? Are custom argument bounds and raw
WriteFile bytes exact? Are suspended path/owned HANDLE identity, first snapshot
and strict reap/leak gates unchanged? Do the new payload members retain every
producer/source/archive guard? Which evidence could identify an extra live
member before exit without granting PID-based termination authority?

Primary contracts:
https://learn.microsoft.com/en-us/windows/win32/api/winnt/ns-winnt-jobobject_basic_process_id_list
https://learn.microsoft.com/en-us/windows/win32/api/winnt/ns-winnt-jobobject_basic_accounting_information
https://learn.microsoft.com/en-us/windows/console/creation-of-a-console
https://learn.microsoft.com/en-us/windows/console/getstdhandle
https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/ns-processthreadsapi-startupinfow
https://learn.microsoft.com/en-us/windows/win32/procthread/process-creation-flags

Validation is source/diff, YAML and embedded Python/PowerShell parsing and
source pins only. No local C/C++/Java/C# compile, configure, runtime fixture,
artifact, clone/cleanup, GitHub write, push or CI dispatch. Common156,
production/Exec/controller/AOT and all other trees unchanged. Exact extra
process origin, controlled runtime result and global Native/atomic spawn/OFD
closure remain unqualified. This change removes proven oracle startup
contamination; it is not a demonstrated causal fix for the unknown member.
