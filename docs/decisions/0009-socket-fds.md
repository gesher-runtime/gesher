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

## eventfd and epoll
libutils' `Looper` waits in `epoll_wait` on its fds and on an eventfd that
other threads write to wake it. Both kinds of fd are, like sockets, CRT fds
on `NUL` that `src/epoll.cc` maps to an object; `dup` and `dup2` share the
object, as Linux shares the open file, and `close`, `read` and `write`
(`fd.cc`) and `fcntl`'s `O_NONBLOCK` route to it.
- **An eventfd** is a 64-bit counter with Linux's semantics (semaphore and
  non-blocking modes, `EINVAL` for short buffers and for writing
  2^64 - 1, writes that would pass 2^64 - 2 block or fail with `EAGAIN`).
  It keeps two manual-reset Win32 events, set while it is readable and while
  it is writable; blocking reads and writes wait on a condition variable.
- **`poll` takes eventfds.** When only eventfds are waited for, `poll`
  blocks on their events with `WaitForMultipleObjects` and the real
  timeout, so another thread's write wakes it at once; with pipes, or with
  sockets as well, it checks every source in 10 ms slices.
- **An epoll** is an interest list, fd -> (events, data), and `epoll_wait`
  is `poll` over it: level-triggered, `EPOLLERR` and `EPOLLHUP` always
  reported, `EPOLLONESHOT` disarming an entry until `EPOLL_CTL_MOD`.
  `epoll_ctl` signals an auto-reset event that the wait also watches, so an
  fd added while a thread waits is seen at once. Errors are Linux's,
  `EPERM` for regular files and directories included. `struct epoll_event`
  is packed, as on x86-64 Linux.
- **An entry goes when its fd number is closed** (or replaced by `dup2`),
  not, as on Linux, when the last fd for the file is; a number reused by a
  later `open` is never reported for the old registration.

`probes/posix/epoll_probe.cc` tests them; CI runs it on Windows.

## Known gaps
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
- `EPOLLET` is accepted but level-triggered. Nested epolls (an epoll fd in
  an interest list) fail with `EINVAL`, and `poll` sees an epoll fd as
  `NUL`, always ready. Several threads in `epoll_wait` on one epoll are
  woken by `epoll_ctl` one at a time.
