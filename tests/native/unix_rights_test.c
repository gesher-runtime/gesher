// SCM_RIGHTS on AF_UNIX socketpairs (ADR 0088 section 3), from an x86-64
// Android library through the bionic shim, with binderd's descriptor broker
// ($GESHER_BINDER): pipes, memfds and sockets sent on SOCK_SEQPACKET and
// SOCK_STREAM pairs arrive as new descriptors of the same open files and
// sockets; MSG_CTRUNC, the SCM_MAX_FD limit, descriptors that cannot travel,
// and a reader without control data behave as on Linux. On a SOCK_STREAM
// pair, descriptors come with the read that returns the first byte sent with
// them, never with bytes sent before them, also after the receiving end moved
// to another descriptor.
// Built by native-libs.yml as unix_rights_test.so; elf_load_test calls
// gesher_selftest().
#define _GNU_SOURCE 1
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <unistd.h>

static int failures;

static void check(const char* what, int ok) {
  printf("  %-64s %s\n", what, ok ? "ok" : "FAIL");
  if (!ok) ++failures;
}

static ssize_t send_fds(int fd, const char* data, size_t size, const int* fds, size_t count) {
  char control[CMSG_SPACE(sizeof(int) * 260)];
  memset(control, 0, sizeof(control));
  struct iovec io = {(void*)data, size};
  struct msghdr m;
  memset(&m, 0, sizeof(m));
  m.msg_iov = &io;
  m.msg_iovlen = 1;
  m.msg_control = control;
  m.msg_controllen = CMSG_SPACE(sizeof(int) * count);
  struct cmsghdr* c = CMSG_FIRSTHDR(&m);
  c->cmsg_level = SOL_SOCKET;
  c->cmsg_type = SCM_RIGHTS;
  c->cmsg_len = CMSG_LEN(sizeof(int) * count);
  memcpy(CMSG_DATA(c), fds, sizeof(int) * count);
  return sendmsg(fd, &m, 0);
}

// Receives into data and up to `room` descriptors (a control buffer for that
// many) with recvmsg's `how`; returns the byte count, *count the descriptors,
// *flags msg_flags.
static ssize_t recv_fds_how(int fd, char* data, size_t size, int* fds, size_t room,
                            size_t* count, int* flags, int how) {
  char control[CMSG_SPACE(sizeof(int) * 8)];
  memset(control, 0, sizeof(control));
  struct iovec io = {data, size};
  struct msghdr m;
  memset(&m, 0, sizeof(m));
  m.msg_iov = &io;
  m.msg_iovlen = 1;
  m.msg_control = room ? control : NULL;
  m.msg_controllen = room ? CMSG_SPACE(sizeof(int) * room) : 0;
  ssize_t n = recvmsg(fd, &m, how);
  *count = 0;
  *flags = m.msg_flags;
  if (n < 0) return n;
  for (struct cmsghdr* c = CMSG_FIRSTHDR(&m); c != NULL; c = CMSG_NXTHDR(&m, c)) {
    if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS) {
      size_t k = (c->cmsg_len - CMSG_LEN(0)) / sizeof(int);
      memcpy(fds + *count, CMSG_DATA(c), k * sizeof(int));
      *count += k;
    }
  }
  return n;
}

static ssize_t recv_fds(int fd, char* data, size_t size, int* fds, size_t room, size_t* count,
                        int* flags) {
  return recv_fds_how(fd, data, size, fds, room, count, flags, 0);
}

static void close_all(const int* fds, size_t count) {
  for (size_t i = 0; i < count; ++i) close(fds[i]);
}

