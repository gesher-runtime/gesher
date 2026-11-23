// IP_PKTINFO on an IPv4 UDP socket (Linux ip(7)): setsockopt/getsockopt, and
// recvmsg delivering each datagram's in_pktinfo (interface, destination and
// local address) as an IPPROTO_IP control message. libcoap (the Aliyun IoT
// SDK's CoAP) refuses to start its network without it.

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include <initializer_list>

// Gesher's socket transfer (internal.h: SCM_RIGHTS through binderd), and the
// Win32 calls that stand in for binderd's handle duplication.
namespace gesher {
bool TransferSocket(int fd, uintptr_t* socket, uint64_t* metadata);
int AdoptSocket(uintptr_t socket, uint64_t metadata);
}  // namespace gesher
extern "C" __declspec(dllimport) void* __stdcall GetCurrentProcess(void);
extern "C" __declspec(dllimport) int __stdcall DuplicateHandle(void*, void*, void*, void**,
                                                               unsigned long, int, unsigned long);

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

// What one recvmsg returned: the IP_PKTINFO control message, if any.
struct Received {
  bool ok = false;           // the datagram itself came back intact
  bool found = false;        // an IPPROTO_IP/IP_PKTINFO header
  bool truncated = false;    // MSG_CTRUNC
  size_t cmsg_len = 0;       // that header's cmsg_len
  size_t controllen = 0;     // msg_controllen after the call
  in_pktinfo info = {};      // as much of it as was delivered
};

// Receives one datagram on `fd` with a control buffer of `control_size`.
static Received ReceiveOne(int fd, size_t control_size, int flags = 0) {
  Received result;
  char data[16] = {};
  iovec io = {data, sizeof(data)};
  alignas(cmsghdr) unsigned char control[256];
  memset(control, 0xa5, sizeof(control));
  sockaddr_storage peer = {};
  msghdr message = {};
  message.msg_name = &peer;
  message.msg_namelen = sizeof(peer);
  message.msg_iov = &io;
  message.msg_iovlen = 1;
  message.msg_control = control;
  message.msg_controllen = control_size;
  const ssize_t n = recvmsg(fd, &message, flags);
  result.ok = n == 4 && memcmp(data, "ping", 4) == 0;
  result.truncated = (message.msg_flags & MSG_CTRUNC) != 0;
  result.controllen = message.msg_controllen;
  for (cmsghdr* c = CMSG_FIRSTHDR(&message); c != nullptr; c = CMSG_NXTHDR(&message, c)) {
    if (c->cmsg_level != IPPROTO_IP || c->cmsg_type != IP_PKTINFO) continue;
    result.found = true;
    result.cmsg_len = c->cmsg_len;
    size_t delivered = c->cmsg_len > CMSG_LEN(0) ? c->cmsg_len - CMSG_LEN(0) : 0;
    if (delivered > sizeof(result.info)) delivered = sizeof(result.info);
    memcpy(&result.info, CMSG_DATA(c), delivered);
  }
  return result;
}

// Whether a datagram is waiting on `fd` within `ms` milliseconds: receives
// that may never be answered (a broadcast or multicast datagram the host's
// firewall drops) wait no longer than that.
static bool Readable(int fd, int ms) {
  pollfd p = {fd, POLLIN, 0};
  return poll(&p, 1, ms) == 1 && (p.revents & POLLIN) != 0;
}

static bool SendTo(int fd, const sockaddr_in& to) {
  return sendto(fd, "ping", 4, 0, reinterpret_cast<const sockaddr*>(&to), sizeof(to)) == 4;
}

// Sends one datagram to `fd`'s own address and receives it with recvmsg.
static Received SendAndReceive(int fd, const sockaddr_in& self, size_t control_size) {
  CHECK(SendTo(fd, self));
  Received result = ReceiveOne(fd, control_size);
  CHECK(result.ok);
  return result;
}

static int PacketInfoOption(int fd) {
  int value = -1;
  socklen_t len = sizeof(value);
  if (getsockopt(fd, IPPROTO_IP, IP_PKTINFO, &value, &len) != 0) return -1;
  return value;
}

