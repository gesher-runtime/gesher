// AF_UNIX socketpair credentials (ADR 0088), from an x86-64 Android library
// through the bionic shim: SO_PEERCRED, SO_PASSCRED and SCM_CREDENTIALS on a
// SOCK_SEQPACKET pair (gesher-base frames its datagrams), with the datagram
// semantics the framing must keep (boundaries, MSG_TRUNC, MSG_PEEK, FIONREAD,
// read/write/readv/writev), and what is not supported failing explicitly
// (SCM_RIGHTS, other credentials, SO_PASSCRED on a stream pair).
// Built by native-libs.yml as unix_credentials_test.so; elf_load_test calls
// gesher_selftest().
#define _GNU_SOURCE 1
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>

static int failures;

static void check(const char* what, int ok) {
  printf("  %-60s %s\n", what, ok ? "ok" : "FAIL");
  if (!ok) ++failures;
}

static int recv_with_credentials(int fd, char* data, size_t size, struct ucred* who,
                                 size_t control_size, int* msg_flags, size_t* control_len) {
  char control[CMSG_SPACE(sizeof(struct ucred)) + 16];
  memset(control, 0, sizeof(control));
  struct iovec io = {data, size};
  struct msghdr m;
  memset(&m, 0, sizeof(m));
  m.msg_iov = &io;
  m.msg_iovlen = 1;
  m.msg_control = control_size ? control : NULL;
  m.msg_controllen = control_size;
  int n = (int)recvmsg(fd, &m, 0);
  *msg_flags = m.msg_flags;
  *control_len = m.msg_controllen;
  memset(who, 0, sizeof(*who));
  if (n >= 0 && m.msg_controllen >= CMSG_LEN(sizeof(struct ucred))) {
    struct cmsghdr* c = CMSG_FIRSTHDR(&m);
    if (c != NULL && c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_CREDENTIALS &&
        c->cmsg_len == CMSG_LEN(sizeof(struct ucred))) {
      memcpy(who, CMSG_DATA(c), sizeof(*who));
    }
  }
  return n;
}

static ssize_t send_with_control(int fd, const char* data, size_t size, int level, int type,
                                 const void* payload, size_t payload_size) {
  char control[CMSG_SPACE(sizeof(struct ucred)) + CMSG_SPACE(sizeof(int))];
  memset(control, 0, sizeof(control));
  struct iovec io = {(void*)data, size};
  struct msghdr m;
  memset(&m, 0, sizeof(m));
  m.msg_iov = &io;
  m.msg_iovlen = 1;
  m.msg_control = control;
  m.msg_controllen = CMSG_SPACE(payload_size);
  struct cmsghdr* c = CMSG_FIRSTHDR(&m);
  c->cmsg_level = level;
  c->cmsg_type = type;
  c->cmsg_len = CMSG_LEN(payload_size);
  memcpy(CMSG_DATA(c), payload, payload_size);
  return sendmsg(fd, &m, 0);
}

