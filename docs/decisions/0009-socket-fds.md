# 0009 — Sockets as file descriptors in gesher-base

**Status:** accepted, 2026-10-21

## Context
libcore treats sockets as file descriptors: `java.net` and `java.nio`
`read`, `write`, `close`, `poll`, `fcntl`, `ioctl`, `dup` and `dup2` them
like files, and pass Linux's socket constants (hard-coded in Java, as with
errno; see [0008](0008-linux-abi.md)). Winsock sockets are `SOCKET`
handles that the CRT's fd functions cannot use, and Winsock's constants
differ from Linux's.

## Decision
- **A socket's fd is a CRT fd open on `NUL`,** which reserves the number in
  the CRT's table; gesher-base maps it to the `SOCKET` (`src/fd.cc`). A CRT
  function that does not know about sockets sees an empty device, not a
  crash or a double close.
- **`close`, `read`, `write`, `dup`, `dup2` and `isatty` are defined by
  gesher-base under the CRT's own names.** gesher-base is linked before the
  CRT, so every caller gets them; they fall through to `_close`, `_read`...
  for other fds. Macros were rejected: `#define close` would also rename
  `close()` methods (libc++'s `basic_filebuf::close`, for one). `poll`,
  `fcntl`, `ioctl` (`FIONREAD`, `FIONBIO`), `readv`/`writev` and `sendfile`
  accept socket fds too.
- **Two sides that never see each other's constants.** `src/winsock.cc`
  includes only Windows' headers; `socket.cc`, `netdb.cc` and `fd.cc` only
  gesher-base's Linux ones. Between them, `src/socket_internal.h` uses
  gesher's own values, and each side translates with its own headers, so no
  Linux or Winsock number is copied by hand.
- **Winsock functions named like Linux ones** (`bind`, `connect`, `listen`,
  `accept`, `getsockname`, `getpeername`, `getsockopt`, `setsockopt`,
  `shutdown`, `inet_pton`, `inet_ntop`) are looked up in `ws2_32.dll` with
  `GetProcAddress`: an import library member that provides `__imp_bind` also
  defines `bind`, which collides with gesher-base's at link time.
- **The CRT's invalid-parameter handler returns** instead of ending the
  process, so `close(-1)` or a second `close` fails with `EBADF`.

Winsock differences that are translated: `WSAEWOULDBLOCK` from a
non-blocking `connect` is `EINPROGRESS`; an expired `SO_RCVTIMEO`/
`SO_SNDTIMEO` is `EAGAIN`; `SO_ERROR` and `SO_TYPE` values; `WSAPoll`'s
`POLLHUP` at end of stream also reports `POLLIN`; UDP sockets ignore ICMP
port-unreachable (`SIO_UDP_CONNRESET`); accepted sockets are blocking;
`MSG_DONTWAIT` on a blocking socket polls first. Sockets are created
overlapped, since Winsock ignores the timeouts otherwise, and never
inherited.

`SO_REUSEADDR` is recorded but not applied: on Windows it lets a second
socket bind a port already in use, while Linux's only reuses ports in
TIME_WAIT, which Windows allows without it.

`socketpair` connects two loopback sockets and checks that the accepted peer
is the one it connected, so another local process cannot take its place.

`probes/posix/socket_probe.cc` tests all of this; CI runs it on Windows.

## Known gaps
- `fstat` on a socket fd reports a character device, not `S_IFSOCK`.
- Duplicated sockets do not share `O_NONBLOCK` as on Linux.
- No ancillary data (`SCM_RIGHTS`, `IP_PKTINFO`), no `MSG_TRUNC` reporting,
  no `POLLPRI`; `AF_NETLINK`, `AF_PACKET` and `AF_VSOCK` sockets fail with
  `EAFNOSUPPORT`.
- `poll` on pipes checks every 10 ms; `WSAPoll` reports a refused
  non-blocking connect only on Windows 10 2004 and later.
- After a receive timeout Winsock leaves a stream socket in an undefined
  state; Java usually closes it then.
- `AF_UNIX` paths and `/dev/null` (which libcore's `preClose0` opens) follow
  the path model, [0010](0010-paths.md).
- `gethostbyname` and the other legacy resolver functions are declared but
  not implemented.
