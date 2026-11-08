// Shared ELF/host regression: unsupported control must not leak a payload to
// the peer. The host uses actual production adapters over a real socket pair.
#include <poll.h>
#include <sys/socket.h>
#include <sys/uio.h>

static void socket_control_expect_empty(int fd) {
  struct pollfd ready = {fd, POLLIN, 0};
  check("rejected control leaves no message readable", poll(&ready, 1, 50) == 0);
  char received[32];
  errno = 0;
  ssize_t result = recv(fd, received, sizeof(received), MSG_DONTWAIT);
  check("rejected control transmits no payload", result == -1 &&
        (errno == EAGAIN || errno == EWOULDBLOCK));
}

static void socket_control_expect_payload(int fd) {
  struct pollfd ready = {fd, POLLIN, 0};
  check("ordinary sendmsg makes the peer readable", poll(&ready, 1, 1000) == 1 &&
        (ready.revents & POLLIN) != 0);
  char received[32];
  ssize_t result = recv(fd, received, sizeof(received), MSG_DONTWAIT);
  check("ordinary sendmsg preserves scatter/gather payload", result == 7 &&
        memcmp(received, "payload", 7) == 0);
}

static void test_socket_control_type(int type) {
  typedef ssize_t (*sendmsg_fn)(int, const struct msghdr*, int);
  typedef int (*sendmmsg_fn)(int, struct mmsghdr*, unsigned int, int);
  sendmsg_fn send_message = (sendmsg_fn)dlsym(RTLD_DEFAULT, "sendmsg");
  sendmmsg_fn send_messages = (sendmmsg_fn)dlsym(RTLD_DEFAULT, "sendmmsg");
  check("sendmsg and sendmmsg runtime exports exist", send_message != NULL && send_messages != NULL);
  if (send_message == NULL || send_messages == NULL) return;
  int pair[2];
  int opened = socketpair(AF_UNIX, type, 0, pair);
  check("socket control fixture opens socketpair", opened == 0);
  if (opened != 0) return;

  char first[] = "pay", second[] = "load";
  struct iovec payload[2] = {{first, 3}, {second, 4}};
  union {
    struct cmsghdr alignment;
    unsigned char bytes[CMSG_SPACE(sizeof(int))];
  } control;
  memset(&control, 0, sizeof(control));
  struct msghdr message;
  memset(&message, 0, sizeof(message));
  message.msg_iov = payload;
  message.msg_iovlen = 2;
  message.msg_control = control.bytes;
  message.msg_controllen = sizeof(control.bytes);
  struct cmsghdr* header = CMSG_FIRSTHDR(&message);
  header->cmsg_len = CMSG_LEN(sizeof(int));
  header->cmsg_level = SOL_SOCKET;
  header->cmsg_type = SCM_RIGHTS;
  memcpy(CMSG_DATA(header), &pair[0], sizeof(int));
  errno = 0;
  check("SCM_RIGHTS send fails explicitly with EOPNOTSUPP",
        send_message(pair[0], &message, MSG_DONTWAIT) == -1 && errno == EOPNOTSUPP);
  socket_control_expect_empty(pair[1]);
  check("rejected SCM_RIGHTS retains the sender descriptor", fcntl(pair[0], F_GETFD) >= 0);

  struct msghdr ordinary = message;
  ordinary.msg_controllen = 0;
  check("zero control length permits a non-null control pointer",
        send_message(pair[0], &ordinary, MSG_DONTWAIT) == 7);
  socket_control_expect_payload(pair[1]);
  ordinary.msg_control = NULL;
  check("ordinary sendmsg without control still succeeds",
        send_message(pair[0], &ordinary, MSG_DONTWAIT) == 7);
  socket_control_expect_payload(pair[1]);

  struct mmsghdr batch[2];
  memset(batch, 0, sizeof(batch));
  batch[0].msg_hdr = ordinary;
  batch[1].msg_hdr = message;
  batch[0].msg_len = batch[1].msg_len = 0x11223344;
  check("sendmmsg returns only the completed prefix before control",
        send_messages(pair[0], batch, 2, MSG_DONTWAIT) == 1 &&
        batch[0].msg_len == 7 && batch[1].msg_len == 0x11223344);
  socket_control_expect_payload(pair[1]);
  socket_control_expect_empty(pair[1]);

  batch[0].msg_hdr = message;
  batch[1].msg_hdr = ordinary;
  batch[0].msg_len = batch[1].msg_len = 0x11223344;
  errno = 0;
  check("sendmmsg stops without a count or length for rejected first message",
        send_messages(pair[0], batch, 2, MSG_DONTWAIT) == -1 && errno == EOPNOTSUPP &&
        batch[0].msg_len == 0x11223344 && batch[1].msg_len == 0x11223344);
  socket_control_expect_empty(pair[1]);
  close(pair[0]);
  close(pair[1]);
}

static void test_socket_control_contracts(void) {
  printf("socket control contracts: stream\n");
  test_socket_control_type(SOCK_STREAM);
  printf("socket control contracts: datagram\n");
  test_socket_control_type(SOCK_DGRAM);
}
