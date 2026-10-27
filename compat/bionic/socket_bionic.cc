// gesher bionic shim: BSD sockets, name resolution, poll/select, epoll,
// eventfd and pipes, over gesher-base (compat/posix: sockets are fds over
// Winsock, docs/decisions/0009-socket-fds.md; epoll/eventfd as libutils'
// Looper uses them).
//
// Most of these take the same Linux structures in the .so and in
// gesher-base (sockaddr*, msghdr, iovec, pollfd, epoll_event) and are
// forwarded as they are. What differs in bionic's LP64 ABI is converted:
// struct timeval (64-bit fields; mingw's are 32-bit) for SO_RCVTIMEO/
// SO_SNDTIMEO and select, struct timespec for ppoll/pselect, struct addrinfo
// (bionic has the BSD field order, ai_canonname before ai_addr), and
// getaddrinfo's AI_*/EAI_* values, which are BSD's in bionic.
//
// No <windows.h> here: its <winsock.h> would clash with gesher-base's
// <sys/socket.h>.

#include "shim_internal.h"
#include <gesher/inet.h>

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <net/if.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/uio.h>
#include <unistd.h>

#include <cstdio>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace gesher::bionic {

namespace {

// --- sockets ----------------------------------------------------------------

struct BionicTimeval {
  int64_t tv_sec;
  int64_t tv_usec;
};

int BIONIC_SYSV Socket(int domain, int type, int protocol) { return socket(domain, type, protocol); }
int BIONIC_SYSV Socketpair(int domain, int type, int protocol, int* fds) {
  return socketpair(domain, type, protocol, fds);
}
int BIONIC_SYSV Bind(int fd, const sockaddr* a, uint32_t n) { return bind(fd, a, n); }
int BIONIC_SYSV Connect(int fd, const sockaddr* a, uint32_t n) { return connect(fd, a, n); }
int BIONIC_SYSV Listen(int fd, int backlog) { return listen(fd, backlog); }
int BIONIC_SYSV Accept(int fd, sockaddr* a, uint32_t* n) { return accept(fd, a, n); }
int BIONIC_SYSV Accept4(int fd, sockaddr* a, uint32_t* n, int flags) {
  return accept4(fd, a, n, flags);
}
int BIONIC_SYSV Getsockname(int fd, sockaddr* a, uint32_t* n) { return getsockname(fd, a, n); }
int BIONIC_SYSV Getpeername(int fd, sockaddr* a, uint32_t* n) { return getpeername(fd, a, n); }
int BIONIC_SYSV Shutdown(int fd, int how) { return shutdown(fd, how); }

bool IsTimeoutOption(int level, int name) {
  return level == SOL_SOCKET && (name == SO_RCVTIMEO || name == SO_SNDTIMEO);
}

int BIONIC_SYSV Setsockopt(int fd, int level, int name, const void* value, uint32_t len) {
  if (IsTimeoutOption(level, name)) {
    if (value == nullptr || len < sizeof(BionicTimeval)) return errno = kEINVAL, -1;
    BionicTimeval in;
    std::memcpy(&in, value, sizeof(in));
    timeval tv;
    tv.tv_sec = static_cast<long>(in.tv_sec > 0x7fffffff ? 0x7fffffff : in.tv_sec);
    tv.tv_usec = static_cast<long>(in.tv_usec);
    return setsockopt(fd, level, name, &tv, sizeof(tv));
  }
  return setsockopt(fd, level, name, value, len);
}
int BIONIC_SYSV Getsockopt(int fd, int level, int name, void* value, uint32_t* len) {
  if (IsTimeoutOption(level, name)) {
    if (value == nullptr || len == nullptr || *len < sizeof(BionicTimeval)) {
      return errno = kEINVAL, -1;
    }
    timeval tv{};
    socklen_t n = sizeof(tv);
    if (getsockopt(fd, level, name, &tv, &n) != 0) return -1;
    BionicTimeval out{tv.tv_sec, tv.tv_usec};
    std::memcpy(value, &out, sizeof(out));
    *len = sizeof(out);
    return 0;
  }
  return getsockopt(fd, level, name, value, len);
}

int64_t BIONIC_SYSV Send(int fd, const void* buf, size_t n, int flags) {
  return send(fd, buf, n, flags);
}
int64_t BIONIC_SYSV Sendto(int fd, const void* buf, size_t n, int flags, const sockaddr* a,
                           uint32_t an) {
  return sendto(fd, buf, n, flags, a, an);
}
// bionic's FORTIFY forms carry the buffer's size after the length.
int64_t BIONIC_SYSV SendtoChk(int fd, const void* buf, size_t n, size_t buf_size, int flags,
                              const sockaddr* a, uint32_t an) {
  if (n > buf_size) {
    std::fprintf(stderr, "gesher bionic: FORTIFY: sendto: prevented buffer overread%c", 10);
    std::abort();
  }
  return sendto(fd, buf, n, flags, a, an);
}
int64_t BIONIC_SYSV RecvfromChk(int fd, void* buf, size_t n, size_t buf_size, int flags,
                                sockaddr* a, uint32_t* an) {
  if (n > buf_size) {
    std::fprintf(stderr, "gesher bionic: FORTIFY: recvfrom: prevented buffer overflow%c", 10);
    std::abort();
  }
  return recvfrom(fd, buf, n, flags, a, an);
}
int64_t BIONIC_SYSV Sendmsg(int fd, const msghdr* m, int flags) { return sendmsg(fd, m, flags); }
int64_t BIONIC_SYSV Recv(int fd, void* buf, size_t n, int flags) { return recv(fd, buf, n, flags); }
int64_t BIONIC_SYSV Recvfrom(int fd, void* buf, size_t n, int flags, sockaddr* a, uint32_t* an) {
  return recvfrom(fd, buf, n, flags, a, an);
}
int64_t BIONIC_SYSV Recvmsg(int fd, msghdr* m, int flags) { return recvmsg(fd, m, flags); }
cmsghdr* BIONIC_SYSV CmsgNxthdr(msghdr* m, cmsghdr* c) { return __cmsg_nxthdr(m, c); }

int BIONIC_SYSV Sendmmsg(int fd, mmsghdr* messages, unsigned count, int flags) {
  if (count != 0 && messages == nullptr) return errno = kEFAULT, -1;
  unsigned sent = 0;
  for (; sent < count && sent < 1024; ++sent) {
    int64_t n = sendmsg(fd, &messages[sent].msg_hdr, flags);
    if (n < 0) return sent != 0 ? static_cast<int>(sent) : -1;
    messages[sent].msg_len = static_cast<unsigned>(n);
  }
  return static_cast<int>(sent);
}

int BIONIC_SYSV Recvmmsg(int fd, mmsghdr* messages, unsigned count, int flags,
                        LinuxTimespec* timeout) {
  if (count != 0 && messages == nullptr) return errno = kEFAULT, -1;
  if (timeout != nullptr && (timeout->tv_sec < 0 || timeout->tv_nsec < 0 ||
                            timeout->tv_nsec >= 1000000000)) return errno = kEINVAL, -1;
  int fd_flags = fcntl(fd, F_GETFL, 0);
  if (fd_flags < 0) return -1;
  constexpr int kWaitForOne = 0x10000;
  bool wait_for_one = (flags & kWaitForOne) != 0;
  flags &= ~kWaitForOne;
  if ((fd_flags & O_NONBLOCK) != 0) flags |= MSG_DONTWAIT;
  auto start = std::chrono::steady_clock::now();
  LinuxTimespec limit = timeout != nullptr ? *timeout : LinuxTimespec{};
  auto remaining = [&] {
    auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now() - start).count();
    LinuxTimespec left{limit.tv_sec - elapsed / 1000000000,
                       limit.tv_nsec - elapsed % 1000000000};
    if (left.tv_nsec < 0) --left.tv_sec, left.tv_nsec += 1000000000;
    return left.tv_sec < 0 ? LinuxTimespec{} : left;
  };
  auto result = [&](int n) {
    if (timeout != nullptr) *timeout = remaining();
    return n;
  };
  unsigned received = 0;
  while (received < count && received < 1024) {
    DeliverPendingSignals();
    if (timeout != nullptr && (flags & MSG_DONTWAIT) == 0) {
      LinuxTimespec left = remaining();
      int64_t millis = left.tv_sec > 2147483 ? 0x7fffffff
                       : left.tv_sec * 1000 + (left.tv_nsec + 999999) / 1000000;
      int ms = millis > 0x7fffffff ? 0x7fffffff : static_cast<int>(millis);
      pollfd p{fd, POLLIN, 0};
      int ready = poll(&p, 1, ms);
      if (ready < 0) return result(received != 0 ? static_cast<int>(received) : -1);
      if (ready == 0) {
        left = remaining();
        if (left.tv_sec != 0 || left.tv_nsec != 0) continue;
        break;
      }
    }
    int64_t n = recvmsg(fd, &messages[received].msg_hdr, flags);
    if (n < 0) return result(received != 0 ? static_cast<int>(received) : -1);
    messages[received++].msg_len = static_cast<unsigned>(n);
    if (wait_for_one) flags |= MSG_DONTWAIT;
  }
  return result(static_cast<int>(received));
}

