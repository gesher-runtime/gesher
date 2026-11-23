// gesher-base: NETLINK_ROUTE interface queries and subscriptions over Windows IP Helper.

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
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>
#include <windows.h>

#include "event_internal.h"
#include "linux_socket.h"
#include "netlink_internal.h"
#include "socket_internal.h"

namespace gesher {

namespace {
constexpr uint32_t kAddressGroups = RTMGRP_IPV4_IFADDR | RTMGRP_IPV6_IFADDR;
constexpr uint32_t kSupportedGroups = kAddressGroups | RTMGRP_LINK;
constexpr size_t kMaxQueueBytes = 4 * 1024 * 1024;
constexpr size_t kMaxNetlinkRequestBytes = 65536;
constexpr size_t kMaxNetlinkPacketBytes = 4096;

using Bytes = std::vector<uint8_t>;
struct Datagram {
  Bytes bytes;
  uint32_t group = 0;  // sockaddr_nl uses a bitmask
};

struct AddressKey {
  uint32_t index;
  uint8_t family;
  std::array<uint8_t, sizeof(in6_addr)> address;
  auto operator<=>(const AddressKey&) const = default;
};

struct InterfaceSnapshot {
  std::map<uint32_t, Bytes> links;
  std::map<AddressKey, Bytes> addresses;
};
}  // namespace

struct NetlinkState {
  explicit NetlinkState(int type) : sock_type(type) {}

