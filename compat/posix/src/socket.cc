// gesher-base: the Linux socket API (<sys/socket.h>), over the Winsock side
// in winsock.cc (see socket_internal.h). Only Linux's constants here.
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

#include <string>
#include <vector>

#include <gesher/inet.h>

#include <atomic>
#include <stdlib.h>

#include "internal.h"
#include "linux_socket.h"
#include "socket_internal.h"
#include "record_lock_descriptors.h"

namespace ws = gesher::ws;
using gesher::SocketFd;
using namespace gesher::sock;

namespace {

// The socket behind fd, or false with errno EBADF/ENOTSOCK.
bool Lookup(int fd, SocketFd* state) {
  if (gesher::LookupSocket(fd, state)) return true;
  errno = gesher::HandleFromFd(fd) == INVALID_HANDLE_VALUE ? EBADF : ENOTSOCK;
  return false;
}

int RegisterNew(ws::Socket s, bool nonblocking) {
  // accept does not inherit O_NONBLOCK on Linux. Normalize both directions
  // before publishing the new open description and its shared status flag.
  if (ws::SetNonBlocking(s, nonblocking) < 0) {
    int saved = errno;
    ws::Close(s);
    errno = saved;
    return -1;
  }
  SocketFd state;
  state.socket = s;
  state.initial_nonblocking = nonblocking;
  return gesher::RegisterSocket(state);
}

// Lifecycle controls invalidate a completed-refusal readiness classification
// before native mutation. Their lease is separate from finite poll metadata.
bool BeginControl(int fd, const SocketFd& state) {
  gesher::record_locks::DescriptorGuard descriptor_guard;
  SocketFd current;
  if (!gesher::LookupSocket(fd, &current) || current.description != state.description) return errno = EBADF, false;
  std::lock_guard<std::mutex> lock(state.description->error_mutex);
  if (state.description->connect_operation || state.description->legacy_connect_active) return errno = EALREADY, false;
  state.description->legacy_connect_active = true;
  state.description->refusal_generation = 0;
  return true;
}

int EndControl(const SocketFd& state, int result) {
  const int saved = errno;
  std::lock_guard<std::mutex> lock(state.description->error_mutex);
  state.description->legacy_connect_active = false;
  if (result == 0) state.description->state.store(ws::ConnectState::kInitial);
  errno = saved;
  return result;
}

constexpr int kTypeFlags = SOCK_NONBLOCK | SOCK_CLOEXEC;

// An AF_UNIX name is a path (path.cc): the names under /dev and /proc are
// not files on Windows. Abstract names (a leading NUL) pass unchanged, and
// Winsock rejects them.
bool MapUnixPath(ws::Address* address, bool binding) {
  if (address->family != ws::Family::kUnix || address->data_len == 0 || address->data[0] == 0) {
    return true;
  }
  const char* name = reinterpret_cast<const char*>(address->data);
  std::string path(name, strnlen(name, address->data_len));
  std::wstring windows;
  switch (gesher::MapPath(path.c_str(), &windows)) {
    case gesher::PathKind::kFile:
      break;
    case gesher::PathKind::kMissing:
      return errno = ENOENT, false;
    default:
      return errno = binding ? EADDRINUSE : ECONNREFUSED, false;
  }
  std::string mapped = gesher::Narrow(windows.c_str());
  if (mapped.size() >= sizeof(sockaddr_un::sun_path)) return errno = ENAMETOOLONG, false;
  memset(address->data, 0, sizeof(address->data));
  memcpy(address->data, mapped.data(), mapped.size());
  if (address->data_len < mapped.size() + 1) address->data_len = mapped.size() + 1;
  return true;
}

}  // namespace

// Android's INTERNET permission (<gesher/inet.h>): -1 until first asked, then 0 or 1.
static std::atomic<int> g_inet_allowed{-1};

