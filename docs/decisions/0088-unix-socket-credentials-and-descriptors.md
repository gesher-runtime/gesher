# 0088: AF_UNIX credentials and descriptor passing

Status: accepted, 2026-11-20 (the maintainer accepted the credentials' trust
boundary in section 2). Phase 1 is implemented for datagram socketpairs.

## Context

Gesher's `AF_UNIX` sockets are loopback TCP (stream) and connected UDP
(`SOCK_SEQPACKET`, `SOCK_DGRAM`) pairs ([0009](0009-socket-fds.md),
[0027](0027-input-channel-and-win32-translation.md)). They carry bytes only:
`sendmsg` with control data fails with `EOPNOTSUPP`, `recvmsg` returns no
control data, and `SO_PASSCRED` and `SO_PEERCRED` fail with `ENOPROTOOPT`
(0009 "Known gaps", [network.md](../network.md) "ADR 0009 socket gap audit").
Binder carries file descriptors backed by a Windows handle (files, pipes,
memfds) but not sockets (`compat/posix/src/binder.cc`, `SendFds`).

Both multi-process browsers stop on this, after the fixes for their earlier
failures (#201, #203–#207):

- **Firefox** (GeckoView): `GeckoCrashHelper` creates
  `socketpair(AF_UNIX, SOCK_SEQPACKET)` and sets `SO_PASSCRED` on its server
  end. That fails with `ENOPROTOOPT`, so the helper's sockets are -1, and
  Gecko's Rust IPC connector aborts in `OwnedFd::from_raw_fd(-1)`
  (`toolkit/crashreporter/crash_helper_common/src/ipc_connector/unix.rs`):
  a deliberate `MOZ_CRASH` in `runGecko`.
- **Opera** (Chromium): `setsockopt(fd, SOL_SOCKET, SO_PASSCRED, 1)` on its
  IPC socket (`base::UnixDomainSocket::EnableReceiveProcessId`) fails, and a
  `CHECK` traps (`0x80000003`) right after the first window.

Beyond these first calls, both pass descriptors between processes:

- Chromium's Mojo and Gecko's IPC send handles (shared memory, sockets) with
  `SCM_RIGHTS` on `AF_UNIX` sockets.
- Both start their content/renderer/GPU processes as Android services and give
  them their IPC sockets through Binder (`ParcelFileDescriptor`).

On Android all three are kernel features: `SCM_CREDENTIALS` carries the
sender's pid, uid and gid, checked by the kernel; `SCM_RIGHTS` installs new
fds for the same open files in the receiver; Binder's `BINDER_TYPE_FD` does
the same for any fd.

Making `setsockopt(SO_PASSCRED)` succeed without delivering credentials
would report a capability Gesher does not have, and the next call
(`SCM_RIGHTS`) would fail anyway. It is not an option.

## Decision

### 1. A framed transport for Gesher's AF_UNIX sockets

Both ends of every Gesher `AF_UNIX` socket are Gesher's, so each message can
carry a small header. The header is internal; applications see Linux
semantics.

- **`SOCK_SEQPACKET` / `SOCK_DGRAM`** (datagrams): each datagram starts with
  a header:
  - a magic value and a version;
  - the sender's credentials (`pid`, `uid`, `gid`);
  - the number of descriptor tokens, followed by the tokens.

  `recvmsg`/`recv`/`read` strip the header. `MSG_TRUNC` and `MSG_CTRUNC`
  report as on Linux.
- **`SOCK_STREAM`**: the byte stream is cut into records, each with a header
  and a payload length. Linux attaches ancillary data to the first byte of
  the `sendmsg` that sent it, and a `recvmsg` does not read across that
  boundary. Records give the same boundary: a read stops at a record that
  carries control data. Plain `read`/`recv` of a record without control data
  behaves as now.
- **`poll`/`epoll` readiness** counts payload bytes, not header bytes. A
  stream socket is readable once a full header and at least one payload byte
  have arrived.
- **Compatibility:** the change switches every Gesher `AF_UNIX` socket at once
  (same version on both ends: one installed Gesher). The input channel (0027)
  and `LocalSocket` (`compat/framework/local_socket.cc`) use the same layer
  and are re-validated.

### 2. Credentials: `SO_PASSCRED`, `SO_PEERCRED`, `SCM_CREDENTIALS`

- **`setsockopt(SO_PASSCRED)`** is recorded per open file description.
  `getsockopt(SO_PASSCRED)` reports it.
- **`recvmsg` on a socket with `SO_PASSCRED`** returns `SCM_CREDENTIALS` with
  the credentials from the message's header. An explicit `SCM_CREDENTIALS`
  that a sender passes is accepted only with its own pid/uid/gid, as Linux
  allows an unprivileged process.
- **`SO_PEERCRED`** returns the peer's credentials, exchanged when the
  connection is made: `socketpair`, `connect`/`accept`.
- **Trust boundary (decided):** Linux's kernel guarantees the credentials.
  Here they are the sender's claim, exactly as trustworthy as the loopback
  transport itself: any local process can already connect to the loopback
  ports that back Gesher's `AF_UNIX` sockets (a pre-existing property of
  0009). A datagram without Gesher's header is dropped, so a foreign sender
  cannot make a message appear without credentials. Broker attestation per
  message was not required. Verifying the claimed pid against the source
  port's owner (`GetExtendedUdpTable`) is possible where the sending socket
  was created by its sender, and is not done in phase 1.