// --- name resolution ----------------------------------------------------------

// bionic's struct addrinfo (BSD order).
struct BionicAddrinfo {
  int32_t ai_flags;
  int32_t ai_family;
  int32_t ai_socktype;
  int32_t ai_protocol;
  uint32_t ai_addrlen;
  char* ai_canonname;
  sockaddr* ai_addr;
  BionicAddrinfo* ai_next;
};

// bionic (BSD) AI_* values -> gesher-base's (glibc's).
int AiFlagsIn(int f) {
  int out = 0;
  if (f & 0x1) out |= AI_PASSIVE;
  if (f & 0x2) out |= AI_CANONNAME;
  if (f & 0x4) out |= AI_NUMERICHOST;
  if (f & 0x8) out |= AI_NUMERICSERV;
  if (f & 0x100) out |= AI_ALL;
  if (f & 0x400) out |= AI_ADDRCONFIG;
  if (f & (0x800 | 0x200)) out |= AI_V4MAPPED;
  return out;
}
int AiFlagsOut(int f) {
  int out = 0;
  if (f & AI_PASSIVE) out |= 0x1;
  if (f & AI_CANONNAME) out |= 0x2;
  if (f & AI_NUMERICHOST) out |= 0x4;
  if (f & AI_NUMERICSERV) out |= 0x8;
  if (f & AI_ALL) out |= 0x100;
  if (f & AI_ADDRCONFIG) out |= 0x400;
  if (f & AI_V4MAPPED) out |= 0x800;
  return out;
}
// glibc EAI_* (negative) -> bionic's (BSD, positive).
int EaiOut(int e) {
  switch (e) {
    case 0: return 0;
    case EAI_ADDRFAMILY: return 1;
    case EAI_AGAIN: return 2;
    case EAI_BADFLAGS: return 3;
    case EAI_FAIL: return 4;
    case EAI_FAMILY: return 5;
    case EAI_MEMORY: return 6;
    case EAI_NODATA: return 7;
    case EAI_NONAME: return 8;
    case EAI_SERVICE: return 9;
    case EAI_SOCKTYPE: return 10;
    case EAI_SYSTEM: return 11;
    case EAI_OVERFLOW: return 14;
    default: return 4;
  }
}
// bionic NI_* -> glibc NI_*.
int NiFlagsIn(int f) {
  int out = 0;
  if (f & 0x1) out |= NI_NOFQDN;
  if (f & 0x2) out |= NI_NUMERICHOST;
  if (f & 0x4) out |= NI_NAMEREQD;
  if (f & 0x8) out |= NI_NUMERICSERV;
  if (f & 0x10) out |= NI_DGRAM;
  return out;
}