namespace gesher {
int ConnectWithObserver(int fd, const void* raw, uint32_t length, ConnectObserver observer, void* context) {
  SocketFd state;
  ws::Address address;
  if (!Lookup(fd, &state) || !ToAddress(static_cast<const sockaddr*>(raw), length, &address) || !MapUnixPath(&address, false)) return -1;
  if (state.description->provider == ws::Provider::kNativeIpv4Tcp && address.family == ws::Family::kInet) {
    std::shared_ptr<ws::ConnectOperation> operation;
    bool nonblocking;
    {
      record_locks::DescriptorGuard descriptor_guard;
      SocketFd current;
      if (!LookupSocket(fd, &current) || current.description != state.description) return errno = EBADF, -1;
      // This transaction fixes logical call behavior, never the native socket
      // mode. ConnectEx itself cannot become blocking during F_SETFL/FIONBIO.
      nonblocking = current.description->nonblocking.load();
      operation = ws::StartConnect(current.socket, address, current.description);
    }
    if (!operation) return -1;
    // The optional private observer sees real submission before a logical
    // blocking caller waits. It owns no native context and holds no locks.
    const int result = ws::AwaitConnect(operation, observer ? true : nonblocking);
    const int saved = errno;
    if (observer) observer(state.socket, result, saved, context);
    if (!nonblocking && result < 0 && saved == EINPROGRESS) return ws::AwaitConnect(operation, false);
    errno = saved;
    return result;
  }
  // Non-TCP and AF_UNSPEC retain their native behavior. They do not create
  // an owned TCP completion token or qualify terminal-refusal filtering.
  uint64_t generation;
  ws::ConnectState previous_state;
  {
    record_locks::DescriptorGuard descriptor_guard;
    SocketFd current;
    if (!LookupSocket(fd, &current) || current.description != state.description) return errno = EBADF, -1;
    std::lock_guard<std::mutex> lock(state.description->error_mutex);
    if (state.description->connect_operation || state.description->legacy_connect_active) return errno = EALREADY, -1;
    if (state.description->connect_generation == UINT64_MAX) return errno = EOVERFLOW, -1;
    generation = ++state.description->connect_generation;
    previous_state = state.description->state.load();
    state.description->legacy_connect_active = true;
    state.description->refusal_generation = 0;
    state.description->connect_error = 0;
  }
  struct Observation { ws::Socket socket; ConnectObserver observer; void* context; } observation{state.socket, observer, context};
  void (*before)(void*) = [](void* raw) {
    auto& o = *static_cast<Observation*>(raw);
    o.observer(o.socket, -2, 0, o.context);  // immutable private pre-native dependency
  };
  const int result = ws::ConnectLegacy(state.socket, address, state.description, observer ? before : nullptr, &observation);
  const int saved = errno;
  {
    std::lock_guard<std::mutex> lock(state.description->error_mutex);
    state.description->legacy_connect_active = false;
    if (state.description->connect_generation == generation) {
      const bool pending = saved == EINPROGRESS || saved == EALREADY || saved == EINTR;
      const ws::ConnectState completed = address.family == ws::Family::kUnspec ? ws::ConnectState::kInitial : ws::ConnectState::kConnected;
      state.description->state.store(result == 0 ? completed : pending ? ws::ConnectState::kConnecting : previous_state);
    }
  }
  if (observer) observer(state.socket, result, saved, context);
  errno = saved;
  return result;
}
}  // namespace gesher

