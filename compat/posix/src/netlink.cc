// gesher-base: AF_NETLINK / NETLINK_ROUTE socket backend over Windows interface enumeration.
//
// Android's NDK/WebRTC callers (such as chromium/webrtc ifaddrs_android.cc) open
// socket(AF_NETLINK, SOCK_RAW, NETLINK_ROUTE) to dump RTM_GETADDR
// instead of calling bionic's getifaddrs(). This module translates those route
// queries into standard Linux rtnetlink replies built from ws::ListInterfaces().

#include <errno.h>
#include <fcntl.h>
#include <linux/if_addr.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <net/if.h>
#include <netinet/in.h>
#include <pthread.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

#include "event_internal.h"
#include "internal.h"
#include <gesher/fork.h>
#include "linux_socket.h"
#include "netlink_internal.h"
#include "socket_internal.h"

namespace gesher {

namespace {

constexpr int kMaxNetlinkRequestBytes = 65536;
constexpr int kMaxNetlinkReceiveBytes = 256 * 1024;
// Pack dump replies into <= 4096-byte datagrams for callers' fixed buffers.
constexpr size_t kMaxNetlinkPacketBytes = 4096;
constexpr DWORD kCancellationSliceMs = 10;

// No process-local pointers or C++ synchronization in the shared section.
struct NetlinkShared {
  alignas(8) volatile LONG64 queue{};  // head (low word), occupied bytes (high word)
  volatile LONG nonblocking{};
  uint32_t bound_pid{};
  volatile LONG rcv_timeout_ms{};
  int rcvbuf{};
  int sndbuf{};
  uint8_t data[kMaxNetlinkReceiveBytes];
};

struct SharedPortCounter {
  volatile LONG next{};
};

}  // namespace

struct NetlinkState {
  int sock_type = SOCK_RAW;
  bool closed = false;  // last descriptor closed in this process
  unsigned active = 0;
  NetlinkState* next = nullptr;
  HANDLE section = nullptr;
  HANDLE lock = nullptr;
  HANDLE ready = nullptr;
  NetlinkShared* shared = nullptr;
};

namespace {

std::mutex g_resources_mutex;
NetlinkState* g_resources = nullptr;
// All descendants of a process that owns route sockets share its synthetic
// port counter. The handle keeps the view alive across guest fork.
HANDLE g_port_section = nullptr;
SharedPortCounter* g_ports = nullptr;
std::atomic<uint32_t> g_pid_port_issued{0};

void RetireResources(NetlinkState& state) {
  if (!state.shared) return;
  UnmapViewOfFile(state.shared);
  state.shared = nullptr;
  for (HANDLE handle : {state.section, state.lock, state.ready}) CloseHandle(handle);
}

using ResourceForkTraits = ForkLockTraits<std::mutex>;
const ForkLockOps kResourcesForkOps{ResourceForkTraits::TryLock, ResourceForkTraits::Unlock,
  [](void* lock) {
    // The shared_ptr control blocks copied by fork can still count parent
    // callers. The child has their descriptor aliases, but no in-flight I/O.
    for (auto* state = g_resources; state; state = state->next) {
      state->active = 0;
      if (state->closed) RetireResources(*state);
    }
    ResourceForkTraits::Child(lock);
  }};
const ForkLockRegistration kResourcesForkLock(kForkOrderLeaf, "route netlink resources",
    []() -> void* { return &g_resources_mutex; }, &kResourcesForkOps);

int WinError() { return ErrnoFromWin32(GetLastError()); }

class Operation {
 public:
  explicit Operation(NetlinkState& state) : state_(state) {
    std::lock_guard<std::mutex> guard(g_resources_mutex);
    if (!state_.closed) {
      ++state_.active;
      held_ = true;
    }
  }
  ~Operation() {
    if (!held_) return;
    std::lock_guard<std::mutex> guard(g_resources_mutex);
    if (--state_.active == 0 && state_.closed) RetireResources(state_);
  }
  explicit operator bool() const { return held_; }
  Operation(const Operation&) = delete;
  Operation& operator=(const Operation&) = delete;
 private:
  NetlinkState& state_;
  bool held_ = false;
};

class QueueLock {
 public:
  explicit QueueLock(NetlinkState& state, DWORD wait = 1000) : lock_(state.lock) {
    const DWORD result = WaitForSingleObject(lock_, wait);
    held_ = result == WAIT_OBJECT_0 || result == WAIT_ABANDONED;
    if (!held_) error_ = result == WAIT_TIMEOUT ? EAGAIN : WinError();
  }
  ~QueueLock() { if (held_) ReleaseMutex(lock_); }
  int error() const { return error_; }
  QueueLock(const QueueLock&) = delete;
  QueueLock& operator=(const QueueLock&) = delete;
 private:
  HANDLE lock_;
  bool held_ = false;
  int error_ = 0;
};

struct Cursor {
  uint32_t head;
  uint32_t used;
};

Cursor GetCursor(NetlinkShared& shared) {
  const auto bits = static_cast<uint64_t>(InterlockedCompareExchange64(&shared.queue, 0, 0));
  return {static_cast<uint32_t>(bits), static_cast<uint32_t>(bits >> 32)};
}

void SetCursor(NetlinkShared& shared, Cursor cursor) {
  const uint64_t bits = (static_cast<uint64_t>(cursor.used) << 32) | cursor.head;
  InterlockedExchange64(&shared.queue, static_cast<LONG64>(bits));
}

uint32_t RingAdvance(uint32_t offset, size_t count) {
  return static_cast<uint32_t>((offset + count) % kMaxNetlinkReceiveBytes);
}

bool CommitRingRange(NetlinkShared& shared, uint32_t offset, size_t count) {
  const size_t first = std::min(count, static_cast<size_t>(kMaxNetlinkReceiveBytes - offset));
  if (!VirtualAlloc(shared.data + offset, first, MEM_COMMIT, PAGE_READWRITE) ||
      (first < count && !VirtualAlloc(shared.data, count - first, MEM_COMMIT, PAGE_READWRITE))) {
    errno = WinError();
    return false;
  }
  return true;
}

void RingWrite(NetlinkShared& shared, uint32_t* offset, const void* data, size_t count) {
  const size_t first = std::min(count, static_cast<size_t>(kMaxNetlinkReceiveBytes - *offset));
  memcpy(shared.data + *offset, data, first);
  if (first < count) memcpy(shared.data, static_cast<const uint8_t*>(data) + first, count - first);
  *offset = RingAdvance(*offset, count);
}

void RingRead(const NetlinkShared& shared, uint32_t* offset, void* data, size_t count) {
  const size_t first = std::min(count, static_cast<size_t>(kMaxNetlinkReceiveBytes - *offset));
  memcpy(data, shared.data + *offset, first);
  if (first < count) memcpy(static_cast<uint8_t*>(data) + first, shared.data, count - first);
  *offset = RingAdvance(*offset, count);
}

bool AutoBindPort(NetlinkShared& shared) {
  if (shared.bound_pid != 0) return true;
  const DWORD pid = GetCurrentProcessId();
  uint32_t expected = g_pid_port_issued.load(std::memory_order_relaxed);
  if (expected != pid &&
      g_pid_port_issued.compare_exchange_strong(expected, pid, std::memory_order_relaxed)) {
    shared.bound_pid = pid;
    return true;
  }
  LONG current = InterlockedCompareExchange(&g_ports->next, 0, 0);
  for (;;) {
    // Keep the synthetic range disjoint from the native process identity.
    if (current >= 0) return errno = ENOSPC, false;
    const LONG next = current + 1;
    const LONG previous = InterlockedCompareExchange(&g_ports->next, next, current);
    if (previous == current) {
      shared.bound_pid = static_cast<uint32_t>(current);
      return true;
    }
    current = previous;
  }
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
  size_t bytes = 0;

  bool Push(const std::vector<uint8_t>& msg) {
    if (msg.size() > kMaxNetlinkPacketBytes ||
        msg.size() > kMaxNetlinkReceiveBytes - bytes) return errno = ENOBUFS, false;
    bytes += msg.size();
    if (!current.empty() && current.size() + msg.size() > kMaxNetlinkPacketBytes) {
      packets.push_back(std::move(current));
      current.clear();
    }
    current.insert(current.end(), msg.begin(), msg.end());
    return true;
  }

  bool PushDone(uint32_t seq, uint32_t pid) {
    std::vector<uint8_t> done(NLMSG_LENGTH(sizeof(int)), 0);
    int err = 0;
    memcpy(done.data() + NLMSG_HDRLEN, &err, sizeof(err));
    FinalizeNlmsg(&done, NLMSG_DONE, NLM_F_MULTI, seq, pid);
    if (!Push(done)) return false;
    if (!current.empty()) {
      packets.push_back(std::move(current));
      current.clear();
    }
    return true;
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
        if (!out->Push(msg)) return false;
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
        ifa.ifa_scope = Ipv6Scope(iface.flags, sin6.sin6_addr);
        ifa.ifa_index = iface.index;
        memcpy(msg.data() + sizeof(nlmsghdr), &ifa, sizeof(ifa));

        AppendAttr(&msg, IFA_ADDRESS, &sin6.sin6_addr, sizeof(sin6.sin6_addr));
        FinalizeNlmsg(&msg, RTM_NEWADDR, NLM_F_MULTI, seq, pid);
        if (!out->Push(msg)) return false;
      }
    }
  }
  return out->PushDone(seq, pid);
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
  if (peer.nl_groups != 0) return errno = EOPNOTSUPP, false;
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

std::shared_ptr<NetlinkState> CreateNetlinkState(int sock_type, bool nonblocking) {
  std::lock_guard<std::mutex> guard(g_resources_mutex);
  if (!g_port_section) {
    HANDLE section = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                                        sizeof(SharedPortCounter), nullptr);
    if (!section) return errno = WinError(), nullptr;
    auto* shared = static_cast<SharedPortCounter*>(MapViewOfFile(section, FILE_MAP_WRITE, 0, 0, 0));
    if (!shared) {
      const int error = WinError();
      CloseHandle(section);
      return errno = error, nullptr;
    }
    g_port_section = section;
    shared->next = INT32_MIN;
    g_ports = shared;
  }

