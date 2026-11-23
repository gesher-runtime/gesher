// IP_PKTINFO on an IPv4 UDP socket (Linux ip(7)): setsockopt/getsockopt, and
// recvmsg delivering each datagram's in_pktinfo (interface, destination and
// local address) as an IPPROTO_IP control message. libcoap (the Aliyun IoT
// SDK's CoAP) refuses to start its network without it.

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static int g_checks = 0;
static int g_failures = 0;

#define CHECK(cond)                                                          \
  do {                                                                       \
    ++g_checks;                                                              \
    if (!(cond)) {                                                           \
      ++g_failures;                                                          \
      printf("FAIL line %d: %s (errno=%d)\n", __LINE__, #cond, errno);       \
    }                                                                        \
  } while (0)

// Sends one datagram to `fd`'s own address and receives it with recvmsg.
// Returns the in_pktinfo control message's presence; *info receives it.
static bool SendAndReceive(int fd, const sockaddr_in& self, in_pktinfo* info, bool* truncated,
                           size_t control_size) {
  CHECK(sendto(fd, "ping", 4, 0, reinterpret_cast<const sockaddr*>(&self), sizeof(self)) == 4);
  char data[16] = {};
  iovec io = {data, sizeof(data)};
  alignas(cmsghdr) unsigned char control[256] = {};
  sockaddr_in peer = {};
  msghdr message = {};
  message.msg_name = &peer;
  message.msg_namelen = sizeof(peer);
  message.msg_iov = &io;
  message.msg_iovlen = 1;
  message.msg_control = control;
  message.msg_controllen = control_size;
  const ssize_t n = recvmsg(fd, &message, 0);
  CHECK(n == 4 && memcmp(data, "ping", 4) == 0);
  CHECK(peer.sin_family == AF_INET && peer.sin_port == self.sin_port);
  *truncated = (message.msg_flags & MSG_CTRUNC) != 0;
  bool found = false;
  for (cmsghdr* c = CMSG_FIRSTHDR(&message); c != nullptr; c = CMSG_NXTHDR(&message, c)) {
    if (c->cmsg_level == IPPROTO_IP && c->cmsg_type == IP_PKTINFO) {
      CHECK(c->cmsg_len == CMSG_LEN(sizeof(in_pktinfo)));
      memcpy(info, CMSG_DATA(c), sizeof(*info));
      found = true;
    }
  }
  return found;
}

int main() {
  int fd = socket(AF_INET, SOCK_DGRAM, 0);
  CHECK(fd >= 0);
  sockaddr_in self = {};
  self.sin_family = AF_INET;
  self.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  CHECK(bind(fd, reinterpret_cast<sockaddr*>(&self), sizeof(self)) == 0);
  socklen_t len = sizeof(self);
  CHECK(getsockname(fd, reinterpret_cast<sockaddr*>(&self), &len) == 0);

  int value = -1;
  socklen_t value_len = sizeof(value);
  CHECK(getsockopt(fd, IPPROTO_IP, IP_PKTINFO, &value, &value_len) == 0 && value == 0);

  in_pktinfo info = {};
  bool truncated = false;
  CHECK(!SendAndReceive(fd, self, &info, &truncated, 256));  // off: no control message

  int on = 1;
  CHECK(setsockopt(fd, IPPROTO_IP, IP_PKTINFO, &on, sizeof(on)) == 0);
  value_len = sizeof(value);
  CHECK(getsockopt(fd, IPPROTO_IP, IP_PKTINFO, &value, &value_len) == 0 && value == 1);
  memset(&info, 0, sizeof(info));
  CHECK(SendAndReceive(fd, self, &info, &truncated, 256));
  CHECK(!truncated);
  CHECK(info.ipi_ifindex > 0);
  CHECK(info.ipi_addr.s_addr == htonl(INADDR_LOOPBACK));
  CHECK(info.ipi_spec_dst.s_addr == htonl(INADDR_LOOPBACK));

  // A control buffer too small for it: MSG_CTRUNC, as Linux's put_cmsg.
  CHECK(!SendAndReceive(fd, self, &info, &truncated, sizeof(cmsghdr)));
  CHECK(truncated);

  int off = 0;
  CHECK(setsockopt(fd, IPPROTO_IP, IP_PKTINFO, &off, sizeof(off)) == 0);
  CHECK(!SendAndReceive(fd, self, &info, &truncated, 256));
  close(fd);

  // A stream socket accepts the option, as on Linux.
  int tcp = socket(AF_INET, SOCK_STREAM, 0);
  CHECK(tcp >= 0 && setsockopt(tcp, IPPROTO_IP, IP_PKTINFO, &on, sizeof(on)) == 0);
  close(tcp);

  printf("udp pktinfo contracts: %d checks, %d failures\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
