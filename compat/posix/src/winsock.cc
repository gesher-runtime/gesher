// gesher-base: the Winsock side of the socket layer (see socket_internal.h).
// Only Windows' headers here, never gesher-base's <sys/socket.h>.
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <mstcpip.h>
#include <mswsock.h>
#include <netioapi.h>
#include <windns.h>
#include <tlhelp32.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <pthread.h>

#include <mutex>
#include <atomic>
#include <new>

#include "internal.h"
#include "socket_internal.h"
#include "socket_user_copy.h"

namespace gesher::ws {
bool CopyFromUser(void* destination, const void* source, size_t size) {
  return usercopy::From(destination, source, size);
}
bool CopyToUser(void* destination, const void* source, size_t size) {
  return usercopy::To(destination, source, size);
}

struct ConnectRetirement {
  HANDLE started, owner;
  HMODULE module;
  bool no_owner, quarantine;
};

// Neither a caller's stack nor its thread owns pending native I/O. The worker
// retains this object, its address and both events until completion is observed.
struct ConnectOperation {
  Socket socket;
  Address address;
  sockaddr_storage native_address{};  // the actual kernel argument lives to drain
  int native_address_length = 0;
  std::shared_ptr<SocketDescription> description;
  uint64_t generation;
  OVERLAPPED overlap{};
  HANDLE submitted = nullptr;
  HANDLE reaper = nullptr;
  ConnectRetirement* retirement = nullptr;  // WinHeap, retained until owner exit
  bool pending = false, issued = false, finished = false;  // error_mutex
  int start_error = 0, result = 0;  // error_mutex, or after submitted/finished
  ULONGLONG deadline = ~ULONGLONG{0};
  std::atomic<bool> audit_close{false};  // set only by private retirement evidence
  ~ConnectOperation() {
    struct Closed { HANDLE handle; BOOL result; DWORD error; };
    auto close = [](HANDLE handle) {
      Closed result{handle, handle ? CloseHandle(handle) : TRUE, 0};
      if (!result.result) result.error = GetLastError();
      return result;
    };
    const Closed a = close(submitted), b = close(overlap.hEvent), c = close(reaper);
    if (audit_close.load()) {
      printf("TCP_PRIORITY_OPERATION_CLOSE submitted=%llu closed=%d error=%lu event=%llu closed=%d error=%lu reaper=%llu closed=%d error=%lu\n",
          static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(a.handle)), a.result, a.error,
          static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(b.handle)), b.result, b.error,
          static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(c.handle)), c.result, c.error); fflush(stdout);
    }
  }
};

namespace {

std::atomic<uint32_t> g_process_interface{0};

// gesher-base defines bind, connect, ... itself, with Linux's types
// (socket.cc, netdb.cc). Linking Winsock's functions of the same names would
// collide with those (an import library member defines both `bind` and
// `__imp_bind`), so they are looked up in ws2_32.dll instead. decltype does
// not reference them.
struct WinsockApi {
  decltype(&::bind) bind;
  decltype(&::connect) connect;
  decltype(&::listen) listen;
  decltype(&::accept) accept;
  decltype(&::getsockname) getsockname;
  decltype(&::getpeername) getpeername;
  decltype(&::getsockopt) getsockopt;
  decltype(&::setsockopt) setsockopt;
  decltype(&::shutdown) shutdown;
  decltype(&::inet_pton) inet_pton;
  decltype(&::inet_ntop) inet_ntop;
  // Exact WinSDK legacy signatures; resolve dynamically like the socket API.
  hostent* (WSAAPI* gethostbyname)(const char*);
  hostent* (WSAAPI* gethostbyaddr)(const char*, int, int);
  servent* (WSAAPI* getservbyname)(const char*, const char*);
  servent* (WSAAPI* getservbyport)(int, const char*);
};

template <typename Fn>
void LoadProc(HMODULE module, const char* name, Fn* fn) {
  FARPROC proc = GetProcAddress(module, name);
  if (proc == nullptr) {
    fprintf(stderr, "gesher-base: ws2_32.dll has no %s\n", name);
    abort();
  }
  *fn = reinterpret_cast<Fn>(reinterpret_cast<void*>(proc));
}

const WinsockApi& Api() {
  static const WinsockApi api = [] {
    WinsockApi a;
    // Loaded already, for the WSA* functions gesher-base imports.
    HMODULE ws2 = GetModuleHandleW(L"ws2_32.dll");
    if (ws2 == nullptr) ws2 = LoadLibraryExW(L"ws2_32.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    LoadProc(ws2, "bind", &a.bind);
    LoadProc(ws2, "connect", &a.connect);
    LoadProc(ws2, "listen", &a.listen);
    LoadProc(ws2, "accept", &a.accept);
    LoadProc(ws2, "getsockname", &a.getsockname);
    LoadProc(ws2, "getpeername", &a.getpeername);
    LoadProc(ws2, "getsockopt", &a.getsockopt);
    LoadProc(ws2, "setsockopt", &a.setsockopt);
    LoadProc(ws2, "shutdown", &a.shutdown);
    LoadProc(ws2, "inet_pton", &a.inet_pton);
    LoadProc(ws2, "inet_ntop", &a.inet_ntop);
    LoadProc(ws2, "gethostbyname", &a.gethostbyname);
    LoadProc(ws2, "gethostbyaddr", &a.gethostbyaddr);
    LoadProc(ws2, "getservbyname", &a.getservbyname);
    LoadProc(ws2, "getservbyport", &a.getservbyport);
    return a;
  }();
  return api;
}

int ErrnoFromWsa(int error) {
  switch (error) {
    case WSAEINTR: return EINTR;
    case WSAEBADF:
    case WSAENOTSOCK: return ENOTSOCK;
    case WSAEACCES: return EACCES;
    case WSAEFAULT: return EFAULT;
    case WSAEINVAL: return EINVAL;
    case WSAEMFILE: return EMFILE;
    case WSAEWOULDBLOCK: return EAGAIN;
    case WSAEINPROGRESS: return EINPROGRESS;
    case WSAEALREADY: return EALREADY;
    case WSAEDESTADDRREQ: return EDESTADDRREQ;
    case WSAEMSGSIZE: return EMSGSIZE;
    case WSAEPROTOTYPE: return EPROTOTYPE;
    case WSAENOPROTOOPT: return ENOPROTOOPT;
    case WSAEPROTONOSUPPORT: return EPROTONOSUPPORT;
    case WSAESOCKTNOSUPPORT: return ESOCKTNOSUPPORT;
    case WSAEOPNOTSUPP: return EOPNOTSUPP;
    case WSAEPFNOSUPPORT: return EPFNOSUPPORT;
    case WSAEAFNOSUPPORT: return EAFNOSUPPORT;
    case WSAEADDRINUSE: return EADDRINUSE;
    case WSAEADDRNOTAVAIL: return EADDRNOTAVAIL;
    case WSAENETDOWN: return ENETDOWN;
    case WSAENETUNREACH: return ENETUNREACH;
    case WSAENETRESET: return ENETRESET;
    case WSAECONNABORTED: return ECONNABORTED;
    case WSAECONNRESET: return ECONNRESET;
    case WSAENOBUFS: return ENOBUFS;
    case WSAEISCONN: return EISCONN;
    case WSAENOTCONN: return ENOTCONN;
    case WSAESHUTDOWN: return EPIPE;  // Linux: writing after shutdown(SHUT_WR)
    case WSAETOOMANYREFS: return ETOOMANYREFS;
    case WSAETIMEDOUT: return ETIMEDOUT;
    case WSAECONNREFUSED: return ECONNREFUSED;
    case WSAELOOP: return ELOOP;
    case WSAENAMETOOLONG: return ENAMETOOLONG;
    case WSAEHOSTDOWN: return EHOSTDOWN;
    case WSAEHOSTUNREACH: return EHOSTUNREACH;
    case WSAENOTEMPTY: return ENOTEMPTY;
    case WSAEUSERS: return EUSERS;
    case WSAEDQUOT: return EDQUOT;
    case WSAESTALE: return ESTALE;
    case WSAEREMOTE: return EREMOTE;
    case WSA_NOT_ENOUGH_MEMORY: return ENOMEM;
    case WSANOTINITIALISED:
    case WSASYSNOTREADY: return ENETDOWN;
    default: return EIO;
  }
}

int Fail() {
  errno = ErrnoFromWsa(WSAGetLastError());
  return -1;
}

// Overlapped, though gesher-base only makes blocking calls: Winsock ignores
// SO_RCVTIMEO and SO_SNDTIMEO on a WSASocket without WSA_FLAG_OVERLAPPED.
// Not inheritable: child processes never see gesher-base's fd table, so an
// inherited socket could not be used there anyway.
constexpr DWORD kSocketFlags = WSA_FLAG_OVERLAPPED | WSA_FLAG_NO_HANDLE_INHERIT;

// For send and receive: when SO_SNDTIMEO/SO_RCVTIMEO expires, Winsock
// reports WSAETIMEDOUT where Linux reports EAGAIN.
int FailIo() {
  if (WSAGetLastError() == WSAETIMEDOUT) return errno = EAGAIN, -1;
  return Fail();
}

int ToWinFamily(Family family) {
  switch (family) {
    case Family::kUnspec: return AF_UNSPEC;
    case Family::kInet: return AF_INET;
    case Family::kInet6: return AF_INET6;
    case Family::kUnix: return AF_UNIX;
    case Family::kOther: break;
  }
  return -1;
}

Family FromWinFamily(int family) {
  switch (family) {
    case AF_UNSPEC: return Family::kUnspec;
    case AF_INET: return Family::kInet;
    case AF_INET6: return Family::kInet6;
    case AF_UNIX: return Family::kUnix;
    default: return Family::kOther;
  }
}

int ToWinType(Type type) {
  switch (type) {
    case Type::kStream: return SOCK_STREAM;
    case Type::kDgram: return SOCK_DGRAM;
    case Type::kRaw: return SOCK_RAW;
    case Type::kSeqpacket: return SOCK_SEQPACKET;
    case Type::kOther: break;
  }
  return 0;
}

Type FromWinType(int type) {
  switch (type) {
    case SOCK_STREAM: return Type::kStream;
    case SOCK_DGRAM: return Type::kDgram;
    case SOCK_RAW: return Type::kRaw;
    case SOCK_SEQPACKET: return Type::kSeqpacket;
    default: return Type::kOther;
  }
}

// Winsock's sockaddr_storage from an Address; false (errno set) if the family
// has no Winsock equivalent.
bool ToWin(const Address& in, sockaddr_storage* out, int* len) {
  int family = ToWinFamily(in.family);
  if (family < 0 || in.data_len > sizeof(*out) - sizeof(out->ss_family)) {
    errno = EAFNOSUPPORT;
    return false;
  }
  memset(out, 0, sizeof(*out));
  out->ss_family = static_cast<ADDRESS_FAMILY>(family);
  memcpy(reinterpret_cast<char*>(out) + sizeof(out->ss_family), in.data, in.data_len);
  *len = static_cast<int>(sizeof(out->ss_family) + in.data_len);
  return true;
}

void FromWin(const sockaddr* in, int len, Address* out) {
  out->family = FromWinFamily(in->sa_family);
  size_t data_len = len > static_cast<int>(sizeof(in->sa_family))
                        ? static_cast<size_t>(len) - sizeof(in->sa_family) : 0;
  if (data_len > sizeof(out->data)) data_len = sizeof(out->data);
  memcpy(out->data, reinterpret_cast<const char*>(in) + sizeof(in->sa_family), data_len);
  out->data_len = data_len;
}

}  // namespace

bool Startup() {
  static std::once_flag once;
  static int result = 0;
  std::call_once(once, [] {
    WSADATA data;
    result = WSAStartup(MAKEWORD(2, 2), &data);
  });
  if (result != 0) {
    errno = ErrnoFromWsa(result);
    return false;
  }
  return true;
}

Socket Open(Family family, Type type, int protocol, bool process_binding) {
  int win_family = ToWinFamily(family);
  int win_type = ToWinType(type);
  if (win_family < 0) return errno = EAFNOSUPPORT, kInvalidSocket;
  if (win_type == 0) return errno = ESOCKTNOSUPPORT, kInvalidSocket;
  SOCKET s = WSASocketW(win_family, win_type, protocol, nullptr, 0, kSocketFlags);
  if (s == INVALID_SOCKET) return Fail(), kInvalidSocket;
  if (win_family == AF_INET6) {
    // Linux's default (net.ipv6.bindv6only = 0): an IPv6 socket also takes
    // IPv4 peers as v4-mapped addresses. Windows defaults to IPv6 only.
    DWORD v6only = 0;
    Api().setsockopt(s, IPPROTO_IPV6, IPV6_V6ONLY, reinterpret_cast<const char*>(&v6only),
                     sizeof(v6only));
  }
  if (win_type == SOCK_DGRAM) {
    // Windows fails the next recv with WSAECONNRESET when an ICMP "port
    // unreachable" arrives for an earlier send; Linux ignores it on
    // unconnected sockets.
    BOOL report = FALSE;
    DWORD bytes;
    WSAIoctl(s, SIO_UDP_CONNRESET, &report, sizeof(report), nullptr, 0, &bytes, nullptr, nullptr);
  }
  uint32_t ifindex = ProcessInterface();
  if (process_binding && (win_family == AF_INET || win_family == AF_INET6) && ifindex != 0 &&
      BindInterface(static_cast<Socket>(s), ifindex) != 0) {
    int saved = errno;
    closesocket(s);
    errno = saved;
    return kInvalidSocket;
  }
  return static_cast<Socket>(s);
}

int ValidateInterface(uint32_t ifindex) {
  if (ifindex == 0) return 0;
  MIB_IF_ROW2 row{};
  row.InterfaceIndex = ifindex;
  if (GetIfEntry2(&row) != NO_ERROR) return errno = ENONET, -1;
  if (row.OperStatus != IfOperStatusUp) return errno = ENETDOWN, -1;
  return 0;
}

int SetProcessInterface(uint32_t ifindex) {
  if (ValidateInterface(ifindex) != 0) return -1;
  g_process_interface.store(ifindex);
  return 0;
}

uint32_t ProcessInterface() { return g_process_interface.load(); }

int DnsServers(uint32_t ifindex, std::vector<Address>* servers, uint32_t* selected) {
  if (!Startup()) return -1;
  if (ifindex == 0) ifindex = ProcessInterface();
  if (ifindex == 0) {
    sockaddr_in destination{};
    destination.sin_family = AF_INET;
    destination.sin_addr.s_addr = htonl(0x08080808);
    DWORD best = 0;
    if (GetBestInterfaceEx(reinterpret_cast<sockaddr*>(&destination), &best) != NO_ERROR) {
      return errno = ENONET, -1;
    }
    ifindex = best;
  }
  if (ValidateInterface(ifindex) != 0) return -1;
  if (selected != nullptr) *selected = ifindex;
  ULONG size = 16384;
  std::vector<unsigned char> buffer;
  ULONG status = ERROR_BUFFER_OVERFLOW;
  for (int retry = 0; retry < 4 && status == ERROR_BUFFER_OVERFLOW; ++retry) {
    buffer.resize(size);
    status = GetAdaptersAddresses(AF_UNSPEC, 0, nullptr,
        reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()), &size);
  }
  if (status != NO_ERROR) return errno = EIO, -1;
  for (auto* adapter = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()); adapter != nullptr;
       adapter = adapter->Next) {
    if (adapter->IfIndex != ifindex && adapter->Ipv6IfIndex != ifindex) continue;
    for (auto* dns = adapter->FirstDnsServerAddress; dns != nullptr; dns = dns->Next) {
      sockaddr_storage address{};
      memcpy(&address, dns->Address.lpSockaddr, dns->Address.iSockaddrLength);
      if (address.ss_family == AF_INET) {
        reinterpret_cast<sockaddr_in*>(&address)->sin_port = htons(53);
      } else if (address.ss_family == AF_INET6) {
        auto* v6 = reinterpret_cast<sockaddr_in6*>(&address);
        // Windows' legacy site-local placeholders are not DNS servers.
        if (v6->sin6_addr.s6_addr[0] == 0xfe && v6->sin6_addr.s6_addr[1] == 0xc0) continue;
        v6->sin6_port = htons(53);
      } else {
        continue;
      }
      Address converted;
      FromWin(reinterpret_cast<sockaddr*>(&address), dns->Address.iSockaddrLength, &converted);
      servers->push_back(converted);
    }
    break;
  }
  return servers->empty() ? (errno = ENONET, -1) : 0;
}