int GetaddrinfoOnInterface(const char* node, const char* service, const BionicAddrinfo* hints,
                          BionicAddrinfo** res, uint32_t index) {
  if (res == nullptr) return EaiOut(EAI_FAIL);
  *res = nullptr;
  addrinfo h{};
  if (hints != nullptr) {
    h.ai_flags = AiFlagsIn(hints->ai_flags);
    h.ai_family = hints->ai_family;
    h.ai_socktype = hints->ai_socktype;
    h.ai_protocol = hints->ai_protocol;
  }
  addrinfo* list = nullptr;
  int rc = gesher_getaddrinfo_interface(node, service, hints != nullptr ? &h : nullptr, index, &list);
  if (rc != 0) return EaiOut(rc);
  // One allocation per entry: the entry, its address and its canonical name,
  // so freeaddrinfo frees each entry with one free().
  BionicAddrinfo* head = nullptr;
  BionicAddrinfo** tail = &head;
  for (addrinfo* a = list; a != nullptr; a = a->ai_next) {
    size_t name_len = a->ai_canonname != nullptr ? std::strlen(a->ai_canonname) + 1 : 0;
    size_t size = sizeof(BionicAddrinfo) + a->ai_addrlen + name_len;
    auto* b = static_cast<BionicAddrinfo*>(std::calloc(1, size));
    if (b == nullptr) break;
    b->ai_flags = AiFlagsOut(a->ai_flags);
    b->ai_family = a->ai_family;
    b->ai_socktype = a->ai_socktype;
    b->ai_protocol = a->ai_protocol;
    b->ai_addrlen = a->ai_addrlen;
    auto* p = reinterpret_cast<char*>(b + 1);
    if (a->ai_addr != nullptr && a->ai_addrlen != 0) {
      std::memcpy(p, a->ai_addr, a->ai_addrlen);
      b->ai_addr = reinterpret_cast<sockaddr*>(p);
    }
    if (name_len != 0) {
      std::memcpy(p + a->ai_addrlen, a->ai_canonname, name_len);
      b->ai_canonname = p + a->ai_addrlen;
    }
    *tail = b;
    tail = &b->ai_next;
  }
  freeaddrinfo(list);
  if (head == nullptr) return 6;  // EAI_MEMORY
  *res = head;
  return 0;
}