extern "C" {

void gesher_set_inet_allowed(int allowed) { g_inet_allowed = allowed ? 1 : 0; }

int gesher_inet_allowed(void) {
  int allowed = g_inet_allowed;
  if (allowed < 0) {
    const char* env = getenv("GESHER_INET");
    allowed = env != nullptr && strcmp(env, "0") == 0 ? 0 : 1;
    int expected = -1;
    g_inet_allowed.compare_exchange_strong(expected, allowed);
    allowed = g_inet_allowed;
  }
  return allowed;
}

int gesher_bind_socket_interface(int fd, uint32_t if_index) {
  SocketFd state;
  if (!Lookup(fd, &state)) return errno;
  return ws::BindInterface(state.socket, if_index) == 0 ? 0 : errno;
}

int gesher_socket_interface(int fd, uint32_t* if_index) {
  if (if_index == nullptr) return EINVAL;
  SocketFd state;
  if (!Lookup(fd, &state)) return errno;
  return ws::BoundInterface(state.socket, if_index) == 0 ? 0 : errno;
}

int gesher_set_process_interface(uint32_t if_index) {
  return ws::SetProcessInterface(if_index) == 0 ? 0 : errno;
}

uint32_t gesher_process_interface(void) { return ws::ProcessInterface(); }

int socket(int domain, int type, int protocol) {
  if (!ws::Startup()) return -1;
  ws::Family family = ToFamily(domain);
  if (family == ws::Family::kOther || family == ws::Family::kUnspec) {
    return errno = EAFNOSUPPORT, -1;
  }
  if ((family == ws::Family::kInet || family == ws::Family::kInet6) && !gesher_inet_allowed()) {
    return errno = EACCES, -1;  // no INTERNET permission: no inet group
  }
  ws::Type ws_type = ToType(type & ~kTypeFlags);
  if (ws_type == ws::Type::kOther) return errno = EINVAL, -1;
  // Sockets are never inherited by child processes, so SOCK_CLOEXEC holds
  // either way.
  ws::Socket s = ws::Open(family, ws_type, protocol);
  if (s == ws::kInvalidSocket) return -1;
  return RegisterNew(s, (type & SOCK_NONBLOCK) != 0);
}

// Windows has no socketpair: this connects two loopback TCP (or UDP)
// sockets, as other ports do. The peer must be the socket just connected,
// so another local process cannot slip in between listen and accept.
// SOCK_SEQPACKET (Android's InputChannel, docs/input.md) is a connected UDP
// pair: messages keep their boundaries and order on loopback, but a full
// receive buffer drops a message instead of blocking the sender, and a
// closed peer is not reported as a hangup.
int socketpair(int domain, int type, int protocol, int fds[2]) {
  if (!ws::Startup()) return -1;
  if (domain != AF_UNIX && domain != AF_INET) return errno = EAFNOSUPPORT, -1;
  if (protocol != 0) return errno = EPROTONOSUPPORT, -1;
  ws::Type ws_type = ToType(type & ~kTypeFlags);
  if (ws_type == ws::Type::kSeqpacket && domain == AF_UNIX) ws_type = ws::Type::kDgram;
  if (ws_type != ws::Type::kStream && ws_type != ws::Type::kDgram) return errno = EOPNOTSUPP, -1;

  sockaddr_in loopback = {};
  loopback.sin_family = AF_INET;
  loopback.sin_addr.s_addr = __builtin_bswap32(0x7F000001);  // 127.0.0.1
  ws::Address any;
  ToAddress(reinterpret_cast<sockaddr*>(&loopback), sizeof(loopback), &any);

  ws::Socket a = ws::kInvalidSocket, b = ws::kInvalidSocket, listener = ws::kInvalidSocket;
  auto fail = [&]() {
    int saved = errno;
    if (a != ws::kInvalidSocket) ws::Close(a);
    if (b != ws::kInvalidSocket) ws::Close(b);
    if (listener != ws::kInvalidSocket) ws::Close(listener);
    errno = saved;
    return -1;
  };
  ws::Address a_name, b_name;
  if (ws_type == ws::Type::kStream) {
    listener = ws::Open(ws::Family::kInet, ws::Type::kStream, 0, false);
    ws::Address listen_name;
    if (listener == ws::kInvalidSocket || ws::Bind(listener, any) < 0 ||
        ws::Listen(listener, 1) < 0 || ws::GetSockName(listener, &listen_name) < 0) {
      return fail();
    }
    a = ws::Open(ws::Family::kInet, ws::Type::kStream, 0, false);
    if (a == ws::kInvalidSocket || ws::Connect(a, listen_name) < 0 ||
        ws::GetSockName(a, &a_name) < 0) {
      return fail();
    }
    ws::Address peer;
    b = ws::Accept(listener, &peer);
    if (b == ws::kInvalidSocket) return fail();
    if (peer.data_len != a_name.data_len || memcmp(peer.data, a_name.data, peer.data_len) != 0) {
      errno = ECONNABORTED;
      return fail();
    }
    ws::Close(listener);
    listener = ws::kInvalidSocket;
  } else {
    a = ws::Open(ws::Family::kInet, ws::Type::kDgram, 0, false);
    b = ws::Open(ws::Family::kInet, ws::Type::kDgram, 0, false);
    if (a == ws::kInvalidSocket || b == ws::kInvalidSocket || ws::Bind(a, any) < 0 ||
        ws::Bind(b, any) < 0 || ws::GetSockName(a, &a_name) < 0 ||
        ws::GetSockName(b, &b_name) < 0 || ws::Connect(a, b_name) < 0 ||
        ws::Connect(b, a_name) < 0) {
      return fail();
    }
  }
  bool nonblocking = (type & SOCK_NONBLOCK) != 0;
  int fd_a = RegisterNew(a, nonblocking);
  a = ws::kInvalidSocket;  // owned by the fd table, or closed
  if (fd_a < 0) return fail();
  int fd_b = RegisterNew(b, nonblocking);
  b = ws::kInvalidSocket;
  if (fd_b < 0) {
    int saved = errno;
    close(fd_a);
    errno = saved;
    return -1;
  }
  fds[0] = fd_a;
  fds[1] = fd_b;
  return 0;
}

int bind(int fd, const struct sockaddr* addr, socklen_t len) {
  SocketFd state;
  ws::Address address;
  if (!Lookup(fd, &state) || !ToAddress(addr, len, &address) || !MapUnixPath(&address, true)) {
    return -1;
  }
  if (state.description->provider == ws::Provider::kOther) return ws::Bind(state.socket, address);
  if (!BeginControl(fd, state)) return -1;
  return EndControl(state, ws::Bind(state.socket, address, state.description));
}

int connect(int fd, const struct sockaddr* addr, socklen_t len) {
  return gesher::ConnectWithObserver(fd, addr, len, nullptr, nullptr);
}

int listen(int fd, int backlog) {
  SocketFd state;
  if (!Lookup(fd, &state)) return -1;
  if (state.description->provider == ws::Provider::kOther) return ws::Listen(state.socket, backlog);
  if (!BeginControl(fd, state)) return -1;
  return EndControl(state, ws::Listen(state.socket, backlog, state.description));
}

int accept4(int fd, struct sockaddr* addr, socklen_t* len, int flags) {
  if ((flags & ~kTypeFlags) != 0) return errno = EINVAL, -1;
  SocketFd state;
  if (!Lookup(fd, &state)) return -1;
  ws::Address peer;
  ws::Socket s = ws::Accept(state.socket, &peer);
  if (s == ws::kInvalidSocket) return -1;
  FromAddress(peer, addr, len);
  return RegisterNew(s, (flags & SOCK_NONBLOCK) != 0);
}

int accept(int fd, struct sockaddr* addr, socklen_t* len) { return accept4(fd, addr, len, 0); }

int getsockname(int fd, struct sockaddr* addr, socklen_t* len) {
  SocketFd state;
  if (!Lookup(fd, &state)) return -1;
  ws::Address address;
  if (ws::GetSockName(state.socket, &address) < 0) return -1;
  FromAddress(address, addr, len);
  return 0;
}

int getpeername(int fd, struct sockaddr* addr, socklen_t* len) {
  SocketFd state;
  if (!Lookup(fd, &state)) return -1;
  ws::Address address;
  if (ws::GetPeerName(state.socket, &address) < 0) return -1;
  FromAddress(address, addr, len);
  return 0;
}

int shutdown(int fd, int how) {
  SocketFd state;
  if (!Lookup(fd, &state)) return -1;
  if (how != SHUT_RD && how != SHUT_WR && how != SHUT_RDWR) return errno = EINVAL, -1;
  return ws::Shutdown(state.socket, how != SHUT_WR, how != SHUT_RD);
}

}  // extern "C"

