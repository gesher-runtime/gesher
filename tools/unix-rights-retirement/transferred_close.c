// The receiving endpoint can itself be in flight on a separate Unix socket.
#include "rights_probe.h"

static int receive_endpoint(int socket) {
  struct pollfd ready = {.fd = socket, .events = POLLIN};
  if (poll(&ready, 1, 2000) != 1 || (ready.revents & POLLIN) == 0) return -1;

  char byte;
  int fd = -1;
  _Alignas(struct cmsghdr) char control[CMSG_SPACE(sizeof(fd))] = {0};
  struct iovec data = {.iov_base = &byte, .iov_len = 1};
  struct msghdr message = {
      .msg_iov = &data, .msg_iovlen = 1, .msg_control = control, .msg_controllen = sizeof(control),
  };
  ssize_t n = recvmsg(socket, &message, MSG_DONTWAIT);
  struct cmsghdr* rights = CMSG_FIRSTHDR(&message);
  if (n != 1 || byte != 'r' || (message.msg_flags & MSG_CTRUNC) != 0 ||
      !rights || rights->cmsg_level != SOL_SOCKET || rights->cmsg_type != SCM_RIGHTS ||
      rights->cmsg_len != CMSG_LEN(sizeof(fd))) return -1;
  memcpy(&fd, CMSG_DATA(rights), sizeof(fd));
  return fd;
}

static void test(int type) {
  int pair[2];
  int carrier[2];
  int pipefd[2];
  if (socketpair(AF_UNIX, type, 0, pair) != 0) {
    check("create the Unix socket pair", 0);
    return;
  }
  if (socketpair(AF_UNIX, SOCK_STREAM, 0, carrier) != 0) {
    check("create the carrier Unix socket pair", 0);
    close_pair(pair);
    return;
  }
  if (pipe2(pipefd, O_NONBLOCK | O_CLOEXEC) != 0) {
    check("create the nonblocking pipe", 0);
    close_pair(carrier);
    close_pair(pair);
    return;
  }

  int writer_queued = check("queue the sole pipe writer", send_fd(pair[0], 'q', pipefd[1]));
  close(pipefd[1]);
  int receiver_queued = writer_queued &&
      check("queue the receiving endpoint on the carrier", send_fd(carrier[0], 'r', pair[1]));
  close(pair[1]);
  if (receiver_queued) {
    check_pipe(pipefd[0], "the receiver in transit keeps the queued writer alive", 0);
    int receiver = receive_endpoint(carrier[1]);
    if (check("receive the endpoint from the carrier", receiver >= 0)) {
      check_pipe(pipefd[0], "the transferred receiver keeps the queued writer alive", 0);
      close(receiver);
      check_pipe(pipefd[0], "last transferred close releases the writer", 1);
    }
  }
  close_pair(carrier);
  close(pair[0]);
  close(pipefd[0]);
}

int main(void) { return run_probes(test); }