int BIONIC_SYSV Getaddrinfo(const char* node, const char* service, const BionicAddrinfo* hints,
                           BionicAddrinfo** res) {
  return GetaddrinfoOnInterface(node, service, hints, res, UINT32_MAX);
}

bool NetworkIndex(uint64_t handle, uint32_t* index) {
  if (handle == 0) { *index = 0; return true; }
  uint32_t net_id = static_cast<uint32_t>(handle >> 32);
  if (static_cast<uint32_t>(handle) != 0xcafed00d || net_id <= 100 || net_id > INT32_MAX) return false;
  *index = net_id - 100;
  return true;
}

int BIONIC_SYSV SetProcNetwork(uint64_t handle) {
  uint32_t index = 0;
  if (!NetworkIndex(handle, &index)) return errno = EINVAL, -1;
  int error = gesher_set_process_interface(index);
  return error == 0 ? 0 : (errno = error, -1);
}

int BIONIC_SYSV SetSockNetwork(uint64_t handle, int fd) {
  uint32_t index = 0;
  if (!NetworkIndex(handle, &index)) return errno = EINVAL, -1;
  int error = gesher_bind_socket_interface(fd, index);
  return error == 0 ? 0 : (errno = error, -1);
}

int BIONIC_SYSV GetaddrinfoForNetwork(uint64_t handle, const char* node, const char* service,
                                    const BionicAddrinfo* hints, BionicAddrinfo** res) {
  uint32_t index = 0;
  if (!NetworkIndex(handle, &index)) return errno = EINVAL, EaiOut(EAI_SYSTEM);
  if (index == 0) index = UINT32_MAX;  // The process binding, otherwise Windows' default.
  return GetaddrinfoOnInterface(node, service, hints, res, index);
}

int BIONIC_SYSV ResNquery(uint64_t handle, const char* name, int dns_class, int type, uint32_t flags) {
  uint32_t index = 0;
  return NetworkIndex(handle, &index) ? gesher_res_nquery(index, name, dns_class, type, flags) : -ENONET;
}

int BIONIC_SYSV ResNsend(uint64_t handle, const uint8_t* query, size_t size, uint32_t flags) {
  uint32_t index = 0;
  return NetworkIndex(handle, &index) ? gesher_res_nsend(index, query, size, flags) : -ENONET;
}

int BIONIC_SYSV ResNresult(int fd, int* rcode, uint8_t* answer, size_t size) {
  return gesher_res_nresult(fd, rcode, answer, size);
}