  std::mutex mu;
  std::condition_variable cv;
  const int sock_type;
  uint32_t bound_pid = 0;
  uint32_t bound_groups = 0;
  HANDLE port_reservation = nullptr;
  ws::InterfaceChangeWatch* link_watch = nullptr;
  ws::InterfaceChangeWatch* address_watch = nullptr;
  InterfaceSnapshot interfaces;
  bool closed = false;
  bool no_enobufs = false;
  int pending_error = 0;
  timeval rcv_timeout{};
  int rcvbuf = 65536;
  int sndbuf = 65536;
  size_t queued_bytes = 0;
  std::deque<Datagram> queue;
};

namespace {

uint32_t SelfNetlinkPid() {
  const int32_t pid = ws::LocalCredentials().pid;
  return pid > 0 ? static_cast<uint32_t>(pid) : 1u;
}

int BindPort(NetlinkState& state) {
  if (state.bound_pid != 0) return 0;
  static std::atomic<uint32_t> next_port{0x80000000u};
  uint32_t candidate = SelfNetlinkPid();
  // The Windows object manager arbitrates port ownership across Gesher
  // processes in the logon session. The name lives as long as the socket.
  for (unsigned attempt = 0; attempt < 1024; ++attempt) {
    const std::wstring name = L"Local\\Gesher.NetlinkRoute." + std::to_wstring(candidate);
    SetLastError(ERROR_SUCCESS);
    HANDLE reservation = CreateEventW(nullptr, TRUE, FALSE, name.c_str());
    const DWORD status = GetLastError();
    if (reservation != nullptr && status != ERROR_ALREADY_EXISTS) {
      state.port_reservation = reservation;
      state.bound_pid = candidate;
      return 0;
    }
    if (reservation != nullptr) CloseHandle(reservation);
    if (reservation == nullptr && status != ERROR_ACCESS_DENIED && status != ERROR_INVALID_HANDLE) {
      return errno = ENOBUFS, -1;
    }
    candidate = next_port.fetch_add(1, std::memory_order_relaxed);
    if (candidate == 0) candidate = next_port.fetch_add(1, std::memory_order_relaxed);
  }
  return errno = EADDRINUSE, -1;
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
  std::vector<Bytes> packets;
  Bytes current;
  size_t bytes = 0;
  bool overflow = false;

  void FinishPacket() {
    if (!current.empty()) {
      packets.push_back(std::move(current));
      current.clear();
    }
  }

  void Push(const Bytes& msg) {
    if (msg.size() > kMaxQueueBytes - bytes) {
      overflow = true;
      return;
    }
    bytes += msg.size();
    if (!current.empty() && current.size() + msg.size() > kMaxNetlinkPacketBytes) {
      FinishPacket();
    }
    current.insert(current.end(), msg.begin(), msg.end());
  }

  void PushDone(uint32_t seq, uint32_t pid, uint16_t flags = NLM_F_MULTI) {
    std::vector<uint8_t> done(NLMSG_LENGTH(sizeof(int)), 0);
    int err = 0;
    memcpy(done.data() + NLMSG_HDRLEN, &err, sizeof(err));
    FinalizeNlmsg(&done, NLMSG_DONE, flags, seq, pid);
    Push(done);
    FinishPacket();
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

Bytes AddressMessage(const ws::Interface& iface, const ws::InterfaceAddress& entry, AddressKey* key) {
  Bytes msg(NLMSG_LENGTH(sizeof(ifaddrmsg)), 0);
  ifaddrmsg ifa{};
  ifa.ifa_index = iface.index;
  if (entry.permanent) ifa.ifa_flags |= IFA_F_PERMANENT;
  using State = ws::InterfaceAddress::State;
  switch (entry.state) {
    case State::kDeprecated: ifa.ifa_flags |= IFA_F_DEPRECATED; break;
    case State::kTentative: ifa.ifa_flags |= IFA_F_TENTATIVE; break;
    case State::kDuplicate: ifa.ifa_flags |= IFA_F_DADFAILED; break;
    case State::kPreferred: break;
  }
  *key = {iface.index, 0, {}};
  if (entry.address.family == ws::Family::kInet) {
    sockaddr_in sin{};
    const size_t tail = sizeof(sin) - sizeof(sa_family_t);
    if (entry.address.data_len < tail) return {};
    memcpy(reinterpret_cast<char*>(&sin) + sizeof(sa_family_t), entry.address.data, tail);
    const int prefix = std::clamp(entry.prefix_len, 0, 32);
    ifa.ifa_family = AF_INET;
    ifa.ifa_prefixlen = static_cast<uint8_t>(prefix);
    ifa.ifa_scope = Ipv4Scope(iface.flags, sin.sin_addr);
    memcpy(key->address.data(), &sin.sin_addr, sizeof(sin.sin_addr));
    AppendAttr(&msg, IFA_ADDRESS, &sin.sin_addr, sizeof(sin.sin_addr));
    AppendAttr(&msg, IFA_LOCAL, &sin.sin_addr, sizeof(sin.sin_addr));
    if ((iface.flags & ws::kIfBroadcast) != 0) {
      const uint32_t mask = prefix == 0 ? 0u : (~0u << (32 - prefix));
      in_addr broadcast{};
      broadcast.s_addr = sin.sin_addr.s_addr | __builtin_bswap32(~mask);
      AppendAttr(&msg, IFA_BROADCAST, &broadcast, sizeof(broadcast));
    }
    AppendAttr(&msg, IFA_LABEL, iface.name.c_str(), iface.name.size() + 1);
  } else if (entry.address.family == ws::Family::kInet6) {
    sockaddr_in6 sin6{};
    const size_t tail = sizeof(sin6) - sizeof(sa_family_t);
    if (entry.address.data_len < tail) return {};
    memcpy(reinterpret_cast<char*>(&sin6) + sizeof(sa_family_t), entry.address.data, tail);
    ifa.ifa_family = AF_INET6;
    ifa.ifa_prefixlen = static_cast<uint8_t>(std::clamp(entry.prefix_len, 0, 128));
    ifa.ifa_scope = Ipv6Scope(iface.flags, sin6.sin6_addr);
    if (entry.temporary) ifa.ifa_flags |= IFA_F_TEMPORARY;
    memcpy(key->address.data(), &sin6.sin6_addr, sizeof(sin6.sin6_addr));
    AppendAttr(&msg, IFA_ADDRESS, &sin6.sin6_addr, sizeof(sin6.sin6_addr));
  } else {
    return {};
  }
  key->family = ifa.ifa_family;
  memcpy(msg.data() + NLMSG_HDRLEN, &ifa, sizeof(ifa));
  return msg;
}

Bytes LinkMessage(const ws::Interface& iface) {
  Bytes msg(NLMSG_LENGTH(sizeof(ifinfomsg)), 0);
  ifinfomsg ifi{};
  ifi.ifi_index = static_cast<int>(iface.index);
  ifi.ifi_flags = sock::ToIfFlags(iface.flags);
  using Kind = ws::Interface::LinkKind;
  switch (iface.kind) {
    case Kind::kLoopback: ifi.ifi_type = ARPHRD_LOOPBACK; break;
    case Kind::kEthernet: ifi.ifi_type = ARPHRD_ETHER; break;
    case Kind::kPpp: ifi.ifi_type = ARPHRD_PPP; break;
    case Kind::kOther: ifi.ifi_type = ARPHRD_NONE; break;
  }
  memcpy(msg.data() + NLMSG_HDRLEN, &ifi, sizeof(ifi));
  AppendAttr(&msg, IFLA_IFNAME, iface.name.c_str(), iface.name.size() + 1);
  AppendAttr(&msg, IFLA_MTU, &iface.mtu, sizeof(iface.mtu));
  return msg;
}

bool CaptureInterfaces(InterfaceSnapshot* snapshot) {
  std::vector<ws::Interface> interfaces;
  if (!sock::ListNamedInterfaces(&interfaces)) return false;
  for (const auto& iface : interfaces) {
    if (iface.index == 0) continue;
    snapshot->links.emplace(iface.index, LinkMessage(iface));
    for (const auto& entry : iface.addresses) {
      AddressKey key{};
      Bytes msg = AddressMessage(iface, entry, &key);
      if (!msg.empty()) snapshot->addresses.emplace(key, std::move(msg));
    }
  }
  return true;
}

void AppendErrorPacket(int error_code, const nlmsghdr& req, const uint8_t* payload,
                       uint32_t pid, PacketBatcher* out) {
  const size_t payload_len = error_code == 0 ? 0 : req.nlmsg_len - NLMSG_HDRLEN;
  const size_t len = NLMSG_LENGTH(sizeof(nlmsgerr)) + payload_len;
  Bytes msg(NLMSG_ALIGN(len), 0);
  nlmsgerr err = {};
  err.error = error_code;
  err.msg = req;
  memcpy(msg.data() + NLMSG_HDRLEN, &err, sizeof(err));
  if (payload_len != 0) {
    memcpy(msg.data() + NLMSG_LENGTH(sizeof(nlmsgerr)), payload, payload_len);
  }
  FinalizeNlmsg(&msg, NLMSG_ERROR, error_code == 0 ? NLM_F_CAPPED : 0, req.nlmsg_seq, pid);
  const uint32_t message_len = static_cast<uint32_t>(len);
  memcpy(msg.data(), &message_len, sizeof(message_len));
  out->Push(msg);
  out->FinishPacket();
}

bool ValidateNetlinkPeerAddr(const void* addr, uint32_t addrlen) {
  if (addrlen < sizeof(sockaddr_nl)) return errno = EINVAL, false;
  sockaddr_nl peer = {};
  if (!ws::CopyFromUser(&peer, addr, sizeof(peer))) return errno = EFAULT, false;
  if (peer.nl_family != AF_NETLINK) return errno = EAFNOSUPPORT, false;
  if (peer.nl_groups != 0) return errno = EPERM, false;
  if (peer.nl_pid != 0) return errno = EOPNOTSUPP, false;
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

void NotifyReady(NetlinkState& state) {
  state.cv.notify_all();
  if (HasPollWaiters()) WakePollers();
}

void QueueEvent(NetlinkState& state, Bytes msg, uint16_t type, uint32_t group) {
  if ((state.bound_groups & group) == 0) return;
  if (msg.size() > static_cast<size_t>(state.rcvbuf) -
                       std::min(state.queued_bytes, static_cast<size_t>(state.rcvbuf))) {
    if (!state.no_enobufs) state.pending_error = ENOBUFS;
    return;
  }
  FinalizeNlmsg(&msg, type, 0, 0, 0);
  state.queued_bytes += msg.size();
  state.queue.push_back({std::move(msg), group});
}

void OnInterfacesChanged(void* context) {
  auto& state = *static_cast<NetlinkState*>(context);
  {
    std::lock_guard<std::mutex> lock(state.mu);
    if (state.closed || state.bound_groups == 0) return;
    InterfaceSnapshot next;
    if (!CaptureInterfaces(&next)) {
      if (!state.no_enobufs) state.pending_error = ENOBUFS;
    } else {
      for (const auto& [key, msg] : state.interfaces.addresses) {
        if (!next.addresses.contains(key)) {
          QueueEvent(state, msg, RTM_DELADDR,
                     key.family == AF_INET ? RTMGRP_IPV4_IFADDR : RTMGRP_IPV6_IFADDR);
        }
      }
      for (const auto& [index, msg] : state.interfaces.links) {
        if (!next.links.contains(index)) QueueEvent(state, msg, RTM_DELLINK, RTMGRP_LINK);
      }
      for (const auto& [index, msg] : next.links) {
        auto before = state.interfaces.links.find(index);
        if (before == state.interfaces.links.end() || before->second != msg) {
          QueueEvent(state, msg, RTM_NEWLINK, RTMGRP_LINK);
        }
      }
      for (const auto& [key, msg] : next.addresses) {
        auto before = state.interfaces.addresses.find(key);
        if (before == state.interfaces.addresses.end() || before->second != msg) {
          QueueEvent(state, msg, RTM_NEWADDR,
                     key.family == AF_INET ? RTMGRP_IPV4_IFADDR : RTMGRP_IPV6_IFADDR);
        }
      }
      state.interfaces = std::move(next);
    }
  }
  NotifyReady(state);
}

struct WatchesToStop {
  ws::InterfaceChangeWatch* link = nullptr;
  ws::InterfaceChangeWatch* address = nullptr;
  ~WatchesToStop() {
    const int saved = errno;
    ws::StopInterfaceChanges(link);
    ws::StopInterfaceChanges(address);
    errno = saved;
  }
};

int SetGroups(NetlinkState& state, uint32_t groups, WatchesToStop* cleanup) {
  if ((groups & ~kSupportedGroups) != 0) return errno = EOPNOTSUPP, -1;
  if (state.closed) return errno = EBADF, -1;
  if (groups == state.bound_groups) return 0;

  auto* link = state.link_watch;
  auto* address = state.address_watch;
  if ((groups & RTMGRP_LINK) != 0 && link == nullptr) {
    link = ws::WatchInterfaceChanges(ws::InterfaceChangeKind::kLink, OnInterfacesChanged, &state);
    if (link == nullptr) return -1;
  }
  if ((groups & kAddressGroups) != 0 && address == nullptr) {
    address = ws::WatchInterfaceChanges(ws::InterfaceChangeKind::kAddress, OnInterfacesChanged, &state);
    if (address == nullptr) {
      if (link != state.link_watch) cleanup->link = link;
      return -1;
    }
  }

  if (state.bound_groups == 0) {
    InterfaceSnapshot initial;
    if (!CaptureInterfaces(&initial)) {
      cleanup->link = link;
      cleanup->address = address;
      return -1;
    }
    state.interfaces = std::move(initial);
  }
  state.bound_groups = groups;
  state.link_watch = (groups & RTMGRP_LINK) != 0 ? link : nullptr;
  state.address_watch = (groups & kAddressGroups) != 0 ? address : nullptr;
  if (groups == 0) state.interfaces = {};
  if (link != state.link_watch) cleanup->link = link;
  if (address != state.address_watch) cleanup->address = address;
  return 0;
}

int AppendReply(const nlmsghdr& request, const uint8_t* payload, uint32_t pid,
                InterfaceSnapshot* interfaces, bool* captured, PacketBatcher* out) {
  if ((request.nlmsg_flags & NLM_F_REQUEST) == 0 || request.nlmsg_type == NLMSG_NOOP) {
    if ((request.nlmsg_flags & NLM_F_ACK) != 0) AppendErrorPacket(0, request, payload, pid, out);
    return 0;
  }
  if (request.nlmsg_type != RTM_GETADDR) return -EOPNOTSUPP;
  const bool dump = (request.nlmsg_flags & NLM_F_DUMP) == NLM_F_DUMP;
  if (!dump || (request.nlmsg_flags & NLM_F_ATOMIC) != 0) return -EOPNOTSUPP;
  const size_t length = request.nlmsg_len - NLMSG_HDRLEN;
  if (length == 0) return -EINVAL;
  const uint8_t family = payload[0];
  if (family != AF_UNSPEC && family != AF_INET && family != AF_INET6) return -EAFNOSUPPORT;
  if (!*captured) {
    if (!CaptureInterfaces(interfaces)) return -errno;
    *captured = true;
  }

  if (length > sizeof(ifaddrmsg)) return -EOPNOTSUPP;
  uint32_t index = 0;
  if (length == sizeof(ifaddrmsg)) {
    ifaddrmsg filter;
    memcpy(&filter, payload, sizeof(filter));
    if (filter.ifa_prefixlen != 0 || filter.ifa_flags != 0 || filter.ifa_scope != 0) return -EOPNOTSUPP;
    index = filter.ifa_index;
  } else if (length > NLMSG_ALIGN(sizeof(rtgenmsg))) {
    return -EINVAL;
  }
  const uint16_t flags = NLM_F_MULTI | (index != 0 ? NLM_F_DUMP_FILTERED : 0);
  for (const auto& [key, message] : interfaces->addresses) {
    if ((family != AF_UNSPEC && family != key.family) || (index != 0 && index != key.index)) continue;
    Bytes packet = message;
    FinalizeNlmsg(&packet, RTM_NEWADDR, flags, request.nlmsg_seq, pid);
    out->Push(packet);
    if (out->overflow) break;
  }
  out->PushDone(request.nlmsg_seq, pid, flags);
  return 0;
}

}  // namespace

std::shared_ptr<NetlinkState> CreateNetlinkState(int sock_type) {
  return std::make_shared<NetlinkState>(sock_type);
}

void NetlinkClose(NetlinkState& state) {
  ws::InterfaceChangeWatch* link;
  ws::InterfaceChangeWatch* address;
  HANDLE port;
  {
    std::lock_guard<std::mutex> lock(state.mu);
    state.closed = true;
    state.queue.clear();
    state.queued_bytes = 0;
    state.interfaces = {};
    link = std::exchange(state.link_watch, nullptr);
    address = std::exchange(state.address_watch, nullptr);
    port = std::exchange(state.port_reservation, nullptr);
  }
  NotifyReady(state);
  ws::StopInterfaceChanges(link);
  ws::StopInterfaceChanges(address);
  if (port != nullptr) CloseHandle(port);
}

int NetlinkPoll(NetlinkState& state, int events) {
  std::lock_guard<std::mutex> lock(state.mu);
  if (state.closed) return ws::kPollNval;
  int result = events & ws::kPollOut;
  if (!state.queue.empty()) result |= events & ws::kPollIn;
  if (state.pending_error != 0) result |= ws::kPollErr;
  return result;
}

int NetlinkConnect(NetlinkState& state, const void* addr, uint32_t len) {
  if (addr == nullptr) return errno = EFAULT, -1;
  if (len < sizeof(sa_family_t)) return errno = EINVAL, -1;
  sa_family_t family;
  if (!ws::CopyFromUser(&family, addr, sizeof(family))) return errno = EFAULT, -1;
  if (family == AF_UNSPEC) return 0;
  if (!ValidateNetlinkPeerAddr(addr, len)) return -1;
  std::lock_guard<std::mutex> lock(state.mu);
  if (state.closed) return errno = EBADF, -1;
  return BindPort(state);
}

int NetlinkGetSockName(NetlinkState& state, void* addr, uint32_t* len, bool peer) {
  if (addr == nullptr || len == nullptr) return errno = EFAULT, -1;
  sockaddr_nl local = {};
  local.nl_family = AF_NETLINK;
  {
    std::lock_guard<std::mutex> lock(state.mu);
    if (state.closed) return errno = EBADF, -1;
    if (!peer) {
      local.nl_pid = state.bound_pid;
      local.nl_groups = state.bound_groups;
    }
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
  if ((flags & MSG_OOB) != 0) return errno = EOPNOTSUPP, -1;
  if (addr != nullptr && !ValidateNetlinkPeerAddr(addr, addrlen)) return -1;
  if (count > 0 && buffers == nullptr) return errno = EFAULT, -1;

  size_t total_len = 0;
  for (size_t i = 0; i < count; ++i) {
    if (buffers[i].len > kMaxNetlinkRequestBytes - total_len) return errno = EMSGSIZE, -1;
    total_len += buffers[i].len;
  }
  if (total_len == 0) return errno = ENODATA, -1;
  {
    std::lock_guard<std::mutex> lock(state.mu);
    if (state.closed) return errno = EBADF, -1;
    if (total_len > static_cast<size_t>(state.sndbuf - 32)) return errno = EMSGSIZE, -1;
  }
  Bytes req(total_len);
  size_t pos = 0;
  for (size_t i = 0; i < count; ++i) {
    if (buffers[i].len == 0) continue;
    if (buffers[i].data == nullptr || !ws::CopyFromUser(req.data() + pos, buffers[i].data, buffers[i].len)) {
      return errno = EFAULT, -1;
    }
    pos += buffers[i].len;
  }
  if (req.size() < sizeof(nlmsghdr)) return errno = EINVAL, -1;

  for (size_t offset = 0; offset < req.size();) {
    if (req.size() - offset < sizeof(nlmsghdr)) return errno = EINVAL, -1;
    nlmsghdr hdr;
    memcpy(&hdr, req.data() + offset, sizeof(hdr));
    if (hdr.nlmsg_len < sizeof(nlmsghdr) || hdr.nlmsg_len > req.size() - offset) {
      return errno = EINVAL, -1;
    }
    // An environment value is insufficient authority to expose hardware
    // addresses. Only the ordinary app NETLINK_ROUTE contract is available.
    if (hdr.nlmsg_type == RTM_GETLINK ||
        hdr.nlmsg_type == RTM_NEWLINK || hdr.nlmsg_type == RTM_DELLINK ||
        hdr.nlmsg_type == RTM_NEWADDR || hdr.nlmsg_type == RTM_DELADDR ||
        hdr.nlmsg_type == RTM_NEWROUTE || hdr.nlmsg_type == RTM_DELROUTE) {
      return errno = EACCES, -1;
    }
    offset += std::min(static_cast<size_t>(NLMSG_ALIGN(hdr.nlmsg_len)), req.size() - offset);
  }

  PacketBatcher batch;
  InterfaceSnapshot interfaces;
  bool captured = false;
  {
    std::lock_guard<std::mutex> lock(state.mu);
    if (state.closed) return errno = EBADF, -1;
    if (BindPort(state) < 0) return -1;
    for (size_t offset = 0; offset < req.size();) {
      nlmsghdr hdr;
      memcpy(&hdr, req.data() + offset, sizeof(hdr));
      const uint8_t* payload = req.data() + offset + NLMSG_HDRLEN;
      const int error = AppendReply(hdr, payload, state.bound_pid, &interfaces, &captured, &batch);
      if (error != 0) AppendErrorPacket(error, hdr, payload, state.bound_pid, &batch);
      if (batch.overflow) break;
      offset += std::min(static_cast<size_t>(NLMSG_ALIGN(hdr.nlmsg_len)), req.size() - offset);
    }
    if (batch.overflow || batch.bytes > kMaxQueueBytes - state.queued_bytes) {
      return errno = ENOBUFS, -1;
    }
    for (auto& pkt : batch.packets) {
      state.queue.push_back({std::move(pkt), 0});
    }
    state.queued_bytes += batch.bytes;
  }
  if (batch.bytes != 0) NotifyReady(state);
  return static_cast<ssize_t>(req.size());
}

ssize_t NetlinkRecv(NetlinkState& state, ws::Buffer* buffers, size_t count, int flags, void* addr,
                    uint32_t* addrlen, bool nonblocking, int* result_flags) {
  if (count > 0 && buffers == nullptr) return errno = EFAULT, -1;
  for (size_t i = 0; i < count; ++i) {
    if (buffers[i].len > 0 && buffers[i].data == nullptr) return errno = EFAULT, -1;
  }

  if ((flags & MSG_OOB) != 0) return errno = EOPNOTSUPP, -1;
  Datagram datagram;
  {
    std::unique_lock<std::mutex> lock(state.mu);
    const auto ready = [&] { return state.closed || state.pending_error != 0 || !state.queue.empty(); };
    if (!ready()) {
      if (nonblocking || (flags & MSG_DONTWAIT) != 0) return errno = EAGAIN, -1;
      const auto timeout = std::chrono::seconds(std::min<int64_t>(state.rcv_timeout.tv_sec, 2147483)) +
                           std::chrono::microseconds(state.rcv_timeout.tv_usec);
      if (timeout.count() == 0) {
        state.cv.wait(lock, ready);
      } else if (!state.cv.wait_for(lock, timeout, ready)) {
        return errno = EAGAIN, -1;
      }
    }
    if (state.closed) return errno = EBADF, -1;
    if (state.pending_error != 0) return errno = std::exchange(state.pending_error, 0), -1;
    if ((flags & MSG_PEEK) != 0) {
      datagram = state.queue.front();
    } else {
      datagram = std::move(state.queue.front());
      state.queued_bytes -= datagram.bytes.size();
      state.queue.pop_front();
    }
  }

  const Bytes& pkt = datagram.bytes;
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
    kernel_addr.nl_groups = datagram.group;
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
      case SO_SNDBUF: {
        if (!ReadIntOpt(value, len, &v)) return -1;
        std::lock_guard<std::mutex> lock(state.mu);
        if (state.closed) return errno = EBADF, -1;
        int& buffer = name == SO_RCVBUF ? state.rcvbuf : state.sndbuf;
        buffer = 2 * std::clamp(v, 256, static_cast<int>(kMaxQueueBytes / 2));
        return 0;
      }
      case SO_RCVTIMEO: {
        if (len < sizeof(timeval)) return errno = EINVAL, -1;
        if (value == nullptr) return errno = EFAULT, -1;
        timeval tv = {};
        if (!ws::CopyFromUser(&tv, value, sizeof(tv))) return errno = EFAULT, -1;
        if (tv.tv_sec < 0 || tv.tv_usec < 0 || tv.tv_usec >= 1000000) return errno = EDOM, -1;
        std::lock_guard<std::mutex> lock(state.mu);
        if (state.closed) return errno = EBADF, -1;
        state.rcv_timeout = tv;
        return 0;
      }
      default:
        return errno = ENOPROTOOPT, -1;
    }
  }
  if (level == SOL_NETLINK) {
    switch (name) {
      case NETLINK_ADD_MEMBERSHIP:
      case NETLINK_DROP_MEMBERSHIP: {
        if (!ReadIntOpt(value, len, &v)) return -1;
        if (v <= 0) return errno = EINVAL, -1;
        if (v > 32) return errno = EOPNOTSUPP, -1;
        const uint32_t group = 1u << (v - 1);
        if ((group & kSupportedGroups) == 0) return errno = EOPNOTSUPP, -1;
        WatchesToStop cleanup;
        std::lock_guard<std::mutex> lock(state.mu);
        const uint32_t groups = name == NETLINK_ADD_MEMBERSHIP ? state.bound_groups | group
                                                              : state.bound_groups & ~group;
        return SetGroups(state, groups, &cleanup);
      }
      case NETLINK_NO_ENOBUFS: {
        if (!ReadIntOpt(value, len, &v)) return -1;
        std::lock_guard<std::mutex> lock(state.mu);
        if (state.closed) return errno = EBADF, -1;
        state.no_enobufs = v != 0;
        return 0;
      }
      default:
        return errno = ENOPROTOOPT, -1;
    }
  }
  return errno = ENOPROTOOPT, -1;
}

int NetlinkGetSockOpt(NetlinkState& state, int level, int name, void* value, uint32_t* len) {
  std::lock_guard<std::mutex> lock(state.mu);
  if (state.closed) return errno = EBADF, -1;
  if (level == SOL_NETLINK && name == NETLINK_NO_ENOBUFS) {
    return WriteIntOpt(state.no_enobufs, value, len) ? 0 : -1;
  }
  if (level == SOL_SOCKET) {
    switch (name) {
      case SO_TYPE:
        return WriteIntOpt(state.sock_type, value, len) ? 0 : -1;
      case SO_DOMAIN:
        return WriteIntOpt(AF_NETLINK, value, len) ? 0 : -1;
      case SO_PROTOCOL:
        return WriteIntOpt(NETLINK_ROUTE, value, len) ? 0 : -1;
      case SO_ERROR:
        if (!WriteIntOpt(state.pending_error, value, len)) return -1;
        state.pending_error = 0;
        return 0;
      case SO_RCVBUF:
        return WriteIntOpt(state.rcvbuf, value, len) ? 0 : -1;
      case SO_SNDBUF:
        return WriteIntOpt(state.sndbuf, value, len) ? 0 : -1;
      case SO_RCVTIMEO: {
        if (value == nullptr || len == nullptr) return errno = EFAULT, -1;
        uint32_t capacity = 0;
        if (!ws::CopyFromUser(&capacity, len, sizeof(capacity))) return errno = EFAULT, -1;
        if (capacity < sizeof(timeval)) return errno = EINVAL, -1;
        const uint32_t actual = sizeof(state.rcv_timeout);
        if (!ws::CopyToUser(value, &state.rcv_timeout, actual) ||
            !ws::CopyToUser(len, &actual, sizeof(actual))) {
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