int SocketFamily(Socket s) {
  WSAPROTOCOL_INFOW info{};
  int length = sizeof(info);
  if (Api().getsockopt(s, SOL_SOCKET, SO_PROTOCOL_INFOW,
                       reinterpret_cast<char*>(&info), &length) != 0) return Fail();
  return info.iAddressFamily;
}

int BoundInterface(Socket s, uint32_t* ifindex) {
  int family = SocketFamily(s);
  if (family < 0) return -1;
  DWORD value = 0;
  int length = sizeof(value);
  int level = family == AF_INET6 ? IPPROTO_IPV6 : IPPROTO_IP;
  int option = family == AF_INET6 ? IPV6_UNICAST_IF : IP_UNICAST_IF;
  if (family != AF_INET && family != AF_INET6) return errno = EAFNOSUPPORT, -1;
  if (Api().getsockopt(s, level, option, reinterpret_cast<char*>(&value), &length) != 0) return Fail();
  // Both getters return host byte order, including IP_UNICAST_IF.
  *ifindex = value;
  return 0;
}

int BindInterface(Socket s, uint32_t ifindex) {
  if (ValidateInterface(ifindex) != 0) return -1;
  int family = SocketFamily(s);
  if (family < 0) return -1;
  if (family != AF_INET && family != AF_INET6) return errno = EAFNOSUPPORT, -1;
  sockaddr_storage peer{};
  int peer_length = sizeof(peer);
  if (Api().getpeername(s, reinterpret_cast<sockaddr*>(&peer), &peer_length) == 0) {
    return errno = EISCONN, -1;
  }
  if (WSAGetLastError() != WSAENOTCONN && WSAGetLastError() != WSAEINVAL) return Fail();
  DWORD v6only = 1;
  int length = sizeof(v6only);
  if (family == AF_INET6 && Api().getsockopt(s, IPPROTO_IPV6, IPV6_V6ONLY,
      reinterpret_cast<char*>(&v6only), &length) != 0) return Fail();
  // Dual-stack sockets need both options. IPv4's index is in network byte
  // order; IPv6's is in host byte order (Microsoft's UNICAST_IF contract).
  DWORD previous4 = 0;
  bool ipv4 = family == AF_INET || v6only == 0;
  if (ipv4) {
    length = sizeof(previous4);
    if (Api().getsockopt(s, IPPROTO_IP, IP_UNICAST_IF,
        reinterpret_cast<char*>(&previous4), &length) != 0) return Fail();
    DWORD index4 = htonl(ifindex);
    if (Api().setsockopt(s, IPPROTO_IP, IP_UNICAST_IF,
        reinterpret_cast<const char*>(&index4), sizeof(index4)) != 0) return Fail();
  }
  if (family == AF_INET6 && Api().setsockopt(s, IPPROTO_IPV6, IPV6_UNICAST_IF,
      reinterpret_cast<const char*>(&ifindex), sizeof(ifindex)) != 0) {
    int saved = ErrnoFromWsa(WSAGetLastError());
    previous4 = htonl(previous4);
    if (ipv4) Api().setsockopt(s, IPPROTO_IP, IP_UNICAST_IF,
        reinterpret_cast<const char*>(&previous4), sizeof(previous4));
    return errno = saved, -1;
  }
  return 0;
}

int Close(Socket s, const std::shared_ptr<SocketDescription>& description, bool retain_failure) {
  if (!description) return closesocket(static_cast<SOCKET>(s)) == 0 ? 0 : Fail();
  {
    std::lock_guard<std::mutex> lock(description->error_mutex);
    description->native_closing = true;
  }
  std::unique_lock<std::mutex> native_call(description->native_call_mutex);
  // SO_LINGER may wait. Never make a finite poll wait for this native call
  // while trying to refresh its completion metadata.
  const int result = closesocket(static_cast<SOCKET>(s)) == 0 ? 0 : Fail();
  const int saved = errno;
  {
    std::lock_guard<std::mutex> lock(description->error_mutex);
    // closesocket cancels pending operations. Its event/OVERLAPPED remain owned
    // by the worker until actual completion, including when the number is reused.
    if (result == 0) description->native_closed = true;
    else if (retain_failure) {
      // No allocation after ownership was removed from the fd table. A failed
      // physical close retains both actual handle and description explicitly.
      description->quarantined_socket = s;
      description->failed_close_owner = description;
    }
    description->native_closing = false;
  }
  native_call.unlock();
  if (result < 0 && retain_failure) fprintf(stderr, "gesher-base: native socket close failed errno=%d; SOCKET=%llu retained in description quarantine\n", saved, static_cast<unsigned long long>(s));
  errno = saved;
  return result;
}

Socket Duplicate(Socket s) {
  WSAPROTOCOL_INFOW info;
  if (WSADuplicateSocketW(static_cast<SOCKET>(s), GetCurrentProcessId(), &info) != 0) {
    return Fail(), kInvalidSocket;
  }
  SOCKET result = WSASocketW(FROM_PROTOCOL_INFO, FROM_PROTOCOL_INFO, FROM_PROTOCOL_INFO, &info, 0,
                             kSocketFlags);
  if (result == INVALID_SOCKET) return Fail(), kInvalidSocket;
  return static_cast<Socket>(result);
}

int SetNonBlocking(Socket s, bool on) {
  u_long mode = on ? 1 : 0;
  return ioctlsocket(static_cast<SOCKET>(s), FIONBIO, &mode) == 0 ? 0 : Fail();
}