  HANDLE section = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE | SEC_RESERVE, 0,
                                      sizeof(NetlinkShared), nullptr);
  if (!section) return errno = WinError(), nullptr;
  auto* shared = static_cast<NetlinkShared*>(MapViewOfFile(section, FILE_MAP_WRITE, 0, 0, 0));
  const bool header = shared &&
      VirtualAlloc(shared, offsetof(NetlinkShared, data), MEM_COMMIT, PAGE_READWRITE);
  HANDLE lock = header ? CreateMutexW(nullptr, FALSE, nullptr) : nullptr;
  HANDLE ready = lock ? CreateEventW(nullptr, TRUE, FALSE, nullptr) : nullptr;
  if (!ready) {
    const int error = WinError();
    if (lock) CloseHandle(lock);
    if (shared) UnmapViewOfFile(shared);
    CloseHandle(section);
    return errno = error, nullptr;
  }
  shared->nonblocking = nonblocking ? 1 : 0;
  shared->rcvbuf = 65536;
  shared->sndbuf = 65536;
  auto state = std::shared_ptr<NetlinkState>(new NetlinkState, [](NetlinkState* released) {
    {
      std::lock_guard<std::mutex> lock(g_resources_mutex);
      auto** link = &g_resources;
      while (*link != released) link = &(*link)->next;
      *link = released->next;
      RetireResources(*released);
    }
    delete released;
  });
  state->sock_type = sock_type;
  state->section = section;
  state->lock = lock;
  state->ready = ready;
  state->shared = shared;
  state->next = g_resources;
  g_resources = state.get();
  return state;
}

