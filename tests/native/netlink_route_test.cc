// compat/posix/src/netlink.cc + fd.cc + socket.cc + system.cc:
// Exercise real Windows interface dumps, descriptor operations, and
// permissions on the application-facing AF_NETLINK / NETLINK_ROUTE path.
//
//   netlink_route_test.exe -> "netlink_route_test all passed" (exit 0)

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <linux/if_addr.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <net/if.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace {

int g_failures = 0;

void Check(bool ok, const char* what) {
  std::printf("%s  %s\n", ok ? "PASS" : "FAIL", what);
  if (!ok) ++g_failures;
}

struct AddrEntry {
  int family = 0;
  uint32_t if_index = 0;
  std::array<uint8_t, 16> bytes = {};

  bool operator<(const AddrEntry& o) const {
    if (family != o.family) return family < o.family;
    if (if_index != o.if_index) return if_index < o.if_index;
    return bytes < o.bytes;
  }
  bool operator==(const AddrEntry& o) const = default;
};

int SendDumpRequest(int fd, uint16_t type, uint32_t seq, uint8_t family) {
  struct {
    nlmsghdr nlh;
    rtgenmsg rt;
  } req = {};
  req.nlh.nlmsg_len = NLMSG_LENGTH(sizeof(req.rt));
  req.nlh.nlmsg_type = type;
  req.nlh.nlmsg_flags = NLM_F_REQUEST | NLM_F_DUMP;
  req.nlh.nlmsg_seq = seq;
  req.rt.rtgen_family = family;

  sockaddr_nl kernel = {};
  kernel.nl_family = AF_NETLINK;
  return static_cast<int>(
      sendto(fd, &req, req.nlh.nlmsg_len, 0, reinterpret_cast<sockaddr*>(&kernel), sizeof(kernel)));
}

void TestSocketValidation() {
  errno = 0;
  int bad_type = socket(AF_NETLINK, SOCK_STREAM, NETLINK_ROUTE);
  Check(bad_type < 0 && errno == ESOCKTNOSUPPORT, "SOCK_STREAM netlink fails with ESOCKTNOSUPPORT");

  errno = 0;
  int bad_proto = socket(AF_NETLINK, SOCK_RAW, NETLINK_KOBJECT_UEVENT);
  Check(bad_proto < 0 && errno == EPROTONOSUPPORT,
        "NETLINK_KOBJECT_UEVENT fails with EPROTONOSUPPORT");

  errno = 0;
  int bad_dom = socket(AF_PACKET, SOCK_RAW, 0);
  Check(bad_dom < 0 && errno == EAFNOSUPPORT, "AF_PACKET remains EAFNOSUPPORT");

  int dgram_fd = socket(AF_NETLINK, SOCK_DGRAM | SOCK_CLOEXEC, NETLINK_ROUTE);
  Check(dgram_fd >= 0, "SOCK_DGRAM | SOCK_CLOEXEC NETLINK_ROUTE succeeds");
  if (dgram_fd >= 0) {
    int st = 0, dom = 0, proto = -1;
    socklen_t len = sizeof(int);
    Check(getsockopt(dgram_fd, SOL_SOCKET, SO_TYPE, &st, &len) == 0 && st == SOCK_DGRAM,
          "getsockopt SO_TYPE returns SOCK_DGRAM");
    len = sizeof(int);
    Check(getsockopt(dgram_fd, SOL_SOCKET, SO_DOMAIN, &dom, &len) == 0 && dom == AF_NETLINK,
          "getsockopt SO_DOMAIN returns AF_NETLINK");
    len = sizeof(int);
    Check(getsockopt(dgram_fd, SOL_SOCKET, SO_PROTOCOL, &proto, &len) == 0 &&
              proto == NETLINK_ROUTE,
          "getsockopt SO_PROTOCOL returns NETLINK_ROUTE");
    Check(close(dgram_fd) == 0, "close dgram netlink fd");
  }
}

void TestInterfaceAddresses() {
  int fd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_ROUTE);
  Check(fd >= 0, "socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_ROUTE)");
  if (fd < 0) return;
  const timeval timeout = {1, 0};
  Check(setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == 0,
        "bound the wait for dump data");

  sockaddr_nl kernel = {};
  kernel.nl_family = AF_NETLINK;
  Check(bind(fd, reinterpret_cast<sockaddr*>(&kernel), sizeof(kernel)) < 0 && errno == EACCES,
        "explicit route socket bind is denied to ordinary apps");

  const int sent = SendDumpRequest(fd, RTM_GETADDR, 1, AF_UNSPEC);
  Check(sent == static_cast<int>(NLMSG_LENGTH(sizeof(rtgenmsg))),
        "sendto RTM_GETADDR sends the whole request");

  sockaddr_nl local_addr = {};
  socklen_t local_len = sizeof(local_addr);
  Check(getsockname(fd, reinterpret_cast<sockaddr*>(&local_addr), &local_len) == 0 &&
            local_len == sizeof(sockaddr_nl) && local_addr.nl_family == AF_NETLINK &&
            local_addr.nl_pid != 0,
        "sending a request automatically binds a local port");

  int second_fd = socket(AF_NETLINK, SOCK_RAW, NETLINK_ROUTE);
  Check(second_fd >= 0, "open a second route socket");
  if (second_fd >= 0) {
    Check(connect(second_fd, reinterpret_cast<sockaddr*>(&kernel), sizeof(kernel)) == 0,
          "connecting a second socket allocates a local port");
    sockaddr_nl second_addr = {};
    socklen_t second_len = sizeof(second_addr);
    Check(getsockname(second_fd, reinterpret_cast<sockaddr*>(&second_addr), &second_len) == 0 &&
              second_addr.nl_pid != 0 && second_addr.nl_pid != local_addr.nl_pid,
          "automatically bound port identities are distinct");
    close(second_fd);
  }

  std::set<AddrEntry> netlink_addrs;
  bool saw_done = false;
  bool valid_interface_labels = true;
  bool all_packets_fit_4k = true;
  char buf[4096];

  while (!saw_done) {
    sockaddr_nl peer = {};
    socklen_t peer_len = sizeof(peer);
    const ssize_t n = recvfrom(fd, buf, sizeof(buf), 0, reinterpret_cast<sockaddr*>(&peer), &peer_len);
    if (n <= 0) {
      all_packets_fit_4k = false;
      break;
    }
    if (n > 4096 || peer.nl_family != AF_NETLINK || peer.nl_pid != 0 || peer_len != sizeof(peer)) {
      all_packets_fit_4k = false;
    }
    int len = static_cast<int>(n);
    for (nlmsghdr* nlh = reinterpret_cast<nlmsghdr*>(buf); NLMSG_OK(nlh, len);
         nlh = NLMSG_NEXT(nlh, len)) {
      if (nlh->nlmsg_seq != 1 || nlh->nlmsg_pid != local_addr.nl_pid ||
          (nlh->nlmsg_flags & NLM_F_MULTI) == 0) all_packets_fit_4k = false;
      if (nlh->nlmsg_type == NLMSG_DONE) {
        saw_done = true;
        break;
      }
      if (nlh->nlmsg_type == NLMSG_ERROR) {
        all_packets_fit_4k = false;
        saw_done = true;
        break;
      }
      if (nlh->nlmsg_type == RTM_NEWADDR) {
        auto* ifa = static_cast<ifaddrmsg*>(NLMSG_DATA(nlh));
        int rta_len = static_cast<int>(IFA_PAYLOAD(nlh));
        const void* addr_ptr = nullptr;
        const void* local_ptr = nullptr;
        std::string label;
        for (rtattr* rta = IFA_RTA(ifa); RTA_OK(rta, rta_len); rta = RTA_NEXT(rta, rta_len)) {
          if (rta->rta_type == IFA_ADDRESS && RTA_PAYLOAD(rta) >= 4) addr_ptr = RTA_DATA(rta);
          if (rta->rta_type == IFA_LOCAL && RTA_PAYLOAD(rta) >= 4) local_ptr = RTA_DATA(rta);
          if (rta->rta_type == IFA_LABEL && RTA_PAYLOAD(rta) > 0) {
            label = static_cast<const char*>(RTA_DATA(rta));
          }
        }
        const void* chosen = local_ptr != nullptr ? local_ptr : addr_ptr;
        if (chosen != nullptr && (ifa->ifa_family == AF_INET || ifa->ifa_family == AF_INET6)) {
          AddrEntry e;
          e.family = ifa->ifa_family;
          e.if_index = ifa->ifa_index;
          memcpy(e.bytes.data(), chosen, ifa->ifa_family == AF_INET ? 4 : 16);
          netlink_addrs.insert(e);
        }
        char ifname[IF_NAMESIZE] = {};
        if (if_indextoname(ifa->ifa_index, ifname) == nullptr) {
          valid_interface_labels = false;
        } else {
          if (ifa->ifa_family == AF_INET && !label.empty() && label != ifname) {
            valid_interface_labels = false;
          }
        }
      }
    }
  }
  Check(saw_done, "RTM_GETADDR terminates with NLMSG_DONE");
  Check(all_packets_fit_4k, "Address packets fit 4096 bytes and carry kernel sender and request identity");
  Check(valid_interface_labels, "IPv4 labels and interface indexes match if_indextoname");

  struct ifaddrs* ifap = nullptr;
  Check(getifaddrs(&ifap) == 0 && ifap != nullptr, "getifaddrs succeeds for parity comparison");
  std::set<AddrEntry> posix_addrs;
  for (struct ifaddrs* p = ifap; p != nullptr; p = p->ifa_next) {
    if (p->ifa_addr == nullptr || p->ifa_name == nullptr) continue;
    const int fam = p->ifa_addr->sa_family;
    if (fam != AF_INET && fam != AF_INET6) continue;
    AddrEntry e;
    e.family = fam;
    e.if_index = if_nametoindex(p->ifa_name);
    if (fam == AF_INET) {
      auto* sin = reinterpret_cast<sockaddr_in*>(p->ifa_addr);
      memcpy(e.bytes.data(), &sin->sin_addr, 4);
    } else {
      auto* sin6 = reinterpret_cast<sockaddr_in6*>(p->ifa_addr);
      memcpy(e.bytes.data(), &sin6->sin6_addr, 16);
    }
    posix_addrs.insert(e);
  }
  if (ifap != nullptr) freeifaddrs(ifap);
  Check(!netlink_addrs.empty() && netlink_addrs == posix_addrs,
        "NETLINK_ROUTE RTM_GETADDR addresses match getifaddrs()");

  // Filtered AF_INET dump should not emit AF_INET6 entries.
  Check(SendDumpRequest(fd, RTM_GETADDR, 2, AF_INET) > 0, "sendto filtered AF_INET RTM_GETADDR");
  bool saw_v6_in_filtered = false;
  bool filtered_done = false;
  while (!filtered_done) {
    const ssize_t n = recv(fd, buf, sizeof(buf), 0);
    if (n <= 0) break;
    int len = static_cast<int>(n);
    for (nlmsghdr* nlh = reinterpret_cast<nlmsghdr*>(buf); NLMSG_OK(nlh, len);
         nlh = NLMSG_NEXT(nlh, len)) {
      if (nlh->nlmsg_type == NLMSG_DONE) {
        filtered_done = true;
        break;
      }
      if (nlh->nlmsg_type == RTM_NEWADDR) {
        auto* ifa = static_cast<ifaddrmsg*>(NLMSG_DATA(nlh));
        if (ifa->ifa_family != AF_INET) saw_v6_in_filtered = true;
      }
    }
  }
  Check(filtered_done && !saw_v6_in_filtered, "AF_INET RTM_GETADDR filter omits IPv6");

  Check(SendDumpRequest(fd, RTM_GETLINK, 3, AF_UNSPEC) < 0 && errno == EACCES,
        "RTM_GETLINK is denied at send time");
  Check(recv(fd, buf, sizeof(buf), MSG_DONTWAIT) < 0 && errno == EAGAIN,
        "denied link query publishes no data");

  // Unsupported request yields NLMSG_ERROR with -EOPNOTSUPP
  nlmsghdr unsupported = {};
  unsupported.nlmsg_len = NLMSG_LENGTH(0);
  unsupported.nlmsg_type = 99;
  unsupported.nlmsg_flags = NLM_F_REQUEST;
  unsupported.nlmsg_seq = 77;
  Check(write(fd, &unsupported, unsupported.nlmsg_len) == static_cast<ssize_t>(unsupported.nlmsg_len),
        "write unsupported netlink message");
  const ssize_t err_n = recv(fd, buf, sizeof(buf), 0);
  bool err_ok = false;
  if (err_n >= static_cast<ssize_t>(NLMSG_LENGTH(sizeof(nlmsgerr)))) {
    auto* nlh = reinterpret_cast<nlmsghdr*>(buf);
    auto* err = static_cast<nlmsgerr*>(NLMSG_DATA(nlh));
    err_ok = nlh->nlmsg_type == NLMSG_ERROR && err->error == -EOPNOTSUPP && err->msg.nlmsg_seq == 77;
  }
  Check(err_ok, "Unsupported netlink msg returns NLMSG_ERROR(-EOPNOTSUPP)");

  close(fd);
}