int ConfigureOwnedDatagram(Socket s) {
  WSAPROTOCOL_INFOW info{};
  int length = sizeof(info);
  if (Api().getsockopt(static_cast<SOCKET>(s), SOL_SOCKET, SO_PROTOCOL_INFOW,
                      reinterpret_cast<char*>(&info), &length) != 0) return Fail();
  if (length != static_cast<int>(sizeof(info))) return errno = EIO, -1;
  if ((info.iAddressFamily != AF_INET && info.iAddressFamily != AF_INET6) ||
      info.iSocketType != SOCK_DGRAM || info.iProtocol != IPPROTO_UDP) return 0;
  // RegisterSocket transfers ownership, including a previously blocking native
  // endpoint. Normalize once BEFORE publication; dup aliases retain this basis.
  return SetNonBlocking(s, true) < 0 ? -1 : 1;
}

int Bind(Socket s, const Address& address, const std::shared_ptr<SocketDescription>& description) {
  std::unique_lock<std::mutex> native_call;
  if (description) {
    native_call = std::unique_lock<std::mutex>(description->native_call_mutex);
    std::lock_guard<std::mutex> lock(description->error_mutex);
    if (description->native_closed || description->native_closing) return errno = EBADF, -1;
  }
  sockaddr_storage ss;
  int len;
  if (!ToWin(address, &ss, &len)) return -1;
  return Api().bind(static_cast<SOCKET>(s), reinterpret_cast<sockaddr*>(&ss), len) == 0 ? 0 : Fail();
}

int Connect(Socket s, const Address& address) {
  sockaddr_storage ss;
  int len;
  if (!ToWin(address, &ss, &len)) return -1;
  if (Api().connect(static_cast<SOCKET>(s), reinterpret_cast<sockaddr*>(&ss), len) == 0) return 0;
  // A non-blocking connect reports WSAEWOULDBLOCK where Linux says EINPROGRESS.
  if (WSAGetLastError() == WSAEWOULDBLOCK) return errno = EINPROGRESS, -1;
  return Fail();
}

int ConnectLegacy(Socket s, const Address& address, const std::shared_ptr<SocketDescription>& description,
                  void (*before_native)(void*), void* context) {
  // These exact provider paths do not initiate a TCP handshake. Coordination
  // is separate from completion metadata, so poll/getters never wait on it.
  const bool coordinated = description->native_nonblocking_datagram ||
      (description->provider == Provider::kNativeIpv4Tcp && address.family == Family::kUnspec);
  if (!coordinated) {
    if (before_native) return errno = EOPNOTSUPP, -1;
    return Connect(s, address);  // unchanged, unqualified provider/legacy path
  }
  std::lock_guard<std::mutex> native_call(description->native_call_mutex);
  if (before_native) before_native(context);
  {
    std::lock_guard<std::mutex> lock(description->error_mutex);
    if (description->native_closed || description->native_closing) return errno = EBADF, -1;
  }
  return Connect(s, address);
}

namespace {

// Called with error_mutex held. Only a signaled native completion can retire
// an issued OVERLAPPED; cancellation or closing its socket alone is not enough.
void FinishConnect(const std::shared_ptr<ConnectOperation>& operation) {
  auto& op = *operation;
  auto& description = *op.description;
  if (op.finished) return;
  if (description.native_closing) return;
  if (op.pending && WaitForSingleObject(op.overlap.hEvent, 0) != WAIT_OBJECT_0) return;
  int result = op.start_error;
  if (op.pending) {
    if (description.native_closed) {
      // Do not query a numeric SOCKET after last-close: it may name a new
      // endpoint now. The signaled event is the operation's completion proof.
      result = ECANCELED;
    } else {
      DWORD bytes = 0, flags = 0;
      if (!WSAGetOverlappedResult(static_cast<SOCKET>(op.socket), &op.overlap, &bytes, FALSE, &flags)) {
        const int error = WSAGetLastError();
        if (error == WSA_IO_INCOMPLETE) return;
        result = error == ERROR_OPERATION_ABORTED ? ECANCELED : ErrnoFromWsa(error);
      } else result = 0;
    }
  }
  if (result == 0 && !description.native_closed &&
      Api().setsockopt(static_cast<SOCKET>(op.socket), SOL_SOCKET, SO_UPDATE_CONNECT_CONTEXT, nullptr, 0) != 0) result = ErrnoFromWsa(WSAGetLastError());
  op.result = result;
  op.finished = true;
  if (description.connect_generation == op.generation && description.connect_operation == operation) {
    if (!description.native_closed) {
      description.connect_result = result;
      description.connect_error = result;
      description.refusal_generation = op.issued && result == ECONNREFUSED ? op.generation : 0;
      description.state.store(result == 0 || result == EISCONN ? ConnectState::kConnected : ConnectState::kFailed);
    }
    description.connect_operation.reset();  // break the description/operation cycle
  }
}

DWORD WINAPI ConnectReaper(void* raw) {
  // Only WinAPI and trivial values. This thread never enters pthread/CRT/C++
  // ownership. Its actual owner HANDLE, not a TID or early event, proves exit.
  auto* retirement = static_cast<ConnectRetirement*>(raw);
  if (WaitForSingleObject(retirement->started, INFINITE) != WAIT_OBJECT_0 ||
      retirement->quarantine || (!retirement->owner && !retirement->no_owner) ||
      (retirement->owner && WaitForSingleObject(retirement->owner, INFINITE) != WAIT_OBJECT_0)) {
    OutputDebugStringW(L"gesher-base: connect owner retirement quarantined; module and native lifetime records retained\n");
    for (;;) Sleep(INFINITE);
  }
  if ((retirement->owner && !CloseHandle(retirement->owner)) || !CloseHandle(retirement->started)) {
    OutputDebugStringW(L"gesher-base: connect owner handle retirement quarantined; module retained\n");
    for (;;) Sleep(INFINITE);
  }
  HMODULE module = retirement->module;
  if (!HeapFree(GetProcessHeap(), 0, retirement)) {
    OutputDebugStringW(L"gesher-base: connect owner ticket retirement quarantined; module retained\n");
    for (;;) Sleep(INFINITE);
  }
  if (module) FreeLibraryAndExitThread(module, 0);
  return 0;
}

void* ConnectOwner(void* raw) {
  {
    std::unique_ptr<std::shared_ptr<ConnectOperation>> transfer(static_cast<std::shared_ptr<ConnectOperation>*>(raw));
    auto operation = *transfer;
    transfer.reset();
    auto& op = *operation;
    // Capture the actual self thread object without numeric TID lookup. The
    // detached pthread record may retire normally; the reaper retains this
    // separate noninherited HANDLE until the entire owning wrapper has exited.
    const bool captured = DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(),
                                         &op.retirement->owner, SYNCHRONIZE, FALSE, 0) != FALSE;
    if (!captured) op.retirement->quarantine = true;
    if (!SetEvent(op.retirement->started)) op.retirement->quarantine = true;
    {
      std::lock_guard<std::mutex> lock(op.description->error_mutex);
      if (!captured) op.start_error = EIO;
      else if (op.description->native_closed || op.description->native_closing || op.description->connect_generation != op.generation) {
        op.start_error = EBADF;
      } else {
        WSAPROTOCOL_INFOW protocol{}; int length = sizeof(protocol);
        GUID id = WSAID_CONNECTEX; LPFN_CONNECTEX connect_ex = nullptr; DWORD bytes = 0;
        if (!ToWin(op.address, &op.native_address, &op.native_address_length)) op.start_error = errno;
        else if (Api().getsockopt(static_cast<SOCKET>(op.socket), SOL_SOCKET, SO_PROTOCOL_INFOW, reinterpret_cast<char*>(&protocol), &length) != 0 ||
                 WSAIoctl(static_cast<SOCKET>(op.socket), SIO_GET_EXTENSION_FUNCTION_POINTER, &id, sizeof(id), &connect_ex, sizeof(connect_ex), &bytes, nullptr, nullptr) != 0) op.start_error = ErrnoFromWsa(WSAGetLastError());
        else {
          sockaddr_storage local{}; length = sizeof(local);
          if (Api().getsockname(static_cast<SOCKET>(op.socket), reinterpret_cast<sockaddr*>(&local), &length) != 0) {
            if (WSAGetLastError() != WSAEINVAL) op.start_error = ErrnoFromWsa(WSAGetLastError());
            else {
              local.ss_family = static_cast<ADDRESS_FAMILY>(protocol.iAddressFamily);
              const int bind_length = protocol.iAddressFamily == AF_INET6 ? sizeof(sockaddr_in6) : sizeof(sockaddr_in);
              if (Api().bind(static_cast<SOCKET>(op.socket), reinterpret_cast<sockaddr*>(&local), bind_length) != 0) op.start_error = ErrnoFromWsa(WSAGetLastError());
            }
          }
          if (op.start_error == 0) {
            op.issued = true;
            if (!connect_ex(static_cast<SOCKET>(op.socket), reinterpret_cast<sockaddr*>(&op.native_address), op.native_address_length, nullptr, 0, nullptr, &op.overlap)) {
              const int error = WSAGetLastError();
              op.pending = error == ERROR_IO_PENDING;
              op.start_error = op.pending ? 0 : ErrnoFromWsa(error);
            }
          }
        }
      }
      if (!op.pending) FinishConnect(operation);
      SetEvent(op.submitted);
    }
    if (op.pending) {
      // A failed wait is not permission to free live native context. Retain
      // the owner/module and continue looking for actual kernel completion.
      while (WaitForSingleObject(op.overlap.hEvent, INFINITE) != WAIT_OBJECT_0) Sleep(100);
    }
    for (;;) {
      bool finished;
      { std::lock_guard<std::mutex> lock(op.description->error_mutex); FinishConnect(operation); finished = op.finished; }
      if (finished) break;
      Sleep(10);  // native close publication may still be in progress
    }
  }  // destroy C++ context, then return through the normal pthread/TLS wrapper
  return nullptr;
}

}  // namespace