void BIONIC_SYSV ResCancel(int fd) { gesher_res_cancel(fd); }
void BIONIC_SYSV Freeaddrinfo(BionicAddrinfo* a) {
  while (a != nullptr) {
    BionicAddrinfo* next = a->ai_next;
    std::free(a);
    a = next;
  }
}
const char* BIONIC_SYSV GaiStrerror(int e) {
  static const char* const kText[] = {
      "Success",
      "Address family for hostname not supported",
      "Temporary failure in name resolution",
      "Invalid value for ai_flags",
      "Non-recoverable failure in name resolution",
      "ai_family not supported",
      "Memory allocation failure",
      "No address associated with hostname",
      "hostname nor servname provided, or not known",
      "servname not supported for ai_socktype",
      "ai_socktype not supported",
      "System error returned in errno",
      "Invalid value for hints",
      "Resolved protocol is unknown",
      "Argument buffer overflow",
  };
  return e >= 0 && e < 15 ? kText[e] : "Unknown error";
}
int BIONIC_SYSV Getnameinfo(const sockaddr* a, uint32_t alen, char* host, uint32_t hostlen,
                            char* serv, uint32_t servlen, int flags) {
  return EaiOut(getnameinfo(a, alen, host, hostlen, serv, servlen, NiFlagsIn(flags)));
}
// struct hostent and servent have the same layout in both.
hostent* BIONIC_SYSV Gethostbyname(const char* name) { return gethostbyname(name); }
hostent* BIONIC_SYSV Gethostbyaddr(const void* a, uint32_t len, int type) {
  return gethostbyaddr(a, len, type);
}
servent* BIONIC_SYSV Getservbyname(const char* name, const char* proto) {
  return getservbyname(name, proto);
}
servent* BIONIC_SYSV Getservbyport(int port, const char* proto) {
  return getservbyport(port, proto);
}
int* BIONIC_SYSV GetHErrno() { return __get_h_errno(); }

int BIONIC_SYSV InetPton(int af, const char* src, void* dst) { return inet_pton(af, src, dst); }
const char* BIONIC_SYSV InetNtop(int af, const void* src, char* dst, uint32_t n) {
  return inet_ntop(af, src, dst, n);
}
uint32_t BIONIC_SYSV InetAddr(const char* cp) { return inet_addr(cp); }
int BIONIC_SYSV InetAton(const char* cp, in_addr* a) { return inet_aton(cp, a); }
char* BIONIC_SYSV InetNtoa(uint32_t a) {
  in_addr in;
  in.s_addr = a;
  return inet_ntoa(in);
}
uint32_t BIONIC_SYSV Htonl(uint32_t v) { return __builtin_bswap32(v); }
uint16_t BIONIC_SYSV Htons(uint16_t v) { return __builtin_bswap16(v); }
unsigned BIONIC_SYSV IfNametoindex(const char* name) { return if_nametoindex(name); }
char* BIONIC_SYSV IfIndextoname(unsigned index, char* name) { return if_indextoname(index, name); }

const in6_addr g_in6addr_any = IN6ADDR_ANY_INIT;
const in6_addr g_in6addr_loopback = IN6ADDR_LOOPBACK_INIT;

// --- poll, select ------------------------------------------------------------

int BIONIC_SYSV Poll(pollfd* fds, uint64_t n, int timeout) {
  DeliverPendingSignals();
  return poll(fds, static_cast<nfds_t>(n), timeout);
}

int TimeoutMillis(const LinuxTimespec* ts) {
  if (ts == nullptr) return -1;
  int64_t ms = ts->tv_sec * 1000 + (ts->tv_nsec + 999999) / 1000000;
  return ms < 0 ? 0 : ms > 0x7fffffff ? 0x7fffffff : static_cast<int>(ms);
}

int BIONIC_SYSV Ppoll(pollfd* fds, uint64_t n, const LinuxTimespec* ts, const void* /*sigmask*/) {
  return Poll(fds, n, TimeoutMillis(ts));
}

// bionic's fd_set: 1024 bits in 64-bit words.
struct BionicFdSet {
  uint64_t bits[16];
};
constexpr int kFdSetSize = 1024;

bool FdIsSet(const BionicFdSet* s, int fd) {
  return s != nullptr && (s->bits[fd / 64] >> (fd % 64) & 1) != 0;
}