static void test_pair(int type, const char* name) {
  char what[128];
  int sv[2];
  snprintf(what, sizeof(what), "%s: socketpair", name);
  check(what, socketpair(AF_UNIX, type, 0, sv) == 0);

  // A pipe and a memfd.
  int pipefd[2];
  check("pipe", pipe(pipefd) == 0);
  int memfd = (int)syscall(SYS_memfd_create, "rights", MFD_CLOEXEC | MFD_ALLOW_SEALING);
  check("memfd_create", memfd >= 0 && write(memfd, "sealed?", 7) == 7);
  check("seal the transferred file", fcntl(memfd, F_ADD_SEALS, F_SEAL_FUTURE_WRITE) == 0);
  int out[2] = {pipefd[1], memfd};
  snprintf(what, sizeof(what), "%s: sendmsg with two descriptors", name);
  check(what, send_fds(sv[0], "fds", 3, out, 2) == 3);
  char buf[64];
  int got[8];
  size_t count = 0;
  int flags = 0;
  ssize_t n = recv_fds(sv[1], buf, sizeof(buf), got, 2, &count, &flags);
  snprintf(what, sizeof(what), "%s: recvmsg gets the bytes and both descriptors", name);
  check(what, n == 3 && memcmp(buf, "fds", 3) == 0 && count == 2 && (flags & MSG_CTRUNC) == 0);
  if (count == 2) {
    check("they are new descriptors", got[0] != pipefd[1] && got[1] != memfd && got[0] >= 0 &&
                                          got[1] >= 0);
    check("the received pipe end writes to the same pipe",
          write(got[0], "pipe", 4) == 4 && read(pipefd[0], buf, 4) == 4 &&
              memcmp(buf, "pipe", 4) == 0);
    check("the received memfd is the same file",
          pread(got[1], buf, 7, 0) == 7 && memcmp(buf, "sealed?", 7) == 0);
    errno = 0;
    check("the received description obeys the original write seal",
          pwrite(got[1], "x", 1, 0) == -1 && errno == EPERM);
    close(got[0]);
    close(got[1]);
  }

  // A socket: one end of a SOCK_SEQPACKET pair keeps its datagram framing.
  int inner[2];
  check("inner SOCK_SEQPACKET pair", socketpair(AF_UNIX, SOCK_SEQPACKET, 0, inner) == 0);
  snprintf(what, sizeof(what), "%s: sendmsg with a socket", name);
  check(what, send_fds(sv[0], "s", 1, &inner[1], 1) == 1);
  close(inner[1]);
  n = recv_fds(sv[1], buf, sizeof(buf), got, 1, &count, &flags);
  check("recvmsg gets the socket", n == 1 && count == 1);
  if (count == 1) {
    check("the received socket is the same endpoint",
          send(inner[0], "datagram", 8, 0) == 8 && recv(got[0], buf, sizeof(buf), 0) == 8 &&
              memcmp(buf, "datagram", 8) == 0);
    int type_value = 0;
    socklen_t len = sizeof(type_value);
    check("and still a datagram socket", send(got[0], "back", 4, 0) == 4 &&
                                             recv(inner[0], buf, sizeof(buf), 0) == 4 &&
                                             memcmp(buf, "back", 4) == 0);
    (void)type_value;
    struct ucred peer = {0, 0, 0};
    len = sizeof(peer);
    check("and SO_PEERCRED is still the pair's creator",
          getsockopt(got[0], SOL_SOCKET, SO_PEERCRED, &peer, &len) == 0 &&
              len == sizeof(peer) && peer.pid == getpid() && peer.uid == getuid() &&
              peer.gid == getgid());
    close(got[0]);
  }
  close(inner[0]);

  // Room for one descriptor of two: MSG_CTRUNC, the other is closed.
  // A control buffer of CMSG_SPACE(sizeof(int)) (24 bytes) holds two: Linux
  // fits (msg_controllen - sizeof(cmsghdr)) / sizeof(int) descriptors.
  int extra = dup(pipefd[1]);
  int three[3] = {pipefd[1], extra, memfd};
  check("sendmsg with three descriptors", send_fds(sv[0], "t", 1, three, 3) == 1);
  n = recv_fds(sv[1], buf, sizeof(buf), got, 1, &count, &flags);
  snprintf(what, sizeof(what), "%s: room for two of three: two and MSG_CTRUNC", name);
  check(what, n == 1 && count == 2 && (flags & MSG_CTRUNC) != 0);
  for (size_t i = 0; i < count; ++i) close(got[i]);
  close(extra);

  // Descriptors that cannot travel, and an invalid one: nothing is sent.
  int ev = eventfd(0, EFD_CLOEXEC);
  errno = 0;
  snprintf(what, sizeof(what), "%s: an eventfd cannot travel (EINVAL)", name);
  check(what, send_fds(sv[0], "e", 1, &ev, 1) < 0 && errno == EINVAL);
  close(ev);
  int bad = 1000;
  errno = 0;
  snprintf(what, sizeof(what), "%s: a closed descriptor (EBADF)", name);
  check(what, send_fds(sv[0], "b", 1, &bad, 1) < 0 && errno == EBADF);
  int many[254];
  for (int i = 0; i < 254; ++i) many[i] = pipefd[1];
  errno = 0;
  snprintf(what, sizeof(what), "%s: more than SCM_MAX_FD (253) is EINVAL", name);
  check(what, send_fds(sv[0], "m", 1, many, 254) < 0 && errno == EINVAL);
  check("and nothing was sent", recv(sv[1], buf, sizeof(buf), MSG_DONTWAIT) < 0 && errno == EAGAIN);

  close(pipefd[0]);
  close(pipefd[1]);
  close(memfd);
  close(sv[0]);
  close(sv[1]);
}