std::shared_ptr<ConnectOperation> StartConnect(Socket s, const Address& address,
    const std::shared_ptr<SocketDescription>& description) {
  std::lock_guard<std::mutex> lock(description->error_mutex);
  if (description->provider != Provider::kNativeIpv4Tcp || address.family != Family::kInet) return errno = EOPNOTSUPP, nullptr;
  if (description->native_closed || description->native_closing) return errno = EBADF, nullptr;
  if (description->connect_operation || description->legacy_connect_active) return errno = EALREADY, nullptr;
  if (description->state.load() == ConnectState::kConnected) return errno = EISCONN, nullptr;
  const ConnectState previous_state = description->state.load();
  if (description->connect_generation == UINT64_MAX) return errno = EOVERFLOW, nullptr;
  auto operation = std::make_shared<ConnectOperation>();
  auto& op = *operation;
  op.socket = s; op.address = address; op.description = description;
  op.generation = ++description->connect_generation;
  int send_timeout = 0;
  if (GetOption(s, Option::kSndTimeo, &send_timeout) < 0) return nullptr;
  if (send_timeout > 0) op.deadline = DeadlineAfter(send_timeout);
  op.overlap.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  op.submitted = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (!op.overlap.hEvent || !op.submitted) return FailWithLastError(), nullptr;
  // No C++ allocation may throw after the reaper has started waiting for its
  // owner publication. Reserve the exact transfer before pin/thread creation.
  std::unique_ptr<std::shared_ptr<ConnectOperation>> transfer(new (std::nothrow) std::shared_ptr<ConnectOperation>(operation));
  if (!transfer) return errno = ENOMEM, nullptr;
  HMODULE module = nullptr;
  if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                         reinterpret_cast<LPCWSTR>(&ConnectOwner), &module)) return FailWithLastError(), nullptr;
  auto* retirement = static_cast<ConnectRetirement*>(HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(ConnectRetirement)));
  if (!retirement) return errno = ENOMEM, nullptr;
  retirement->started = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (!retirement->started) { const int saved = ErrnoFromWin32(GetLastError()); HeapFree(GetProcessHeap(), 0, retirement); return errno = saved, nullptr; }
  // Pin before either thread can start. The WinAPI-only reaper releases it
  // only after observing the actual owning pthread wrapper's native exit.
  if (module != GetModuleHandleW(nullptr)) {
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, reinterpret_cast<LPCWSTR>(&ConnectOwner), &retirement->module)) {
      const int saved = ErrnoFromWin32(GetLastError()); CloseHandle(retirement->started); HeapFree(GetProcessHeap(), 0, retirement); return errno = saved, nullptr;
    }
  }
  op.retirement = retirement;
  op.reaper = CreateThread(nullptr, 0, ConnectReaper, retirement, 0, nullptr);
  if (!op.reaper) {
    const int saved = ErrnoFromWin32(GetLastError());
    if (retirement->module) FreeLibrary(retirement->module);
    CloseHandle(retirement->started); HeapFree(GetProcessHeap(), 0, retirement);
    return errno = saved, nullptr;
  }
  description->connect_result = description->connect_error = 0;
  description->refusal_generation = 0;
  description->connect_operation = operation;
  description->state.store(ConnectState::kConnecting);
  pthread_attr_t attributes;
  int created = pthread_attr_init(&attributes);
  const bool initialized = created == 0;
  if (created == 0) created = pthread_attr_setdetachstate(&attributes, PTHREAD_CREATE_DETACHED);
  pthread_t thread;
  if (created == 0) created = pthread_create(&thread, &attributes, ConnectOwner, transfer.get());
  if (initialized) pthread_attr_destroy(&attributes);
  if (created != 0) {
    description->connect_operation.reset();
    description->state.store(previous_state);
    retirement->no_owner = true;
    // A failed creation has no running owner. The pre-reserved reaper still
    // owns ticket/pin cleanup. A failed publication keeps that owner quarantined.
    if (!SetEvent(retirement->started)) retirement->quarantine = true;
    return errno = gesher_errno_from_crt(created), nullptr;
  }
  transfer.release();  // ownership transferred even if the worker already began
  return operation;
}

int AwaitConnectRetirement(const std::shared_ptr<ConnectOperation>& operation, int timeout_ms) {
  const DWORD wait = WaitForSingleObject(operation->reaper, timeout_ms < 0 ? INFINITE : static_cast<DWORD>(timeout_ms));
  if (wait == WAIT_OBJECT_0) {
    operation->audit_close.store(true);
    HMODULE image = nullptr;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                          reinterpret_cast<LPCWSTR>(&ConnectOwner), &image)) {
      wchar_t path[1024]{}; const DWORD size = GetModuleFileNameW(image, path, 1024);
      if (size > 0 && size < 1024) {
        const wchar_t* name = wcsrchr(path, L'\\'); name = name ? name + 1 : path;
        printf("TCP_PRIORITY_OWNER_CODE module=%ls owner_rva=%llu reaper_rva=%llu\n", name,
            static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(&ConnectOwner) - reinterpret_cast<uintptr_t>(image)),
            static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(&ConnectReaper) - reinterpret_cast<uintptr_t>(image)));
      }
    }
    printf("TCP_PRIORITY_OPERATION_HANDLES submitted=%llu event=%llu reaper=%llu native_retired=1\n",
        static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(operation->submitted)),
        static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(operation->overlap.hEvent)),
        static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(operation->reaper))); fflush(stdout);
  }
  return wait == WAIT_OBJECT_0 ? 0 : (errno = wait == WAIT_TIMEOUT ? ETIMEDOUT : EIO, -1);
}

bool ReadConnectOwnerCensus(OwnerCensus* result) {
  HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
  if (snapshot == INVALID_HANDLE_VALUE) return FailWithLastError(), false;
  THREADENTRY32 entry{}; entry.dwSize = sizeof(entry); uint32_t count = 0;
  if (!Thread32First(snapshot, &entry)) { const DWORD saved = GetLastError(); CloseHandle(snapshot); return errno = ErrnoFromWin32(saved), false; }
  do {
    if (entry.th32OwnerProcessID == GetCurrentProcessId()) ++count;
    entry.dwSize = sizeof(entry);
  } while (Thread32Next(snapshot, &entry));
  const DWORD enumeration = GetLastError();
  if (!CloseHandle(snapshot)) return FailWithLastError(), false;
  if (enumeration != ERROR_NO_MORE_FILES) return errno = ErrnoFromWin32(enumeration), false;
  DWORD handles = 0;
  if (!GetProcessHandleCount(GetCurrentProcess(), &handles)) return FailWithLastError(), false;
  result->handles = handles; result->threads = count;
  return true;
}

void RefreshConnect(const std::shared_ptr<SocketDescription>& description) {
  std::lock_guard<std::mutex> lock(description->error_mutex);
  auto operation = description->connect_operation;
  if (operation && WaitForSingleObject(operation->submitted, 0) == WAIT_OBJECT_0) FinishConnect(operation);
}

namespace {
int WaitConnectEvent(HANDLE event, ULONGLONG deadline) {
  int result = 0;
  for (;;) {
    // Linux cancels the waiting caller, not the socket's TCP attempt. The
    // independent owner retains all context until completion or last-close.
    pthread_testcancel();
    int remaining = deadline == ~ULONGLONG{0} ? 10 : MillisUntil(deadline);
    const DWORD wait = WaitForSingleObject(event, static_cast<DWORD>(remaining > 10 ? 10 : remaining));
    if (wait == WAIT_OBJECT_0) break;
    if (wait == WAIT_FAILED) { result = -1; break; }
    if (deadline != ~ULONGLONG{0} && MonotonicMicros() >= deadline) { result = 1; break; }
  }
  return result;
}
}  // namespace

int AwaitConnect(const std::shared_ptr<ConnectOperation>& operation, bool nonblocking) {
  if (WaitConnectEvent(operation->submitted, ~ULONGLONG{0}) != 0) return errno = EIO, -1;
  if (nonblocking && operation->pending) return errno = EINPROGRESS, -1;
  if (operation->pending) {
    const int wait = WaitConnectEvent(operation->overlap.hEvent, operation->deadline);
    // Linux's SO_SNDTIMEO returns EINPROGRESS and leaves the attempt alive.
    if (wait != 0) return errno = wait > 0 ? EINPROGRESS : EIO, -1;
  }
  for (;;) {
    {
      std::lock_guard<std::mutex> lock(operation->description->error_mutex);
      FinishConnect(operation);
      if (operation->finished) {
        if (operation->description->connect_generation == operation->generation) operation->description->connect_error = 0;
        return operation->result == 0 ? 0 : (errno = operation->result, -1);
      }
    }
    pthread_testcancel();
    if (operation->deadline != ~ULONGLONG{0} && MonotonicMicros() >= operation->deadline) return errno = EINPROGRESS, -1;
    Sleep(1);  // wait for close publication without spinning on its signaled I/O event
  }
}

int Listen(Socket s, int backlog, const std::shared_ptr<SocketDescription>& description) {
  std::unique_lock<std::mutex> native_call;
  if (description) {
    native_call = std::unique_lock<std::mutex>(description->native_call_mutex);
    std::lock_guard<std::mutex> lock(description->error_mutex);
    if (description->native_closed || description->native_closing) return errno = EBADF, -1;
  }
  return Api().listen(static_cast<SOCKET>(s), backlog) == 0 ? 0 : Fail();
}

Socket Accept(Socket s, Address* peer) {
  sockaddr_storage ss;
  int len = sizeof(ss);
  SOCKET result = Api().accept(static_cast<SOCKET>(s), reinterpret_cast<sockaddr*>(&ss), &len);
  if (result == INVALID_SOCKET) return Fail(), kInvalidSocket;
  // Accepted sockets inherit the listener's (non-)blocking mode in Winsock,
  // but are always blocking on Linux.
  u_long mode = 0;
  ioctlsocket(result, FIONBIO, &mode);
  SetHandleInformation(reinterpret_cast<HANDLE>(result), HANDLE_FLAG_INHERIT, 0);
  if (peer != nullptr) FromWin(reinterpret_cast<sockaddr*>(&ss), len, peer);
  return static_cast<Socket>(result);
}

int GetSockName(Socket s, Address* address) {
  sockaddr_storage ss;
  int len = sizeof(ss);
  if (Api().getsockname(static_cast<SOCKET>(s), reinterpret_cast<sockaddr*>(&ss), &len) != 0) {
    return Fail();
  }
  FromWin(reinterpret_cast<sockaddr*>(&ss), len, address);
  return 0;
}

int GetPeerName(Socket s, Address* address) {
  sockaddr_storage ss;
  int len = sizeof(ss);
  if (Api().getpeername(static_cast<SOCKET>(s), reinterpret_cast<sockaddr*>(&ss), &len) != 0) {
    return Fail();
  }
  FromWin(reinterpret_cast<sockaddr*>(&ss), len, address);
  return 0;
}

int Shutdown(Socket s, bool read, bool write) {
  int how = read && write ? SD_BOTH : read ? SD_RECEIVE : SD_SEND;
  return Api().shutdown(static_cast<SOCKET>(s), how) == 0 ? 0 : Fail();
}

int Available(Socket s, int* bytes) {
  u_long n = 0;
  if (ioctlsocket(static_cast<SOCKET>(s), FIONREAD, &n) != 0) return Fail();
  *bytes = static_cast<int>(n);
  return 0;
}

namespace {

DWORD ToWinMsgFlags(int flags) {
  DWORD result = 0;
  if (flags & kMsgOob) result |= MSG_OOB;
  if (flags & kMsgPeek) result |= MSG_PEEK;
  if (flags & kMsgDontRoute) result |= MSG_DONTROUTE;
  if (flags & kMsgWaitAll) result |= MSG_WAITALL;
  return result;
}

constexpr size_t kMaxBuffers = 64;

bool ToWsaBufs(const Buffer* buffers, size_t count, WSABUF* out) {
  if (count > kMaxBuffers) return errno = EMSGSIZE, false;
  for (size_t i = 0; i < count; ++i) {
    if (buffers[i].len > 0xFFFFFFFFu) return errno = EINVAL, false;
    out[i].buf = static_cast<char*>(buffers[i].data);
    out[i].len = static_cast<ULONG>(buffers[i].len);
  }
  return true;
}

}  // namespace