static void TestLoopback() {
  int fd = socket(AF_INET, SOCK_DGRAM, 0);
  CHECK(fd >= 0);
  sockaddr_in self = {};
  self.sin_family = AF_INET;
  self.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  CHECK(bind(fd, reinterpret_cast<sockaddr*>(&self), sizeof(self)) == 0);
  socklen_t len = sizeof(self);
  CHECK(getsockname(fd, reinterpret_cast<sockaddr*>(&self), &len) == 0);

  CHECK(PacketInfoOption(fd) == 0);
  CHECK(!SendAndReceive(fd, self, 256).found);  // off: no control message

  int on = 1;
  CHECK(setsockopt(fd, IPPROTO_IP, IP_PKTINFO, &on, sizeof(on)) == 0);
  CHECK(PacketInfoOption(fd) == 1);
  Received r = SendAndReceive(fd, self, 256);
  CHECK(r.found && !r.truncated);
  CHECK(r.cmsg_len == CMSG_LEN(sizeof(in_pktinfo)));
  CHECK(r.controllen == CMSG_SPACE(sizeof(in_pktinfo)));
  CHECK(r.info.ipi_ifindex > 0);
  CHECK(r.info.ipi_addr.s_addr == htonl(INADDR_LOOPBACK));
  CHECK(r.info.ipi_spec_dst.s_addr == htonl(INADDR_LOOPBACK));

  // Control buffers too small for it, as Linux's put_cmsg: room for the
  // header only gives the header, cmsg_len cut to the room; room for part of
  // the data gives that part; less than a header gives nothing. MSG_CTRUNC.
  r = SendAndReceive(fd, self, sizeof(cmsghdr));
  CHECK(r.found && r.truncated && r.cmsg_len == sizeof(cmsghdr) && r.controllen == sizeof(cmsghdr));
  const size_t partial = CMSG_LEN(sizeof(int) + sizeof(in_addr));  // ifindex and spec_dst
  r = SendAndReceive(fd, self, partial);
  CHECK(r.found && r.truncated && r.cmsg_len == partial && r.controllen == partial);
  CHECK(r.info.ipi_ifindex > 0 && r.info.ipi_spec_dst.s_addr == htonl(INADDR_LOOPBACK));
  r = SendAndReceive(fd, self, sizeof(cmsghdr) - 1);
  CHECK(!r.found && r.truncated && r.controllen == 0);

  // The option travels with the socket (SCM_RIGHTS through binderd).
  uintptr_t handle = 0;
  uint64_t metadata = 0;
  CHECK(gesher::TransferSocket(fd, &handle, &metadata));
  void* copy = nullptr;
  CHECK(DuplicateHandle(GetCurrentProcess(), reinterpret_cast<void*>(handle), GetCurrentProcess(),
                        &copy, 0, 0, 2 /* DUPLICATE_SAME_ACCESS */) != 0);
  const int adopted = gesher::AdoptSocket(reinterpret_cast<uintptr_t>(copy), metadata);
  CHECK(adopted >= 0);
  if (adopted >= 0) {
    CHECK(PacketInfoOption(adopted) == 1);
    CHECK(SendTo(fd, self));
    r = ReceiveOne(adopted, 256);
    CHECK(r.ok && r.found && r.info.ipi_addr.s_addr == htonl(INADDR_LOOPBACK));
    close(adopted);
  }

  // MSG_PEEK returns the datagram and its in_pktinfo and leaves it queued:
  // the next recvmsg gets both again.
  CHECK(SendTo(fd, self));
  CHECK(Readable(fd, 2000));
  r = ReceiveOne(fd, 256, MSG_PEEK);
  CHECK(r.ok && r.found && r.info.ipi_addr.s_addr == htonl(INADDR_LOOPBACK) &&
        r.info.ipi_spec_dst.s_addr == htonl(INADDR_LOOPBACK));
  r = ReceiveOne(fd, 256, MSG_PEEK);
  CHECK(r.ok && r.found);
  r = ReceiveOne(fd, 256);
  CHECK(r.ok && r.found && r.info.ipi_addr.s_addr == htonl(INADDR_LOOPBACK));

  // Nothing queued: a non-blocking receive fails with EAGAIN, per call
  // (MSG_DONTWAIT) and per descriptor (O_NONBLOCK).
  char data[16];
  iovec io = {data, sizeof(data)};
  alignas(cmsghdr) unsigned char control[256];
  msghdr message = {};
  message.msg_iov = &io;
  message.msg_iovlen = 1;
  message.msg_control = control;
  message.msg_controllen = sizeof(control);
  errno = 0;
  CHECK(recvmsg(fd, &message, MSG_DONTWAIT) == -1 && errno == EAGAIN);
  const int fl = fcntl(fd, F_GETFL);
  CHECK(fl >= 0 && fcntl(fd, F_SETFL, fl | O_NONBLOCK) == 0);
  message.msg_controllen = sizeof(control);
  errno = 0;
  CHECK(recvmsg(fd, &message, 0) == -1 && errno == EAGAIN);
  errno = 0;
  CHECK(recv(fd, data, sizeof(data), 0) == -1 && errno == EAGAIN);
  errno = 0;
  CHECK(read(fd, data, sizeof(data)) == -1 && errno == EAGAIN);
  CHECK(fcntl(fd, F_SETFL, fl) == 0);

  // Receives with no control buffer to fill still return the datagram.
  CHECK(SendTo(fd, self));
  CHECK(Readable(fd, 2000) && recv(fd, data, sizeof(data), 0) == 4 && memcmp(data, "ping", 4) == 0);
  CHECK(SendTo(fd, self));
  CHECK(Readable(fd, 2000) && read(fd, data, sizeof(data)) == 4 && memcmp(data, "ping", 4) == 0);
  sockaddr_in from = {};
  socklen_t from_len = sizeof(from);
  CHECK(SendTo(fd, self));
  CHECK(Readable(fd, 2000) &&
        recvfrom(fd, data, sizeof(data), 0, reinterpret_cast<sockaddr*>(&from), &from_len) == 4 &&
        from.sin_port == self.sin_port);
  CHECK(!Readable(fd, 0));

  // The option's length, as Linux's do_ip_setsockopt: an int, else a byte,
  // else (optlen 0, the value not read) off.
  const unsigned char byte_on[3] = {1, 0, 0};
  CHECK(setsockopt(fd, IPPROTO_IP, IP_PKTINFO, nullptr, 0) == 0 && PacketInfoOption(fd) == 0);
  CHECK(setsockopt(fd, IPPROTO_IP, IP_PKTINFO, byte_on, 1) == 0 && PacketInfoOption(fd) == 1);
  CHECK(setsockopt(fd, IPPROTO_IP, IP_PKTINFO, &on, 0) == 0 && PacketInfoOption(fd) == 0);
  CHECK(setsockopt(fd, IPPROTO_IP, IP_PKTINFO, byte_on, 3) == 0 && PacketInfoOption(fd) == 1);
  errno = 0;
  CHECK(setsockopt(fd, IPPROTO_IP, IP_PKTINFO, nullptr, sizeof(on)) == -1 && errno == EFAULT);
  CHECK(PacketInfoOption(fd) == 1);

  int off = 0;
  CHECK(setsockopt(fd, IPPROTO_IP, IP_PKTINFO, &off, sizeof(off)) == 0);
  CHECK(!SendAndReceive(fd, self, 256).found);
  // ...and its absence too.
  CHECK(gesher::TransferSocket(fd, &handle, &metadata));
  copy = nullptr;
  CHECK(DuplicateHandle(GetCurrentProcess(), reinterpret_cast<void*>(handle), GetCurrentProcess(),
                        &copy, 0, 0, 2) != 0);
  const int plain = gesher::AdoptSocket(reinterpret_cast<uintptr_t>(copy), metadata);
  CHECK(plain >= 0 && PacketInfoOption(plain) == 0);
  if (plain >= 0) close(plain);
  close(fd);
}