void NetlinkClose(NetlinkState& state) {
  {
    std::lock_guard<std::mutex> guard(g_resources_mutex);
    state.closed = true;
    // Another process can still own an inherited descriptor. Retire only
    // this process's mapped view and handles, leaving the shared queue intact.
    if (state.active == 0) RetireResources(state);
  }
  if (HasPollWaiters()) WakePollers();
}

int NetlinkPoll(NetlinkState& state, int events) {
  const Operation operation(state);
  if (!operation) return ws::kPollNval;
  int ready = events & ws::kPollOut;
  if ((events & ws::kPollIn) && GetCursor(*state.shared).used != 0) ready |= ws::kPollIn;
  return ready;
}

int NetlinkGetStatus(NetlinkState& state) {
  const Operation operation(state);
  if (!operation) return errno = EBADF, -1;
  return O_RDWR | (InterlockedCompareExchange(&state.shared->nonblocking, 0, 0) ? O_NONBLOCK : 0);
}

int NetlinkSetStatus(NetlinkState& state, bool nonblocking) {
  const Operation operation(state);
  if (!operation) return errno = EBADF, -1;
  InterlockedExchange(&state.shared->nonblocking, nonblocking ? 1 : 0);
  return 0;
}

int NetlinkBind(NetlinkState& state, const void* addr, uint32_t len) {
  if (addr == nullptr) return errno = EFAULT, -1;
  if (len < sizeof(sockaddr_nl)) return errno = EINVAL, -1;
  sockaddr_nl local = {};
  if (!ws::CopyFromUser(&local, addr, sizeof(local))) return errno = EFAULT, -1;
  if (local.nl_family != AF_NETLINK) return errno = EINVAL, -1;
  if (local.nl_groups) return errno = EOPNOTSUPP, -1;
  const Operation operation(state);
  if (!operation) return errno = EBADF, -1;
  // The ordinary Android app domains lack route socket bind permission.
  return errno = EACCES, -1;
}

