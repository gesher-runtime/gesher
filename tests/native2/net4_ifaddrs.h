// Pinned bionic LP64 callers exercise the same checks in JNI/APKs and ELF.
#pragma once
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <net/if_arp.h>
#include <netpacket/packet.h>
#include <array>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace net4 {
static_assert(sizeof(void*) == 8 && sizeof(ifaddrs) == 56 && alignof(ifaddrs) == 8);
static_assert(offsetof(ifaddrs, ifa_next) == 0 && offsetof(ifaddrs, ifa_name) == 8);
static_assert(offsetof(ifaddrs, ifa_flags) == 16 && offsetof(ifaddrs, ifa_addr) == 24);
static_assert(offsetof(ifaddrs, ifa_netmask) == 32 && offsetof(ifaddrs, ifa_ifu) == 40);
static_assert(offsetof(ifaddrs, ifa_data) == 48 && sizeof(sockaddr_ll) == 20);
static_assert(offsetof(sockaddr_ll, sll_ifindex) == 4 && offsetof(sockaddr_ll, sll_addr) == 12);
static_assert(sizeof(sockaddr_in) == 16 && offsetof(sockaddr_in, sin_addr) == 4);
static_assert(sizeof(sockaddr_in6) == 28 && offsetof(sockaddr_in6, sin6_addr) == 8);
static_assert(offsetof(sockaddr_in6, sin6_scope_id) == 24);
static_assert(AF_INET == 2 && AF_INET6 == 10 && AF_PACKET == 17);

struct Node {
  const ifaddrs* pointer;
  std::string name;
  unsigned flags, index;
  int family, prefix = -1;
  uint32_t scope = 0;
  std::array<unsigned char, 16> address{};
  std::string hardware;
  bool operator==(const Node&) const = default;
};
struct Result {
  std::vector<std::string> failures;
  size_t entries = 0, oracle = 0;
  void Check(bool okay, const std::string& what) { if (!okay) failures.push_back(what); }
  std::string Summary() const {
    return "LP64; IPv4/IPv6 loopback; netmasks/broadcast; packet index/hardware; independent lists; entries "
        + std::to_string(entries) + "; host oracle " + std::to_string(oracle);
  }
};

inline std::string Hardware(const sockaddr_ll* packet) {
  std::string hex;
  for (unsigned i = 0; i < packet->sll_halen && i < sizeof(packet->sll_addr); ++i) {
    char byte[3];
    std::snprintf(byte, sizeof(byte), "%02x", static_cast<unsigned>(packet->sll_addr[i]));
    hex += byte;
  }
  return hex.empty() ? "-" : hex;
}

inline std::vector<Node> Snapshot(ifaddrs* list, Result& result) {
  std::vector<Node> nodes;
  std::set<const ifaddrs*> seen;
  for (ifaddrs* p = list; p != nullptr; p = p->ifa_next) {
    if (!seen.insert(p).second || seen.size() > 4096) {
      result.Check(false, "bounded acyclic list"); break;
    }
    if (p->ifa_name == nullptr || p->ifa_addr == nullptr) {
      result.Check(false, "owned name and address are present"); continue;
    }
    size_t length = strnlen(p->ifa_name, IF_NAMESIZE);
    result.Check(length > 0 && length < IF_NAMESIZE, "terminated interface name");
    if (length == IF_NAMESIZE) continue;
    Node node{p, p->ifa_name, p->ifa_flags, if_nametoindex(p->ifa_name), p->ifa_addr->sa_family};
    char name[IF_NAMESIZE]{};
    result.Check(node.index != 0 && if_indextoname(node.index, name) != nullptr
                     && node.name == name, "actual name/index round trip");
    result.Check(p->ifa_data == nullptr, "pinned bionic ifa_data remains unused");
    if (node.family == AF_PACKET) {
      auto* packet = reinterpret_cast<const sockaddr_ll*>(p->ifa_addr);
      result.Check(packet->sll_ifindex > 0 && static_cast<unsigned>(packet->sll_ifindex) == node.index,
                   "packet index matches the interface");
      result.Check(packet->sll_halen <= sizeof(packet->sll_addr), "bounded hardware address");
      result.Check(packet->sll_halen != 6 || packet->sll_hatype == ARPHRD_ETHER, "six-byte Ethernet hardware type");
      result.Check(p->ifa_netmask == nullptr && p->ifa_broadaddr == nullptr, "packet has no IP mask/destination");
      result.Check((node.flags & IFF_LOOPBACK) == 0 ||
                       (packet->sll_hatype == ARPHRD_LOOPBACK && packet->sll_halen == 0),
                   "loopback hardware type and empty address");
      node.hardware = Hardware(packet);
    } else if (node.family == AF_INET || node.family == AF_INET6) {
      const unsigned char* bytes;
      const unsigned char* mask;
      size_t size = node.family == AF_INET ? 4 : 16;
      if (p->ifa_netmask == nullptr || p->ifa_netmask->sa_family != node.family) {
        result.Check(false, "IP netmask retains Linux address family"); continue;
      }
      if (node.family == AF_INET) {
        auto* ip = reinterpret_cast<const sockaddr_in*>(p->ifa_addr);
        auto* netmask = reinterpret_cast<const sockaddr_in*>(p->ifa_netmask);
        bytes = reinterpret_cast<const unsigned char*>(&ip->sin_addr);
        mask = reinterpret_cast<const unsigned char*>(&netmask->sin_addr);
        result.Check(ip->sin_port == 0 && netmask->sin_port == 0, "interface IPv4 ports are zero");
      } else {
        auto* ip = reinterpret_cast<const sockaddr_in6*>(p->ifa_addr);
        auto* netmask = reinterpret_cast<const sockaddr_in6*>(p->ifa_netmask);
        bytes = reinterpret_cast<const unsigned char*>(&ip->sin6_addr);
        mask = reinterpret_cast<const unsigned char*>(&netmask->sin6_addr);
        node.scope = ip->sin6_scope_id;
        result.Check(ip->sin6_port == 0 && netmask->sin6_port == 0 && netmask->sin6_scope_id == 0,
                     "interface IPv6 port/netmask scope are zero");
      }
      std::memcpy(node.address.data(), bytes, size);
      bool zero = false, contiguous = true;
      node.prefix = 0;
      for (size_t bit = 0; bit < size * 8; ++bit) {
        bool one = (mask[bit / 8] & (0x80 >> (bit % 8))) != 0;
        if (one) { ++node.prefix; if (zero) contiguous = false; } else zero = true;
      }
      result.Check(contiguous, "contiguous network-order prefix mask");
      if (node.family == AF_INET && (node.flags & IFF_BROADCAST) != 0) {
        auto* broadcast = reinterpret_cast<const sockaddr_in*>(p->ifa_broadaddr);
        bool valid = broadcast != nullptr && broadcast->sin_family == AF_INET;
        if (valid) {
          auto* b = reinterpret_cast<const unsigned char*>(&broadcast->sin_addr);
          for (size_t i = 0; i < size; ++i) valid &= b[i] == static_cast<unsigned char>(bytes[i] | ~mask[i]);
        }
        result.Check(valid, "IPv4 broadcast equals address OR inverse netmask");
      }
    } else result.Check(false, "no Windows AF_INET6 or foreign address layout");
    nodes.push_back(node);
  }
  return nodes;
}