// A dual-stack IPv6 socket's IPv4 datagrams carry in_pktinfo too (Linux's
// udpv6_recvmsg, ip_cmsg_recv for v4-mapped ones); IPv6 ones do not.
static void TestDualStack() {
  int fd = socket(AF_INET6, SOCK_DGRAM, 0);
  CHECK(fd >= 0);
  int off = 0, on = 1;
  CHECK(setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &off, sizeof(off)) == 0);
  sockaddr_in6 self = {};
  self.sin6_family = AF_INET6;
  CHECK(bind(fd, reinterpret_cast<sockaddr*>(&self), sizeof(self)) == 0);
  socklen_t len = sizeof(self);
  CHECK(getsockname(fd, reinterpret_cast<sockaddr*>(&self), &len) == 0);
  CHECK(setsockopt(fd, IPPROTO_IP, IP_PKTINFO, &on, sizeof(on)) == 0);
  CHECK(PacketInfoOption(fd) == 1);

  int v4 = socket(AF_INET, SOCK_DGRAM, 0);
  sockaddr_in to = {};
  to.sin_family = AF_INET;
  to.sin_port = self.sin6_port;
  to.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  CHECK(SendTo(v4, to));
  Received r = ReceiveOne(fd, 256);
  CHECK(r.ok && r.found && !r.truncated);
  CHECK(r.info.ipi_ifindex > 0);
  CHECK(r.info.ipi_addr.s_addr == htonl(INADDR_LOOPBACK));
  CHECK(r.info.ipi_spec_dst.s_addr == htonl(INADDR_LOOPBACK));
  close(v4);

  int v6 = socket(AF_INET6, SOCK_DGRAM, 0);
  sockaddr_in6 to6 = self;
  to6.sin6_addr = in6addr_loopback;
  if (v6 >= 0 && sendto(v6, "ping", 4, 0, reinterpret_cast<sockaddr*>(&to6), sizeof(to6)) == 4) {
    r = ReceiveOne(fd, 256);
    CHECK(r.ok && !r.found);
  } else {
    printf("note: no IPv6 loopback; IPv6 datagram case skipped\n");
  }
  if (v6 >= 0) close(v6);
  close(fd);
}