int NetlinkConnect(NetlinkState& state, const void* addr, uint32_t len) {
  if (addr == nullptr) return errno = EFAULT, -1;
  sa_family_t family = AF_UNSPEC;
  if (len < sizeof(family)) return errno = EINVAL, -1;
  if (!ws::CopyFromUser(&family, addr, sizeof(family))) return errno = EFAULT, -1;
  // The kernel is the only destination, including after disconnecting.
  if (family == AF_UNSPEC) return 0;
  if (!ValidateNetlinkPeerAddr(addr, len)) return -1;
  const Operation operation(state);
  if (!operation) return errno = EBADF, -1;
  const QueueLock lock(state);
  if (lock.error()) return errno = lock.error(), -1;
  return AutoBindPort(*state.shared) ? 0 : -1;
}

int NetlinkGetName(NetlinkState& state, void* addr, uint32_t* len, bool peer) {
  if (addr == nullptr || len == nullptr) return errno = EFAULT, -1;
  sockaddr_nl local = {};
  local.nl_family = AF_NETLINK;
  if (!peer) {
    const Operation operation(state);
    if (!operation) return errno = EBADF, -1;
    const QueueLock lock(state);
    if (lock.error()) return errno = lock.error(), -1;
    local.nl_pid = state.shared->bound_pid;
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
  constexpr int kSupportedFlags = MSG_DONTWAIT | MSG_NOSIGNAL;
  if ((flags & ~kSupportedFlags) != 0) return errno = EOPNOTSUPP, -1;
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

  // A multipart dump request occupies one datagram. Reading only its
  // first message would silently ignore subsequent requests.
  nlmsghdr hdr = {};
  memcpy(&hdr, req.data(), sizeof(hdr));
  if (hdr.nlmsg_len < NLMSG_HDRLEN || hdr.nlmsg_len > req.size()) return errno = EINVAL, -1;
  if (req.size() > NLMSG_ALIGN(hdr.nlmsg_len)) return errno = EOPNOTSUPP, -1;
  // SELinux denies RTM_GETLINK to ordinary Android apps at send time.
  if (hdr.nlmsg_type == RTM_GETLINK) return errno = EACCES, -1;

  const Operation operation(state);
  if (!operation) return errno = EBADF, -1;
  auto& shared = *state.shared;
  const bool immediate = (flags & MSG_DONTWAIT) ||
                         InterlockedCompareExchange(&shared.nonblocking, 0, 0);
  const DWORD lock_wait = immediate ? 0 : 1000;
  const bool address_request = hdr.nlmsg_type == RTM_GETADDR &&
      (hdr.nlmsg_flags & (NLM_F_REQUEST | NLM_F_DUMP)) == (NLM_F_REQUEST | NLM_F_DUMP) &&
      hdr.nlmsg_len > NLMSG_HDRLEN;
  const uint8_t family = address_request ? req[NLMSG_HDRLEN] : AF_UNSPEC;
  const bool dump = address_request && (family == AF_UNSPEC || family == AF_INET || family == AF_INET6);
  uint32_t reply_pid;
  {
    const QueueLock lock(state, lock_wait);
    if (lock.error()) return errno = lock.error(), -1;
    if (req.size() > static_cast<size_t>(shared.sndbuf)) return errno = EMSGSIZE, -1;
    if (dump && GetCursor(shared).used != 0) return errno = EBUSY, -1;
    if (!AutoBindPort(shared)) return -1;
    reply_pid = shared.bound_pid;
  }

  // Adapter enumeration may call Windows network providers. No reader,
  // descriptor alias, or process sharing this socket waits for it under the
  // receive queue's mutex.
  PacketBatcher batch;
  if (dump) {
    if (!AppendAddrDump(family, hdr.nlmsg_seq, reply_pid, &batch)) return -1;
  } else if (address_request) {
    AppendErrorPacket(-EAFNOSUPPORT, hdr, reply_pid, &batch);
  } else if (hdr.nlmsg_type == NLMSG_NOOP) {
    if (hdr.nlmsg_flags & NLM_F_ACK) AppendErrorPacket(0, hdr, reply_pid, &batch);
  } else {
    AppendErrorPacket(-EOPNOTSUPP, hdr, reply_pid, &batch);
  }

  const size_t bytes = batch.bytes + batch.packets.size() * sizeof(uint32_t);
  if (bytes == 0) return static_cast<ssize_t>(req.size());
  {
    const QueueLock lock(state, lock_wait);
    if (lock.error()) return errno = lock.error(), -1;
    auto cursor = GetCursor(shared);
    if (dump && cursor.used != 0) return errno = EBUSY, -1;
    if (bytes + cursor.used > static_cast<size_t>(shared.rcvbuf)) return errno = ENOBUFS, -1;
    uint32_t tail = RingAdvance(cursor.head, cursor.used);
    // Keep empty sockets cheap. If a commitment fails, the old cursor and
    // every already-published record remain intact.
    if (!CommitRingRange(shared, tail, bytes)) return -1;
    for (const auto& packet : batch.packets) {
      const auto size = static_cast<uint32_t>(packet.size());
      RingWrite(shared, &tail, &size, sizeof(size));
      RingWrite(shared, &tail, packet.data(), size);
    }
    // One store publishes all datagrams, even if a process dies under the mutex.
    SetCursor(shared, {cursor.head, cursor.used + static_cast<uint32_t>(bytes)});
    SetEvent(state.ready);
  }
  if (HasPollWaiters()) WakePollers();
  return static_cast<ssize_t>(req.size());
}

namespace {

void ReleaseNetlinkCaller(void* raw) {
  // The deferred cancellation path may skip C++ destructors.
  static_cast<std::shared_ptr<ws::SocketDescription>*>(raw)->reset();
}

}  // namespace

ssize_t NetlinkRecv(std::shared_ptr<ws::SocketDescription>& description,
                    ws::Buffer* buffers, size_t count, int flags, void* addr,
                    uint32_t* addrlen, bool nonblocking, int* result_flags) {
  if (count > 0 && buffers == nullptr) return errno = EFAULT, -1;
  for (size_t i = 0; i < count; ++i) {
    if (buffers[i].len > 0 && buffers[i].data == nullptr) return errno = EFAULT, -1;
  }

  auto& state = *description->netlink;
  using Clock = std::chrono::steady_clock;
  auto deadline = Clock::time_point::max();
  bool immediate = nonblocking || (flags & MSG_DONTWAIT);
  {
    const Operation operation(state);
    if (!operation) return errno = EBADF, -1;
    const int timeout = InterlockedCompareExchange(&state.shared->rcv_timeout_ms, 0, 0);
    if (timeout > 0) deadline = Clock::now() + std::chrono::milliseconds(timeout);
    immediate |= InterlockedCompareExchange(&state.shared->nonblocking, 0, 0) != 0;
  }

  std::vector<uint8_t> pkt;
  int error = 0;
  pthread_cleanup_push(ReleaseNetlinkCaller, &description);
  for (;;) {
    // Like the Winsock datagram wait, check cancellation without a held lock
    // or a retained temporary buffer. Data and close notify the wait directly.
    pthread_testcancel();
    const Operation operation(state);
    if (!operation) {
      error = EBADF;
      break;
    }
    auto& shared = *state.shared;
    {
      const QueueLock lock(state, immediate ? 0 : kCancellationSliceMs);
      if (!lock.error()) {
        auto cursor = GetCursor(shared);
        if (cursor.used != 0) {
          uint32_t length = 0;
          uint32_t head = cursor.head;
          RingRead(shared, &head, &length, sizeof(length));
          if (length == 0 || length > kMaxNetlinkPacketBytes ||
              length + sizeof(length) > cursor.used) {
            error = EIO;
            break;
          }
          pkt.resize(length);
          RingRead(shared, &head, pkt.data(), length);
          if (!(flags & MSG_PEEK)) {
            cursor.head = head;
            cursor.used -= length + sizeof(length);
            SetCursor(shared, cursor);
            if (cursor.used == 0) ResetEvent(state.ready);
          }
          break;
        }
        // An abandoned writer may have published a cursor without setting
        // the event (or an abandoned reader may not have cleared it).
        ResetEvent(state.ready);
      } else if (lock.error() != EAGAIN) {
        error = lock.error();
        break;
      }
    }
    const auto now = Clock::now();
    if (immediate || now >= deadline) {
      error = EAGAIN;
      break;
    }
    const auto remaining = std::chrono::ceil<std::chrono::milliseconds>(deadline - now).count();
    const DWORD wait = static_cast<DWORD>(std::min<int64_t>(kCancellationSliceMs, remaining));
    if (WaitForSingleObject(state.ready, wait) == WAIT_FAILED) {
      error = WinError();
      break;
    }
  }
  pthread_cleanup_pop(0);
  if (error) return errno = error, -1;

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
  const Operation operation(state);
  if (!operation) return errno = EBADF, -1;
  const QueueLock lock(state);
  if (lock.error()) return errno = lock.error(), -1;
  auto& shared = *state.shared;
  int v = 0;
  if (level == SOL_SOCKET) {
    switch (name) {
      case SO_RCVBUF:
        if (!ReadIntOpt(value, len, &v)) return -1;
        {
          shared.rcvbuf = std::clamp(v, 128, kMaxNetlinkReceiveBytes / 2) * 2;
        }
        return 0;
      case SO_SNDBUF:
        if (!ReadIntOpt(value, len, &v)) return -1;
        {
          shared.sndbuf = std::clamp(v, 128, kMaxNetlinkRequestBytes / 2) * 2;
        }
        return 0;
      case SO_RCVTIMEO: {
        if (value == nullptr || len < sizeof(timeval)) return errno = EINVAL, -1;
        timeval tv = {};
        if (!ws::CopyFromUser(&tv, value, sizeof(tv))) return errno = EFAULT, -1;
        if (tv.tv_sec < 0 || tv.tv_usec < 0 || tv.tv_usec >= 1000000) return errno = EDOM, -1;
        const long long ms = std::min(2147483647LL,
            std::min(2147484LL, static_cast<long long>(tv.tv_sec)) * 1000 + (tv.tv_usec + 999) / 1000);
        InterlockedExchange(&shared.rcv_timeout_ms, static_cast<LONG>(ms));
        return 0;
      }
      default:
        return errno = ENOPROTOOPT, -1;
    }
  }
  if (level == SOL_NETLINK &&
      (name == NETLINK_ADD_MEMBERSHIP || name == NETLINK_DROP_MEMBERSHIP)) {
    return errno = EOPNOTSUPP, -1;
  }
  return errno = ENOPROTOOPT, -1;
}

int NetlinkGetSockOpt(NetlinkState& state, int level, int name, void* value, uint32_t* len) {
  const Operation operation(state);
  if (!operation) return errno = EBADF, -1;
  const QueueLock lock(state);
  if (lock.error()) return errno = lock.error(), -1;
  auto& shared = *state.shared;
  if (level == SOL_SOCKET) {
    switch (name) {
      case SO_TYPE: {
        int sock_type = SOCK_RAW;
        {
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
          buf = shared.rcvbuf;
        }
        return WriteIntOpt(buf, value, len) ? 0 : -1;
      }
      case SO_SNDBUF: {
        int buf = 65536;
        {
          buf = shared.sndbuf;
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
          ms = InterlockedCompareExchange(&shared.rcv_timeout_ms, 0, 0);
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
      default:
        return errno = ENOPROTOOPT, -1;
    }
  }
  return errno = ENOPROTOOPT, -1;
}

}  // namespace gesher
