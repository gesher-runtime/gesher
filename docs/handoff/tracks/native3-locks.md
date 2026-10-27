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