namespace {

// Linux message flags to the neutral ones. MSG_NOSIGNAL needs nothing (there
// is no SIGPIPE), and the flags only meaningful to Linux are ignored.
int ToMsgFlags(int flags) {
  int result = 0;
  if (flags & MSG_OOB) result |= ws::kMsgOob;
  if (flags & MSG_PEEK) result |= ws::kMsgPeek;
  if (flags & MSG_DONTROUTE) result |= ws::kMsgDontRoute;
  if (flags & MSG_WAITALL) result |= ws::kMsgWaitAll;
  return result;
}

// MSG_DONTWAIT on a blocking socket: fail with EAGAIN unless the socket is
// ready now. (Another thread may still take the data first.)
bool WouldBlock(const SocketFd& state, int flags, int event) {
  if ((flags & MSG_DONTWAIT) == 0 || state.description->nonblocking.load()) return false;
  ws::PollEntry entry = {state.socket, event, 0, state.description};
  if (ws::Poll(&entry, 1, 0) > 0) return false;
  errno = EAGAIN;
  return true;
}

ssize_t SendBuffers(int fd, ws::Buffer* buffers, size_t count, int flags,
                    const sockaddr* addr, socklen_t addrlen) {
  SocketFd state;
  if (!Lookup(fd, &state)) return -1;
  ws::Address to;
  if (addr != nullptr && (!ToAddress(addr, addrlen, &to) || !MapUnixPath(&to, false))) return -1;
  if (WouldBlock(state, flags, ws::kPollOut)) return -1;
  return ws::Send(state.socket, buffers, count, ToMsgFlags(flags), addr != nullptr ? &to : nullptr);
}

ssize_t RecvBuffers(int fd, ws::Buffer* buffers, size_t count, int flags, sockaddr* addr,
                    socklen_t* addrlen) {
  SocketFd state;
  if (!Lookup(fd, &state)) return -1;
  // There is no error queue: Linux reports EAGAIN when it is empty.
  if (flags & MSG_ERRQUEUE) return errno = EAGAIN, -1;
  if (WouldBlock(state, flags, ws::kPollIn)) return -1;
  ws::Address from;
  ssize_t n = ws::Recv(state.socket, buffers, count, ToMsgFlags(flags),
                       addr != nullptr ? &from : nullptr);
  if (n >= 0 && addr != nullptr) FromAddress(from, addr, addrlen);
  return n;
}

bool ToBuffers(const struct msghdr* msg, std::vector<ws::Buffer>* buffers) {
  if (msg->msg_iovlen > 1024) return errno = EMSGSIZE, false;
  for (size_t i = 0; i < msg->msg_iovlen; ++i) {
    buffers->push_back({msg->msg_iov[i].iov_base, msg->msg_iov[i].iov_len});
  }
  return true;
}

}  // namespace