// A broadcast datagram's ipi_spec_dst is the local address facing its sender
// (Linux's fib_compute_spec_dst), not the broadcast address: for the limited
// broadcast and a local subnet's directed broadcast alike.
static void TestBroadcast() {
  ifaddrs* list = nullptr;
  if (getifaddrs(&list) != 0) {
    printf("note: getifaddrs failed; broadcast cases skipped\n");
    return;
  }
  in_addr local = {}, mask = {};
  unsigned index = 0;
  for (ifaddrs* a = list; a != nullptr; a = a->ifa_next) {
    if (a->ifa_addr == nullptr || a->ifa_netmask == nullptr || a->ifa_addr->sa_family != AF_INET) continue;
    if ((a->ifa_flags & IFF_LOOPBACK) != 0 || (a->ifa_flags & IFF_UP) == 0) continue;
    in_addr address = reinterpret_cast<sockaddr_in*>(a->ifa_addr)->sin_addr;
    in_addr netmask = reinterpret_cast<sockaddr_in*>(a->ifa_netmask)->sin_addr;
    if (ntohl(netmask.s_addr) >= 0xfffffffeu) continue;  // no broadcast address
    if ((ntohl(address.s_addr) >> 16) == 0xa9fe) continue;  // 169.254/16 (no DHCP lease)
    local = address;
    mask = netmask;
    index = if_nametoindex(a->ifa_name);
    break;
  }
  freeifaddrs(list);
  if (local.s_addr == 0) {
    printf("note: no IPv4 interface with a broadcast address; broadcast cases skipped\n");
    return;
  }
  int fd = socket(AF_INET, SOCK_DGRAM, 0);
  int on = 1;
  CHECK(setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &on, sizeof(on)) == 0);
  sockaddr_in self = {};
  self.sin_family = AF_INET;
  CHECK(bind(fd, reinterpret_cast<sockaddr*>(&self), sizeof(self)) == 0);
  socklen_t len = sizeof(self);
  CHECK(getsockname(fd, reinterpret_cast<sockaddr*>(&self), &len) == 0);
  CHECK(setsockopt(fd, IPPROTO_IP, IP_PKTINFO, &on, sizeof(on)) == 0);

  // Unicast to the interface's own address: ipi_spec_dst is that address.
  sockaddr_in to = self;
  to.sin_addr = local;
  CHECK(SendTo(fd, to));
  if (!Readable(fd, 2000)) {
    // Delivered within the host: no firewall rule applies to it.
    CHECK(!"unicast datagram to the interface's own address arrived within 2 s");
    close(fd);
    return;
  }
  Received r = ReceiveOne(fd, 256);
  CHECK(r.ok && r.found && r.info.ipi_addr.s_addr == local.s_addr &&
        r.info.ipi_spec_dst.s_addr == local.s_addr);

  in_addr directed = {};
  directed.s_addr = local.s_addr | ~mask.s_addr;
  in_addr limited = {};
  limited.s_addr = htonl(INADDR_BROADCAST);
  for (in_addr target : {directed, limited}) {
    to.sin_addr = target;
    if (!SendTo(fd, to)) {
      printf("note: broadcast send to %s failed (errno=%d); case skipped\n", inet_ntoa(target), errno);
      continue;
    }
    if (!Readable(fd, 2000)) {
      // Windows Firewall may drop a broadcast this host sent to itself.
      printf("note: broadcast to %s not received within 2 s (dropped by a firewall?); case skipped\n",
             inet_ntoa(target));
      continue;
    }
    r = ReceiveOne(fd, 256);
    char spec[INET_ADDRSTRLEN] = {};
    inet_ntop(AF_INET, &r.info.ipi_spec_dst, spec, sizeof(spec));
    CHECK(r.ok && r.found && r.info.ipi_addr.s_addr == target.s_addr);
    // Sent from this host to the interface's subnet: the local address on it.
    if (r.info.ipi_spec_dst.s_addr != local.s_addr) printf("  spec_dst %s for %s\n", spec, inet_ntoa(target));
    CHECK(r.info.ipi_spec_dst.s_addr == local.s_addr);
    if (index != 0) CHECK(static_cast<unsigned>(r.info.ipi_ifindex) == index);
  }
  close(fd);
}