ptrdiff_t Send(Socket s, const Buffer* buffers, size_t count, int flags, const Address* to) {
  WSABUF bufs[kMaxBuffers];
  if (!ToWsaBufs(buffers, count, bufs)) return -1;
  sockaddr_storage ss;
  int len = 0;
  if (to != nullptr && !ToWin(*to, &ss, &len)) return -1;
  DWORD sent = 0;
  if (WSASendTo(static_cast<SOCKET>(s), bufs, static_cast<DWORD>(count), &sent,
                ToWinMsgFlags(flags), to != nullptr ? reinterpret_cast<sockaddr*>(&ss) : nullptr,
                len, nullptr, nullptr) != 0) {
    return FailIo();
  }
  return static_cast<ptrdiff_t>(sent);
}

ptrdiff_t Recv(Socket s, Buffer* buffers, size_t count, int flags, Address* from,
               int* result_flags) {
  int type = 0;
  int type_len = sizeof(type);
  if (Api().getsockopt(static_cast<SOCKET>(s), SOL_SOCKET, SO_TYPE,
                       reinterpret_cast<char*>(&type), &type_len) != 0) return FailIo();
  WSAPROTOCOL_INFOW protocol = {};
  int protocol_len = sizeof(protocol);
  if (type == SOCK_DGRAM &&
      Api().getsockopt(static_cast<SOCKET>(s), SOL_SOCKET, SO_PROTOCOL_INFOW,
                       reinterpret_cast<char*>(&protocol), &protocol_len) != 0) return FailIo();
  if (type == SOCK_DGRAM && protocol.iProtocol == IPPROTO_UDP &&
      (protocol.iAddressFamily == AF_INET || protocol.iAddressFamily == AF_INET6)) {
    // Windows discards the tail on WSAEMSGSIZE. Receive an entire ordinary IP
    // datagram in ONE operation instead of FIONREAD/peek followed by consume:
    // another receiver (including a duplicated handle) can take the head packet
    // between those operations. No endpoint lock or cached packet is needed.
    // 65536 exceeds the maximum UDP payload on Windows for both IP families.
    if (count > 1024) return errno = EMSGSIZE, -1;
    for (size_t i = 0; i < count; ++i) {
      if (buffers[i].len > PTRDIFF_MAX) return errno = EINVAL, -1;
    }
    std::unique_ptr<char, decltype(&std::free)> packet(
        static_cast<char*>(std::malloc(65536)), &std::free);
    if (!packet) return errno = ENOMEM, -1;
    WSABUF whole = {65536, packet.get()};
    sockaddr_storage peer = {};
    int peer_len = sizeof(peer);
    DWORD received = 0;
    // Actual Linux UDP receives ignore OOB and WAITALL; Winsock may reject them.
    DWORD win_flags = ToWinMsgFlags(flags & ~(kMsgWaitAll | kMsgOob));
    if (WSARecvFrom(static_cast<SOCKET>(s), &whole, 1, &received, &win_flags,
                    from != nullptr ? reinterpret_cast<sockaddr*>(&peer) : nullptr,
                    from != nullptr ? &peer_len : nullptr, nullptr, nullptr) != 0) return FailIo();
    size_t copied = 0;
    for (size_t i = 0; i < count; ++i) {
      size_t n = buffers[i].len < received - copied ? buffers[i].len : received - copied;
      if (!CopyToUser(buffers[i].data, packet.get() + copied, n)) return errno = EFAULT, -1;
      copied += n;
    }
    if (from != nullptr) FromWin(reinterpret_cast<sockaddr*>(&peer), peer_len, from);
    if (result_flags != nullptr) *result_flags = copied < received ? kMsgTrunc : 0;
    return static_cast<ptrdiff_t>((flags & kMsgTrunc) ? received : copied);
  }
  WSABUF bufs[kMaxBuffers];
  if (!ToWsaBufs(buffers, count, bufs)) return -1;
  sockaddr_storage ss;
  int len = sizeof(ss);
  DWORD received = 0;
  DWORD win_flags = ToWinMsgFlags(flags);
  if (WSARecvFrom(static_cast<SOCKET>(s), bufs, static_cast<DWORD>(count), &received, &win_flags,
                  from != nullptr ? reinterpret_cast<sockaddr*>(&ss) : nullptr,
                  from != nullptr ? &len : nullptr, nullptr, nullptr) != 0) {
    if (WSAGetLastError() != WSAEMSGSIZE) return FailIo();
    // A datagram larger than the buffers: Linux returns the part that fit.
    received = 0;
    for (size_t i = 0; i < count; ++i) received += bufs[i].len;
  }
  if (from != nullptr) FromWin(reinterpret_cast<sockaddr*>(&ss), len, from);
  if (result_flags != nullptr) *result_flags = 0;
  return static_cast<ptrdiff_t>(received);
}

namespace {

void ReleaseDatagramCaller(void* raw) {
  // The normal caller owns this SocketFd reference. Deferred cancellation in
  // the pinned -fno-exceptions build need not run its C++ destructor.
  static_cast<std::shared_ptr<SocketDescription>*>(raw)->reset();
}

ptrdiff_t DatagramIo(Socket s, Buffer* buffers, size_t count, int flags,
                     Address* from, const Address* to, bool writing,
                     std::shared_ptr<SocketDescription>& description, bool dont_wait,
                     int* result_flags) {
  const bool nonblocking = dont_wait || description->nonblocking.load();
  int timeout = 0;
  {
    std::lock_guard<std::mutex> lock(description->error_mutex);
    if (description->native_closed || description->native_closing) return errno = EBADF, -1;
    if (GetOption(s, writing ? Option::kSndTimeo : Option::kRcvTimeo, &timeout) < 0) return -1;
  }
  const ULONGLONG deadline = timeout > 0 ? DeadlineAfter(timeout) : ~ULONGLONG{0};
  ptrdiff_t result = -1;
  int saved = 0;
  pthread_cleanup_push(ReleaseDatagramCaller, &description);
  for (;;) {
    pthread_testcancel();  // no lock or temporary owning poll snapshot survives
    {
      std::lock_guard<std::mutex> lock(description->error_mutex);
      if (description->native_closed || description->native_closing) {
        result = -1; saved = EBADF;
      } else {
        // Permanent native nonblocking mode bounds this call even if another
        // reader consumed readiness or an alias changed logical O_NONBLOCK.
        result = writing ? Send(s, buffers, count, flags, to)
                         : Recv(s, buffers, count, flags, from, result_flags);
        saved = errno;
      }
    }
    if (result >= 0 || saved != EAGAIN || nonblocking) break;
    int wait = 10;
    if (deadline != ~ULONGLONG{0}) {
      const int remaining = MillisUntil(deadline);
      if (remaining == 0) { saved = EAGAIN; break; }
      if (remaining < wait) wait = remaining;
    }
    {
      PollEntry entry{s, writing ? kPollOut : kPollIn, 0, description};
      if (Poll(&entry, 1, wait) < 0) { saved = errno; break; }
    }  // release the snapshot before the next cancellation point
  }
  pthread_cleanup_pop(0);
  errno = saved;
  return result;
}

}  // namespace

ptrdiff_t SendOwned(Socket s, const Buffer* buffers, size_t count, int flags, const Address* to,
                    std::shared_ptr<SocketDescription>& description, bool dont_wait) {
  if (!description->native_nonblocking_datagram) return Send(s, buffers, count, flags, to);
  return DatagramIo(s, const_cast<Buffer*>(buffers), count, flags, nullptr, to, true,
                    description, dont_wait, nullptr);
}

ptrdiff_t RecvOwned(Socket s, Buffer* buffers, size_t count, int flags, Address* from,
                    std::shared_ptr<SocketDescription>& description, bool dont_wait,
                    int* result_flags) {
  if (!description->native_nonblocking_datagram) return Recv(s, buffers, count, flags, from, result_flags);
  return DatagramIo(s, buffers, count, flags, from, nullptr, false, description, dont_wait, result_flags);
}

namespace {

bool ToWinOption(Option option, int* level, int* name) {
  switch (option) {
    case Option::kReuseAddr: *level = SOL_SOCKET; *name = SO_REUSEADDR; return true;
    case Option::kKeepAlive: *level = SOL_SOCKET; *name = SO_KEEPALIVE; return true;
    case Option::kBroadcast: *level = SOL_SOCKET; *name = SO_BROADCAST; return true;
    case Option::kOobInline: *level = SOL_SOCKET; *name = SO_OOBINLINE; return true;
    case Option::kRcvBuf: *level = SOL_SOCKET; *name = SO_RCVBUF; return true;
    case Option::kSndBuf: *level = SOL_SOCKET; *name = SO_SNDBUF; return true;
    case Option::kRcvLowat: *level = SOL_SOCKET; *name = SO_RCVLOWAT; return true;
    case Option::kSndLowat: *level = SOL_SOCKET; *name = SO_SNDLOWAT; return true;
    case Option::kRcvTimeo: *level = SOL_SOCKET; *name = SO_RCVTIMEO; return true;
    case Option::kSndTimeo: *level = SOL_SOCKET; *name = SO_SNDTIMEO; return true;
    case Option::kError: *level = SOL_SOCKET; *name = SO_ERROR; return true;
    case Option::kType: *level = SOL_SOCKET; *name = SO_TYPE; return true;
    case Option::kAcceptConn: *level = SOL_SOCKET; *name = SO_ACCEPTCONN; return true;
    case Option::kDontRoute: *level = SOL_SOCKET; *name = SO_DONTROUTE; return true;
    case Option::kTcpNoDelay: *level = IPPROTO_TCP; *name = TCP_NODELAY; return true;
    case Option::kTcpKeepIdle: *level = IPPROTO_TCP; *name = TCP_KEEPIDLE; return true;
    case Option::kTcpKeepIntvl: *level = IPPROTO_TCP; *name = TCP_KEEPINTVL; return true;
    case Option::kTcpKeepCnt: *level = IPPROTO_TCP; *name = TCP_KEEPCNT; return true;
    case Option::kIpTos: *level = IPPROTO_IP; *name = IP_TOS; return true;
    case Option::kIpTtl: *level = IPPROTO_IP; *name = IP_TTL; return true;
    case Option::kIpMulticastIf: *level = IPPROTO_IP; *name = IP_MULTICAST_IF; return true;
    case Option::kIpMulticastTtl: *level = IPPROTO_IP; *name = IP_MULTICAST_TTL; return true;
    case Option::kIpMulticastLoop: *level = IPPROTO_IP; *name = IP_MULTICAST_LOOP; return true;
    case Option::kIpv6UnicastHops: *level = IPPROTO_IPV6; *name = IPV6_UNICAST_HOPS; return true;
    case Option::kIpv6MulticastIf: *level = IPPROTO_IPV6; *name = IPV6_MULTICAST_IF; return true;
    case Option::kIpv6MulticastHops: *level = IPPROTO_IPV6; *name = IPV6_MULTICAST_HOPS; return true;
    case Option::kIpv6MulticastLoop: *level = IPPROTO_IPV6; *name = IPV6_MULTICAST_LOOP; return true;
    case Option::kIpv6V6Only: *level = IPPROTO_IPV6; *name = IPV6_V6ONLY; return true;
    case Option::kIpv6TClass: *level = IPPROTO_IPV6; *name = IPV6_TCLASS; return true;
  }
  return false;
}

}  // namespace