inline Result RunIfaddrs(const char* oracle = nullptr) {
  Result result;
  ifaddrs* first = nullptr;
  if (getifaddrs(&first) != 0 || first == nullptr) {
    result.Check(false, "getifaddrs returns a real nonempty interface list"); return result;
  }
  auto original = Snapshot(first, result);
  result.entries = original.size();
  bool loop4 = false, loop6 = false, ethernet = false;
  for (const Node& node : original) {
    if ((node.flags & IFF_LOOPBACK) != 0) {
      result.Check((node.flags & (IFF_UP | IFF_RUNNING)) == (IFF_UP | IFF_RUNNING), "loopback is up and running");
      std::array<unsigned char, 16> four{}; four[0] = 127; four[3] = 1;
      if (node.family == AF_INET && node.address == four && node.prefix == 8) loop4 = true;
      std::array<unsigned char, 16> one{}; one[15] = 1;
      if (node.family == AF_INET6 && node.address == one && node.prefix == 128) loop6 = true;
    }
    if (node.family == AF_PACKET && node.hardware.size() == 12 && (node.flags & IFF_LOOPBACK) == 0) ethernet = true;
    if (node.family == AF_INET || node.family == AF_INET6) {
      bool packet = false;
      for (const Node& other : original) packet |= other.family == AF_PACKET && other.name == node.name
          && other.index == node.index && other.flags == node.flags;
      result.Check(packet, "IP entry shares flags/index/name with its link entry");
    }
  }
  result.Check(loop4 && loop6, "actual 127.0.0.1/8 and ::1/128 loopback addresses");
  result.Check(ethernet, "real non-loopback six-byte hardware address");
  for (int iteration = 0; iteration < 12; ++iteration) {
    ifaddrs* second = nullptr;
    if (getifaddrs(&second) != 0 || second == nullptr) {
      result.Check(false, "independent subsequent allocation"); break;
    }
    for (const Node& node : Snapshot(second, result)) {
      for (const Node& old : original) result.Check(node.pointer != old.pointer && node.pointer->ifa_name != old.pointer->ifa_name,
                                                 "simultaneous lists own disjoint entries/names");
    }
    if (second->ifa_name != nullptr && second->ifa_name[0] != '\0') second->ifa_name[0] = '#';
    freeifaddrs(second);
    result.Check(Snapshot(first, result) == original, "held list survives other enumeration/mutation/free");
  }
  freeifaddrs(nullptr);
  if (oracle != nullptr) {
    std::ifstream input(oracle);
    result.Check(input.is_open(), "independent Windows interface oracle opens");
    std::string line;
    while (std::getline(input, line)) {
      std::istringstream fields(line);
      char kind; unsigned index; fields >> kind >> index;
      if (!fields) { result.Check(false, "valid host oracle row"); continue; }
      bool found = false;
      if (kind == 'P') {
        std::string hardware; fields >> hardware;
        for (const Node& node : original) found |= node.family == AF_PACKET && node.index == index && node.hardware == hardware;
      } else if (kind == 'A') {
        int family, prefix; unsigned scope; std::string text; fields >> family >> prefix >> text >> scope;
        std::array<unsigned char, 16> address{};
        result.Check(inet_pton(family, text.c_str(), address.data()) == 1, "host oracle numeric IP parses");
        for (const Node& node : original) found |= node.family == family && node.index == index
            && node.prefix == prefix && node.address == address && node.scope == scope;
      }
      result.Check(static_cast<bool>(fields) && found, "matches Windows host row: " + line);
      ++result.oracle;
    }
    result.Check(result.oracle >= 3, "host oracle includes real link and IP records");
  }
  freeifaddrs(first);
  return result;
}
}  // namespace net4
