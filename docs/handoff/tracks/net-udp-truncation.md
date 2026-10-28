# Net4 UDP truncation — source checkpoint

Status: implementation and fixtures prepared; **no CI execution or acceptance**.
Base f1550540186b4ae3f17a5e501a886ec1c4d5d613, exclusive branch
`wip/net-udp-trunc`, test89, existing ADR0070. Source peer is required before
push/dispatch. This work does not modify native-api-coverage.json or any of
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
are outside this contract. MSG_WAITALL is ignored for UDP; MSG_OOB fails
EOPNOTSUPP; the preexisting empty MSG_ERRQUEUE returns EAGAIN. Existing stream
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