int GetOption(Socket s, Option option, int* value) {
  int level, name;
  if (!ToWinOption(option, &level, &name)) return errno = ENOPROTOOPT, -1;
  DWORD result = 0;
  int len = sizeof(result);
  if (Api().getsockopt(static_cast<SOCKET>(s), level, name, reinterpret_cast<char*>(&result), &len) != 0) {
    return Fail();
  }
  *value = static_cast<int>(result);
  // These two hold Winsock constants.
  if (option == Option::kError && result != 0) *value = ErrnoFromWsa(static_cast<int>(result));
  if (option == Option::kType) *value = static_cast<int>(FromWinType(static_cast<int>(result)));
  return 0;
}

int SetOption(Socket s, Option option, int value) {
  int level, name;
  if (!ToWinOption(option, &level, &name)) return errno = ENOPROTOOPT, -1;
  DWORD v = static_cast<DWORD>(value);
  return Api().setsockopt(static_cast<SOCKET>(s), level, name, reinterpret_cast<const char*>(&v),
                    sizeof(v)) == 0 ? 0 : Fail();
}

int GetLinger(Socket s, bool* on, int* seconds) {
  linger l = {};
  int len = sizeof(l);
  if (Api().getsockopt(static_cast<SOCKET>(s), SOL_SOCKET, SO_LINGER, reinterpret_cast<char*>(&l),
                 &len) != 0) {
    return Fail();
  }
  *on = l.l_onoff != 0;
  *seconds = l.l_linger;
  return 0;
}

int SetLinger(Socket s, bool on, int seconds) {
  if (seconds < 0 || seconds > 0xFFFF) return errno = EINVAL, -1;
  linger l = {static_cast<u_short>(on ? 1 : 0), static_cast<u_short>(seconds)};
  return Api().setsockopt(static_cast<SOCKET>(s), SOL_SOCKET, SO_LINGER,
                    reinterpret_cast<const char*>(&l), sizeof(l)) == 0 ? 0 : Fail();
}

int Membership(Socket s, bool join, Family family, const uint8_t* group, uint32_t ifindex,
               const uint8_t* source) {
  if (family != Family::kInet && family != Family::kInet6) return errno = EAFNOSUPPORT, -1;
  auto fill = [family](const uint8_t* address, SOCKADDR_STORAGE* out) {
    memset(out, 0, sizeof(*out));
    if (family == Family::kInet) {
      auto* in = reinterpret_cast<sockaddr_in*>(out);
      in->sin_family = AF_INET;
      memcpy(&in->sin_addr, address, 4);
    } else {
      auto* in6 = reinterpret_cast<sockaddr_in6*>(out);
      in6->sin6_family = AF_INET6;
      memcpy(&in6->sin6_addr, address, 16);
    }
  };
  int level = family == Family::kInet ? IPPROTO_IP : IPPROTO_IPV6;
  int rc;
  if (source == nullptr) {
    GROUP_REQ req = {};
    req.gr_interface = ifindex;
    fill(group, &req.gr_group);
    rc = Api().setsockopt(static_cast<SOCKET>(s), level, join ? MCAST_JOIN_GROUP : MCAST_LEAVE_GROUP,
                    reinterpret_cast<const char*>(&req), sizeof(req));
  } else {
    GROUP_SOURCE_REQ req = {};
    req.gsr_interface = ifindex;
    fill(group, &req.gsr_group);
    fill(source, &req.gsr_source);
    rc = Api().setsockopt(static_cast<SOCKET>(s), level,
                    join ? MCAST_JOIN_SOURCE_GROUP : MCAST_LEAVE_SOURCE_GROUP,
                    reinterpret_cast<const char*>(&req), sizeof(req));
  }
  return rc == 0 ? 0 : Fail();
}

Provider ClassifyProvider(Socket s) {
  WSAPROTOCOL_INFOW info{}; int length = sizeof(info);
  static constexpr GUID native = {0xe70f1aa0, 0xab8b, 0x11cf, {0x8c, 0xa3, 0x00, 0x80, 0x5f, 0x48, 0xa1, 0x92}};
  if (Api().getsockopt(static_cast<SOCKET>(s), SOL_SOCKET, SO_PROTOCOL_INFOW,
                      reinterpret_cast<char*>(&info), &length) != 0 || length != static_cast<int>(sizeof(info)) || info.iAddressFamily != AF_INET ||
      info.ProtocolChain.ChainLen != BASE_PROTOCOL || !IsEqualGUID(info.ProviderId, native)) return Provider::kOther;
  if (info.iSocketType == SOCK_STREAM && info.iProtocol == IPPROTO_TCP) return Provider::kNativeIpv4Tcp;
  if (info.iSocketType == SOCK_DGRAM && info.iProtocol == IPPROTO_UDP) return Provider::kNativeIpv4Udp;
  return Provider::kOther;
}

int Poll(PollEntry* entries, size_t count, int timeout_ms, NativePollObserver observer, void* context) {
  const ULONGLONG sample_started = GetTickCount64();
  std::vector<WSAPOLLFD> fds(count);
  std::vector<bool> lifecycle_hup(count, false);
  std::vector<bool> owned_pending(count, false), closed(count, false);
  bool has_lifecycle_hup = false;
  bool has_owned_pending = false;
  for (size_t i = 0; i < count; ++i) {
    if (entries[i].description) {
      RefreshConnect(entries[i].description);
      std::lock_guard<std::mutex> lock(entries[i].description->error_mutex);
      owned_pending[i] = entries[i].description->connect_operation != nullptr;
      closed[i] = entries[i].description->native_closed || entries[i].description->native_closing;
      has_owned_pending |= owned_pending[i];
    }
    fds[i].fd = static_cast<SOCKET>(entries[i].socket);
    // Winsock rejects its POLLPRI, but supports POLLRDBAND for TCP OOB.
    // Keep this distinct from normal-read readiness and from connect errors.
    fds[i].events = static_cast<SHORT>(((entries[i].events & kPollIn) != 0 ? POLLRDNORM : 0) |
                                       ((entries[i].events & kPollOut) != 0 ? POLLWRNORM : 0) |
                                       ((entries[i].events & kPollPri) != 0 ? POLLRDBAND : 0));
    // Native UDP reports an unread reset as ordinary readability. Observe that
    // channel even with no read interest: POSIX errors are unconditional.
    if (entries[i].description && entries[i].description->native_nonblocking_datagram)
      fds[i].events |= POLLRDNORM;
    if (closed[i]) { fds[i].revents = POLLNVAL; continue; }
    // Windows gates failed-connect ERR/HUP on normal interest, even though
    // those errors are unconditional in Linux poll. Arm write interest only
    // for connecting, non-listening streams. Connected writable sockets and
    // unconnected datagrams must not cause an unrequested-write busy loop.
    if ((fds[i].events & POLLWRNORM) == 0 || entries[i].description) {
      int type = 0, accepting = 0, length = sizeof(type);
      if (Api().getsockopt(fds[i].fd, SOL_SOCKET, SO_TYPE,
                           reinterpret_cast<char*>(&type), &length) == 0 && type == SOCK_STREAM) {
        length = sizeof(accepting);
        if (Api().getsockopt(fds[i].fd, SOL_SOCKET, SO_ACCEPTCONN,
                             reinterpret_cast<char*>(&accepting), &length) == 0 && !accepting) {
          const ConnectState state = entries[i].description ? entries[i].description->state.load() : ConnectState::kConnecting;
          sockaddr_storage peer{}; length = sizeof(peer);
          const int peer_result = Api().getpeername(fds[i].fd, reinterpret_cast<sockaddr*>(&peer), &length);
          if (peer_result == 0) {
            if (entries[i].description) entries[i].description->state.store(ConnectState::kConnected);
          } else if (WSAGetLastError() == WSAENOTCONN) {
            if (state != ConnectState::kConnecting) {
              // Linux's unconnected TCP_CLOSE socket has HUP. Native Winsock
              // reports neither that state nor a connecting socket as HUP.
              // The shared lifecycle distinguishes them without SO_ERROR.
              lifecycle_hup[i] = true;
              has_lifecycle_hup = true;
            }
            // Completed failures retain write interest so unread native ERR
            // survives until the caller consumes SO_ERROR, alongside HUP.
            if (state != ConnectState::kInitial) fds[i].events |= POLLWRNORM;
          }
        }
      }
    }
    fds[i].revents = 0;
  }
  // Band-only WSAPoll never wakes for failed ConnectEx. Sample its owned
  // completion on the same bounded slices already used for mixed poll sources.
  const int wait = has_lifecycle_hup ? 0 : has_owned_pending && (timeout_ms < 0 || timeout_ms > 10) ? 10 : timeout_ms;
  // An owned pending connection has no usable connected context yet. Its
  // completion is the readiness source; a native write wake could precede
  // context publication and otherwise spin or expose a premature success.
  std::vector<WSAPOLLFD> active;
  std::vector<size_t> active_index;
  for (size_t i = 0; i < count; ++i) {
    if (!owned_pending[i] && !closed[i]) { active.push_back(fds[i]); active_index.push_back(i); }
  }
  if (!active.empty()) {
    if (observer) observer(true, 0, 0, context);  // finalized real native inputs; no locks
    int sample_wait = wait;
    for (;;) {
      const int result = WSAPoll(active.data(), static_cast<ULONG>(active.size()), sample_wait);
      const int native_error = result == SOCKET_ERROR ? WSAGetLastError() : 0;
      if (observer) { observer(false, result, native_error, context); observer = nullptr; }
      if (result != SOCKET_ERROR) break;
      if (native_error != WSAEINVAL && native_error != WSAENOTSOCK) return errno = ErrnoFromWsa(native_error), -1;
      // An invalid sentinel or a positively closed SOCKET can produce global
      // WSAEINVAL/WSAENOTSOCK rather than counted POLLNVAL. Recover only endpoints
      // positively retired by their original description; arbitrary native
      // errors retain their original failure. Every retry removes an entry,
      // takes no further wait and never extends the caller's finite deadline.
      std::vector<WSAPOLLFD> remaining;
      std::vector<size_t> remaining_index;
      for (size_t k = 0; k < active.size(); ++k) {
        const size_t i = active_index[k];
        bool retired = false;
        if (entries[i].description) {
          std::lock_guard<std::mutex> lock(entries[i].description->error_mutex);
          retired = entries[i].description->native_closed || entries[i].description->native_closing;
        }
        if (retired) { fds[i].revents = POLLNVAL; closed[i] = true; }
        else { remaining.push_back(active[k]); remaining_index.push_back(i); }
      }
      if (remaining.size() == active.size()) return errno = ErrnoFromWsa(native_error), -1;
      active = std::move(remaining); active_index = std::move(remaining_index);
      if (active.empty()) break;
      sample_wait = 0;
    }
    for (size_t k = 0; k < active.size(); ++k) fds[active_index[k]].revents = active[k].revents;
  } else if (wait > 0) Sleep(static_cast<DWORD>(wait));
  int ready = 0;
  bool suppressed_datagram_read = false;
  for (size_t i = 0; i < count; ++i) {
    SHORT r = fds[i].revents;
    if (entries[i].description) {
      RefreshConnect(entries[i].description);
      auto& description = *entries[i].description;
      std::lock_guard<std::mutex> lock(description.error_mutex);
      if (description.native_closed || description.native_closing) r = POLLNVAL;
      else if (description.native_nonblocking_datagram && (r & POLLRDNORM)) {
        // A non-overlapped MSG_PEEK is supported here because registration
        // fixed native UDP mode permanently nonblocking. The short closure
        // transaction prevents a recycled numeric SOCKET from being queried.
        // It never changes alias mode, reads SO_ERROR, or consumes a datagram.
        char byte = 0;
        Buffer buffer{&byte, 1};
        const int saved = errno;
        const ptrdiff_t peeked = Recv(entries[i].socket, &buffer, 1, kMsgPeek, nullptr);
        const int error = errno;
        errno = saved;
        if (peeked < 0 && error == ECONNRESET) r = static_cast<SHORT>((r & ~POLLRDNORM) | POLLERR);
        else if (peeked < 0 && error == EAGAIN) r = static_cast<SHORT>(r & ~POLLRDNORM);
        if ((r & POLLRDNORM) && !(entries[i].events & kPollIn)) suppressed_datagram_read = true;
      }
      else if (description.provider == Provider::kNativeIpv4Tcp && description.refusal_generation != 0 &&
               description.refusal_generation == description.connect_generation &&
               description.connect_result == ECONNREFUSED &&
               !description.connect_operation && !description.legacy_connect_active &&
               description.state.load() == ConnectState::kFailed) {
        // This generation completed without ever establishing a transport.
        // Its raw ERR|HUP|WRNORM residue is distinct from its caller token.
        // A new connect invalidates this classification before native submit;
        // successful transports and all other native masks remain untouched.
        if (r == (POLLERR | POLLHUP | POLLWRNORM)) r &= ~POLLERR;
        r |= POLLHUP;
        if (description.connect_error != 0) r |= POLLERR;
      } else if ((r & (POLLERR | POLLHUP)) && !description.connect_operation) {
        ConnectState expected = ConnectState::kConnecting;
        description.state.compare_exchange_strong(expected, ConnectState::kFailed);
      }
    }
    if (lifecycle_hup[i]) r |= POLLHUP;
    int out = 0;
    if ((r & POLLRDNORM) && (entries[i].events & kPollIn)) out |= kPollIn;
    if ((r & POLLRDBAND) && (entries[i].events & kPollPri)) out |= kPollPri;
    if ((r & POLLWRNORM) && (entries[i].events & kPollOut)) out |= kPollOut;
    if (r & POLLERR) out |= kPollErr;
    if (r & POLLHUP) out |= kPollHup;
    if (r & POLLNVAL) out |= kPollNval;
    // At end of stream Linux reports the socket readable (read returns 0);
    // WSAPoll may report only POLLHUP.
    if ((out & kPollHup) != 0 && (entries[i].events & kPollIn) != 0) out |= kPollIn;
    entries[i].revents = out;
    if (out != 0) ++ready;
  }
  if (ready == 0 && suppressed_datagram_read && timeout_ms != 0) {
    // Queued unrequested data must not spin, delay a different requested fd
    // for the whole timeout, or return an old zero after sleeping past a new
    // event. Re-sample every bounded slice, including at the final deadline.
    for (;;) {
      const ULONGLONG elapsed = GetTickCount64() - sample_started;
      DWORD pause = 10;
      if (timeout_ms >= 0) {
        const ULONGLONG budget = static_cast<ULONGLONG>(timeout_ms);
        if (elapsed >= budget) return Poll(entries, count, 0, nullptr, nullptr);
        if (budget - elapsed < pause) pause = static_cast<DWORD>(budget - elapsed);
      }
      Sleep(pause);
      const int sampled = Poll(entries, count, 0, nullptr, nullptr);
      if (sampled != 0) return sampled;
      if (timeout_ms >= 0 && GetTickCount64() - sample_started >= static_cast<ULONGLONG>(timeout_ms)) return 0;
    }
  }
  return ready;
}