extern "C" {

ssize_t send(int fd, const void* buf, size_t len, int flags) {
  ws::Buffer buffer = {const_cast<void*>(buf), len};
  return SendBuffers(fd, &buffer, 1, flags, nullptr, 0);
}

ssize_t sendto(int fd, const void* buf, size_t len, int flags, const struct sockaddr* addr,
               socklen_t addrlen) {
  ws::Buffer buffer = {const_cast<void*>(buf), len};
  return SendBuffers(fd, &buffer, 1, flags, addr, addrlen);
}

// Ancillary data (msg_control) is not supported and is ignored.
ssize_t sendmsg(int fd, const struct msghdr* msg, int flags) {
  std::vector<ws::Buffer> buffers;
  if (!ToBuffers(msg, &buffers)) return -1;
  return SendBuffers(fd, buffers.data(), buffers.size(), flags,
                     static_cast<const sockaddr*>(msg->msg_name), msg->msg_namelen);
}

ssize_t recv(int fd, void* buf, size_t len, int flags) {
  ws::Buffer buffer = {buf, len};
  return RecvBuffers(fd, &buffer, 1, flags, nullptr, nullptr);
}

ssize_t recvfrom(int fd, void* buf, size_t len, int flags, struct sockaddr* addr,
                 socklen_t* addrlen) {
  ws::Buffer buffer = {buf, len};
  return RecvBuffers(fd, &buffer, 1, flags, addr, addrlen);
}

ssize_t recvmsg(int fd, struct msghdr* msg, int flags) {
  std::vector<ws::Buffer> buffers;
  if (!ToBuffers(msg, &buffers)) return -1;
  ssize_t n = RecvBuffers(fd, buffers.data(), buffers.size(), flags,
                          static_cast<sockaddr*>(msg->msg_name),
                          msg->msg_name != nullptr ? &msg->msg_namelen : nullptr);
  if (n >= 0) {
    msg->msg_controllen = 0;  // no ancillary data
    msg->msg_flags = 0;
  }
  return n;
}

struct cmsghdr* __cmsg_nxthdr(struct msghdr* mhdr, struct cmsghdr* cmsg) {
  if (mhdr == nullptr || mhdr->msg_control == nullptr || cmsg == nullptr) return nullptr;
  uintptr_t begin = reinterpret_cast<uintptr_t>(mhdr->msg_control);
  uintptr_t current = reinterpret_cast<uintptr_t>(cmsg);
  if (current < begin || current - begin > mhdr->msg_controllen) return nullptr;
  size_t left = mhdr->msg_controllen - (current - begin);
  if (left < sizeof(cmsghdr) || cmsg->cmsg_len < sizeof(cmsghdr) ||
      cmsg->cmsg_len > left || cmsg->cmsg_len > SIZE_MAX - sizeof(size_t) + 1) return nullptr;
  size_t step = CMSG_ALIGN(cmsg->cmsg_len);
  if (step > left || left - step < sizeof(cmsghdr)) return nullptr;
  auto* next = reinterpret_cast<cmsghdr*>(static_cast<unsigned char*>(mhdr->msg_control) +
                                         (current - begin) + step);
  if (next->cmsg_len < sizeof(cmsghdr) || next->cmsg_len > left - step) return nullptr;
  return next;
}

}  // extern "C"

// --- socket options -------------------------------------------------------------

