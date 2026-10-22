// gesher-base: name resolution (<netdb.h>), address text (<arpa/inet.h>),
// and network interfaces (<net/if.h>, <ifaddrs.h>), over winsock.cc.
//
// gethostbyname, gethostbyaddr, getservbyname, getservbyport and
// getprotobyname are declared but not implemented: nothing built uses them,
// and a link error is clearer than a stub that always fails.
#include <arpa/inet.h>
#include <errno.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <net/if_arp.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netpacket/packet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>

#include <string>
#include <vector>

#include "linux_socket.h"
#include "socket_internal.h"

namespace ws = gesher::ws;
using namespace gesher::sock;

namespace {

int ToGaiError(ws::GaiError error) {
  switch (error) {
    case ws::GaiError::kOk: return 0;
    case ws::GaiError::kBadFlags: return EAI_BADFLAGS;
    case ws::GaiError::kNoName: return EAI_NONAME;
    case ws::GaiError::kAgain: return EAI_AGAIN;
    case ws::GaiError::kFail: return EAI_FAIL;
    case ws::GaiError::kNoData: return EAI_NODATA;
    case ws::GaiError::kFamily: return EAI_FAMILY;
    case ws::GaiError::kSocktype: return EAI_SOCKTYPE;
    case ws::GaiError::kService: return EAI_SERVICE;
    case ws::GaiError::kMemory: return EAI_MEMORY;
    case ws::GaiError::kSystem: return EAI_SYSTEM;
    case ws::GaiError::kOverflow: return EAI_OVERFLOW;
  }
  return EAI_FAIL;
}

constexpr int kKnownAiFlags = AI_PASSIVE | AI_CANONNAME | AI_NUMERICHOST | AI_NUMERICSERV |
                              AI_V4MAPPED | AI_ALL | AI_ADDRCONFIG;

int ToAiFlags(int flags) {
  int result = 0;
  if (flags & AI_PASSIVE) result |= ws::kAiPassive;
  if (flags & AI_CANONNAME) result |= ws::kAiCanonName;
  if (flags & AI_NUMERICHOST) result |= ws::kAiNumericHost;
  if (flags & AI_NUMERICSERV) result |= ws::kAiNumericServ;
  if (flags & AI_V4MAPPED) result |= ws::kAiV4Mapped;
  if (flags & AI_ALL) result |= ws::kAiAll;
  if (flags & AI_ADDRCONFIG) result |= ws::kAiAddrConfig;
  return result;
}

// One allocation per result: the addrinfo, its address and its name.
struct AddrInfoBlock {
  addrinfo info;
  sockaddr_storage address;
  char name[];  // ai_canonname, when present
};

}  // namespace