void TestFlagsPollTimeoutAndDup() {
  int fd = socket(AF_NETLINK, SOCK_RAW | SOCK_NONBLOCK, NETLINK_ROUTE);
  Check(fd >= 0, "socket SOCK_NONBLOCK NETLINK_ROUTE");
  if (fd < 0) return;

  char buf[4096];
  errno = 0;
  Check(recv(fd, buf, sizeof(buf), 0) < 0 && errno == EAGAIN,
        "Nonblocking recv on empty queue returns EAGAIN");

  int off = 0;
  Check(ioctl(fd, FIONBIO, &off) < 0 && errno == EACCES, "route netlink ioctl is denied");
  const int file_flags = fcntl(fd, F_GETFL);
  Check(file_flags >= 0 && (file_flags & O_NONBLOCK) &&
            fcntl(fd, F_SETFL, file_flags & ~O_NONBLOCK) == 0 &&
            fcntl(fd, F_GETFL) == O_RDWR,
        "fcntl reads and clears nonblocking");
  timeval tv = {0, 30000};  // 30 ms
  Check(setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) == 0, "setsockopt SO_RCVTIMEO");

  // MSG_PEEK, MSG_TRUNC
  Check(SendDumpRequest(fd, RTM_GETADDR, 4, AF_UNSPEC) > 0, "send address dump for peek/trunc");

  const ssize_t peek_len = recv(fd, buf, 16, MSG_PEEK | MSG_TRUNC);
  Check(peek_len > 16, "MSG_PEEK | MSG_TRUNC returns full datagram length");
  const ssize_t trunc_len = recv(fd, buf, 16, MSG_TRUNC);
  Check(trunc_len == peek_len, "MSG_TRUNC returns full datagram length while consuming");
  // Drain rest
  while (recv(fd, buf, sizeof(buf), MSG_DONTWAIT) > 0) {
  }

  // Timeout check with SO_RCVTIMEO
  int group = 1;
  for (int option : {NETLINK_ADD_MEMBERSHIP, NETLINK_DROP_MEMBERSHIP}) {
    Check(setsockopt(fd, SOL_NETLINK, option, &group, sizeof(group)) < 0 && errno == EOPNOTSUPP,
          "NETLINK_ROUTE multicast membership is explicitly unsupported");
  }
  Check(setsockopt(fd, SOL_NETLINK, NETLINK_GET_STRICT_CHK, &group, sizeof(group)) < 0 &&
            errno == ENOPROTOOPT,
        "unsupported strict parsing mode reports ENOPROTOOPT");

  errno = 0;
  const auto t0 = std::chrono::steady_clock::now();
  const ssize_t timed_out = recv(fd, buf, sizeof(buf), 0);
  const auto elapsed_ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0)
          .count();
  Check(timed_out < 0 && errno == EAGAIN && elapsed_ms >= 15,
        "SO_RCVTIMEO times out with EAGAIN on empty netlink socket");

  // Connection/control edge cases
  sockaddr_nl kernel = {};
  kernel.nl_family = AF_NETLINK;
  kernel.nl_groups = 1;
  Check(bind(fd, reinterpret_cast<sockaddr*>(&kernel), sizeof(kernel)) < 0 && errno == EOPNOTSUPP,
        "multicast bind reports EOPNOTSUPP");
  Check(connect(fd, reinterpret_cast<sockaddr*>(&kernel), sizeof(kernel)) < 0 &&
            errno == EOPNOTSUPP,
        "multicast destination reports EOPNOTSUPP");
  kernel.nl_groups = 0;
  Check(connect(fd, reinterpret_cast<sockaddr*>(&kernel), sizeof(kernel)) == 0,
        "connect to AF_NETLINK kernel pid=0 succeeds");
  errno = 0;
  Check(listen(fd, 1) < 0 && errno == EOPNOTSUPP, "listen on netlink returns EOPNOTSUPP");
  socklen_t peerlen = sizeof(kernel);
  Check(getpeername(fd, reinterpret_cast<sockaddr*>(&kernel), &peerlen) == 0 &&
            peerlen == sizeof(kernel) && kernel.nl_family == AF_NETLINK &&
            kernel.nl_pid == 0 && kernel.nl_groups == 0,
        "getpeername reports the kernel destination");

  // poll() immediate POLLOUT and cross-thread POLLIN wakeup
  pollfd pfd = {fd, POLLIN | POLLOUT, 0};
  Check(poll(&pfd, 1, 0) == 1 && (pfd.revents & (POLLIN | POLLOUT)) == POLLOUT,
        "poll on empty netlink returns POLLOUT immediately");

  std::thread sender([fd]() {
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    SendDumpRequest(fd, RTM_GETADDR, 9, AF_UNSPEC);
  });
  pollfd wait_in = {fd, POLLIN, 0};
  const int pr = poll(&wait_in, 1, 2000);
  sender.join();
  Check(pr == 1 && (wait_in.revents & POLLIN) != 0,
        "poll(POLLIN) wakes promptly when another thread sends dump request");

  // dup + close preserves socket until last fd closed
  int dup_fd = dup(fd);
  Check(dup_fd >= 0 && dup_fd != fd, "dup(netlink fd) succeeds");
  Check(close(fd) == 0, "close original netlink fd");
  if (dup_fd >= 0) {
    Check(recv(dup_fd, buf, sizeof(buf), MSG_DONTWAIT) > 0, "dup fd remains readable after close(orig)");
    while (recv(dup_fd, buf, sizeof(buf), MSG_DONTWAIT) > 0) {
    }
    Check(close(dup_fd) == 0, "close dup netlink fd");
  }
}

