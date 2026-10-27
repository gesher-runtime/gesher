#pragma once
#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <stdint.h>
#include <string.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace net4resolver {
static_assert(sizeof(void*) == 8 && sizeof(hostent) == 32);
static_assert(AF_INET == 2 && AF_INET6 == 10 && NETDB_INTERNAL == -1);
static_assert(offsetof(hostent, h_addrtype) == 16 && sizeof(hostent::h_addrtype) == 4);
static_assert(offsetof(hostent, h_length) == 20 && offsetof(hostent, h_addr_list) == 24);
static_assert(sizeof(servent) == 32 && offsetof(servent, s_port) == 16);
static_assert(sizeof(servent::s_port) == 4 && offsetof(servent, s_proto) == 24);

inline std::string Hex(const void* data, size_t length) {
  const auto* bytes = static_cast<const unsigned char*>(data);
  std::string text;
  for (size_t i = 0; i < length; ++i) {
    text += "0123456789abcdef"[bytes[i] >> 4];
    text += "0123456789abcdef"[bytes[i] & 15];
  }
  return text;
}
inline std::string Names(std::vector<std::string> values) {
  std::sort(values.begin(), values.end());
  std::string text;
  for (const auto& value : values) { if (!text.empty()) text += ','; text += value; }
  return text;
}
inline std::string Aliases(char* const* aliases) {
  std::vector<std::string> values;
  if (aliases != nullptr) for (size_t i = 0; aliases[i] != nullptr && i < 128; ++i) {
    values.emplace_back(aliases[i]);
  }
  return Names(values);
}
inline bool HostShape(const hostent* host, int family, int length) {
  return host != nullptr && host->h_addrtype == family && host->h_length == length
      && host->h_name != nullptr && *host->h_name != '\0' && host->h_aliases != nullptr
      && host->h_addr_list != nullptr && host->h_addr_list[0] != nullptr;
}
inline bool HasAddress(const hostent* host, const void* bytes) {
  if (host == nullptr) return false;
  for (size_t i = 0; host->h_addr_list[i] != nullptr && i < 128; ++i) {
    if (memcmp(host->h_addr_list[i], bytes, host->h_length) == 0) return true;
  }
  return false;
}
inline std::string HostRow(const std::string& key, const hostent* host) {
  std::vector<std::string> addresses;
  for (size_t i = 0; host->h_addr_list[i] != nullptr && i < 128; ++i) {
    addresses.push_back(Hex(host->h_addr_list[i], host->h_length));
  }
  return "H|" + key + '|' + std::to_string(host->h_addrtype) + '|'
      + std::to_string(host->h_length) + '|' + host->h_name + '|'
      + Aliases(host->h_aliases) + '|' + Names(addresses);
}
inline bool ServiceShape(const servent* service, int port, const char* protocol) {
  return service != nullptr && service->s_port == static_cast<int>(htons(port))
      && service->s_name != nullptr && service->s_aliases != nullptr
      && service->s_proto != nullptr && strcmp(service->s_proto, protocol) == 0;
}
inline std::string ServiceRow(const std::string& key, const servent* service) {
  return "S|" + key + '|' + service->s_name + '|' + std::to_string(service->s_port)
      + '|' + service->s_proto + '|' + Aliases(service->s_aliases);
}