int SelectImpl(int nfds, BionicFdSet* r, BionicFdSet* w, BionicFdSet* e, int timeout) {
  if (nfds < 0 || nfds > kFdSetSize) return errno = kEINVAL, -1;
  std::vector<pollfd> fds;
  for (int fd = 0; fd < nfds; ++fd) {
    short events = 0;
    if (FdIsSet(r, fd)) events |= POLLIN;
    if (FdIsSet(w, fd)) events |= POLLOUT;
    if (FdIsSet(e, fd)) events |= POLLPRI;
    if (events != 0) fds.push_back({fd, events, 0});
  }
  int rc = Poll(fds.data(), fds.size(), timeout);
  if (rc < 0) return -1;
  for (BionicFdSet* s : {r, w, e}) {
    if (s != nullptr) std::memset(s, 0, sizeof(*s));
  }
  int count = 0;
  for (const pollfd& p : fds) {
    if (p.revents & POLLNVAL) return errno = 9 /* EBADF */, -1;
    auto set = [&](BionicFdSet* s) {
      s->bits[p.fd / 64] |= uint64_t{1} << (p.fd % 64);
      ++count;
    };
    if (r != nullptr && (p.events & POLLIN) && (p.revents & (POLLIN | POLLHUP | POLLERR))) set(r);
    if (w != nullptr && (p.events & POLLOUT) && (p.revents & (POLLOUT | POLLERR))) set(w);
    if (e != nullptr && (p.events & POLLPRI) && (p.revents & POLLPRI)) set(e);
  }
  return count;
}

int BIONIC_SYSV Select(int nfds, BionicFdSet* r, BionicFdSet* w, BionicFdSet* e,
                       BionicTimeval* tv) {
  int timeout = -1;
  if (tv != nullptr) {
    int64_t ms = tv->tv_sec * 1000 + (tv->tv_usec + 999) / 1000;
    timeout = ms < 0 ? 0 : ms > 0x7fffffff ? 0x7fffffff : static_cast<int>(ms);
  }
  int rc = SelectImpl(nfds, r, w, e, timeout);
  if (tv != nullptr && rc >= 0) *tv = {0, 0};  // Linux updates the remaining time
  return rc;
}
int BIONIC_SYSV Pselect(int nfds, BionicFdSet* r, BionicFdSet* w, BionicFdSet* e,
                        const LinuxTimespec* ts, const void* /*sigmask*/) {
  return SelectImpl(nfds, r, w, e, TimeoutMillis(ts));
}

[[noreturn]] void FdSetOverflow(int fd) {
  std::fprintf(stderr, "gesher bionic: FORTIFY: FD_SET: file descriptor %d >= FD_SETSIZE\n", fd);
  std::abort();
}
void BIONIC_SYSV FdSetChk(int fd, BionicFdSet* s, size_t) {
  if (fd < 0 || fd >= kFdSetSize) FdSetOverflow(fd);
  s->bits[fd / 64] |= uint64_t{1} << (fd % 64);
}
void BIONIC_SYSV FdClrChk(int fd, BionicFdSet* s, size_t) {
  if (fd < 0 || fd >= kFdSetSize) FdSetOverflow(fd);
  s->bits[fd / 64] &= ~(uint64_t{1} << (fd % 64));
}
int BIONIC_SYSV FdIssetChk(int fd, const BionicFdSet* s, size_t) {
  if (fd < 0 || fd >= kFdSetSize) FdSetOverflow(fd);
  return FdIsSet(s, fd) ? 1 : 0;
}

// --- epoll, eventfd, pipes -----------------------------------------------------

int BIONIC_SYSV EpollCreate(int size) { return epoll_create(size); }
int BIONIC_SYSV EpollCreate1(int flags) { return epoll_create1(flags); }
int BIONIC_SYSV EpollCtl(int ep, int op, int fd, epoll_event* ev) { return epoll_ctl(ep, op, fd, ev); }
int BIONIC_SYSV EpollWait(int ep, epoll_event* ev, int max, int timeout) {
  DeliverPendingSignals();
  return epoll_wait(ep, ev, max, timeout);
}
int BIONIC_SYSV EpollPwait(int ep, epoll_event* ev, int max, int timeout, const void*) {
  return EpollWait(ep, ev, max, timeout);
}
int BIONIC_SYSV Eventfd(unsigned initval, int flags) { return eventfd(initval, flags); }
int BIONIC_SYSV EventfdRead(int fd, uint64_t* v) { return eventfd_read(fd, v); }
int BIONIC_SYSV EventfdWrite(int fd, uint64_t v) { return eventfd_write(fd, v); }
int BIONIC_SYSV Pipe(int* fds) { return pipe(fds); }
int BIONIC_SYSV Pipe2(int* fds, int flags) { return pipe2(fds, flags); }

