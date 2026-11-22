// gesher-base: AF_NETLINK / NETLINK_ROUTE socket backend over Windows interface enumeration.
//
// Android's NDK/WebRTC callers (such as chromium/webrtc ifaddrs_android.cc) open
// socket(AF_NETLINK, SOCK_RAW, NETLINK_ROUTE) to dump RTM_GETADDR / RTM_GETLINK
// instead of calling bionic's getifaddrs(). This module translates those route
// queries into standard Linux rtnetlink replies built from ws::ListInterfaces().

#include <errno.h>
#include <linux/if_addr.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <net/if.h>
#include <net/if_arp.h>
#include <netinet/in.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "event_internal.h"
#include "linux_socket.h"
#include "netlink_internal.h"
#include "socket_internal.h"

namespace gesher {

struct NetlinkState {
  explicit NetlinkState(int type) : sock_type(type) {}

  std::mutex mu;
  std::condition_variable cv;
  int sock_type = SOCK_RAW;
  uint32_t bound_pid = 0;
  uint32_t bound_groups = 0;
  bool closed = false;
  int rcv_timeout_ms = 0;
  int rcvbuf = 65536;
  int sndbuf = 65536;
  std::deque<std::vector<uint8_t>> queue;
};

namespace {

constexpr size_t kMaxNetlinkRequestBytes = 65536;
// Pack dump replies into <= 4096-byte datagrams so callers using fixed 4 KiB
// buffers (including WebRTC's ifaddrs_android.cc) never truncate an nlmsghdr.
constexpr size_t kMaxNetlinkPacketBytes = 4096;

uint32_t SelfNetlinkPid() {
  const int32_t pid = ws::LocalCredentials().pid;
  return pid > 0 ? static_cast<uint32_t>(pid) : 1u;
}

void AppendAttr(std::vector<uint8_t>* msg, unsigned short type, const void* data, size_t len) {
  const size_t raw_len = RTA_LENGTH(len);
  const size_t aligned_len = RTA_ALIGN(raw_len);
  const size_t offset = msg->size();
  msg->resize(offset + aligned_len, 0);
  rtattr attr = {};
  attr.rta_len = static_cast<unsigned short>(raw_len);
  attr.rta_type = type;
  memcpy(msg->data() + offset, &attr, sizeof(attr));
  if (len > 0 && data != nullptr) {
    memcpy(msg->data() + offset + RTA_LENGTH(0), data, len);
  }
}

void FinalizeNlmsg(std::vector<uint8_t>* msg, uint16_t type, uint16_t flags,
                   uint32_t seq, uint32_t pid) {
  nlmsghdr nlh = {};
  nlh.nlmsg_len = static_cast<uint32_t>(msg->size());
  nlh.nlmsg_type = type;
  nlh.nlmsg_flags = flags;
  nlh.nlmsg_seq = seq;
  nlh.nlmsg_pid = pid;
  memcpy(msg->data(), &nlh, sizeof(nlh));
}

struct PacketBatcher {
  std::vector<std::vector<uint8_t>> packets;
  std::vector<uint8_t> current;

  void Push(const std::vector<uint8_t>& msg) {
    if (!current.empty() && current.size() + msg.size() > kMaxNetlinkPacketBytes) {
      packets.push_back(std::move(current));
      current.clear();
    }
    current.insert(current.end(), msg.begin(), msg.end());
  }