int gesher_selftest(void) {
  printf("unix_credentials: AF_UNIX socketpair credentials (ADR 0088)\n");
  int sv[2];
  check("socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK)",
        socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK, 0, sv) == 0);
  const pid_t pid = getpid();
  const uid_t uid = getuid();
  const gid_t gid = getgid();

  struct ucred peer;
  socklen_t len = sizeof(peer);
  memset(&peer, 0, sizeof(peer));
  check("SO_PEERCRED is the pair's creator",
        getsockopt(sv[0], SOL_SOCKET, SO_PEERCRED, &peer, &len) == 0 && len == sizeof(peer) &&
            peer.pid == pid && peer.uid == uid && peer.gid == gid);
  int on = -1;
  len = sizeof(on);
  check("SO_PASSCRED is off by default",
        getsockopt(sv[1], SOL_SOCKET, SO_PASSCRED, &on, &len) == 0 && on == 0);
  on = 1;
  check("setsockopt(SO_PASSCRED, 1)", setsockopt(sv[1], SOL_SOCKET, SO_PASSCRED, &on, sizeof(on)) == 0);
  on = -1;
  len = sizeof(on);
  check("SO_PASSCRED reads back as 1",
        getsockopt(sv[1], SOL_SOCKET, SO_PASSCRED, &on, &len) == 0 && on == 1);

  char buf[64];
  struct ucred who;
  int flags = 0;
  size_t control_len = 0;
  check("send a datagram", send(sv[0], "hello", 5, 0) == 5);
  int avail = -1;
  check("FIONREAD counts its payload only", ioctl(sv[1], FIONREAD, &avail) == 0 && avail == 5);
  int n = recv_with_credentials(sv[1], buf, sizeof(buf), &who, CMSG_SPACE(sizeof(struct ucred)),
                                &flags, &control_len);
  check("recvmsg returns the payload", n == 5 && memcmp(buf, "hello", 5) == 0);
  check("SCM_CREDENTIALS carries the sender", who.pid == pid && who.uid == uid && who.gid == gid);
  check("its control length and no flags", control_len == CMSG_SPACE(sizeof(struct ucred)) && flags == 0);

  check("send to the end without SO_PASSCRED", send(sv[1], "abc", 3, 0) == 3);
  n = recv_with_credentials(sv[0], buf, sizeof(buf), &who, CMSG_SPACE(sizeof(struct ucred)),
                            &flags, &control_len);
  check("no credentials without SO_PASSCRED", n == 3 && control_len == 0 && who.pid == 0);

  check("send for a too-small control buffer", send(sv[0], "x", 1, 0) == 1);
  n = recv_with_credentials(sv[1], buf, sizeof(buf), &who, 8, &flags, &control_len);
  check("a too-small control buffer is MSG_CTRUNC", n == 1 && (flags & MSG_CTRUNC) && control_len == 0);

  check("two datagrams", send(sv[0], "one", 3, 0) == 3 && send(sv[0], "second", 6, 0) == 6);
  check("keep their boundaries",
        recv(sv[1], buf, sizeof(buf), 0) == 3 && recv(sv[1], buf, sizeof(buf), 0) == 6 &&
            memcmp(buf, "second", 6) == 0);

  check("an empty datagram is sent", send(sv[0], "", 0, 0) == 0);
  check("and received as one", recv(sv[1], buf, sizeof(buf), 0) == 0);

  check("send 10 bytes", send(sv[0], "0123456789", 10, 0) == 10);
  check("MSG_PEEK sees them", recv(sv[1], buf, sizeof(buf), MSG_PEEK) == 10);
  check("MSG_TRUNC returns the datagram's length",
        recv(sv[1], buf, 4, MSG_TRUNC) == 10 && memcmp(buf, "0123", 4) == 0);
  check("send 10 bytes again", send(sv[0], "0123456789", 10, 0) == 10);
  n = recv_with_credentials(sv[1], buf, 4, &who, CMSG_SPACE(sizeof(struct ucred)), &flags, &control_len);
  check("a short buffer is MSG_TRUNC, the rest discarded",
        n == 4 && (flags & MSG_TRUNC) && recv(sv[1], buf, sizeof(buf), 0) < 0 && errno == EAGAIN);

  check("write()", write(sv[0], "wr", 2) == 2);
  check("read()", read(sv[1], buf, sizeof(buf)) == 2 && memcmp(buf, "wr", 2) == 0);
  struct iovec out[2] = {{(void*)"ab", 2}, {(void*)"cd", 2}};
  check("writev()", writev(sv[0], out, 2) == 4);
  char p1[3] = {0}, p2[8] = {0};
  struct iovec in[2] = {{p1, 3}, {p2, sizeof(p2)}};
  check("readv() scatters the payload", readv(sv[1], in, 2) == 4 && memcmp(p1, "abc", 3) == 0 && p2[0] == 'd');

  struct ucred self = {pid, uid, gid};
  check("sendmsg with its own SCM_CREDENTIALS",
        send_with_control(sv[0], "me", 2, SOL_SOCKET, SCM_CREDENTIALS, &self, sizeof(self)) == 2);
  n = recv_with_credentials(sv[1], buf, sizeof(buf), &who, CMSG_SPACE(sizeof(struct ucred)),
                            &flags, &control_len);
  check("arrives with them", n == 2 && who.pid == pid);
  struct ucred other = {pid + 1, uid, gid};
  errno = 0;
  check("another process's credentials are EPERM",
        send_with_control(sv[0], "no", 2, SOL_SOCKET, SCM_CREDENTIALS, &other, sizeof(other)) < 0 &&
            errno == EPERM);
  int fd = sv[0];
  errno = 0;
  check("SCM_RIGHTS is not supported yet (EOPNOTSUPP)",
        send_with_control(sv[0], "fd", 2, SOL_SOCKET, SCM_RIGHTS, &fd, sizeof(fd)) < 0 &&
            errno == EOPNOTSUPP);
  errno = 0;
  check("and nothing was sent", recv(sv[1], buf, sizeof(buf), 0) < 0 && errno == EAGAIN);
  close(sv[0]);
  close(sv[1]);

  int st[2];
  check("socketpair(AF_UNIX, SOCK_STREAM)", socketpair(AF_UNIX, SOCK_STREAM, 0, st) == 0);
  memset(&peer, 0, sizeof(peer));
  len = sizeof(peer);
  check("a stream pair's SO_PEERCRED is its creator",
        getsockopt(st[1], SOL_SOCKET, SO_PEERCRED, &peer, &len) == 0 && peer.pid == pid);
  on = 1;
  errno = 0;
  check("SO_PASSCRED on a stream pair is not supported yet",
        setsockopt(st[0], SOL_SOCKET, SO_PASSCRED, &on, sizeof(on)) < 0 && errno == ENOPROTOOPT);
  check("a stream pair still carries bytes",
        write(st[0], "stream", 6) == 6 && read(st[1], buf, sizeof(buf)) == 6 && memcmp(buf, "stream", 6) == 0);
  close(st[0]);
  close(st[1]);

  printf(failures == 0 ? "unix_credentials: all passed\n" : "unix_credentials: %d failed\n", failures);
  return failures;
}