namespace {

// Options whose value is an int on both sides.
bool ToIntOption(int level, int name, ws::Option* option) {
  if (level == SOL_SOCKET) {
    switch (name) {
      case SO_KEEPALIVE: *option = ws::Option::kKeepAlive; return true;
      case SO_BROADCAST: *option = ws::Option::kBroadcast; return true;
      case SO_OOBINLINE: *option = ws::Option::kOobInline; return true;
      case SO_RCVBUF: *option = ws::Option::kRcvBuf; return true;
      case SO_SNDBUF: *option = ws::Option::kSndBuf; return true;
      case SO_RCVLOWAT: *option = ws::Option::kRcvLowat; return true;
      case SO_SNDLOWAT: *option = ws::Option::kSndLowat; return true;
      case SO_ERROR: *option = ws::Option::kError; return true;
      case SO_ACCEPTCONN: *option = ws::Option::kAcceptConn; return true;
      case SO_DONTROUTE: *option = ws::Option::kDontRoute; return true;
    }
  } else if (level == IPPROTO_TCP) {
    switch (name) {
      case TCP_NODELAY: *option = ws::Option::kTcpNoDelay; return true;
      case TCP_KEEPIDLE: *option = ws::Option::kTcpKeepIdle; return true;
      case TCP_KEEPINTVL: *option = ws::Option::kTcpKeepIntvl; return true;
      case TCP_KEEPCNT: *option = ws::Option::kTcpKeepCnt; return true;
    }
  } else if (level == IPPROTO_IP) {
    switch (name) {
      case IP_TOS: *option = ws::Option::kIpTos; return true;
      case IP_TTL: *option = ws::Option::kIpTtl; return true;
      case IP_MULTICAST_TTL: *option = ws::Option::kIpMulticastTtl; return true;
      case IP_MULTICAST_LOOP: *option = ws::Option::kIpMulticastLoop; return true;
    }
  } else if (level == IPPROTO_IPV6) {
    switch (name) {
      case IPV6_UNICAST_HOPS: *option = ws::Option::kIpv6UnicastHops; return true;
      case IPV6_MULTICAST_IF: *option = ws::Option::kIpv6MulticastIf; return true;
      case IPV6_MULTICAST_HOPS: *option = ws::Option::kIpv6MulticastHops; return true;
      case IPV6_MULTICAST_LOOP: *option = ws::Option::kIpv6MulticastLoop; return true;
      case IPV6_V6ONLY: *option = ws::Option::kIpv6V6Only; return true;
      case IPV6_TCLASS: *option = ws::Option::kIpv6TClass; return true;
    }
  }
  return false;
}

// Linux takes an int, or a single byte for the IPv4 multicast options.
bool ReadInt(const void* value, socklen_t len, int* out) {
  if (value == nullptr) return errno = EFAULT, false;
  if (len >= sizeof(int)) {
    memcpy(out, value, sizeof(int));
    return true;
  }
  if (len == 1) {
    *out = *static_cast<const unsigned char*>(value);
    return true;
  }
  return errno = EINVAL, false;
}

bool WriteInt(int v, void* value, socklen_t* len) {
  if (*len >= sizeof(int)) {
    memcpy(value, &v, sizeof(int));
    *len = sizeof(int);
    return true;
  }
  if (*len >= 1) {
    *static_cast<unsigned char*>(value) = static_cast<unsigned char>(v);
    *len = 1;
    return true;
  }
  return errno = EINVAL, false;
}

// The index of the interface with this IPv4 address; 0 for INADDR_ANY.
bool IndexForAddress(in_addr address, uint32_t* index) {
  *index = 0;
  if (address.s_addr == 0) return true;
  std::vector<ws::Interface> interfaces;
  if (!ws::ListInterfaces(&interfaces)) return false;
  for (const ws::Interface& iface : interfaces) {
    for (const ws::InterfaceAddress& a : iface.addresses) {
      sockaddr_in sin = {};
      size_t tail = sizeof(sin) - sizeof(sa_family_t);
      if (a.address.family != ws::Family::kInet || a.address.data_len < tail) continue;
      memcpy(reinterpret_cast<char*>(&sin) + sizeof(sa_family_t), a.address.data, tail);
      if (sin.sin_addr.s_addr == address.s_addr) {
        *index = iface.index;
        return true;
      }
    }
  }
  return errno = ENODEV, false;
}

// The address bytes of a sockaddr_storage holding AF_INET or AF_INET6.
bool GroupAddress(const sockaddr_storage& ss, ws::Family* family, const uint8_t** bytes) {
  if (ss.ss_family == AF_INET) {
    *family = ws::Family::kInet;
    *bytes = reinterpret_cast<const uint8_t*>(&reinterpret_cast<const sockaddr_in*>(&ss)->sin_addr);
    return true;
  }
  if (ss.ss_family == AF_INET6) {
    *family = ws::Family::kInet6;
    *bytes =
        reinterpret_cast<const uint8_t*>(&reinterpret_cast<const sockaddr_in6*>(&ss)->sin6_addr);
    return true;
  }
  return errno = EAFNOSUPPORT, false;
}

// Multicast membership in any of the Linux forms. Returns 1 if `name` is not
// a membership option.
int SetMembership(ws::Socket s, int level, int name, const void* value, socklen_t len) {
  if (value == nullptr) return errno = EFAULT, -1;
  if (level == IPPROTO_IP && (name == IP_ADD_MEMBERSHIP || name == IP_DROP_MEMBERSHIP)) {
    ip_mreqn req = {};
    if (len >= sizeof(ip_mreqn)) {
      memcpy(&req, value, sizeof(req));
    } else if (len >= sizeof(ip_mreq)) {
      memcpy(&req, value, sizeof(ip_mreq));  // imr_interface lands in imr_address
    } else {
      return errno = EINVAL, -1;
    }
    uint32_t index = static_cast<uint32_t>(req.imr_ifindex);
    if (index == 0 && !IndexForAddress(req.imr_address, &index)) return -1;
    return ws::Membership(s, name == IP_ADD_MEMBERSHIP, ws::Family::kInet,
                          reinterpret_cast<const uint8_t*>(&req.imr_multiaddr), index, nullptr);
  }
  if (level == IPPROTO_IP &&
      (name == IP_ADD_SOURCE_MEMBERSHIP || name == IP_DROP_SOURCE_MEMBERSHIP)) {
    if (len < sizeof(ip_mreq_source)) return errno = EINVAL, -1;
    ip_mreq_source req;
    memcpy(&req, value, sizeof(req));
    uint32_t index;
    if (!IndexForAddress(req.imr_interface, &index)) return -1;
    return ws::Membership(s, name == IP_ADD_SOURCE_MEMBERSHIP, ws::Family::kInet,
                          reinterpret_cast<const uint8_t*>(&req.imr_multiaddr), index,
                          reinterpret_cast<const uint8_t*>(&req.imr_sourceaddr));
  }
  if (level == IPPROTO_IPV6 && (name == IPV6_JOIN_GROUP || name == IPV6_LEAVE_GROUP)) {
    if (len < sizeof(ipv6_mreq)) return errno = EINVAL, -1;
    ipv6_mreq req;
    memcpy(&req, value, sizeof(req));
    return ws::Membership(s, name == IPV6_JOIN_GROUP, ws::Family::kInet6,
                          reinterpret_cast<const uint8_t*>(&req.ipv6mr_multiaddr),
                          req.ipv6mr_interface, nullptr);
  }
  if ((level == IPPROTO_IP || level == IPPROTO_IPV6) &&
      (name == MCAST_JOIN_GROUP || name == MCAST_LEAVE_GROUP)) {
    if (len < sizeof(group_req)) return errno = EINVAL, -1;
    group_req req;
    memcpy(&req, value, sizeof(req));
    ws::Family family;
    const uint8_t* group;
    if (!GroupAddress(req.gr_group, &family, &group)) return -1;
    return ws::Membership(s, name == MCAST_JOIN_GROUP, family, group, req.gr_interface, nullptr);
  }
  if ((level == IPPROTO_IP || level == IPPROTO_IPV6) &&
      (name == MCAST_JOIN_SOURCE_GROUP || name == MCAST_LEAVE_SOURCE_GROUP)) {
    if (len < sizeof(group_source_req)) return errno = EINVAL, -1;
    group_source_req req;
    memcpy(&req, value, sizeof(req));
    ws::Family family, source_family;
    const uint8_t* group;
    const uint8_t* source;
    if (!GroupAddress(req.gsr_group, &family, &group) ||
        !GroupAddress(req.gsr_source, &source_family, &source)) {
      return -1;
    }
    if (family != source_family) return errno = EINVAL, -1;
    return ws::Membership(s, name == MCAST_JOIN_SOURCE_GROUP, family, group, req.gsr_interface,
                          source);
  }
  return 1;
}

}  // namespace