  void PushDone(uint32_t seq, uint32_t pid) {
    std::vector<uint8_t> done(NLMSG_LENGTH(sizeof(int)), 0);
    int err = 0;
    memcpy(done.data() + NLMSG_HDRLEN, &err, sizeof(err));
    FinalizeNlmsg(&done, NLMSG_DONE, NLM_F_MULTI, seq, pid);
    Push(done);
    if (!current.empty()) {
      packets.push_back(std::move(current));
      current.clear();
    }
  }
};

uint8_t Ipv4Scope(unsigned iface_flags, in_addr addr) {
  const uint32_t host = __builtin_bswap32(addr.s_addr);
  if ((iface_flags & ws::kIfLoopback) != 0 || (host >> 24) == 127u) return RT_SCOPE_HOST;
  if ((host >> 16) == 0xA9FEu) return RT_SCOPE_LINK;  // 169.254.0.0/16
  return RT_SCOPE_UNIVERSE;
}

uint8_t Ipv6Scope(unsigned iface_flags, const in6_addr& addr) {
  bool is_loopback = true;
  for (int i = 0; i < 15; ++i) {
    if (addr.s6_addr[i] != 0) {
      is_loopback = false;
      break;
    }
  }
  is_loopback = is_loopback && addr.s6_addr[15] == 1;
  if ((iface_flags & ws::kIfLoopback) != 0 || is_loopback) return RT_SCOPE_HOST;
  if (addr.s6_addr[0] == 0xfe && (addr.s6_addr[1] & 0xc0) == 0x80) return RT_SCOPE_LINK;
  return RT_SCOPE_UNIVERSE;
}

bool AppendAddrDump(uint8_t req_family, uint32_t seq, uint32_t pid, PacketBatcher* out) {
  std::vector<ws::Interface> interfaces;
  if (!sock::ListNamedInterfaces(&interfaces)) return false;

  for (const ws::Interface& iface : interfaces) {
    if (iface.index == 0) continue;
    for (const ws::InterfaceAddress& entry : iface.addresses) {
      if (entry.address.family == ws::Family::kInet) {
        if (req_family != AF_UNSPEC && req_family != AF_INET) continue;
        sockaddr_in sin = {};
        const size_t tail = sizeof(sin) - sizeof(sa_family_t);
        if (entry.address.data_len < tail) continue;
        memcpy(reinterpret_cast<char*>(&sin) + sizeof(sa_family_t), entry.address.data, tail);

        const int prefix = std::clamp(entry.prefix_len, 0, 32);
        std::vector<uint8_t> msg(sizeof(nlmsghdr) + sizeof(ifaddrmsg), 0);
        ifaddrmsg ifa = {};
        ifa.ifa_family = AF_INET;
        ifa.ifa_prefixlen = static_cast<uint8_t>(prefix);
        ifa.ifa_flags = IFA_F_PERMANENT;
        ifa.ifa_scope = Ipv4Scope(iface.flags, sin.sin_addr);
        ifa.ifa_index = iface.index;
        memcpy(msg.data() + sizeof(nlmsghdr), &ifa, sizeof(ifa));

        AppendAttr(&msg, IFA_ADDRESS, &sin.sin_addr, sizeof(sin.sin_addr));
        AppendAttr(&msg, IFA_LOCAL, &sin.sin_addr, sizeof(sin.sin_addr));
        if ((iface.flags & ws::kIfBroadcast) != 0) {
          const uint32_t host_ip = __builtin_bswap32(sin.sin_addr.s_addr);
          const uint32_t mask_host = prefix == 0 ? 0u : (~0u << (32 - prefix));
          in_addr broadcast = {};
          broadcast.s_addr = __builtin_bswap32(host_ip | ~mask_host);
          AppendAttr(&msg, IFA_BROADCAST, &broadcast, sizeof(broadcast));
        }
        if (!iface.name.empty()) {
          AppendAttr(&msg, IFA_LABEL, iface.name.c_str(), iface.name.size() + 1);
        }
        FinalizeNlmsg(&msg, RTM_NEWADDR, NLM_F_MULTI, seq, pid);
        out->Push(msg);
      } else if (entry.address.family == ws::Family::kInet6) {
        if (req_family != AF_UNSPEC && req_family != AF_INET6) continue;
        sockaddr_in6 sin6 = {};
        const size_t tail = sizeof(sin6) - sizeof(sa_family_t);
        if (entry.address.data_len < tail) continue;
        memcpy(reinterpret_cast<char*>(&sin6) + sizeof(sa_family_t), entry.address.data, tail);

        const int prefix = std::clamp(entry.prefix_len, 0, 128);
        std::vector<uint8_t> msg(sizeof(nlmsghdr) + sizeof(ifaddrmsg), 0);
        ifaddrmsg ifa = {};
        ifa.ifa_family = AF_INET6;
        ifa.ifa_prefixlen = static_cast<uint8_t>(prefix);
        ifa.ifa_flags = IFA_F_PERMANENT;
        ifa.ifa_scope = Ipv6Scope(iface.flags, sin6.sin6_addr);
        ifa.ifa_index = iface.index;
        memcpy(msg.data() + sizeof(nlmsghdr), &ifa, sizeof(ifa));

        AppendAttr(&msg, IFA_ADDRESS, &sin6.sin6_addr, sizeof(sin6.sin6_addr));
        FinalizeNlmsg(&msg, RTM_NEWADDR, NLM_F_MULTI, seq, pid);
        out->Push(msg);
      }
    }
  }
  out->PushDone(seq, pid);
  return true;
}

bool AppendLinkDump(uint32_t seq, uint32_t pid, PacketBatcher* out) {
  std::vector<ws::Interface> interfaces;
  if (!sock::ListNamedInterfaces(&interfaces)) return false;

  for (const ws::Interface& iface : interfaces) {
    if (iface.index == 0) continue;
    std::vector<uint8_t> msg(sizeof(nlmsghdr) + sizeof(ifinfomsg), 0);
    ifinfomsg ifi = {};
    ifi.ifi_family = AF_UNSPEC;
    ifi.ifi_type = (iface.flags & ws::kIfLoopback) != 0 ? ARPHRD_LOOPBACK : ARPHRD_ETHER;
    ifi.ifi_index = static_cast<int>(iface.index);
    ifi.ifi_flags = sock::ToIfFlags(iface.flags);
    ifi.ifi_change = 0;
    memcpy(msg.data() + sizeof(nlmsghdr), &ifi, sizeof(ifi));

    if (!iface.name.empty()) {
      AppendAttr(&msg, IFLA_IFNAME, iface.name.c_str(), iface.name.size() + 1);
    }
    uint32_t mtu = iface.mtu;
    AppendAttr(&msg, IFLA_MTU, &mtu, sizeof(mtu));
    if (iface.hardware_len > 0 && iface.hardware_len <= sizeof(iface.hardware)) {
      AppendAttr(&msg, IFLA_ADDRESS, iface.hardware, iface.hardware_len);
    }
    FinalizeNlmsg(&msg, RTM_NEWLINK, NLM_F_MULTI, seq, pid);
    out->Push(msg);
  }
  out->PushDone(seq, pid);
  return true;
}

void AppendErrorPacket(int error_code, const nlmsghdr& req, uint32_t pid, PacketBatcher* out) {
  std::vector<uint8_t> msg(NLMSG_LENGTH(sizeof(nlmsgerr)), 0);
  nlmsgerr err = {};
  err.error = error_code;
  err.msg = req;
  memcpy(msg.data() + NLMSG_HDRLEN, &err, sizeof(err));
  FinalizeNlmsg(&msg, NLMSG_ERROR, 0, req.nlmsg_seq, pid);
  out->Push(msg);
  out->packets.push_back(std::move(out->current));
  out->current.clear();
}

bool ValidateNetlinkPeerAddr(const void* addr, uint32_t addrlen) {
  if (addrlen < sizeof(sockaddr_nl)) return errno = EINVAL, false;
  sockaddr_nl peer = {};
  if (!ws::CopyFromUser(&peer, addr, sizeof(peer))) return errno = EFAULT, false;
  if (peer.nl_family != AF_NETLINK) return errno = EAFNOSUPPORT, false;
  if (peer.nl_pid != 0) return errno = EPERM, false;
  return true;
}

bool ReadIntOpt(const void* value, uint32_t len, int* out) {
  if (value == nullptr) return errno = EFAULT, false;
  if (len < sizeof(int)) return errno = EINVAL, false;
  if (!ws::CopyFromUser(out, value, sizeof(int))) return errno = EFAULT, false;
  return true;
}

bool WriteIntOpt(int v, void* value, uint32_t* len) {
  if (value == nullptr || len == nullptr) return errno = EFAULT, false;
  uint32_t capacity = 0;
  if (!ws::CopyFromUser(&capacity, len, sizeof(capacity))) return errno = EFAULT, false;
  if (capacity < sizeof(int)) return errno = EINVAL, false;
  const uint32_t actual = sizeof(int);
  if (!ws::CopyToUser(value, &v, actual) || !ws::CopyToUser(len, &actual, sizeof(actual))) {
    return errno = EFAULT, false;
  }
  return true;
}

}  // namespace

std::shared_ptr<NetlinkState> CreateNetlinkState(int sock_type) {
  return std::make_shared<NetlinkState>(sock_type);
}

void NetlinkClose(NetlinkState& state) {
  {
    std::lock_guard<std::mutex> lock(state.mu);
    state.closed = true;
    state.queue.clear();
  }
  state.cv.notify_all();
  if (HasPollWaiters()) WakePollers();
}

bool NetlinkReadable(NetlinkState& state) {
  std::lock_guard<std::mutex> lock(state.mu);
  return !state.queue.empty() || state.closed;
}

int NetlinkAvailable(NetlinkState& state) {
  std::lock_guard<std::mutex> lock(state.mu);
  if (state.queue.empty()) return 0;
  const size_t sz = state.queue.front().size();
  return sz > 0x7fffffff ? 0x7fffffff : static_cast<int>(sz);
}

int NetlinkBind(NetlinkState& state, const void* addr, uint32_t len) {
  if (addr == nullptr) return errno = EFAULT, -1;
  if (len < sizeof(sockaddr_nl)) return errno = EINVAL, -1;
  sockaddr_nl local = {};
  if (!ws::CopyFromUser(&local, addr, sizeof(local))) return errno = EFAULT, -1;
  if (local.nl_family != AF_NETLINK) return errno = EINVAL, -1;
  std::lock_guard<std::mutex> lock(state.mu);
  state.bound_pid = local.nl_pid != 0 ? local.nl_pid : SelfNetlinkPid();
  state.bound_groups = local.nl_groups;
  return 0;
}

int NetlinkConnect(NetlinkState& state, const void* addr, uint32_t len) {
  (void)state;
  if (addr == nullptr) return errno = EFAULT, -1;
  return ValidateNetlinkPeerAddr(addr, len) ? 0 : -1;
}

int NetlinkGetSockName(NetlinkState& state, void* addr, uint32_t* len) {
  if (addr == nullptr || len == nullptr) return errno = EFAULT, -1;
  sockaddr_nl local = {};
  local.nl_family = AF_NETLINK;
  {
    std::lock_guard<std::mutex> lock(state.mu);
    local.nl_pid = state.bound_pid;
    local.nl_groups = state.bound_groups;
  }
  uint32_t capacity = 0;
  if (!ws::CopyFromUser(&capacity, len, sizeof(capacity))) return errno = EFAULT, -1;
  const uint32_t actual = sizeof(local);
  const size_t copied = capacity < actual ? capacity : actual;
  if ((copied > 0 && !ws::CopyToUser(addr, &local, copied)) ||
      !ws::CopyToUser(len, &actual, sizeof(actual))) {
    return errno = EFAULT, -1;
  }
  return 0;
}

ssize_t NetlinkSend(NetlinkState& state, const ws::Buffer* buffers, size_t count, int flags,
                    const void* addr, uint32_t addrlen) {
  (void)flags;
  if (addr != nullptr && !ValidateNetlinkPeerAddr(addr, addrlen)) return -1;
  if (count > 0 && buffers == nullptr) return errno = EFAULT, -1;

  size_t total_len = 0;
  for (size_t i = 0; i < count; ++i) {
    if (buffers[i].len > kMaxNetlinkRequestBytes - total_len) return errno = EMSGSIZE, -1;
    total_len += buffers[i].len;
  }
  std::vector<uint8_t> req(total_len);
  size_t pos = 0;
  for (size_t i = 0; i < count; ++i) {
    if (buffers[i].len == 0) continue;
    if (buffers[i].data == nullptr || !ws::CopyFromUser(req.data() + pos, buffers[i].data, buffers[i].len)) {
      return errno = EFAULT, -1;
    }
    pos += buffers[i].len;
  }
  if (req.size() < sizeof(nlmsghdr)) return errno = EINVAL, -1;

  uint32_t reply_pid = 0;
  {
    std::lock_guard<std::mutex> lock(state.mu);
    if (state.closed) return errno = EPIPE, -1;
    reply_pid = state.bound_pid != 0 ? state.bound_pid : SelfNetlinkPid();
  }

  PacketBatcher batch;
  size_t offset = 0;
  while (offset + sizeof(nlmsghdr) <= req.size()) {
    nlmsghdr hdr = {};
    memcpy(&hdr, req.data() + offset, sizeof(hdr));
    if (hdr.nlmsg_len < sizeof(nlmsghdr) || hdr.nlmsg_len > req.size() - offset) {
      return errno = EINVAL, -1;
    }
    const uint8_t* payload = req.data() + offset + NLMSG_HDRLEN;
    const size_t payload_len = hdr.nlmsg_len - NLMSG_HDRLEN;

    if (hdr.nlmsg_type == RTM_GETADDR) {
      const uint8_t req_family = payload_len >= 1 ? payload[0] : AF_UNSPEC;
      if (!AppendAddrDump(req_family, hdr.nlmsg_seq, reply_pid, &batch)) return -1;
    } else if (hdr.nlmsg_type == RTM_GETLINK) {
      if (!AppendLinkDump(hdr.nlmsg_seq, reply_pid, &batch)) return -1;
    } else if (hdr.nlmsg_type == NLMSG_NOOP) {
      if ((hdr.nlmsg_flags & NLM_F_ACK) != 0) {
        AppendErrorPacket(0, hdr, reply_pid, &batch);
      }
    } else {
      AppendErrorPacket(-EOPNOTSUPP, hdr, reply_pid, &batch);
    }

    const size_t step = NLMSG_ALIGN(hdr.nlmsg_len);
    if (step > req.size() - offset) break;
    offset += step;
  }

  {
    std::lock_guard<std::mutex> lock(state.mu);
    for (auto& pkt : batch.packets) {
      state.queue.push_back(std::move(pkt));
    }
  }
  state.cv.notify_all();
  if (HasPollWaiters()) WakePollers();
  return static_cast<ssize_t>(req.size());
}

ssize_t NetlinkRecv(NetlinkState& state, ws::Buffer* buffers, size_t count, int flags, void* addr,
                    uint32_t* addrlen, bool nonblocking, int* result_flags) {
  if (count > 0 && buffers == nullptr) return errno = EFAULT, -1;
  for (size_t i = 0; i < count; ++i) {
    if (buffers[i].len > 0 && buffers[i].data == nullptr) return errno = EFAULT, -1;
  }

  std::vector<uint8_t> pkt;
  {
    std::unique_lock<std::mutex> lock(state.mu);
    while (state.queue.empty()) {
      if (state.closed) return 0;
      if (nonblocking || (flags & MSG_DONTWAIT) != 0) return errno = EAGAIN, -1;
      if (state.rcv_timeout_ms > 0) {
        const auto status = state.cv.wait_for(lock, std::chrono::milliseconds(state.rcv_timeout_ms));
        if (state.queue.empty()) {
          if (state.closed) return 0;
          if (status == std::cv_status::timeout) return errno = EAGAIN, -1;
        }
      } else {
        state.cv.wait(lock);
      }
    }
    pkt = state.queue.front();
    if ((flags & MSG_PEEK) == 0) state.queue.pop_front();
  }

  size_t copied = 0;
  for (size_t i = 0; i < count && copied < pkt.size(); ++i) {
    if (buffers[i].len == 0) continue;
    const size_t chunk = std::min(buffers[i].len, pkt.size() - copied);
    if (!ws::CopyToUser(buffers[i].data, pkt.data() + copied, chunk)) {
      return errno = EFAULT, -1;
    }
    copied += chunk;
  }
  if (copied < pkt.size() && result_flags != nullptr) {
    *result_flags |= ws::kMsgTrunc;
  }

  if (addr != nullptr) {
    if (addrlen == nullptr) return errno = EFAULT, -1;
    uint32_t capacity = 0;
    if (!ws::CopyFromUser(&capacity, addrlen, sizeof(capacity))) return errno = EFAULT, -1;
    sockaddr_nl kernel_addr = {};
    kernel_addr.nl_family = AF_NETLINK;
    const uint32_t actual = sizeof(kernel_addr);
    const size_t to_copy = capacity < actual ? capacity : actual;
    if ((to_copy > 0 && !ws::CopyToUser(addr, &kernel_addr, to_copy)) ||
        !ws::CopyToUser(addrlen, &actual, sizeof(actual))) {
      return errno = EFAULT, -1;
    }
  }

  return (flags & MSG_TRUNC) != 0 ? static_cast<ssize_t>(pkt.size())
                                  : static_cast<ssize_t>(copied);
}

int NetlinkSetSockOpt(NetlinkState& state, int level, int name, const void* value, uint32_t len) {
  int v = 0;
  if (level == SOL_SOCKET) {
    switch (name) {
      case SO_RCVBUF:
        if (!ReadIntOpt(value, len, &v)) return -1;
        {
          std::lock_guard<std::mutex> lock(state.mu);
          state.rcvbuf = std::max(v, 256);
        }
        return 0;
      case SO_SNDBUF:
        if (!ReadIntOpt(value, len, &v)) return -1;
        {
          std::lock_guard<std::mutex> lock(state.mu);
          state.sndbuf = std::max(v, 256);
        }
        return 0;
      case SO_RCVTIMEO: {
        if (value == nullptr || len < sizeof(timeval)) return errno = EINVAL, -1;
        timeval tv = {};
        if (!ws::CopyFromUser(&tv, value, sizeof(tv))) return errno = EFAULT, -1;
        if (tv.tv_sec < 0 || tv.tv_usec < 0 || tv.tv_usec >= 1000000) return errno = EDOM, -1;
        long long ms = static_cast<long long>(tv.tv_sec) * 1000 + (tv.tv_usec + 999) / 1000;
        if (ms > 0x7fffffff) ms = 0x7fffffff;
        std::lock_guard<std::mutex> lock(state.mu);
        state.rcv_timeout_ms = static_cast<int>(ms);
        return 0;
      }
      case SO_SNDTIMEO: {
        if (value == nullptr || len < sizeof(timeval)) return errno = EINVAL, -1;
        timeval tv = {};
        if (!ws::CopyFromUser(&tv, value, sizeof(tv))) return errno = EFAULT, -1;
        if (tv.tv_sec < 0 || tv.tv_usec < 0 || tv.tv_usec >= 1000000) return errno = EDOM, -1;
        return 0;
      }
      case SO_REUSEADDR:
        return ReadIntOpt(value, len, &v) ? 0 : -1;
      default:
        return errno = ENOPROTOOPT, -1;
    }
  }
  if (level == SOL_NETLINK) {
    switch (name) {
      case NETLINK_ADD_MEMBERSHIP:
      case NETLINK_DROP_MEMBERSHIP:
      case NETLINK_PKTINFO:
      case NETLINK_BROADCAST_ERROR:
      case NETLINK_NO_ENOBUFS:
      case NETLINK_CAP_ACK:
      case NETLINK_EXT_ACK:
      case NETLINK_GET_STRICT_CHK:
        return ReadIntOpt(value, len, &v) ? 0 : -1;
      default:
        return errno = ENOPROTOOPT, -1;
    }
  }
  return errno = ENOPROTOOPT, -1;
}

int NetlinkGetSockOpt(NetlinkState& state, int level, int name, void* value, uint32_t* len) {
  if (level == SOL_SOCKET) {
    switch (name) {
      case SO_TYPE: {
        int sock_type = SOCK_RAW;
        {
          std::lock_guard<std::mutex> lock(state.mu);
          sock_type = state.sock_type;
        }
        return WriteIntOpt(sock_type, value, len) ? 0 : -1;
      }
      case SO_DOMAIN:
        return WriteIntOpt(AF_NETLINK, value, len) ? 0 : -1;
      case SO_PROTOCOL:
        return WriteIntOpt(NETLINK_ROUTE, value, len) ? 0 : -1;
      case SO_ERROR:
        return WriteIntOpt(0, value, len) ? 0 : -1;
      case SO_RCVBUF: {
        int buf = 65536;
        {
          std::lock_guard<std::mutex> lock(state.mu);
          buf = state.rcvbuf;
        }
        return WriteIntOpt(buf, value, len) ? 0 : -1;
      }
      case SO_SNDBUF: {
        int buf = 65536;
        {
          std::lock_guard<std::mutex> lock(state.mu);
          buf = state.sndbuf;
        }
        return WriteIntOpt(buf, value, len) ? 0 : -1;
      }
      case SO_RCVTIMEO: {
        if (value == nullptr || len == nullptr) return errno = EFAULT, -1;
        uint32_t capacity = 0;
        if (!ws::CopyFromUser(&capacity, len, sizeof(capacity))) return errno = EFAULT, -1;
        if (capacity < sizeof(timeval)) return errno = EINVAL, -1;
        int ms = 0;
        {
          std::lock_guard<std::mutex> lock(state.mu);
          ms = state.rcv_timeout_ms;
        }
        timeval tv = {};
        tv.tv_sec = ms / 1000;
        tv.tv_usec = (ms % 1000) * 1000;
        const uint32_t actual = sizeof(tv);
        if (!ws::CopyToUser(value, &tv, actual) || !ws::CopyToUser(len, &actual, sizeof(actual))) {
          return errno = EFAULT, -1;
        }
        return 0;
      }
      case SO_SNDTIMEO: {
        if (value == nullptr || len == nullptr) return errno = EFAULT, -1;
        uint32_t capacity = 0;
        if (!ws::CopyFromUser(&capacity, len, sizeof(capacity))) return errno = EFAULT, -1;
        if (capacity < sizeof(timeval)) return errno = EINVAL, -1;
        timeval tv = {};
        const uint32_t actual = sizeof(tv);
        if (!ws::CopyToUser(value, &tv, actual) || !ws::CopyToUser(len, &actual, sizeof(actual))) {
          return errno = EFAULT, -1;
        }
        return 0;
      }
      default:
        return errno = ENOPROTOOPT, -1;
    }
  }
  return errno = ENOPROTOOPT, -1;
}

}  // namespace gesher
