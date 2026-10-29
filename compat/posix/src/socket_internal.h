// gesher-base: Winsock behind the Linux socket API.
//
// <winsock2.h> declares socket(), bind(), ... with Windows' types and
// constants, so it cannot share a file with the Linux declarations. The
// Linux side (socket.cc, netdb.cc, fd.cc) sees only gesher-base's headers,
// winsock.cc only Winsock's, and this interface between them uses neither:
// each side translates its own constants to the values here. Failures return
// -1 (or kInvalidSocket) with errno set, in Linux's numbering.
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <cstdlib>
#include <memory>
#include <atomic>
#include <mutex>

#include <string>
#include <vector>

namespace gesher::ws {

using Socket = uintptr_t;
inline constexpr Socket kInvalidSocket = ~Socket{0};

enum class Family { kUnspec, kInet, kInet6, kUnix, kOther };
enum class Type { kStream, kDgram, kRaw, kSeqpacket, kOther };

// A socket address. Past the family field, Linux's and Winsock's
// sockaddr_in, sockaddr_in6 and sockaddr_un have the same layout, so `data`
// holds those bytes unchanged.
struct Address {
  Family family = Family::kUnspec;
  uint8_t data[126] = {};
  size_t data_len = 0;  // bytes used in `data`
};

// Message flags.
inline constexpr int kMsgOob = 1;
inline constexpr int kMsgPeek = 2;
inline constexpr int kMsgDontRoute = 4;
inline constexpr int kMsgWaitAll = 8;

// Options with an int value. Timeouts are in milliseconds.
enum class Option {
  kReuseAddr, kKeepAlive, kBroadcast, kOobInline, kRcvBuf, kSndBuf, kRcvLowat,
  kSndLowat, kRcvTimeo, kSndTimeo, kError, kType, kAcceptConn, kDontRoute,
  kTcpNoDelay, kTcpKeepIdle, kTcpKeepIntvl, kTcpKeepCnt,
  kIpTos, kIpTtl, kIpMulticastIf, kIpMulticastTtl, kIpMulticastLoop,
  kIpv6UnicastHops, kIpv6MulticastIf, kIpv6MulticastHops, kIpv6MulticastLoop,
  kIpv6V6Only, kIpv6TClass,
};

struct Buffer {
  void* data;
  size_t len;
};

// poll events.
inline constexpr int kPollIn = 1;
inline constexpr int kPollPri = 2;
inline constexpr int kPollOut = 4;
inline constexpr int kPollErr = 8;
inline constexpr int kPollHup = 16;
inline constexpr int kPollNval = 32;

enum class ConnectState { kInitial, kConnecting, kConnected, kFailed };
struct SocketDescription {
  std::atomic<ConnectState> state{ConnectState::kInitial};
  bool tcp = false;  // immutable provider protocol identity, set before publication
  uint64_t connect_generation = 0, consumed_generation = 0; // error_mutex
  int consumed_error = 0;  // exact caller-observed code, never a generic error mask
  bool connect_call_active = false;  // error_mutex
  bool nonblocking_connect = false;  // only this bounded attempt is virtualized
  std::atomic<bool> nonblocking{false}, reuse_addr{false};
  std::mutex error_mutex;
  std::mutex status_mutex;
  size_t descriptor_refs = 0;  // fd.cc's table mutex; snapshots are not fd refs
};
struct PollEntry {
  Socket socket;
  int events;
  int revents;
  std::shared_ptr<SocketDescription> description;
};

// Starts Winsock once; false (errno set) if it cannot.
bool Startup();
bool IsTcpSocket(Socket s);  // provider protocol identity; never reads SO_ERROR

Socket Open(Family family, Type type, int protocol, bool process_binding = true);
int Close(Socket s);
Socket Duplicate(Socket s);  // a second socket for the same endpoint
int SetNonBlocking(Socket s, bool on);
int Bind(Socket s, const Address& address);
int Connect(Socket s, const Address& address);  // EINPROGRESS when non-blocking
int Listen(Socket s, int backlog);
Socket Accept(Socket s, Address* peer);
int GetSockName(Socket s, Address* address);
int GetPeerName(Socket s, Address* address);
int Shutdown(Socket s, bool read, bool write);
int Available(Socket s, int* bytes);  // FIONREAD
int BindInterface(Socket s, uint32_t ifindex);
int BoundInterface(Socket s, uint32_t* ifindex);
int SetProcessInterface(uint32_t ifindex);
uint32_t ProcessInterface();
int ValidateInterface(uint32_t ifindex);
int DnsServers(uint32_t ifindex, std::vector<Address>* servers, uint32_t* selected = nullptr);

// Gather/scatter I/O; `to`/`from` may be null.
ptrdiff_t Send(Socket s, const Buffer* buffers, size_t count, int flags, const Address* to);
ptrdiff_t Recv(Socket s, Buffer* buffers, size_t count, int flags, Address* from);

int GetOption(Socket s, Option option, int* value);
int SetOption(Socket s, Option option, int value);
int GetLinger(Socket s, bool* on, int* seconds);
int SetLinger(Socket s, bool on, int seconds);
// Joins or leaves a multicast group (any-source when `source` is null).
// IPv4 addresses are 4 bytes, IPv6 16, in network order.
int Membership(Socket s, bool join, Family family, const uint8_t* group, uint32_t ifindex,
               const uint8_t* source);

// Returns how many entries have revents set, 0 on timeout (timeout_ms < 0
// waits forever).
int Poll(PollEntry* entries, size_t count, int timeout_ms);

// --- names --------------------------------------------------------------------

enum class GaiError {
  kOk, kBadFlags, kNoName, kAgain, kFail, kNoData, kFamily, kSocktype, kService,
  kMemory, kSystem, kOverflow,
};

inline constexpr int kAiPassive = 1;
inline constexpr int kAiCanonName = 2;
inline constexpr int kAiNumericHost = 4;
inline constexpr int kAiNumericServ = 8;
inline constexpr int kAiV4Mapped = 16;
inline constexpr int kAiAll = 32;
inline constexpr int kAiAddrConfig = 64;

struct AddrInfo {
  int flags = 0;
  Family family = Family::kUnspec;
  Type type = Type::kOther;  // kOther: any
  int protocol = 0;
  Address address;
  std::string canonname;
};

GaiError Resolve(const char* node, const char* service, const AddrInfo* hints,
                 std::vector<AddrInfo>* results, uint32_t ifindex = UINT32_MAX);

inline constexpr int kNiNumericHost = 1;
inline constexpr int kNiNumericServ = 2;
inline constexpr int kNiNoFqdn = 4;
inline constexpr int kNiNameReqd = 8;
inline constexpr int kNiDgram = 16;

GaiError ReverseResolve(const Address& address, char* host, size_t host_len, char* serv,
                     size_t serv_len, int flags);

// Legacy database results own every byte. No Winsock pointer or 16-bit host
// field crosses this boundary; netdb.cc publishes Linux-layout TLS objects.
enum class HostError { kOk, kNotFound, kAgain, kNoRecovery, kNoData, kInternal };
struct LegacyHost {
  std::unique_ptr<char, decltype(&std::free)> storage{nullptr, &std::free};
  char* name = nullptr;
  char** aliases = nullptr;
  Family family = Family::kOther;
  size_t length = 0;
  char** addresses = nullptr;
};
struct LegacyService {
  std::unique_ptr<char, decltype(&std::free)> storage{nullptr, &std::free};
  char* name = nullptr;
  char** aliases = nullptr;
  uint16_t port = 0;  // network byte order, without signed-short extension
  char* protocol = nullptr;
};
HostError LegacyHostByName(const char* name, LegacyHost* result);
HostError LegacyHostByAddress(const void* address, size_t length, Family family,
                             LegacyHost* result);
bool LegacyServiceByName(const char* name, const char* protocol, LegacyService* result);
bool LegacyServiceByPort(int port, const char* protocol, LegacyService* result);

// inet_pton/inet_ntop for kInet and kInet6.
int Pton(Family family, const char* text, void* out);
bool Ntop(Family family, const void* in, char* out, size_t out_len);

// --- interfaces -------------------------------------------------------------

inline constexpr unsigned kIfUp = 1;
inline constexpr unsigned kIfRunning = 2;
inline constexpr unsigned kIfLoopback = 4;
inline constexpr unsigned kIfBroadcast = 8;
inline constexpr unsigned kIfMulticast = 16;
inline constexpr unsigned kIfPointToPoint = 32;

struct InterfaceAddress {
  Address address;
  int prefix_len;
};

struct Interface {
  std::string name;
  uint32_t index = 0;
  unsigned flags = 0;
  uint32_t mtu = 0;
  uint8_t hardware[8] = {};
  size_t hardware_len = 0;
  std::vector<InterfaceAddress> addresses;
};

bool ListInterfaces(std::vector<Interface>* interfaces);

}  // namespace gesher::ws

// --- socket fds (fd.cc; Linux side only) ------------------------------------
//
// A socket's fd is a CRT fd open on NUL, which reserves the number in the
// CRT's table, mapped here to its socket. close, read, write, dup and dup2
// (fd.cc) check the map before the CRT; CRT functions that do not see a
// socket's fd as an empty device.
namespace gesher {

struct SocketFd {
  ws::Socket socket = ws::kInvalidSocket;
  bool initial_nonblocking = false;  // registration hint; live flags are shared
  // Open-description lifetime: dup/dup2 retain the same connect lifecycle.
  std::shared_ptr<ws::SocketDescription> description;
};

// Internal operation dependency for a real post-native-call CI barrier. The
// POSIX entry always passes nullptr; no mutable hook or guest symbol exists.
using ConnectObserver = void (*)(ws::Socket, int, int, void*);
int ConnectWithObserver(int fd, const void* address, uint32_t length, ConnectObserver observer, void* context);

// Takes ownership of `state.socket`: returns its new fd, or -1 with errno set
// (the socket is then closed).
int RegisterSocket(const SocketFd& state);
// False if fd is not a socket.
bool LookupSocket(int fd, SocketFd* state);

}  // namespace gesher