inline std::string Run(bool allowed, const char* oracle = nullptr) {
  std::ostringstream out;
  int failures = 0, checks = 0, compared = 0;
  std::set<std::string> expected;
  if (oracle != nullptr) {
    std::ifstream input(oracle);
    std::string line;
    while (std::getline(input, line)) { if (!line.empty() && line.back() == '\r') line.pop_back(); expected.insert(line); }
  }
  auto check = [&](bool okay, const std::string& name) {
    ++checks;
    if (!okay) ++failures;
    out << (okay ? "PASS" : "FAIL") << " net4 resolver: " << name << '\n';
  };
  auto compare = [&](const std::string& line) {
    out << "DATA net4 resolver: " << line << '\n';
    if (oracle != nullptr) { ++compared; check(expected.count(line) == 1, "independent WinSDK oracle " + line); }
  };
  in_addr ipv4{};
  in6_addr ipv6{};
  inet_pton(AF_INET, "127.0.0.1", &ipv4);
  inet_pton(AF_INET6, "::1", &ipv6);
  h_errno = 321;
  hostent* numeric = gethostbyname("127.0.0.1");
  bool shape = HostShape(numeric, AF_INET, 4);
  check(shape && h_errno == 0 && HasAddress(numeric, &ipv4), "real numeric IPv4 has Linux int32 family/length and resets h_errno");
  if (shape) compare(HostRow("numeric", numeric));
  std::string held = shape ? HostRow("held", numeric) : "";
  const int error_sentinel = 971;
  h_errno = error_sentinel;
  for (const auto& request : std::vector<std::pair<std::string, std::string>>{
      {"http", "tcp"}, {"https", "tcp"}, {"domain", "udp"}, {"domain", "tcp"}}) {
    const int port = request.first == "http" ? 80 : request.first == "https" ? 443 : 53;
    servent* service = getservbyname(request.first.c_str(), request.second.c_str());
    bool okay = ServiceShape(service, port, request.second.c_str());
    check(okay, request.first + '/' + request.second + " has Linux service fields and unsigned network-order port");
    if (okay) {
      const std::string by_name = ServiceRow(request.first + '-' + request.second, service);
      compare(by_name);
      std::string aliases = Aliases(service->s_aliases);
      service->s_name[0] = '#';
      if (service->s_aliases[0] != nullptr && service->s_aliases[0][0] != '\0') service->s_aliases[0][0] = '#';
      servent* again = getservbyport(htons(port), request.second.c_str());
      check(ServiceShape(again, port, request.second.c_str()) && again->s_name[0] != '#'
          && Aliases(again->s_aliases) == aliases, "service mutation cannot corrupt provider data; lookup by port/protocol");
    }
  }
  check(h_errno == error_sentinel && (!shape || HostRow("held", numeric) == held),
      "service calls preserve held host strings/address pointers and thread h_errno");
  check(getservbyname("gesher-test82-absent-service", "tcp") == nullptr
      && getservbyname("http", "gesher-absent-protocol") == nullptr
      && getservbyport(-1, "tcp") == nullptr && getservbyport(65536, "tcp") == nullptr,
      "missing service/protocol and out-of-range ports fail without a manufactured result");
  servent* any = getservbyname("http", nullptr);
  check(ServiceShape(any, 80, "tcp"), "null service protocol uses actual first matching database entry");
  errno = 0;
  check(gethostbyaddr(&ipv4, 4, AF_UNIX) == nullptr && errno == EAFNOSUPPORT && h_errno == NETDB_INTERNAL,
      "unsupported reverse family reports NETDB_INTERNAL/EAFNOSUPPORT");
  errno = 0;
  check(gethostbyaddr(&ipv4, 3, AF_INET) == nullptr && errno == EINVAL && h_errno == NETDB_INTERNAL,
      "wrong reverse address length reports NETDB_INTERNAL/EINVAL");
  check(gethostbyname("127.1") == nullptr && h_errno == HOST_NOT_FOUND
      && gethostbyname("300.0.0.1") == nullptr && h_errno == HOST_NOT_FOUND,
      "pinned strict numeric names reject shorthand and invalid IPv4 without DNS");
  in6_addr scoped{};
  inet_pton(AF_INET6, "fe80::1", &scoped);
  check(gethostbyaddr(&scoped, 16, AF_INET6) == nullptr && h_errno == HOST_NOT_FOUND,
      "pinned scoped local IPv6 reverse rejection");
  if (allowed) {
    hostent* local = gethostbyname("localhost");
    bool okay = HostShape(local, AF_INET, 4);
    check(okay && h_errno == 0 && HasAddress(local, &ipv4), "actual localhost host database returns IPv4 bytes and real names/aliases");
    if (okay) compare(HostRow("localhost", local));
    hostent* reverse4 = gethostbyaddr(&ipv4, 4, AF_INET);
    okay = HostShape(reverse4, AF_INET, 4);
    check(okay && h_errno == 0 && HasAddress(reverse4, &ipv4), "actual IPv4 reverse host lookup owns its returned bytes");
    if (okay) compare(HostRow("reverse4", reverse4));
    hostent* reverse6 = gethostbyaddr(&ipv6, 16, AF_INET6);
    okay = HostShape(reverse6, AF_INET6, 16);
    check(okay && h_errno == 0 && HasAddress(reverse6, &ipv6), "actual IPv6 reverse lookup translates Win32 AF_INET6 to Linux10");
    if (okay) compare(HostRow("reverse6", reverse6));
    in6_addr mapped{};
    inet_pton(AF_INET6, "::ffff:127.0.0.1", &mapped);
    hostent* unmapped = gethostbyaddr(&mapped, 16, AF_INET6);
    check(HostShape(unmapped, AF_INET, 4) && HasAddress(unmapped, &ipv4), "mapped IPv6 reverse address follows pinned IPv4 unmapping");
  } else {
    errno = 0;
    check(gethostbyname("localhost") == nullptr && errno == EACCES && h_errno == NETDB_INTERNAL,
        "missing INTERNET permission denies non-numeric name resolution");
    errno = 0;
    check(gethostbyaddr(&ipv4, 4, AF_INET) == nullptr && errno == EACCES && h_errno == NETDB_INTERNAL,
        "missing INTERNET permission denies reverse name resolution");
  }

  std::atomic<int> ready{0};
  std::atomic<bool> release{false};
  bool thread_ok[4] = {};
  uintptr_t host_ptr[4] = {}, error_ptr[4] = {}, service_ptr[4] = {};
  std::vector<std::thread> threads;
  for (int i = 0; i < 4; ++i) threads.emplace_back([&, i] {
    const std::string name = "127.0.0." + std::to_string(i + 2);
    hostent* host = gethostbyname(name.c_str());
    servent* service = getservbyname(i % 2 ? "http" : "https", "tcp");
    const int port = i % 2 ? 80 : 443;
    bool okay = HostShape(host, AF_INET, 4) && ServiceShape(service, port, "tcp");
    const std::string before = okay ? HostRow("thread", host) : "";
    h_errno = 700 + i;
    host_ptr[i] = reinterpret_cast<uintptr_t>(host);
    error_ptr[i] = reinterpret_cast<uintptr_t>(__get_h_errno());
    service_ptr[i] = reinterpret_cast<uintptr_t>(service);
    ready.fetch_add(1);
    while (!release.load()) std::this_thread::yield();
    okay = okay && h_errno == 700 + i && HostRow("thread", host) == before
        && ServiceShape(service, port, "tcp");
    okay = okay && gethostbyname("300.0.0.1") == nullptr && h_errno == HOST_NOT_FOUND;
    thread_ok[i] = okay;
  });
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
  while (ready.load() != 4 && std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
  check(ready.load() == 4, "four actual native threads reached the held-result/error-state barrier");
  gethostbyname("127.0.0.9");
  h_errno = 999;
  release.store(true);
  for (auto& thread : threads) thread.join();
  bool distinct = true;
  for (int i = 0; i < 4; ++i) {
    distinct = distinct && thread_ok[i];
    for (int j = 0; j < i; ++j) distinct = distinct && host_ptr[i] != host_ptr[j]
        && error_ptr[i] != error_ptr[j] && service_ptr[i] != service_ptr[j];
  }
  check(distinct && h_errno == 999, "host/service/error TLS remains distinct and stable during concurrent lookups/failures");
  if (oracle != nullptr) check(allowed && compared == 8 && expected.size() == 8,
      "all eight independent Windows host/service rows were consumed");
  out << "net4 resolver: " << (failures == 0 ? "all passed" : "FAILED")
      << "; allowed=" << (allowed ? 1 : 0) << "; checks=" << checks << "; oracle=" << compared << '\n';
  return out.str();
}
}  // namespace net4resolver