void TestReceiveBufferLimit() {
  int fd = socket(AF_NETLINK, SOCK_RAW | SOCK_NONBLOCK, NETLINK_ROUTE);
  Check(fd >= 0, "create nonblocking netlink descriptor for queue test");
  if (fd < 0) return;
  int bufsize = 128;
  socklen_t len = sizeof(bufsize);
  Check(setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &bufsize, len) == 0 &&
            getsockopt(fd, SOL_SOCKET, SO_RCVBUF, &bufsize, &len) == 0 && bufsize == 256,
        "SO_RCVBUF sets the bounded queue budget");
  nlmsghdr request = {};
  request.nlmsg_len = NLMSG_LENGTH(0);
  request.nlmsg_type = NLMSG_NOOP;
  request.nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK;
  const int expected = bufsize / NLMSG_LENGTH(sizeof(nlmsgerr));
  int admitted = 0;
  while (admitted <= expected && write(fd, &request, sizeof(request)) == sizeof(request)) {
    ++admitted;
  }
  Check(admitted == expected && errno == ENOBUFS, "queue exhaustion returns ENOBUFS");
  char message[NLMSG_LENGTH(sizeof(nlmsgerr))];
  Check(recv(fd, message, sizeof(message), 0) == sizeof(message) &&
            write(fd, &request, sizeof(request)) == sizeof(request),
        "reading a reply releases queue capacity");
  close(fd);
}

