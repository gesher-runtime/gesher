# Net4 UDP truncation — source checkpoint

Status: implementation and fixtures prepared; **bounded source/helper checks
only; no real bionic socket ABI qualification or full CI acceptance**.
Base f1550540186b4ae3f17a5e501a886ec1c4d5d613, exclusive branch
`wip/net-udp-trunc`, test89, existing ADR0070. Source peer is required before
production dispatch. This work does not modify native-api-coverage.json or any of
its historical proofs and does not close the 82 resolver entries, ancillary
data, generic socket ownership or aggregate Net4 scope.

## Contract

For ordinary IPv4/IPv6 UDP, recv/recvfrom/recvmsg copy at most the supplied
capacity. Input MSG_TRUNC returns the original datagram length; recvmsg
output MSG_TRUNC reports an oversized datagram regardless of the input flag.
Exact fit and empty datagrams do not set it. Zero capacity, including empty
iovecs, consumes one datagram; MSG_PEEK retains it. No partial tail survives
a consuming call. Peer Linux sockaddr fields come from that same receive.

The Winsock bridge receives one complete datagram into a bounded 65536-byte
allocation and then scatters it. This avoids a FIONREAD/peek/consume race
between concurrent receivers or dup aliases. Allocation failure returns
ENOMEM before touching the queue. Native WSARecvFrom failures remain errors;
the new UDP path never fabricates a length from WSAEMSGSIZE. Windows ordinary
UDP payload maxima (65507 IPv4, 65527 IPv6) fit the allocation. IPv6 jumbograms
are outside this contract. MSG_WAITALL and MSG_OOB are ignored for UDP; the preexisting empty MSG_ERRQUEUE returns EAGAIN. Existing stream
receive behavior and unrelated families are retained.