// --- names --------------------------------------------------------------------

namespace {

static_assert(sizeof(void*) == 8 && sizeof(hostent) == 32);
static_assert(offsetof(hostent, h_addrtype) == 16 && sizeof(hostent::h_addrtype) == 2);
static_assert(offsetof(hostent, h_length) == 18 && sizeof(hostent::h_length) == 2);
static_assert(offsetof(hostent, h_addr_list) == 24);
static_assert(sizeof(servent) == 32 && offsetof(servent, s_proto) == 16);
static_assert(offsetof(servent, s_port) == 24 && sizeof(servent::s_port) == 2);

HostError FromLegacyError(int error) {
  switch (error) {
    case WSAHOST_NOT_FOUND: return HostError::kNotFound;
    case WSATRY_AGAIN: return HostError::kAgain;
    case WSANO_RECOVERY: return HostError::kNoRecovery;
    case WSANO_DATA: return HostError::kNoData;
    default: errno = ErrnoFromWsa(error); return HostError::kInternal;
  }
}

size_t LegacyCount(char* const* values) {
  size_t count = 0;
  if (values != nullptr) while (values[count] != nullptr) ++count;
  return count;
}

bool LegacySize(size_t amount, size_t* total) {
  if (amount > SIZE_MAX - *total) { errno = EOVERFLOW; return false; }
  *total += amount;
  return true;
}

char* CopyLegacyString(const char* source, char** cursor) {
  char* result = *cursor;
  const size_t length = strlen(source) + 1;
  memcpy(result, source, length);
  *cursor += length;
  return result;
}

HostError CopyLegacyHost(const hostent* source, LegacyHost* result) {
  if (source == nullptr) return FromLegacyError(WSAGetLastError());
  result->family = FromWinFamily(source->h_addrtype);
  const int length = result->family == Family::kInet ? 4
      : result->family == Family::kInet6 ? 16 : 0;
  if (length == 0 || source->h_length != length || source->h_name == nullptr
      || source->h_addr_list == nullptr) {
    errno = EIO;
    return HostError::kInternal;
  }
  const size_t aliases = LegacyCount(source->h_aliases);
  const size_t addresses = LegacyCount(source->h_addr_list);
  if (addresses == 0) return HostError::kNoData;
  if (addresses > SIZE_MAX / sizeof(char*) - 2
      || aliases > SIZE_MAX / sizeof(char*) - 2 - addresses
      || addresses > SIZE_MAX / static_cast<size_t>(length)) {
    errno = EOVERFLOW;
    return HostError::kInternal;
  }
  size_t bytes = (aliases + addresses + 2) * sizeof(char*);
  if (!LegacySize(strlen(source->h_name) + 1, &bytes)
      || !LegacySize(addresses * length, &bytes)) return HostError::kInternal;
  for (size_t i = 0; i < aliases; ++i) {
    if (!LegacySize(strlen(source->h_aliases[i]) + 1, &bytes)) return HostError::kInternal;
  }
  result->storage.reset(static_cast<char*>(malloc(bytes)));
  if (!result->storage) { errno = ENOMEM; return HostError::kInternal; }
  result->aliases = reinterpret_cast<char**>(result->storage.get());
  result->addresses = result->aliases + aliases + 1;
  char* cursor = reinterpret_cast<char*>(result->addresses + addresses + 1);
  result->name = CopyLegacyString(source->h_name, &cursor);
  for (size_t i = 0; i < aliases; ++i) {
    result->aliases[i] = CopyLegacyString(source->h_aliases[i], &cursor);
  }
  result->aliases[aliases] = nullptr;
  for (size_t i = 0; i < addresses; ++i) {
    result->addresses[i] = cursor;
    memcpy(cursor, source->h_addr_list[i], length);
    cursor += length;
  }
  result->addresses[addresses] = nullptr;
  result->length = length;
  return HostError::kOk;
}

bool CopyLegacyService(const servent* source, LegacyService* result) {
  if (source == nullptr) return false;
  if (source->s_name == nullptr || source->s_proto == nullptr) return false;
  const size_t aliases = LegacyCount(source->s_aliases);
  if (aliases > SIZE_MAX / sizeof(char*) - 1) { errno = EOVERFLOW; return false; }
  size_t bytes = (aliases + 1) * sizeof(char*);
  if (!LegacySize(strlen(source->s_name) + 1, &bytes)
      || !LegacySize(strlen(source->s_proto) + 1, &bytes)) return false;
  for (size_t i = 0; i < aliases; ++i) {
    if (!LegacySize(strlen(source->s_aliases[i]) + 1, &bytes)) return false;
  }
  result->storage.reset(static_cast<char*>(malloc(bytes)));
  if (!result->storage) { errno = ENOMEM; return false; }
  result->aliases = reinterpret_cast<char**>(result->storage.get());
  char* cursor = reinterpret_cast<char*>(result->aliases + aliases + 1);
  result->name = CopyLegacyString(source->s_name, &cursor);
  result->protocol = CopyLegacyString(source->s_proto, &cursor);
  result->port = static_cast<uint16_t>(source->s_port);
  for (size_t i = 0; i < aliases; ++i) {
    result->aliases[i] = CopyLegacyString(source->s_aliases[i], &cursor);
  }
  result->aliases[aliases] = nullptr;
  return true;
}

GaiError FromWinGai(int error) {
  switch (error) {
    case 0: return GaiError::kOk;
    case WSATRY_AGAIN: return GaiError::kAgain;
    case WSAEINVAL: return GaiError::kBadFlags;
    case WSANO_RECOVERY: return GaiError::kFail;
    case WSAEAFNOSUPPORT: return GaiError::kFamily;
    case WSA_NOT_ENOUGH_MEMORY: return GaiError::kMemory;
    case WSAHOST_NOT_FOUND: return GaiError::kNoName;
    case WSANO_DATA: return GaiError::kNoData;
    case WSATYPE_NOT_FOUND: return GaiError::kService;
    case WSAESOCKTNOSUPPORT: return GaiError::kSocktype;
    default: errno = ErrnoFromWsa(error); return GaiError::kSystem;
  }
}

int ToWinAiFlags(int flags) {
  int result = 0;
  if (flags & kAiPassive) result |= AI_PASSIVE;
  if (flags & kAiCanonName) result |= AI_CANONNAME;
  if (flags & kAiNumericHost) result |= AI_NUMERICHOST;
  if (flags & kAiNumericServ) result |= AI_NUMERICSERV;
  if (flags & kAiV4Mapped) result |= AI_V4MAPPED;
  if (flags & kAiAll) result |= AI_ALL;
  if (flags & kAiAddrConfig) result |= AI_ADDRCONFIG;
  return result;
}

// UTF-16 to UTF-8 into a caller's buffer; false if it does not fit.
bool NarrowInto(const wchar_t* utf16, char* out, size_t out_len) {
  int n = WideCharToMultiByte(CP_UTF8, 0, utf16, -1, out, static_cast<int>(out_len), nullptr,
                              nullptr);
  return n > 0;
}

}  // namespace

