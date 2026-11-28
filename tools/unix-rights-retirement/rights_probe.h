#ifndef GESHER_RIGHTS_PROBE_H
#define GESHER_RIGHTS_PROBE_H

#define _GNU_SOURCE 1
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>

static int failures;

static inline int check(const char* what, int ok) {
  printf("  %-64s %s\n", what, ok ? "ok" : "FAIL");
  failures += !ok;
  return ok;
}

static inline void close_pair(const int fds[2]) {
  close(fds[0]);
  close(fds[1]);
}

static inline int send_fd(int socket, char byte, int fd) {
  _Alignas(struct cmsghdr) char control[CMSG_SPACE(sizeof(fd))] = {0};
  struct iovec data = {.iov_base = &byte, .iov_len = 1};
  struct msghdr message = {
      .msg_iov = &data, .msg_iovlen = 1, .msg_control = control, .msg_controllen = sizeof(control),
  };
  struct cmsghdr* rights = CMSG_FIRSTHDR(&message);
  rights->cmsg_level = SOL_SOCKET;
  rights->cmsg_type = SCM_RIGHTS;
  rights->cmsg_len = CMSG_LEN(sizeof(fd));
  memcpy(CMSG_DATA(rights), &fd, sizeof(fd));
  return sendmsg(socket, &message, MSG_DONTWAIT) == 1;
}

static inline void check_pipe(int reader, const char* what, int expect_eof) {
  char byte;
  errno = 0;
  ssize_t result = read(reader, &byte, 1);
  int error = errno;
  char report[128];
  snprintf(report, sizeof(report), "%s (read=%zd, errno=%d)", what, result, error);
  check(report, expect_eof ? result == 0
                           : result == -1 && (error == EAGAIN || error == EWOULDBLOCK));
}

static inline int run_probes(void (*test)(int)) {
  static const struct {
    int type;
    const char* name;
  } pairs[] = {{SOCK_STREAM, "SOCK_STREAM"}, {SOCK_DGRAM, "SOCK_DGRAM"},
               {SOCK_SEQPACKET, "SOCK_SEQPACKET"}};
  for (size_t i = 0; i < sizeof(pairs) / sizeof(pairs[0]); ++i) {
    printf("%s\n", pairs[i].name);
    test(pairs[i].type);
  }
  return failures != 0;
}

#endif