struct ReceiveWait {
  int fd;
  std::atomic<bool> started{false};
  ssize_t result = 0;
  int error = 0;
};

void* WaitForReply(void* opaque) {
  auto* wait = static_cast<ReceiveWait*>(opaque);
  char byte;
  wait->started.store(true, std::memory_order_release);
  wait->result = recv(wait->fd, &byte, sizeof(byte), 0);
  wait->error = errno;
  return nullptr;
}

void TestReceiveRetirement() {
  for (bool cancel : {false, true}) {
    int fd = socket(AF_NETLINK, SOCK_RAW, NETLINK_ROUTE);
    Check(fd >= 0, "open route socket for blocked receive retirement");
    if (fd < 0) return;
    const timeval fallback = {2, 0};
    if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &fallback, sizeof(fallback)) != 0) {
      Check(false, "install receive timeout as a test safety bound");
      close(fd);
      return;
    }
    ReceiveWait wait{fd};
    pthread_t reader;
    const int started = pthread_create(&reader, nullptr, WaitForReply, &wait);
    Check(started == 0, "start route socket reader");
    if (started != 0) {
      close(fd);
      return;
    }
    while (!wait.started.load(std::memory_order_acquire)) std::this_thread::yield();
    std::this_thread::sleep_for(std::chrono::milliseconds(35));
    void* result = nullptr;
    const auto begin = std::chrono::steady_clock::now();
    if (cancel) {
      Check(pthread_cancel(reader) == 0, "cancel route socket reader");
      Check(pthread_join(reader, &result) == 0 && result == PTHREAD_CANCELED,
            "blocked receive joins as canceled");
      Check(SendDumpRequest(fd, RTM_GETADDR, 12, AF_UNSPEC) > 0,
            "canceled receive releases the socket lock");
      Check(close(fd) == 0, "last close still succeeds after cancellation");
    } else {
      Check(close(fd) == 0, "close route socket while a receive is blocked");
      Check(pthread_join(reader, &result) == 0 && wait.result < 0 && wait.error == EBADF,
            "last close wakes receive with EBADF");
    }
    Check(std::chrono::steady_clock::now() - begin < std::chrono::seconds(1),
          "reader retires before the socket timeout");
  }
}

}  // namespace

int main() {
  TestSocketValidation();
  TestInterfaceAddresses();
  TestFlagsPollTimeoutAndDup();
  TestReceiveBufferLimit();
  TestReceiveRetirement();

  if (g_failures == 0) {
    std::printf("netlink_route_test all passed\n");
    return 0;
  }
  return 1;
}