extern "C" {

int getaddrinfo(const char* node, const char* service, const struct addrinfo* hints,
                struct addrinfo** res) {
  if (res == nullptr) return EAI_FAIL;
  *res = nullptr;
  if (node == nullptr && service == nullptr) return EAI_NONAME;
  ws::AddrInfo ws_hints;
  if (hints != nullptr) {
    if ((hints->ai_flags & ~kKnownAiFlags) != 0) return EAI_BADFLAGS;
    ws_hints.flags = ToAiFlags(hints->ai_flags);
    ws_hints.family = ToFamily(hints->ai_family);
    if (ws_hints.family != ws::Family::kUnspec && ws_hints.family != ws::Family::kInet &&
        ws_hints.family != ws::Family::kInet6) {
      return EAI_FAMILY;
    }
    if (hints->ai_socktype != 0) {
      ws_hints.type = ToType(hints->ai_socktype);
      if (ws_hints.type == ws::Type::kOther) return EAI_SOCKTYPE;
    }
    ws_hints.protocol = hints->ai_protocol;
  }
  std::vector<ws::AddrInfo> results;
  ws::GaiError error =
      ws::Resolve(node, service, hints != nullptr ? &ws_hints : nullptr, &results);
  if (error != ws::GaiError::kOk) return ToGaiError(error);

  addrinfo** tail = res;
  for (size_t i = 0; i < results.size(); ++i) {
    const ws::AddrInfo& r = results[i];
    // Linux gives the canonical name in the first result only.
    size_t name_len = i == 0 && !r.canonname.empty() ? r.canonname.size() + 1 : 0;
    auto* block = static_cast<AddrInfoBlock*>(calloc(1, sizeof(AddrInfoBlock) + name_len));
    if (block == nullptr) {
      freeaddrinfo(*res);
      *res = nullptr;
      return EAI_MEMORY;
    }
    socklen_t len = sizeof(block->address);
    FromAddress(r.address, reinterpret_cast<sockaddr*>(&block->address), &len);
    block->info.ai_flags = hints != nullptr ? hints->ai_flags : 0;
    block->info.ai_family = FromFamily(r.family);
    block->info.ai_socktype = FromType(r.type);
    block->info.ai_protocol = r.protocol;
    block->info.ai_addrlen = len;
    block->info.ai_addr = reinterpret_cast<sockaddr*>(&block->address);
    if (name_len > 0) {
      memcpy(block->name, r.canonname.c_str(), name_len);
      block->info.ai_canonname = block->name;
    }
    *tail = &block->info;
    tail = &block->info.ai_next;
  }
  return 0;
}

void freeaddrinfo(struct addrinfo* res) {
  while (res != nullptr) {
    addrinfo* next = res->ai_next;
    free(res);  // the start of its AddrInfoBlock
    res = next;
  }
}

// glibc's messages.
const char* gai_strerror(int errcode) {
  switch (errcode) {
    case 0: return "Success";
    case EAI_BADFLAGS: return "Bad value for ai_flags";
    case EAI_NONAME: return "Name or service not known";
    case EAI_AGAIN: return "Temporary failure in name resolution";
    case EAI_FAIL: return "Non-recoverable failure in name resolution";
    case EAI_NODATA: return "No address associated with hostname";
    case EAI_FAMILY: return "ai_family not supported";
    case EAI_SOCKTYPE: return "ai_socktype not supported";
    case EAI_SERVICE: return "Servname not supported for ai_socktype";
    case EAI_ADDRFAMILY: return "Address family for hostname not supported";
    case EAI_MEMORY: return "Memory allocation failure";
    case EAI_SYSTEM: return "System error";
    case EAI_OVERFLOW: return "Argument buffer overflow";
    default: return "Unknown error";
  }
}

int getnameinfo(const struct sockaddr* addr, socklen_t addrlen, char* host, socklen_t hostlen,
                char* serv, socklen_t servlen, int flags) {
  ws::Address address;
  if (!ToAddress(addr, addrlen, &address)) return EAI_FAMILY;
  if (address.family != ws::Family::kInet && address.family != ws::Family::kInet6) {
    return EAI_FAMILY;
  }
  int ws_flags = 0;
  if (flags & NI_NUMERICHOST) ws_flags |= ws::kNiNumericHost;
  if (flags & NI_NUMERICSERV) ws_flags |= ws::kNiNumericServ;
  if (flags & NI_NOFQDN) ws_flags |= ws::kNiNoFqdn;
  if (flags & NI_NAMEREQD) ws_flags |= ws::kNiNameReqd;
  if (flags & NI_DGRAM) ws_flags |= ws::kNiDgram;
  return ToGaiError(ws::ReverseResolve(address, hostlen > 0 ? host : nullptr, hostlen,
                                    servlen > 0 ? serv : nullptr, servlen, ws_flags));
}

int* __get_h_errno(void) {
  static thread_local int h_errno_value = 0;
  return &h_errno_value;
}

// --- address text -------------------------------------------------------------

int inet_pton(int af, const char* src, void* dst) {
  if (af != AF_INET && af != AF_INET6) return errno = EAFNOSUPPORT, -1;
  return ws::Pton(ToFamily(af), src, dst);
}

const char* inet_ntop(int af, const void* src, char* dst, socklen_t size) {
  if (af != AF_INET && af != AF_INET6) return errno = EAFNOSUPPORT, nullptr;
  return ws::Ntop(ToFamily(af), src, dst, size) ? dst : nullptr;
}

// The classic forms: a.b.c.d, a.b.c (c is 16 bits), a.b (b is 24 bits) and a
// (32 bits); each part decimal, octal (0...) or hex (0x...).
int inet_aton(const char* cp, struct in_addr* addr) {
  unsigned long long parts[4];
  int count = 0;
  const char* p = cp;
  for (;;) {
    if (count == 4) return 0;
    char* end;
    if (*p < '0' || *p > '9') return 0;  // strtoull would accept signs and spaces
    errno = 0;
    unsigned long long value = strtoull(p, &end, 0);
    if (end == p || errno == ERANGE || value > 0xFFFFFFFFull) return 0;
    parts[count++] = value;
    p = end;
    if (*p == '.') {
      ++p;
      continue;
    }
    if (*p != '\0') return 0;
    break;
  }
  unsigned long long result;
  switch (count) {
    case 1:
      result = parts[0];
      break;
    case 2:
      if (parts[0] > 0xFF || parts[1] > 0xFFFFFF) return 0;
      result = parts[0] << 24 | parts[1];
      break;
    case 3:
      if (parts[0] > 0xFF || parts[1] > 0xFF || parts[2] > 0xFFFF) return 0;
      result = parts[0] << 24 | parts[1] << 16 | parts[2];
      break;
    default:
      for (int i = 0; i < 4; ++i) {
        if (parts[i] > 0xFF) return 0;
      }
      result = parts[0] << 24 | parts[1] << 16 | parts[2] << 8 | parts[3];
      break;
  }
  if (addr != nullptr) addr->s_addr = __builtin_bswap32(static_cast<uint32_t>(result));
  return 1;
}

in_addr_t inet_addr(const char* cp) {
  in_addr addr;
  return inet_aton(cp, &addr) ? addr.s_addr : INADDR_NONE;
}

char* inet_ntoa(struct in_addr in) {
  static thread_local char buffer[16];
  const unsigned char* b = reinterpret_cast<const unsigned char*>(&in.s_addr);
  snprintf(buffer, sizeof(buffer), "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
  return buffer;
}

}  // extern "C"

// --- interfaces -----------------------------------------------------------------

namespace {

// Windows' interface names ("ethernet_32769", the names its own
// if_indextoname gives); one that does not fit IFNAMSIZ becomes "if<index>",
// the same way in every function here.
bool ListNamedInterfaces(std::vector<ws::Interface>* interfaces) {
  if (!ws::ListInterfaces(interfaces)) return false;
  for (ws::Interface& iface : *interfaces) {
    if (iface.name.size() >= IFNAMSIZ) iface.name = "if" + std::to_string(iface.index);
  }
  return true;
}

unsigned ToIfFlags(unsigned flags) {
  unsigned result = 0;
  if (flags & ws::kIfUp) result |= IFF_UP;
  if (flags & ws::kIfRunning) result |= IFF_RUNNING;
  if (flags & ws::kIfLoopback) result |= IFF_LOOPBACK;
  if (flags & ws::kIfBroadcast) result |= IFF_BROADCAST;
  if (flags & ws::kIfMulticast) result |= IFF_MULTICAST;
  if (flags & ws::kIfPointToPoint) result |= IFF_POINTOPOINT;
  return result;
}

// One allocation per entry, freed by freeifaddrs.
struct IfAddrsBlock {
  ifaddrs ifa;
  sockaddr_storage address;
  sockaddr_storage netmask;
  sockaddr_storage broadcast;
  char name[IFNAMSIZ];
};

IfAddrsBlock* NewEntry(const ws::Interface& iface) {
  auto* block = static_cast<IfAddrsBlock*>(calloc(1, sizeof(IfAddrsBlock)));
  if (block == nullptr) return nullptr;
  snprintf(block->name, sizeof(block->name), "%s", iface.name.c_str());
  block->ifa.ifa_name = block->name;
  block->ifa.ifa_flags = ToIfFlags(iface.flags);
  return block;
}

// The netmask for a prefix length, in an address of the same family.
void SetNetmask(const sockaddr_storage& address, int prefix_len, sockaddr_storage* out) {
  out->ss_family = address.ss_family;
  uint8_t* bytes;
  int max_bits;
  if (address.ss_family == AF_INET) {
    bytes = reinterpret_cast<uint8_t*>(&reinterpret_cast<sockaddr_in*>(out)->sin_addr);
    max_bits = 32;
  } else {
    bytes = reinterpret_cast<uint8_t*>(&reinterpret_cast<sockaddr_in6*>(out)->sin6_addr);
    max_bits = 128;
  }
  if (prefix_len > max_bits) prefix_len = max_bits;
  for (int i = 0; i < prefix_len; ++i) bytes[i / 8] |= static_cast<uint8_t>(0x80 >> (i % 8));
}

}  // namespace

extern "C" {

unsigned int if_nametoindex(const char* name) {
  std::vector<ws::Interface> interfaces;
  if (name == nullptr || !ListNamedInterfaces(&interfaces)) return 0;
  for (const ws::Interface& iface : interfaces) {
    if (iface.name == name) return iface.index;
  }
  errno = ENODEV;
  return 0;
}

char* if_indextoname(unsigned int index, char* name) {
  std::vector<ws::Interface> interfaces;
  if (!ListNamedInterfaces(&interfaces)) return nullptr;
  for (const ws::Interface& iface : interfaces) {
    if (iface.index == index) {
      snprintf(name, IFNAMSIZ, "%s", iface.name.c_str());
      return name;
    }
  }
  errno = ENXIO;
  return nullptr;
}

struct if_nameindex* if_nameindex(void) {
  std::vector<ws::Interface> interfaces;
  if (!ListNamedInterfaces(&interfaces)) return nullptr;
  auto* result = static_cast<struct if_nameindex*>(
      calloc(interfaces.size() + 1, sizeof(struct if_nameindex)));
  if (result == nullptr) return errno = ENOBUFS, nullptr;
  for (size_t i = 0; i < interfaces.size(); ++i) {
    result[i].if_index = interfaces[i].index;
    result[i].if_name = strdup(interfaces[i].name.c_str());
    if (result[i].if_name == nullptr) {
      if_freenameindex(result);
      return errno = ENOBUFS, nullptr;
    }
  }
  return result;  // ends with {0, nullptr}
}

void if_freenameindex(struct if_nameindex* ptr) {
  if (ptr == nullptr) return;
  for (struct if_nameindex* p = ptr; p->if_name != nullptr; ++p) free(p->if_name);
  free(ptr);
}

// As on Linux: an AF_PACKET entry per interface, holding its hardware
// address (which libcore's NetworkInterface reads), then one per IP address.
int getifaddrs(struct ifaddrs** ifap) {
  if (ifap == nullptr) return errno = EFAULT, -1;
  *ifap = nullptr;
  std::vector<ws::Interface> interfaces;
  if (!ListNamedInterfaces(&interfaces)) return -1;
  ifaddrs** tail = ifap;
  auto append = [&tail](IfAddrsBlock* block) {
    *tail = &block->ifa;
    tail = &block->ifa.ifa_next;
  };
  for (const ws::Interface& iface : interfaces) {
    IfAddrsBlock* packet = NewEntry(iface);
    if (packet == nullptr) {
      freeifaddrs(*ifap);
      *ifap = nullptr;
      return errno = ENOMEM, -1;
    }
    auto* sll = reinterpret_cast<sockaddr_ll*>(&packet->address);
    sll->sll_family = AF_PACKET;
    sll->sll_ifindex = static_cast<int>(iface.index);
    sll->sll_hatype = (iface.flags & ws::kIfLoopback) != 0 ? ARPHRD_LOOPBACK : ARPHRD_ETHER;
    sll->sll_halen = static_cast<unsigned char>(iface.hardware_len);
    memcpy(sll->sll_addr, iface.hardware, iface.hardware_len);
    packet->ifa.ifa_addr = reinterpret_cast<sockaddr*>(&packet->address);
    append(packet);

    for (const ws::InterfaceAddress& a : iface.addresses) {
      IfAddrsBlock* block = NewEntry(iface);
      if (block == nullptr) {
        freeifaddrs(*ifap);
        *ifap = nullptr;
        return errno = ENOMEM, -1;
      }
      socklen_t len = sizeof(block->address);
      FromAddress(a.address, reinterpret_cast<sockaddr*>(&block->address), &len);
      block->ifa.ifa_addr = reinterpret_cast<sockaddr*>(&block->address);
      SetNetmask(block->address, a.prefix_len, &block->netmask);
      block->ifa.ifa_netmask = reinterpret_cast<sockaddr*>(&block->netmask);
      if (block->address.ss_family == AF_INET && (iface.flags & ws::kIfBroadcast) != 0) {
        auto* address = reinterpret_cast<sockaddr_in*>(&block->address);
        auto* mask = reinterpret_cast<sockaddr_in*>(&block->netmask);
        auto* broadcast = reinterpret_cast<sockaddr_in*>(&block->broadcast);
        broadcast->sin_family = AF_INET;
        broadcast->sin_addr.s_addr = address->sin_addr.s_addr | ~mask->sin_addr.s_addr;
        block->ifa.ifa_broadaddr = reinterpret_cast<sockaddr*>(&block->broadcast);
      }
      append(block);
    }
  }
  return 0;
}

}  // extern "C"

namespace gesher::sock {

bool InterfaceIoctl(unsigned long request, void* arg, int* result) {
  if (request != SIOCGIFFLAGS && request != SIOCGIFMTU) return false;
  auto* req = static_cast<struct ifreq*>(arg);
  if (req == nullptr) {
    *result = (errno = EFAULT, -1);
    return true;
  }
  std::vector<ws::Interface> interfaces;
  if (!ListNamedInterfaces(&interfaces)) {
    *result = -1;
    return true;
  }
  char name[IFNAMSIZ];
  memcpy(name, req->ifr_name, IFNAMSIZ);
  name[IFNAMSIZ - 1] = '\0';
  for (const ws::Interface& iface : interfaces) {
    if (iface.name != name) continue;
    if (request == SIOCGIFFLAGS) {
      req->ifr_flags = static_cast<short>(ToIfFlags(iface.flags));
    } else {
      req->ifr_mtu = static_cast<int>(iface.mtu);
    }
    *result = 0;
    return true;
  }
  *result = (errno = ENODEV, -1);
  return true;
}

}  // namespace gesher::sock

extern "C" {

void freeifaddrs(struct ifaddrs* ifa) {
  while (ifa != nullptr) {
    ifaddrs* next = ifa->ifa_next;
    free(ifa);  // the start of its IfAddrsBlock
    ifa = next;
  }
}

}  // extern "C"