Primary references: pinned [bionic socket header](https://android.googlesource.com/platform/bionic/+/android-16.0.0_r4/libc/include/sys/socket.h),
[Linux UDP receive implementation](https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/tree/net/ipv4/udp.c),
[Microsoft WSARecvFrom](https://learn.microsoft.com/en-us/windows/win32/api/winsock2/nf-winsock2-wsarecvfrom),
[Microsoft recvfrom](https://learn.microsoft.com/en-us/windows/win32/api/winsock/nf-winsock-recvfrom).
Microsoft documents that unreliable datagram tails are discarded on
WSAEMSGSIZE; full-length reconstruction after that error is impossible.

## Required fresh evidence

native2 builds the portable C++ fixture with the pinned NDK for both actual
bionic ELF ABIs. The same source is independently built and executed against
Linux libc in CI. Its actual 192 matrix rows cover three receive APIs, both
IP families, with/without input MSG_TRUNC, peek/consume, empty/nonempty
datagrams and zero/short/exact/large capacities. Additional rows cover
source addresses, zero-length intermediate iovecs, 0/1024/1025 iovecs,
error-flag queue integrity, maximum IP payloads, 128 concurrent packets through
dup aliases and a TCP stream-tail control. The missing-MSG_TRUNC negative
fixture must fail on Linux and on both Windows ABI paths.

test89 compares the actual bionic results with executed Linux rows. The
existing canonical native2 supplier receipt verifies current source/run/
attempt/producer/artifact ZIP digest. An added exact-member manifest binds
ELF machine types, compiler identity files, source hash and SHA256/role for
each ABI executable and Linux reference. No old native2 binaries are reused.

Before acceptance: independent source peer; fresh full CI at the exact
source; all existing gates retained; both Windows actual suite zero failures,
all test89 rows/negatives, package installed checks and both e2e zero
unexpected. No local C/C++/Java/C# build or native fixture execution occurred.

Shared additive changes: `.github/workflows/native2.yml` builds two new ELF
members, includes them in symbol provenance, and creates Linux reference/
identity members. Existing net, native2, package and full gates are unchanged.

## Peer blocker and observational CI checkpoint

Implementation source3148c3dbecf2eb3faf51702f135cb2a6e556c5b9 was independently
reviewed. Its direct scatter memcpy can fault on a non-null invalid or
unwritable guest destination after receiving the packet. Production acceptance
is blocked until this becomes a fault-safe errno path matching independently
observed Linux behavior. Do not assume that Linux preserves the datagram on
EFAULT: partial scatter copying, MSG_PEEK and MSG_TRUNC need actual evidence.

Observer source0fc59dad164675a89138f6ad103402a818878ba9 adds only a bounded
Linux workflow and assertion-free C observation fixture. That observer-only
slice received an independent source pass before push. Run40790499313,
attempt1, was queued on2026-10-29; integration owns the sole watch. It records
80 actual rows across both IP families, three APIs, four flag combinations,
invalid/null/read-only destinations and partial scatter, including copied
prefix, errno, output flags and a subsequent queue probe. No result is accepted
yet. No native code was compiled or run locally.

The completed40790499313 Linux log is saved at
`C:/work/gesher-ci-review/udp-fault-40790499313-linux.log`. All80 rows return
EFAULT14. Flags0/32 consume the packet (next read EAGAIN11); flags2/34 retain
it (next read9bytes). Partial scatter always copies the first four bytes
`abcd`; read-only bytes and input msg_flags0x5a5a remain unchanged on error.
This is Linux evidence, not a Windows implementation proof.

Observer0c9ba94961e6a2fd59a5a9fe0a32e21400fe32fd received an observer-only
source pass and started40791021504/attempt1. Its96 Linux rows add invalid
iovec-descriptor/message-header pointers. Both Windows runners independently
observe ReadProcessMemory with valid, invalid, null, read-only, inaccessible,
cross-page and guarded destinations, logging return/error/copied count,
protection and bytes. No copy mechanism has been selected pending those
actual results. Integration owns the sole watch. Both NDK ABI fault executables
and96-row actual Linux comparison are prepared as required full gates.

The existing canonical full preflight rejects this additional production
delta. Before full, retain tools/native3-locks/current_payload.py unchanged
and introduce a separately reviewed immutable reusable UDP guard pinned by
commit in the caller, with external NET_UDP_FULL_CALLER_COMMIT authority
resolved only inside that trusted guard, actual run identity, exact f155 plus
reviewed blob inventory, caller bytes, full producer DAG and mutation controls.
A mutable candidate script that approves its own pin list is insufficient.

## Guarded-copy implementation and current proof boundary

Production22fd8e06 removes the unchecked UDP scatter memcpy. The shared
socket_user_copy.h helper prefilters page protection/guard state and performs
each page through ReadProcessMemory with the caller destination as the local
output buffer. It uses no WriteProcessMemory override of read-only pages.
The kernel operation remains necessary after the protection query to avoid
process faults on a later unmap/protection change. Stable guarded pages are
rejected before that API; guard state under concurrent mutations has no
snapshot guarantee. Message headers/iovec descriptors are safely imported
before receive; packet payload/address/output metadata copies fail with
EFAULT, preserving the measured consume/peek and partial-prefix behavior.

Scoped407924/22fd passed both Windows exact shared-helper checks and Linux120;
socket.cc/winsock.cc compiled with the pinned LLVM-Mingw and Werror. The
bionic bridge compile failed because the probe omitted the existing ARM64
header include directory. Changed-sourcea8af4a14 fixes that probe configuration.
Scoped40792815067/a8af4a14 has all four jobs successful: all three actual
production sources compile with Werror/exceptions disabled, Linux120 and
seven exact shared-helper Windows controls pass. This is not socket ABI proof.

Scoped40793392653/72c93c55 has all four jobs successful. Linux176 now covers
invalid/read-only peer addresses, addrlen pointers and read-only msghdr
outputs. All56 added cases return EFAULT after copying payload prefixabcd;
nonpeek consumes and peek retains the packet, output flags remain0x5a5a.
Both actual Windows helper probes pass20,000 calls while an owned target
changes RW/RO/GUARD protection, record172138/91863 concurrent changes and
confirm target join/no process fault. This does not establish stable guard
state during mutation. Exact logs are retained under
`C:/work/gesher-ci-review/udp-helper-40793392653-job-*.log`.

The complete192-row Linux main matrix is added to the narrow workflow at
f942da28, pending peer/CI. Both bionic ELF ABIs still require actual execution,
followed by the immutable full caller policy and every original full gate.

## Actual ABI component preparation (successor checkpoint)

HEAD859d5193 was preserved without resetting any work. The standalone
net-udp-component workflow rebuilds the production gesher-exec host and
matching ARM64 translator with the pinned LLVM-Mingw, plus four fresh NDK
ELFs, before either Windows runner executes sockets. It independently executes
Linux192 main/176 fault rows and the missing-MSG_TRUNC negative. Exact-source
source/member/compiler roles and actual current-run/attempt producer ZIP
receipts are mandatory. No previous component archive is reused.

The component workflow, payload verifier and byte-pinned source manifest are
prepared for independent source peer before push. No actual socket ABI result
or full acceptance is claimed. The preserved immutable full guard draft remains
uncommitted and must be completed only after component/code review.

Independent peer passed exact component sourceb30b0af8 before push. Runtime
component40795059692 and observer40795059724 were triggered by that push;
integration owns their sole watch. Both are pending; no actual socket ABI
qualification is claimed.

Immutable full guard654f6168229e39eabfe1d068531104fb82fbca76 pins all15
non-doc delta members fromb30b against the completef155 tree. The caller
adds this guard before artifact-preflight; all existing producer dependencies,
artifact tests and historical current_payload.py bytes remain unchanged. The
maintainer repository variable NET_UDP_FULL_CALLER_COMMIT must name the
independently reviewed caller commit, resolved only inside the immutable guard.
It is never accepted as candidate input. Exact API run/source/attempt, external
caller bytes, immutable guard invocation and complete checkout/source inventory
are required. Actual individual and simultaneous no-op historical-guard/caller
mutations must reject and restore exact bytes. Guard/caller source peer and
component results remain prerequisites to any full dispatch.

Component40795059692/b30b failed its actual Linux main reference before
Windows execution. Observer40795059724 has three successful helper/production
jobs but its Linux main exits1 with `FAIL UDP: UDP OOB errno=0`, after96
IPv4 main rows; all176 fault rows precede it. Saved raw job123189404590 is
`C:/work/gesher-ci-review/udp-observer-40795059724-linux.log`. This lacks the
OOB return value/queue state, so no corrected OOB semantics is claimed yet.
The next observer adds an assertion-free both-family OOB diagnostic before
the unchanged default matrix assertions. Component Linux compiler command,
version/digest, stdout/stderr and exact exit now print before failure.
The saved immutable guard/caller remains unqualified and must be refreshed
after any reviewed source corrections; no full is dispatched.

Observer40795705106/0e2ab488 actual job123191553091 measures both families:
`n=9 errno=0 prefix=61626364 queue=-1 qerrno=11`. Linux ignores MSG_OOB
for this ordinary UDP receive and consumes the packet. The original assertion
remains red, preserving the observed counterexample. Raw log is saved at
`C:/work/gesher-ci-review/udp-observer-40795705106-linux.log`.

The correction strips OOB together with WAITALL only on the ordinary IP UDP
receive branch before Winsock. The strict portable assertion now requires
9 copied bytes matching the packet plus an empty queue. A separate packet
still must survive the empty MSG_ERRQUEUE call and be consumed by WAITALL.
All192 matrix rows,176 fault comparisons, boundaries, alias and stream
controls and negatives remain required. Fresh source peer and actual both-ABI
component are pending; the saved full guard map is stale and not qualified.