HostError LegacyHostByName(const char* name, LegacyHost* result) {
  if (!Startup()) return HostError::kInternal;
  return CopyLegacyHost(Api().gethostbyname(name), result);
}

HostError LegacyHostByAddress(const void* address, size_t length, Family family,
                             LegacyHost* result) {
  if (!Startup()) return HostError::kInternal;
  return CopyLegacyHost(Api().gethostbyaddr(static_cast<const char*>(address),
      static_cast<int>(length), ToWinFamily(family)), result);
}

bool LegacyServiceByName(const char* name, const char* protocol, LegacyService* result) {
  if (!Startup()) return false;
  return CopyLegacyService(Api().getservbyname(name, protocol), result);
}

bool LegacyServiceByPort(int port, const char* protocol, LegacyService* result) {
  if (!Startup()) return false;
  return CopyLegacyService(Api().getservbyport(port, protocol), result);
}

GaiError ResolveDefault(const char* node, const char* service, const AddrInfo* hints,
                       std::vector<AddrInfo>* results) {
  if (!Startup()) return GaiError::kSystem;
  ADDRINFOW win_hints = {};
  if (hints != nullptr) {
    int family = ToWinFamily(hints->family);
    if (family < 0) return GaiError::kFamily;
    win_hints.ai_flags = ToWinAiFlags(hints->flags);
    win_hints.ai_family = family;
    win_hints.ai_socktype = hints->type == Type::kOther ? 0 : ToWinType(hints->type);
    win_hints.ai_protocol = hints->protocol;
  }
  std::wstring wnode = node != nullptr ? Widen(node) : std::wstring();
  std::wstring wservice = service != nullptr ? Widen(service) : std::wstring();
  ADDRINFOW* list = nullptr;
  int rc = GetAddrInfoW(node != nullptr ? wnode.c_str() : nullptr,
                        service != nullptr ? wservice.c_str() : nullptr,
                        hints != nullptr ? &win_hints : nullptr, &list);
  if (rc != 0) return FromWinGai(rc);
  for (ADDRINFOW* ai = list; ai != nullptr; ai = ai->ai_next) {
    AddrInfo info;
    info.flags = hints != nullptr ? hints->flags : 0;
    info.family = FromWinFamily(ai->ai_family);
    info.type = FromWinType(ai->ai_socktype);
    info.protocol = ai->ai_protocol;
    if (info.family == Family::kOther || ai->ai_addr == nullptr) continue;
    FromWin(ai->ai_addr, static_cast<int>(ai->ai_addrlen), &info.address);
    if (ai->ai_canonname != nullptr) {
      char name[NI_MAXHOST];
      if (NarrowInto(ai->ai_canonname, name, sizeof(name))) info.canonname = name;
    }
    results->push_back(std::move(info));
  }
  FreeAddrInfoW(list);
  return results->empty() ? GaiError::kNoData : GaiError::kOk;
}

// mingw-w64 has DNS_QUERY_RESULT but omits DNS_QUERY_REQUEST/DnsQueryEx.
// This is Windows' version-1 request layout, with synchronous completion.
struct InterfaceDnsRequest {
  ULONG version;
  PCWSTR name;
  WORD type;
  ULONG64 options;
  void* servers;
  ULONG ifindex;
  PDNS_QUERY_COMPLETION_ROUTINE completion;
  void* context;
};

GaiError Resolve(const char* node, const char* service, const AddrInfo* hints,
                 std::vector<AddrInfo>* results, uint32_t ifindex) {
  if (ifindex == UINT32_MAX) ifindex = ProcessInterface();
  if (ifindex != 0 && ValidateInterface(ifindex) != 0) return GaiError::kSystem;
  unsigned char numeric[16];
  if (ifindex == 0 || node == nullptr || (hints != nullptr && (hints->flags & kAiNumericHost)) ||
      Pton(Family::kInet, node, numeric) == 1 || Pton(Family::kInet6, node, numeric) == 1) {
    return ResolveDefault(node, service, hints, results);
  }
  using Query = DNS_STATUS (WINAPI*)(InterfaceDnsRequest*, PDNS_QUERY_RESULT, void*);
  struct DnsApi {
    Query query;
    decltype(&DnsFree) release;
  };
  static const DnsApi dns = [] {
    HMODULE module = LoadLibraryExW(L"dnsapi.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    DnsApi api{};
    LoadProc(module, "DnsQueryEx", &api.query);
    LoadProc(module, "DnsFree", &api.release);
    return api;
  }();
  std::wstring name = Widen(node);
  Family family = hints != nullptr ? hints->family : Family::kUnspec;
  if (family != Family::kUnspec && family != Family::kInet && family != Family::kInet6) {
    return GaiError::kFamily;
  }
  bool mapped = hints != nullptr && (hints->flags & kAiV4Mapped);
  const WORD types[] = {DNS_TYPE_AAAA, DNS_TYPE_A};
  for (WORD type : types) {
    if (type == DNS_TYPE_AAAA && family == Family::kInet) continue;
    if (type == DNS_TYPE_A && family == Family::kInet6 && !mapped) continue;
    if (type == DNS_TYPE_A && family == Family::kInet6 && !results->empty() &&
        !(hints->flags & kAiAll)) continue;
    InterfaceDnsRequest request{DNS_QUERY_REQUEST_VERSION1, name.c_str(), type,
                               DNS_QUERY_BYPASS_CACHE, nullptr, ifindex, nullptr, nullptr};
    DNS_QUERY_RESULT answer{};
    answer.Version = DNS_QUERY_RESULTS_VERSION1;
    DNS_STATUS status = dns.query(&request, &answer, nullptr);
    if (status == ERROR_SUCCESS) {
      for (auto* record = answer.pQueryRecords; record != nullptr; record = record->pNext) {
        if (record->wType != type) continue;
        char address[64];
        const void* bytes = type == DNS_TYPE_A ? static_cast<const void*>(&record->Data.A.IpAddress)
                                              : static_cast<const void*>(&record->Data.AAAA.Ip6Address);
        if (!Ntop(type == DNS_TYPE_A ? Family::kInet : Family::kInet6, bytes,
                  address, sizeof(address))) continue;
        AddrInfo numeric_hints = hints != nullptr ? *hints : AddrInfo();
        numeric_hints.flags |= kAiNumericHost;
        ResolveDefault(address, service, &numeric_hints, results);
      }
    }
    if (answer.pQueryRecords != nullptr) dns.release(answer.pQueryRecords, DnsFreeRecordList);
  }
  return results->empty() ? GaiError::kNoName : GaiError::kOk;
}

GaiError ReverseResolve(const Address& address, char* host, size_t host_len, char* serv,
                     size_t serv_len, int flags) {
  if (!Startup()) return GaiError::kSystem;
  sockaddr_storage ss;
  int len;
  if (!ToWin(address, &ss, &len)) return GaiError::kFamily;
  int win_flags = 0;
  if (flags & kNiNumericHost) win_flags |= NI_NUMERICHOST;
  if (flags & kNiNumericServ) win_flags |= NI_NUMERICSERV;
  if (flags & kNiNoFqdn) win_flags |= NI_NOFQDN;
  if (flags & kNiNameReqd) win_flags |= NI_NAMEREQD;
  if (flags & kNiDgram) win_flags |= NI_DGRAM;
  wchar_t whost[NI_MAXHOST];
  wchar_t wserv[NI_MAXSERV];
  int rc = GetNameInfoW(reinterpret_cast<sockaddr*>(&ss), len, host != nullptr ? whost : nullptr,
                        host != nullptr ? NI_MAXHOST : 0, serv != nullptr ? wserv : nullptr,
                        serv != nullptr ? NI_MAXSERV : 0, win_flags);
  if (rc != 0) return FromWinGai(WSAGetLastError());
  if (host != nullptr && !NarrowInto(whost, host, host_len)) return GaiError::kOverflow;
  if (serv != nullptr && !NarrowInto(wserv, serv, serv_len)) return GaiError::kOverflow;
  return GaiError::kOk;
}

int Pton(Family family, const char* text, void* out) {
  int win_family = family == Family::kInet ? AF_INET : family == Family::kInet6 ? AF_INET6 : -1;
  if (win_family < 0) return errno = EAFNOSUPPORT, -1;
  return Api().inet_pton(win_family, text, out);
}

bool Ntop(Family family, const void* in, char* out, size_t out_len) {
  int win_family = family == Family::kInet ? AF_INET : family == Family::kInet6 ? AF_INET6 : -1;
  if (win_family < 0) return errno = EAFNOSUPPORT, false;
  if (Api().inet_ntop(win_family, in, out, out_len) == nullptr) return errno = ENOSPC, false;
  return true;
}

// --- interfaces -------------------------------------------------------------

bool ListInterfaces(std::vector<Interface>* interfaces) {
  ULONG size = 16 * 1024;
  std::vector<unsigned char> buffer;
  ULONG rc;
  do {
    buffer.resize(size);
    rc = GetAdaptersAddresses(AF_UNSPEC,
                              GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST |
                                  GAA_FLAG_SKIP_DNS_SERVER,
                              nullptr, reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()),
                              &size);
  } while (rc == ERROR_BUFFER_OVERFLOW);
  if (rc == ERROR_NO_DATA) return true;
  if (rc != ERROR_SUCCESS) return errno = ENOBUFS, false;
  for (auto* a = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()); a != nullptr;
       a = a->Next) {
    Interface iface;
    char name[IF_MAX_STRING_SIZE + 1];
    if (ConvertInterfaceLuidToNameA(&a->Luid, name, sizeof(name)) != NO_ERROR) continue;
    iface.name = name;
    iface.index = a->IfIndex != 0 ? a->IfIndex : a->Ipv6IfIndex;
    iface.mtu = a->Mtu;
    if (a->OperStatus == IfOperStatusUp) iface.flags |= kIfUp | kIfRunning;
    if (a->IfType == IF_TYPE_SOFTWARE_LOOPBACK) iface.flags |= kIfLoopback;
    if ((a->Flags & IP_ADAPTER_NO_MULTICAST) == 0) iface.flags |= kIfMulticast;
    if (a->IfType == IF_TYPE_ETHERNET_CSMACD || a->IfType == IF_TYPE_IEEE80211) {
      iface.flags |= kIfBroadcast;
    }
    if (a->IfType == IF_TYPE_PPP || a->IfType == IF_TYPE_TUNNEL) iface.flags |= kIfPointToPoint;
    iface.hardware_len = a->PhysicalAddressLength <= sizeof(iface.hardware)
                             ? a->PhysicalAddressLength : 0;
    memcpy(iface.hardware, a->PhysicalAddress, iface.hardware_len);
    for (auto* u = a->FirstUnicastAddress; u != nullptr; u = u->Next) {
      InterfaceAddress address;
      FromWin(u->Address.lpSockaddr, u->Address.iSockaddrLength, &address.address);
      address.prefix_len = u->OnLinkPrefixLength;
      if (address.address.family != Family::kOther) iface.addresses.push_back(address);
    }
    interfaces->push_back(std::move(iface));
  }
  return true;
}

}  // namespace gesher::ws
