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