- **Phase 1 as implemented:**
  - socketpair datagram pairs (`SOCK_SEQPACKET`, `SOCK_DGRAM`) are framed,
    with `SO_PASSCRED`, `SCM_CREDENTIALS` and `MSG_CTRUNC`;
  - `SO_PEERCRED` works on every socketpair;
  - `SO_PASSCRED` on stream pairs still fails with `ENOPROTOOPT`, until stream
    framing.

### 3. Descriptors: a broker for `SCM_RIGHTS` (and Binder)

`binderd` already opens Gesher processes with `PROCESS_DUP_HANDLE` and
duplicates handles between them for Binder (`DuplicateRemoteHandle`). It
becomes the descriptor broker:

- **Send:** for each fd in `SCM_RIGHTS`, the sender:
  1. registers the descriptor with the broker. The broker duplicates the
     Windows handle into itself; for a socket, it duplicates the `SOCKET`
     with the descriptor's Gesher state: type, family, `O_NONBLOCK`, options.
  2. receives a random 128-bit token;
  3. puts the token in the message header.

  An fd that cannot travel (an eventfd, an epoll fd, an inotify fd, the
  binder fd) fails the `sendmsg` with `EINVAL`. Nothing is sent.
- **Receive:** for each token, the receiver asks the broker. The broker
  duplicates the handle into the receiver; for a socket, it uses
  `WSADuplicateSocketW` for the receiver's pid, and the receiver creates the
  socket with `WSASocketW(FROM_PROTOCOL_INFO)` and registers it. The new fd
  number is put in the `SCM_RIGHTS` data. `MSG_CMSG_CLOEXEC` applies.
- **Lifetime:** as on Linux, in-flight descriptors stay valid until they are
  received or the message is discarded. The broker drops a token when it is
  claimed, when the receiving end discards a message (close of the last
  reference to the socket that held it), or when the sending process group is
  gone. Per-sender limits mirror Linux's: `SCM_MAX_FD` (253) per message and
  an in-flight limit (`ETOOMANYREFS`).
- **Binder:** `BINDER_TYPE_FD` objects for sockets use the same tokens, so a
  socket in a `ParcelFileDescriptor` reaches a service in another process.

### 4. Phases

1. Framing and credentials (sections 1 and 2): Firefox's and Opera's first
   calls succeed honestly.
2. The broker and `SCM_RIGHTS` for handle-backed fds and sockets (section 3).
3. Binder `BINDER_TYPE_FD` for sockets through the broker.

Each phase is a PR with contract tests:

- NDK programs on the installed Gesher, for sockets across processes:
  fork/exec children, sockets passed through Binder;
- byte-level stream/datagram boundary tests;
- the browsers as e2e evidence.

## Consequences

- Every `AF_UNIX` message pays a small header and, for descriptors, broker
  round trips. Throughput is measured against the input channel's needs
  (0027) before phase 1 is merged.
- Apps that check `SO_PEERCRED`/`SCM_CREDENTIALS` get the real pid, uid and
  gid of a Gesher sender. The trust boundary in section 2 is documented, and
  not silently equated with the kernel's.
- Descriptor kinds outside section 3's list keep failing explicitly. Each
  added kind needs its own transfer.

## Rejected

- **Accepting `SO_PASSCRED` without delivering credentials:** a fabricated
  capability. The browsers would fail one call later.
- **Pushing handles from the sender into the receiver:** the sender does not
  know which process holds the other end now. Sockets move through `exec`,
  Binder and `SCM_RIGHTS` itself.
- **Pulling handles straight from the sender's process with a claimed
  pid/handle pair:** a forged message could make the receiver duplicate, or
  close with `DUPLICATE_CLOSE_SOURCE`, arbitrary handles of another Gesher
  process. Tokens issued by the broker cannot be forged that way.