extern "C" {

int setsockopt(int fd, int level, int name, const void* value, socklen_t len) {
  SocketFd state;
  if (!Lookup(fd, &state)) return -1;
  ws::Option option;
  int v;
  if (ToIntOption(level, name, &option)) {
    if (!ReadInt(value, len, &v)) return -1;
    return ws::SetOption(state.socket, option, v);
  }
  if (level == SOL_SOCKET && name == SO_REUSEADDR) {
    // On Windows SO_REUSEADDR lets a second socket bind a port that is in
    // use. On Linux it only allows ports in TIME_WAIT, which Windows allows
    // without it. So the value is only recorded, for getsockopt.
    if (!ReadInt(value, len, &v)) return -1;
    state.description->reuse_addr.store(v != 0);
    return 0;
  }
  if (level == SOL_SOCKET && (name == SO_RCVTIMEO || name == SO_SNDTIMEO)) {
    if (value == nullptr || len < sizeof(timeval)) return errno = EINVAL, -1;
    timeval tv;
    memcpy(&tv, value, sizeof(tv));
    if (tv.tv_sec < 0 || tv.tv_usec < 0 || tv.tv_usec >= 1000000) return errno = EDOM, -1;
    long long ms = static_cast<long long>(tv.tv_sec) * 1000 + (tv.tv_usec + 999) / 1000;
    if (ms > 0x7FFFFFFF) ms = 0x7FFFFFFF;  // 0 means no timeout, on both sides
    return ws::SetOption(state.socket,
                         name == SO_RCVTIMEO ? ws::Option::kRcvTimeo : ws::Option::kSndTimeo,
                         static_cast<int>(ms));
  }
  if (level == SOL_SOCKET && name == SO_LINGER) {
    if (value == nullptr || len < sizeof(linger)) return errno = EINVAL, -1;
    linger l;
    memcpy(&l, value, sizeof(l));
    return ws::SetLinger(state.socket, l.l_onoff != 0, l.l_linger);
  }
  if (level == IPPROTO_IP && name == IP_MULTICAST_IF) {
    // An in_addr, or an ip_mreqn whose index takes precedence over its
    // address. Winsock takes an index as the address 0.0.0.index.
    if (value == nullptr) return errno = EFAULT, -1;
    in_addr address;
    if (len >= sizeof(ip_mreqn)) {
      ip_mreqn req;
      memcpy(&req, value, sizeof(req));
      address = req.imr_address;
      if (req.imr_ifindex != 0) {
        address.s_addr = __builtin_bswap32(static_cast<uint32_t>(req.imr_ifindex));
      }
    } else if (len >= sizeof(in_addr)) {
      memcpy(&address, value, sizeof(address));
    } else {
      return errno = EINVAL, -1;
    }
    return ws::SetOption(state.socket, ws::Option::kIpMulticastIf,
                         static_cast<int>(address.s_addr));
  }
  if (level == IPPROTO_IP && name == IP_MULTICAST_ALL) {
    // Linux-only. Android's DatagramSocket turns it off (0): a socket then
    // receives only the multicast groups it joined itself, which is how
    // Winsock always behaves. Accepted; getsockopt reports Winsock's 0.
    if (!ReadInt(value, len, &v)) return -1;
    return 0;
  }
  int rc = SetMembership(state.socket, level, name, value, len);
  if (rc != 1) return rc;
  return errno = ENOPROTOOPT, -1;
}

int getsockopt(int fd, int level, int name, void* value, socklen_t* len) {
  SocketFd state;
  if (!Lookup(fd, &state)) return -1;
  if (value == nullptr || len == nullptr) return errno = EFAULT, -1;
  ws::Option option;
  int v;
  if (ToIntOption(level, name, &option)) {
    if (option == ws::Option::kError) {
      gesher::record_locks::DescriptorGuard descriptor_guard;
      SocketFd current;
      if (!gesher::LookupSocket(fd, &current) || current.description != state.description) return errno = EBADF, -1;
      ws::RefreshConnect(state.description);
      std::lock_guard<std::mutex> lock(state.description->error_mutex);
      // ConnectEx reports its failure on the owned completion channel. Always
      // read native SO_ERROR as well, including identical later native codes.
      if (ws::GetOption(state.socket, option, &v) < 0) return -1;
      if (!WriteInt(v != 0 ? v : state.description->connect_error, value, len)) return -1;
      if (v == 0) state.description->connect_error = 0;
      return 0;
    }
    if (ws::GetOption(state.socket, option, &v) < 0) return -1;
    return WriteInt(v, value, len) ? 0 : -1;
  }
  if (level == SOL_SOCKET && name == SO_REUSEADDR) {
    return WriteInt(state.description->reuse_addr.load() ? 1 : 0, value, len) ? 0 : -1;
  }
  if (level == SOL_SOCKET && name == SO_TYPE) {
    if (ws::GetOption(state.socket, ws::Option::kType, &v) < 0) return -1;
    return WriteInt(FromType(static_cast<ws::Type>(v)), value, len) ? 0 : -1;
  }
  if (level == SOL_SOCKET && name == SO_DOMAIN) {
    ws::Address address;
    if (ws::GetSockName(state.socket, &address) < 0) return -1;
    return WriteInt(FromFamily(address.family), value, len) ? 0 : -1;
  }
  if (level == SOL_SOCKET && name == SO_PROTOCOL) {
    if (ws::GetOption(state.socket, ws::Option::kType, &v) < 0) return -1;
    ws::Type type = static_cast<ws::Type>(v);
    int protocol = type == ws::Type::kStream ? IPPROTO_TCP
                   : type == ws::Type::kDgram ? IPPROTO_UDP : 0;
    return WriteInt(protocol, value, len) ? 0 : -1;
  }
  if (level == SOL_SOCKET && (name == SO_RCVTIMEO || name == SO_SNDTIMEO)) {
    if (*len < sizeof(timeval)) return errno = EINVAL, -1;
    if (ws::GetOption(state.socket,
                      name == SO_RCVTIMEO ? ws::Option::kRcvTimeo : ws::Option::kSndTimeo,
                      &v) < 0) {
      return -1;
    }
    timeval tv = {};
    tv.tv_sec = v / 1000;
    tv.tv_usec = (v % 1000) * 1000;
    memcpy(value, &tv, sizeof(tv));
    *len = sizeof(tv);
    return 0;
  }
  if (level == SOL_SOCKET && name == SO_LINGER) {
    if (*len < sizeof(linger)) return errno = EINVAL, -1;
    bool on;
    int seconds;
    if (ws::GetLinger(state.socket, &on, &seconds) < 0) return -1;
    linger l = {on ? 1 : 0, seconds};
    memcpy(value, &l, sizeof(l));
    *len = sizeof(l);
    return 0;
  }
  if (level == IPPROTO_IP && name == IP_MULTICAST_IF) {
    if (*len < sizeof(in_addr)) return errno = EINVAL, -1;
    if (ws::GetOption(state.socket, ws::Option::kIpMulticastIf, &v) < 0) return -1;
    in_addr address;
    address.s_addr = static_cast<in_addr_t>(v);
    memcpy(value, &address, sizeof(address));
    *len = sizeof(address);
    return 0;
  }
  if (level == IPPROTO_IP && name == IP_MULTICAST_ALL) {
    return WriteInt(0, value, len) ? 0 : -1;  // see setsockopt
  }
  return errno = ENOPROTOOPT, -1;
}

}  // extern "C"
