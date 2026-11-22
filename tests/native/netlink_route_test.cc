// compat/posix/src/netlink.cc + fd.cc + socket.cc + system.cc:
// Exercise AF_NETLINK / NETLINK_ROUTE including WebRTC's ifaddrs_android.cc
// dump sequence (RTM_GETADDR + recvfrom 4096 + if_indextoname + SIOCGIFFLAGS),
// RTM_GETLINK, poll() wakeup, O_NONBLOCK/MSG_DONTWAIT, MSG_PEEK/MSG_TRUNC,
// setsockopt/getsockopt, dup/close, and error cases.
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
#include <net/if_arp.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <array>
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

void TestWebRtcIfaddrsFlow() {
  int fd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_ROUTE);
  Check(fd >= 0, "socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_ROUTE)");
  if (fd < 0) return;

  sockaddr_nl bind_addr = {};
  bind_addr.nl_family = AF_NETLINK;
  Check(bind(fd, reinterpret_cast<sockaddr*>(&bind_addr), sizeof(bind_addr)) == 0,
        "bind netlink auto-pid");

  sockaddr_nl local_addr = {};
  socklen_t local_len = sizeof(local_addr);
  Check(getsockname(fd, reinterpret_cast<sockaddr*>(&local_addr), &local_len) == 0 &&
            local_len == sizeof(sockaddr_nl) && local_addr.nl_family == AF_NETLINK &&
            local_addr.nl_pid != 0,
        "getsockname returns bound nl_pid");

  const int sent = SendDumpRequest(fd, RTM_GETADDR, 1, AF_UNSPEC);
  Check(sent == static_cast<int>(NLMSG_LENGTH(sizeof(rtgenmsg))),
        "sendto RTM_GETADDR sends 0x18 bytes");

  std::set<AddrEntry> netlink_addrs;
  bool saw_done = false;
  bool seen_v4_label_and_flags = true;
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
          seen_v4_label_and_flags = false;
        } else {
          if (ifa->ifa_family == AF_INET && !label.empty() && label != ifname) {
            seen_v4_label_and_flags = false;
          }
          ifreq ifr = {};
          snprintf(ifr.ifr_name, sizeof(ifr.ifr_name), "%s", ifname);
          if (ioctl(fd, SIOCGIFFLAGS, &ifr) != 0) seen_v4_label_and_flags = false;
        }
      }
    }
  }
  Check(saw_done, "RTM_GETADDR terminates with NLMSG_DONE");
  Check(all_packets_fit_4k, "All RTM_GETADDR netlink packets fit 4096 bytes with AF_NETLINK peer");
  Check(seen_v4_label_and_flags, "if_indextoname, IFA_LABEL, and ioctl(SIOCGIFFLAGS) succeed");

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

  // RTM_GETLINK check
  Check(SendDumpRequest(fd, RTM_GETLINK, 3, AF_UNSPEC) > 0, "sendto RTM_GETLINK");
  bool saw_link = false;
  bool link_done = false;
  while (!link_done) {
    const ssize_t n = recv(fd, buf, sizeof(buf), 0);
    if (n <= 0) break;
    int len = static_cast<int>(n);
    for (nlmsghdr* nlh = reinterpret_cast<nlmsghdr*>(buf); NLMSG_OK(nlh, len);
         nlh = NLMSG_NEXT(nlh, len)) {
      if (nlh->nlmsg_type == NLMSG_DONE) {
        link_done = true;
        break;
      }
      if (nlh->nlmsg_type == RTM_NEWLINK) {
        auto* ifi = static_cast<ifinfomsg*>(NLMSG_DATA(nlh));
        if (ifi->ifi_index > 0) saw_link = true;
      }
    }
  }
  Check(link_done && saw_link, "RTM_GETLINK returns RTM_NEWLINK and NLMSG_DONE");

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
  Check(ioctl(fd, FIONBIO, &off) == 0, "ioctl FIONBIO disables nonblocking");

  // MSG_PEEK, MSG_TRUNC, FIONREAD
  Check(SendDumpRequest(fd, RTM_GETLINK, 4, AF_UNSPEC) > 0, "Send RTM_GETLINK for peek/trunc");
  int avail = 0;
  Check(ioctl(fd, FIONREAD, &avail) == 0 && avail > 0, "FIONREAD reports queued bytes");

  const ssize_t peek_len = recv(fd, buf, 16, MSG_PEEK | MSG_TRUNC);
  Check(peek_len == avail && peek_len > 16, "MSG_PEEK | MSG_TRUNC returns full datagram length");
  const ssize_t trunc_len = recv(fd, buf, 16, MSG_TRUNC);
  Check(trunc_len == avail, "MSG_TRUNC returns full datagram length while consuming");
  // Drain rest
  while (recv(fd, buf, sizeof(buf), MSG_DONTWAIT) > 0) {
  }

  // Timeout check with SO_RCVTIMEO
  timeval tv = {0, 30000};  // 30 ms
  Check(setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) == 0, "setsockopt SO_RCVTIMEO");
  int group = 1;
  Check(setsockopt(fd, SOL_NETLINK, NETLINK_ADD_MEMBERSHIP, &group, sizeof(group)) == 0,
        "setsockopt SOL_NETLINK NETLINK_ADD_MEMBERSHIP");

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
  Check(connect(fd, reinterpret_cast<sockaddr*>(&kernel), sizeof(kernel)) == 0,
        "connect to AF_NETLINK kernel pid=0 succeeds");
  errno = 0;
  Check(listen(fd, 1) < 0 && errno == EOPNOTSUPP, "listen on netlink returns EOPNOTSUPP");
  errno = 0;
  Check(getpeername(fd, reinterpret_cast<sockaddr*>(&kernel), nullptr) < 0 && errno == ENOTCONN,
        "getpeername on netlink returns ENOTCONN");

  // poll() immediate POLLOUT and cross-thread POLLIN wakeup
  pollfd pfd = {fd, POLLIN | POLLOUT, 0};
  Check(poll(&pfd, 1, 0) == 1 && (pfd.revents & (POLLIN | POLLOUT)) == POLLOUT,
        "poll on empty netlink returns POLLOUT immediately");

  std::thread sender([fd]() {
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    SendDumpRequest(fd, RTM_GETLINK, 9, AF_UNSPEC);
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

}  // namespace

int main() {
  TestSocketValidation();
  TestWebRtcIfaddrsFlow();
  TestFlagsPollTimeoutAndDup();

  if (g_failures == 0) {
    std::printf("netlink_route_test all passed\n");
    return 0;
  }
  return 1;
}