int64_t BIONIC_SYSV Readv(int fd, const iovec* iov, int n) { return readv(fd, iov, n); }
int64_t BIONIC_SYSV Writev(int fd, const iovec* iov, int n) { return writev(fd, iov, n); }
int64_t BIONIC_SYSV Preadv(int fd, const iovec* iov, int n, int64_t off) {
  return preadv(fd, iov, n, static_cast<off_t>(off));
}
int64_t BIONIC_SYSV Pwritev(int fd, const iovec* iov, int n, int64_t off) {
  return pwritev(fd, iov, n, static_cast<off_t>(off));
}

}  // namespace

void RegisterSocket(SymbolTable& t) {
#define E(name, fn) t[name] = reinterpret_cast<void*>(&fn)
  E("socket", Socket);
  E("socketpair", Socketpair);
  E("bind", Bind);
  E("connect", Connect);
  E("listen", Listen);
  E("accept", Accept);
  E("accept4", Accept4);
  E("getsockname", Getsockname);
  E("getpeername", Getpeername);
  E("shutdown", Shutdown);
  E("setsockopt", Setsockopt);
  E("getsockopt", Getsockopt);
  E("send", Send);
  E("sendto", Sendto);
  E("sendmsg", Sendmsg);
  E("sendmmsg", Sendmmsg);
  E("recv", Recv);
  E("recvfrom", Recvfrom);
  E("recvmsg", Recvmsg);
  E("recvmmsg", Recvmmsg);
  E("__cmsg_nxthdr", CmsgNxthdr);
  E("__sendto_chk", SendtoChk);
  E("__recvfrom_chk", RecvfromChk);
  E("getaddrinfo", Getaddrinfo);
  E("android_setprocnetwork", SetProcNetwork);
  E("android_setsocknetwork", SetSockNetwork);
  E("android_getaddrinfofornetwork", GetaddrinfoForNetwork);
  E("android_res_nquery", ResNquery);
  E("android_res_nsend", ResNsend);
  E("android_res_nresult", ResNresult);
  E("android_res_cancel", ResCancel);
  E("freeaddrinfo", Freeaddrinfo);
  E("gai_strerror", GaiStrerror);
  E("getnameinfo", Getnameinfo);
  E("gethostbyname", Gethostbyname);
  E("gethostbyaddr", Gethostbyaddr);
  E("getservbyname", Getservbyname);
  E("getservbyport", Getservbyport);
  E("__get_h_errno", GetHErrno);
  E("inet_pton", InetPton);
  E("inet_ntop", InetNtop);
  E("inet_addr", InetAddr);
  E("inet_aton", InetAton);
  E("inet_ntoa", InetNtoa);
  E("htonl", Htonl);
  E("ntohl", Htonl);
  E("htons", Htons);
  E("ntohs", Htons);
  E("if_nametoindex", IfNametoindex);
  E("if_indextoname", IfIndextoname);
  t["in6addr_any"] = const_cast<in6_addr*>(&g_in6addr_any);
  t["in6addr_loopback"] = const_cast<in6_addr*>(&g_in6addr_loopback);
  E("poll", Poll);
  E("__poll_chk", Poll);
  E("ppoll", Ppoll);
  E("ppoll64", Ppoll);
  E("__ppoll_chk", Ppoll);
  E("__ppoll64_chk", Ppoll);
  E("select", Select);
  E("pselect", Pselect);
  E("pselect64", Pselect);
  E("__FD_SET_chk", FdSetChk);
  E("__FD_CLR_chk", FdClrChk);
  E("__FD_ISSET_chk", FdIssetChk);
  E("epoll_create", EpollCreate);
  E("epoll_create1", EpollCreate1);
  E("epoll_ctl", EpollCtl);
  E("epoll_wait", EpollWait);
  E("epoll_pwait", EpollPwait);
  E("epoll_pwait64", EpollPwait);
  E("eventfd", Eventfd);
  E("eventfd_read", EventfdRead);
  E("eventfd_write", EventfdWrite);
  E("pipe", Pipe);
  E("pipe2", Pipe2);
  E("readv", Readv);
  E("writev", Writev);
  E("preadv", Preadv);
  E("preadv64", Preadv);
  E("pwritev", Pwritev);
  E("pwritev64", Pwritev);
#undef E
}

}  // namespace gesher::bionic