// A multicast datagram's ipi_spec_dst is, as for a broadcast, the local
// address facing its sender: sent from 127.0.0.1 through the loopback
// interface, 127.0.0.1 (ipi_addr stays the group).
static void TestMulticast() {
  int fd = socket(AF_INET, SOCK_DGRAM, 0);
  CHECK(fd >= 0);
  int on = 1;
  sockaddr_in self = {};
  self.sin_family = AF_INET;
  CHECK(bind(fd, reinterpret_cast<sockaddr*>(&self), sizeof(self)) == 0);
  socklen_t len = sizeof(self);
  CHECK(getsockname(fd, reinterpret_cast<sockaddr*>(&self), &len) == 0);
  CHECK(setsockopt(fd, IPPROTO_IP, IP_PKTINFO, &on, sizeof(on)) == 0);
  in_addr loopback = {};
  loopback.s_addr = htonl(INADDR_LOOPBACK);
  ip_mreq join = {};
  join.imr_multiaddr.s_addr = inet_addr("239.255.77.13");
  join.imr_interface = loopback;
  if (setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &join, sizeof(join)) != 0 ||
      setsockopt(fd, IPPROTO_IP, IP_MULTICAST_IF, &loopback, sizeof(loopback)) != 0 ||
      setsockopt(fd, IPPROTO_IP, IP_MULTICAST_LOOP, &on, sizeof(on)) != 0) {
    printf("note: no multicast on the loopback interface (errno=%d); multicast case skipped\n", errno);
    close(fd);
    return;
  }
  sockaddr_in to = self;
  to.sin_addr = join.imr_multiaddr;
  if (!SendTo(fd, to)) {
    printf("note: multicast send failed (errno=%d); multicast case skipped\n", errno);
    close(fd);
    return;
  }
  if (!Readable(fd, 2000)) {
    printf("note: looped-back multicast not received within 2 s (dropped by a firewall?); case skipped\n");
    close(fd);
    return;
  }
  Received r = ReceiveOne(fd, 256);
  CHECK(r.ok && r.found && !r.truncated);
  CHECK(r.info.ipi_addr.s_addr == join.imr_multiaddr.s_addr);
  if (r.info.ipi_spec_dst.s_addr != loopback.s_addr) printf("  multicast spec_dst %s\n", inet_ntoa(r.info.ipi_spec_dst));
  CHECK(r.info.ipi_spec_dst.s_addr == loopback.s_addr);
  CHECK(r.info.ipi_ifindex > 0);
  close(fd);
}

int main() {
  TestLoopback();
  TestDualStack();
  TestBroadcast();
  TestMulticast();

  // A stream socket accepts the option, as on Linux.
  int on = 1;
  int tcp = socket(AF_INET, SOCK_STREAM, 0);
  CHECK(tcp >= 0 && setsockopt(tcp, IPPROTO_IP, IP_PKTINFO, &on, sizeof(on)) == 0);
  close(tcp);

  // An AF_UNIX socket has no IP options: EOPNOTSUPP, as on Linux.
  for (int type : {SOCK_DGRAM, SOCK_STREAM}) {
    int sv[2] = {-1, -1};
    CHECK(socketpair(AF_UNIX, type, 0, sv) == 0);
    errno = 0;
    CHECK(setsockopt(sv[0], IPPROTO_IP, IP_PKTINFO, &on, sizeof(on)) == -1 && errno == EOPNOTSUPP);
    int value = 0;
    socklen_t len = sizeof(value);
    errno = 0;
    CHECK(getsockopt(sv[0], IPPROTO_IP, IP_PKTINFO, &value, &len) == -1 && errno == EOPNOTSUPP);
    close(sv[0]);
    close(sv[1]);
  }

  printf("udp pktinfo contracts: %d checks, %d failures\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