// A datagram read without control data: its descriptors are discarded, as on
// Linux, and the next message's arrive with that message.
static void test_discard(void) {
  int sv[2];
  check("discard: SOCK_SEQPACKET pair", socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sv) == 0);
  int pipefd[2];
  check("pipe", pipe(pipefd) == 0);
  check("first message with a descriptor", send_fds(sv[0], "1", 1, &pipefd[1], 1) == 1);
  check("second message with a descriptor", send_fds(sv[0], "2", 1, &pipefd[1], 1) == 1);
  char buf[8];
  check("recv without control data reads the first", recv(sv[1], buf, sizeof(buf), 0) == 1 &&
                                                          buf[0] == '1');
  int got[8];
  size_t count = 0;
  int flags = 0;
  check("recvmsg reads the second with its descriptor",
        recv_fds(sv[1], buf, sizeof(buf), got, 1, &count, &flags) == 1 && buf[0] == '2' &&
            count == 1);
  if (count == 1) close(got[0]);
  close(pipefd[0]);
  close(pipefd[1]);
  close(sv[0]);
  close(sv[1]);
}

// SOCK_STREAM: Linux attaches descriptors to the first byte sent with them. A
// read that stops before that byte gets none, even when they were sent before
// it began (Gecko's IPC closes a channel that has descriptors left over at a
// message boundary); the read that returns the byte gets them, together with
// bytes before it; a read without control data discards them; and all of this
// holds after the receiving end has moved to another descriptor.
static void test_stream_order(void) {
  int sv[2];
  check("stream order: SOCK_STREAM pair", socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
  int pipefd[2];
  check("pipe", pipe(pipefd) == 0);
  char buf[16];
  int got[8];
  size_t count = 0;
  int flags = 0;

  check("ten bytes without descriptors", send(sv[0], "0123456789", 10, 0) == 10);
  check("then a byte with a descriptor", send_fds(sv[0], "X", 1, &pipefd[1], 1) == 1);
  ssize_t n = recv_fds_how(sv[1], buf, 10, got, 4, &count, &flags, MSG_WAITALL);
  check("a read of the ten bytes gets no descriptor",
        n == 10 && memcmp(buf, "0123456789", 10) == 0 && count == 0 &&
            (flags & MSG_CTRUNC) == 0);
  close_all(got, count);
  n = recv_fds(sv[1], buf, sizeof(buf), got, 4, &count, &flags);
  check("the read of the next byte gets it", n == 1 && buf[0] == 'X' && count == 1);
  close_all(got, count);

  check("two bytes, then a byte with a descriptor",
        send(sv[0], "pq", 2, 0) == 2 && send_fds(sv[0], "R", 1, &pipefd[1], 1) == 1);
  n = recv_fds_how(sv[1], buf, 3, got, 4, &count, &flags, MSG_WAITALL);
  check("one read of all three gets the descriptor",
        n == 3 && memcmp(buf, "pqR", 3) == 0 && count == 1);
  close_all(got, count);

  check("a byte with a descriptor", send_fds(sv[0], "Y", 1, &pipefd[1], 1) == 1);
  check("read() without control data reads it", read(sv[1], buf, 1) == 1 && buf[0] == 'Y');
  check("another byte with a descriptor", send_fds(sv[0], "Z", 1, &pipefd[1], 1) == 1);
  n = recv_fds(sv[1], buf, sizeof(buf), got, 4, &count, &flags);
  check("recvmsg gets that byte's descriptor only (the first was discarded)",
        n == 1 && buf[0] == 'Z' && count == 1);
  close_all(got, count);

  // The receiving end moves (SCM_RIGHTS over another pair) between reads.
  check("two bytes before the move", send(sv[0], "ab", 2, 0) == 2);
  check("one read before the move", recv(sv[1], buf, 1, MSG_WAITALL) == 1 && buf[0] == 'a');
  int carrier[2];
  check("a carrier pair", socketpair(AF_UNIX, SOCK_STREAM, 0, carrier) == 0);
  check("the receiving end travels", send_fds(carrier[0], "c", 1, &sv[1], 1) == 1);
  n = recv_fds(carrier[1], buf, sizeof(buf), got, 1, &count, &flags);
  check("and arrives as a new descriptor", n == 1 && count == 1);
  int moved = count == 1 ? got[0] : -1;
  close(sv[1]);
  check("a byte with a descriptor after the move", send_fds(sv[0], "W", 1, &pipefd[1], 1) == 1);
  n = recv_fds_how(moved, buf, 1, got, 4, &count, &flags, MSG_WAITALL);
  check("the moved end reads the byte before it without a descriptor",
        n == 1 && buf[0] == 'b' && count == 0);
  close_all(got, count);
  n = recv_fds(moved, buf, sizeof(buf), got, 4, &count, &flags);
  check("and the byte sent with it with the descriptor", n == 1 && buf[0] == 'W' && count == 1);
  close_all(got, count);

  close(moved);
  close(carrier[0]);
  close(carrier[1]);
  close(pipefd[0]);
  close(pipefd[1]);
  close(sv[0]);
}

int gesher_selftest(void) {
  printf("unix_rights: SCM_RIGHTS through the descriptor broker\n");
  const char* device = getenv("GESHER_BINDER");
  if (device == NULL || device[0] == '\0') {
    // No broker: SCM_RIGHTS fails explicitly, and nothing is sent.
    int sv[2], pipefd[2];
    char buf[8];
    check("no broker: socketpair and pipe",
          socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0 && pipe(pipefd) == 0);
    errno = 0;
    check("no broker: SCM_RIGHTS fails with EOPNOTSUPP",
          send_fds(sv[0], "x", 1, &pipefd[1], 1) < 0 && errno == EOPNOTSUPP);
    check("no broker: nothing was sent",
          recv(sv[1], buf, sizeof(buf), MSG_DONTWAIT) < 0 && errno == EAGAIN);
    if (failures == 0) printf("unix_rights: all passed (without a broker)\n");
    else printf("unix_rights: %d check(s) failed\n", failures);
    fflush(stdout);
    return failures;
  }
  test_pair(SOCK_SEQPACKET, "SOCK_SEQPACKET");
  test_pair(SOCK_STREAM, "SOCK_STREAM");
  test_discard();
  test_stream_order();
  if (failures == 0) printf("unix_rights: all passed\n");
  else printf("unix_rights: %d check(s) failed\n", failures);
  fflush(stdout);
  return failures;
}
