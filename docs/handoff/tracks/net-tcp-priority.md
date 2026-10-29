# TCP urgent priority readiness (ADR0070, test93)

Exclusive source: `wip/net-tcp-priority`, based on qualified coherent source
`c254c6c19fb3c8553208a096116eab7350d4de49`. No integration acceptance yet.

## Contract

Linux `poll(POLLPRI)`, `select(exceptfds)` and level `epoll(EPOLLPRI)` report
queued TCP urgent data with SO_OOBINLINE both disabled and enabled. A normal
prefix can remain readable while urgent readiness is present; consuming the
out-of-band byte clears priority immediately, before consuming that prefix.
Inline consumption preserves the urgent byte in stream order. Readiness must
not consume payload or SO_ERROR or alter socket blocking/notification state.

Independent Linux source `tests/net-tcp-priority/contract.cc` compiles and runs
only in CI. It is also intended for the actual Gesher POSIX host target and
both NDK executable ABIs, rather than a translated model of the implementation.
It covers no-data and normal controls, bounded finite waits, queue/re-poll,
simultaneous normal/urgent data, descriptor aliases, multiple descriptors,
level epoll, EOF, and delayed urgent arrival from another thread.

## Backend choice prerequisite

Microsoft documents that WSAPoll rejects POLLPRI. Winsock select exception
readiness includes failed nonblocking connect as well as OOB; it suppresses
OOB exceptions entirely for SO_OOBINLINE. Its SIOCATMARK semantics differ
from Linux and must be characterized rather than assumed. The independent
WinSDK oracle records normal/urgent queue transitions with actual TCP_INFO
receive counters, prefix and urgent consumption, and refused-connect state.
No Gesher runtime is loaded by that diagnostic. Runtime source changes and
fresh matched host/DLL/both-ABI CI are still required after it.

Primary references:
- https://learn.microsoft.com/en-us/windows/win32/api/winsock2/nf-winsock2-wsapoll
- https://learn.microsoft.com/en-us/windows/win32/api/winsock2/nf-winsock2-select
- https://learn.microsoft.com/en-us/windows/win32/winsock/protocol-independent-out-of-band-data-2
- https://learn.microsoft.com/en-us/windows/win32/winsock/winsock-ioctls
- https://kernel.googlesource.com/pub/scm/linux/kernel/git/brauner/man-pages/+/refs/heads/master/man7/tcp.7

## Actual diagnostic evidence

All compilation/execution below was CI-only. Integrator reviewed source before
push; each push checked the palmoni5 account and used gh-authenticated HTTPS.
No artifacts were downloaded locally and no production runtime was reused.

- `0efbf7eca4c870cdd10b2216ed744e780e75cb76` / `40849173838`:
  independent Linux contract PASS, both Windows urgent queue/consumption
  observations recorded. Overall RED because refused-connect notification did
  not arrive within the original two-second diagnostic bound. No unchanged retry.
- `388b1868cc3f329c2e62b53b5b128eca2a7494c0` / `40849990611`:
  GREEN Linux plus both Windows. Added suffix/boundary observations and held
  non-listening port to exclude reuse. Refusal notification arrived near two
  seconds inside the changed five-second bound; getpeername failed with10057.
- `3b92a7f0555aef523774a902b28cebf15151cf30` / `40850431776`:
  GREEN Linux plus both Windows. Additional non-consuming WSAPoll POLLRDBAND
  observation at every existing phase. Non-inline urgent yields512 until
  MSG_OOB consumes the byte, including after the normal prefix is consumed.
  Native inline mode yields0 in every phase despite proved urgent arrival.

Native Windows SIOCATMARK is0 while out-of-band data remains unread and1
otherwise. In inline mode it is1 in every phase. FIONREAD includes normal
bytes on both sides of the urgent byte: prefix6+urgent1+suffix4 reports10,
and reports4 after reading the prefix while urgent data remains unread.
Therefore neither ioctl can recover the urgent byte's position for a safe
logical-inline implementation. Blanket normal-read or exception readiness
must not be converted to Linux priority readiness. SO_ERROR remains untouched
by all diagnostic observations.

Actual raw logs are preserved in C:/work/gesher-ci-review:
`tcp-priority-40849173838-{linux,windows2022,windows2025}.log`,
`tcp-408499-win2022.log`, `tcp-408499-win2025.log`,
`tcp-408504-win2022.log`, `tcp-408504-win2025.log`.

## Pending production source and remaining work

Generic `fd.cc` dropped POLLPRI in both request and result translation.
The pending change forwards those bits. The Winsock source requests its
supported POLLRDBAND for neutral priority interest and maps that result back
to priority, independently of normal POLLRDNORM, errors or connect failure.
This removes the old incorrect treatment of band data as normal readiness.
It preserves WSAPoll's existing ownership, real timeout and cancellation
behavior without installing socket events, spinning, consuming OOB or reading
SO_ERROR. select/bionic select and epoll already preserve exceptional masks.

The same contract source has a CMake POSIX host target. New refused-connect,
SO_ERROR consumption and closed-descriptor controls are source-only until
fresh Linux and production CI validate them. Both NDK ABIs must be built and
run against a fresh matching host and translator DLL with strict source/run/
attempt/compiler/member-role digest evidence. No production implementation
has passed CI yet. The full contract fixture deliberately continues to require
SO_OOBINLINE1; it must not be silently skipped to claim complete parity.

The existing SocketFd table copies values and ws::Duplicate creates another
provider handle to the same endpoint. Any future logical SO_OOBINLINE state
must live in shared open-description storage copied by dup/dup2, while accept
creates a new description with inherited option values. A receive-side design
must preserve the prefix/urgent/suffix boundary, MSG_PEEK, MSG_OOB rejection
when inline, scatter/gather, concurrent readers, timeout and close lifetime.
The public Winsock observations above do not yet supply that boundary; an
approximate userspace byte queue would corrupt ordering and is not implemented.

Strict coherent component and canonical whole-suite caller/guard plumbing
remain pending. No all-Net4, all-poll, ancillary, hardware, main or release
closure is implied. Inventory and historical proofs are unchanged.
