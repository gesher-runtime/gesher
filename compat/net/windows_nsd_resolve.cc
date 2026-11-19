// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
// Binary-preserving DNS-SD query, advertising and subscription transport. The
// Java worker owns socket I/O and cleanup; subscriber callbacks additionally
// record real per-interface continuity under their separate state mutex.
#define FD_SETSIZE 128
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mswsock.h>
#include <windows.h>
#include <iphlpapi.h>
#include <netioapi.h>
#include <gesher/builtin_library.h>
#include <gesher/inet.h>
#include <gesher/nsd_subscription_lifetimes.h>
#include <jni.h>
#include <nativehelper/jni_macros.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <climits>
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <type_traits>
#include <vector>

namespace {
// The guest POSIX layer exports these names with Linux signatures. A Winsock
// import-library member also defines the callable thunk alongside __imp_*, so
// direct imports can collide with (or bypass) that guest ABI. Resolve the real
// Windows entry points like compat/posix/src/winsock.cc and bluetooth_win.cc.
struct WinsockApi {
  decltype(&::bind) bind = nullptr;
  decltype(&::sendto) sendto = nullptr;
  decltype(&::setsockopt) setsockopt = nullptr;
  decltype(&::select) select = nullptr;
  DWORD error = 0;
};
const WinsockApi& winsock_api() {
  static const WinsockApi api = [] {
    WinsockApi value;
    // WSA* imports already keep ws2_32 loaded. Retain any fallback load for the
    // process lifetime too: active and retiring requests can still use it.
    HMODULE module = GetModuleHandleW(L"ws2_32.dll");
    if (!module) module = LoadLibraryExW(L"ws2_32.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!module) {
      value.error = GetLastError();
      if (!value.error) value.error = ERROR_MOD_NOT_FOUND;
      return value;
    }
    auto load = [&](auto& function, const char* name) {
      FARPROC proc = GetProcAddress(module, name);
      if (!proc) {
        value.error = GetLastError();
        if (!value.error) value.error = ERROR_PROC_NOT_FOUND;
        return false;
      }
      function = reinterpret_cast<std::remove_reference_t<decltype(function)>>(
          reinterpret_cast<void*>(proc));
      return true;
    };
    load(value.bind, "bind") && load(value.sendto, "sendto") &&
        load(value.setsockopt, "setsockopt") && load(value.select, "select");
    return value;
  }();
  return api;
}

constexpr size_t kMaxRequests = 200;
constexpr size_t kMaxInterfaces = 64;
constexpr size_t kMaxEnumeratedInterfaces = 1024;
constexpr size_t kMaxSockets = 512;
constexpr size_t kMaxPacket = 65535;
constexpr size_t kMaxAdapterBytes = 4 * 1024 * 1024;
constexpr size_t kMaxAddressesPerInterface = 64;
constexpr size_t kMaxAddresses = 4096;
constexpr size_t kMaxObservedEndpoints = 1024;
constexpr size_t kMaxBurstDatagrams = 1024;
constexpr size_t kMaxBurstBytes = 8 * 1024 * 1024;
constexpr unsigned char kGroup4[] = {224, 0, 0, 251};
constexpr unsigned char kGroup6[] = {0xff, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xfb};

struct Adapter {
  ULONG index = 0, ipv4_index = 0, ipv6_index = 0;
  ULONG families = 0;
  uint64_t luid = 0, generation = 0;
  ULONG mtu4 = 0, mtu6 = 0;
  bool operator==(const Adapter&) const = default;
};
struct Address {
  Adapter adapter;
  int family = 0;
  std::array<unsigned char, 16> bytes{};
  int prefix_length = 0, flags = 0, prefix_origin = 0, suffix_origin = 0, dad_state = 0;
  int skip_as_source = -1;
};
struct Channel {
  SOCKET socket = INVALID_SOCKET;
  Adapter adapter;
  int family = 0;
  ULONG interface_index = 0;
  LPFN_WSARECVMSG receive = nullptr;
  std::vector<Address> addresses;
};
enum class Mode : unsigned { Resolver, Advertiser, Subscriber };
struct SubscriptionState {
  std::mutex lock;
  gesher::nsd::SubscriptionSnapshotFence fence;
  gesher::nsd::SubscriptionLifetimes lifetimes;
};
struct Endpoint {
  uint64_t luid = 0, generation = 0;
  ULONG index = 0, interface_index = 0;
  int family = 0;
  USHORT port = 0;
  std::array<unsigned char, 16> bytes{};
  bool operator==(const Endpoint&) const = default;
};
struct Request {
  explicit Request(Mode value) : mode(value), subscription(value == Mode::Subscriber
      ? std::make_unique<SubscriptionState>() : nullptr) {}
  const Mode mode;
  const std::unique_ptr<SubscriptionState> subscription;
  std::mutex lock;
  std::mutex retirement_lock;
  std::atomic<uint64_t> generation{1};
  std::atomic<bool> retiring{false};
  // Bounded request-owned progress diagnostics: registry allocation alone does
  // not prove that a scope was reconciled, a poll completed or a query was sent.
  std::atomic<bool> completed_poll{false}, sent_query{false};
  std::atomic<uint64_t> sent_datagrams{0};
  DWORD initialization_error = 0;
  bool winsock = false;
  HANDLE interface_watch = nullptr, address_watch = nullptr;
  std::vector<Channel> channels;
  size_t next_channel = 0;
  // LRU observations, not a protocol timer. Only delivered attributed opcode-0
  // queries authorize unicast output on the same current interface generation.
  std::vector<Endpoint> observed_queries;
};
struct Counters {
  std::atomic<size_t> sockets{0}, watches{0}, callbacks{0};
};
struct Registry {
  std::mutex lock;
  std::map<uintptr_t, std::shared_ptr<Request>> requests;
  uintptr_t next_id = 1;
  std::atomic<size_t> sockets{0}; // Shared real socket budget across all providers.
  Counters modes[3];
};
// Only the empty registry shell is process-owned. A failed cancellation keeps
// its bounded request entry until a worker observes actual cancellation success.
Registry& registry() { static auto* value = new Registry; return *value; }
std::shared_ptr<Request> find(uintptr_t id) {
  std::lock_guard<std::mutex> guard(registry().lock);
  auto it = registry().requests.find(id);
  return it == registry().requests.end() ? nullptr : it->second;
}
std::shared_ptr<Request> find_mode(uintptr_t id, Mode mode) {
  auto request = find(id);
  return request && request->mode == mode ? request : nullptr;
}
Counters& counters(const Request& request) {
  return registry().modes[static_cast<unsigned>(request.mode)];
}

void changed(void* context) {
  auto request = find(reinterpret_cast<uintptr_t>(context));
  if (!request) return;
  ++counters(*request).callbacks;
  // No JNI, application reference, socket access or worker lock in a callback.
  // A loss and return with the same index still advances this generation.
  request->generation.fetch_add(1);
  --counters(*request).callbacks;
}
void CALLBACK interface_changed(void* context, PMIB_IPINTERFACE_ROW, MIB_NOTIFICATION_TYPE) {
  changed(context);
}
void CALLBACK address_changed(void* context, PMIB_UNICASTIPADDRESS_ROW, MIB_NOTIFICATION_TYPE) {
  changed(context);
}

// A callback owns only the request's small continuity state. Its lease begins
// before it waits for that state, so a concurrent snapshot cannot overlook it.
struct SubscriptionObservation {
  std::shared_ptr<Request> request;
  explicit SubscriptionObservation(void* context)
      : request(find_mode(reinterpret_cast<uintptr_t>(context), Mode::Subscriber)) {
    if (!request) return;
    ++counters(*request).callbacks;
    request->subscription->fence.begin(request->generation);
  }
  ~SubscriptionObservation() {
    if (!request) return;
    request->subscription->fence.end(request->generation);
    --counters(*request).callbacks;
  }
};
unsigned subscription_family(ADDRESS_FAMILY family) {
  return family == AF_INET ? 1 : family == AF_INET6 ? 2 : 0;
}

// The callback row contains identifiers only. Query real complete operational
// state; neither Connected nor any other partial-row state is trusted.
bool subscription_interface(Request& request, uint64_t luid, ULONG index,
    ADDRESS_FAMILY family, ULONG* current_index) {
  auto& observed = request.subscription->lifetimes;
  MIB_IF_ROW2 actual{};
  actual.InterfaceLuid.Value = luid;
  actual.InterfaceIndex = index;
  DWORD error = GetIfEntry2(&actual);
  if (error == ERROR_FILE_NOT_FOUND) { observed.interface_absent(luid); return false; }
  if (error || actual.InterfaceLuid.Value != luid) { observed.unavailable(luid); return false; }
  if (actual.OperStatus != IfOperStatusUp || actual.Type == IF_TYPE_SOFTWARE_LOOPBACK) {
    observed.interface_absent(luid);
    return false;
  }
  MIB_IPINTERFACE_ROW ip{};
  InitializeIpInterfaceEntry(&ip);
  ip.Family = family;
  ip.InterfaceLuid.Value = luid;
  ip.InterfaceIndex = index;
  error = GetIpInterfaceEntry(&ip);
  if (error == ERROR_FILE_NOT_FOUND) { observed.interface_absent(luid); return false; }
  if (error == ERROR_NOT_FOUND) {
    observed.interface_changed(luid, subscription_family(family), index, false);
    return false;
  }
  if (error || ip.InterfaceLuid.Value != luid || ip.Family != family || !ip.InterfaceIndex) {
    observed.unavailable(luid);
    return false;
  }
  *current_index = ip.InterfaceIndex;
  observed.interface_changed(luid, subscription_family(family), ip.InterfaceIndex, true);
  return true;
}

void CALLBACK subscription_interface_changed(void* context, PMIB_IPINTERFACE_ROW row,
    MIB_NOTIFICATION_TYPE type) {
  SubscriptionObservation lease(context);
  const auto& request = lease.request;
  if (!request || request->retiring || !row || type == MibInitialNotification) return;
  std::lock_guard<std::mutex> guard(request->subscription->lock);
  auto& observed = request->subscription->lifetimes;
  const uint64_t luid = row->InterfaceLuid.Value;
  if (!luid) { observed.unavailable(); return; }
  if (!observed.contains(luid)) return; // no earlier lifetime exists to preserve
  unsigned family = subscription_family(row->Family);
  if (!family || !row->InterfaceIndex) { observed.unavailable(luid); return; }
  if (type == MibDeleteInstance) {
    observed.interface_changed(luid, family, row->InterfaceIndex, false);
    return;
  }
  if (type != MibAddInstance && type != MibParameterNotification) {
    observed.unavailable(luid);
    return;
  }
  ULONG current_index = 0;
  subscription_interface(*request, luid, row->InterfaceIndex, row->Family, &current_index);
}

void CALLBACK subscription_address_changed(void* context, PMIB_UNICASTIPADDRESS_ROW row,
    MIB_NOTIFICATION_TYPE type) {
  SubscriptionObservation lease(context);
  const auto& request = lease.request;
  if (!request || request->retiring || !row || type == MibInitialNotification) return;
  std::lock_guard<std::mutex> guard(request->subscription->lock);
  auto& observed = request->subscription->lifetimes;
  const uint64_t luid = row->InterfaceLuid.Value;
  if (!luid) { observed.unavailable(); return; }
  if (!observed.contains(luid)) return;
  ADDRESS_FAMILY family = row->Address.si_family;
  gesher::nsd::SubscriptionLifetimes::Address address;
  address.family = subscription_family(family);
  if (!address.family || !row->InterfaceIndex) { observed.unavailable(luid); return; }
  if (family == AF_INET) std::memcpy(address.bytes.data(), &row->Address.Ipv4.sin_addr, 4);
  else std::memcpy(address.bytes.data(), &row->Address.Ipv6.sin6_addr, 16);
  if (type == MibDeleteInstance) {
    observed.address_changed(luid, row->InterfaceIndex, address, false);
    return;
  }
  if (type != MibAddInstance && type != MibParameterNotification) {
    observed.unavailable(luid);
    return;
  }
  ULONG current_index = 0;
  if (!subscription_interface(*request, luid, row->InterfaceIndex, family, &current_index)) return;
  MIB_UNICASTIPADDRESS_ROW actual{};
  InitializeUnicastIpAddressEntry(&actual);
  actual.InterfaceLuid.Value = luid;
  actual.InterfaceIndex = current_index;
  actual.Address = row->Address;
  DWORD error = GetUnicastIpAddressEntry(&actual);
  if (error == ERROR_FILE_NOT_FOUND) { observed.interface_absent(luid); return; }
  if (error == ERROR_NOT_FOUND) {
    observed.address_changed(luid, current_index, address, false);
    return;
  }
  if (error || actual.InterfaceLuid.Value != luid || actual.InterfaceIndex != current_index) {
    observed.unavailable(luid);
    return;
  }
  observed.address_changed(luid, current_index, address,
      actual.DadState == IpDadStatePreferred || actual.DadState == IpDadStateDeprecated);
}

void throw_error(JNIEnv* env, const char* type, const std::string& message) {
  jclass cls = env->FindClass(type);
  if (cls) { env->ThrowNew(cls, message.c_str()); env->DeleteLocalRef(cls); }
}
void io_error(JNIEnv* env, const char* operation, DWORD error) {
  throw_error(env, "java/io/IOException", std::string(operation) + " (Windows error " + std::to_string(error) + ")");
}
bool internet(JNIEnv* env) {
  if (gesher_inet_allowed()) return true;
  throw_error(env, "java/lang/SecurityException", "NsdService requires android.permission.INTERNET");
  return false;
}
bool usable(JNIEnv* env, const std::shared_ptr<Request>& request) {
  if (!request || request->retiring) {
    io_error(env, "DNS-SD request is closed", ERROR_INVALID_HANDLE); return false;
  }
  if (request->initialization_error) {
    io_error(env, "Cannot initialize DNS-SD transport", request->initialization_error); return false;
  }
  return true;
}

DWORD interface_mtu(const Adapter& adapter, int family, ULONG* mtu) {
  MIB_IPINTERFACE_ROW row{};
  InitializeIpInterfaceEntry(&row);
  row.Family = static_cast<ADDRESS_FAMILY>(family);
  row.InterfaceLuid.Value = adapter.luid;
  DWORD error = GetIpInterfaceEntry(&row);
  if (error) return error;
  ULONG expected = family == AF_INET ? adapter.ipv4_index : adapter.ipv6_index;
  if (row.InterfaceLuid.Value != adapter.luid || row.InterfaceIndex != expected) return ERROR_RETRY;
  *mtu = row.NlMtu;
  return ERROR_SUCCESS;
}
DWORD adapters(std::vector<Adapter>* out, uint64_t generation, bool with_mtu = false,
    std::vector<Address>* address_rows = nullptr) {
  ULONG bytes = 16384;
  std::vector<unsigned char> data;
  DWORD error = ERROR_BUFFER_OVERFLOW;
  for (int attempt = 0; attempt < 4 && error == ERROR_BUFFER_OVERFLOW; ++attempt) {
    if (bytes > kMaxAdapterBytes) return ERROR_NOT_ENOUGH_MEMORY;
    data.resize(bytes);
    error = GetAdaptersAddresses(AF_UNSPEC, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST |
        GAA_FLAG_SKIP_DNS_SERVER, nullptr, reinterpret_cast<IP_ADAPTER_ADDRESSES*>(data.data()), &bytes);
  }
  if (error == ERROR_NO_DATA) return ERROR_SUCCESS;
  if (error != ERROR_SUCCESS) return error;
  for (auto* value = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(data.data()); value; value = value->Next) {
    if (value->OperStatus != IfOperStatusUp || value->IfType == IF_TYPE_SOFTWARE_LOOPBACK ||
        (value->Flags & IP_ADAPTER_NO_MULTICAST)) continue;
    Adapter row;
    // This is WindowsNetworks' canonical adapter index, not necessarily the
    // distinct IPv6 index required for memberships, packet metadata and scope.
    row.index = value->IfIndex ? value->IfIndex : value->Ipv6IfIndex;
    if (!row.index || row.index > 0x7fffff9bUL) continue;
    row.ipv4_index = value->IfIndex; row.ipv6_index = value->Ipv6IfIndex;
    row.luid = value->Luid.Value; row.generation = generation;
    std::vector<Address> owned_addresses;
    for (auto* address = value->FirstUnicastAddress; address; address = address->Next) {
      if (!address->Address.lpSockaddr || (address->DadState != IpDadStatePreferred &&
          address->DadState != IpDadStateDeprecated)) continue;
      if (address->Address.lpSockaddr->sa_family == AF_INET && row.ipv4_index) row.families |= 1;
      if (address->Address.lpSockaddr->sa_family == AF_INET6 && row.ipv6_index) row.families |= 2;
      if (!address_rows) continue;
      int family = address->Address.lpSockaddr->sa_family;
      if ((family != AF_INET && family != AF_INET6) ||
          (family == AF_INET ? !row.ipv4_index : !row.ipv6_index)) continue;
      if (owned_addresses.size() >= kMaxAddressesPerInterface ||
          address_rows->size() + owned_addresses.size() >= kMaxAddresses) return ERROR_NOT_ENOUGH_MEMORY;
      Address owned;
      owned.family = family;
      owned.prefix_length = address->OnLinkPrefixLength;
      owned.flags = static_cast<int>(address->Flags);
      owned.prefix_origin = address->PrefixOrigin; owned.suffix_origin = address->SuffixOrigin;
      owned.dad_state = address->DadState;
      MIB_UNICASTIPADDRESS_ROW detailed{};
      InitializeUnicastIpAddressEntry(&detailed);
      detailed.InterfaceLuid.Value = row.luid;
      if (family == AF_INET) {
        if (address->Address.iSockaddrLength < static_cast<int>(sizeof(sockaddr_in))) return ERROR_INVALID_DATA;
        const auto* source = reinterpret_cast<const sockaddr_in*>(address->Address.lpSockaddr);
        std::memcpy(owned.bytes.data(), &source->sin_addr, 4);
        detailed.Address.Ipv4 = *source;
      } else {
        if (address->Address.iSockaddrLength < static_cast<int>(sizeof(sockaddr_in6))) return ERROR_INVALID_DATA;
        const auto* source = reinterpret_cast<const sockaddr_in6*>(address->Address.lpSockaddr);
        std::memcpy(owned.bytes.data(), &source->sin6_addr, 16);
        detailed.Address.Ipv6 = *source;
      }
      // Raw Windows state remains raw. Neither TRANSIENT nor a random suffix is
      // relabeled as Android/Linux temporary-address privacy policy.
      if (GetUnicastIpAddressEntry(&detailed) == NO_ERROR) owned.skip_as_source = detailed.SkipAsSource ? 1 : 0;
      owned_addresses.push_back(owned);
    }
    if (with_mtu) {
      if (row.families & 1) interface_mtu(row, AF_INET, &row.mtu4);
      if (row.families & 2) interface_mtu(row, AF_INET6, &row.mtu6);
      // A failed lookup leaves an explicitly unknown zero, never a default MTU.
    }
    for (auto& address : owned_addresses) {
      address.adapter = row;
      address_rows->push_back(address);
    }
    if (row.families) out->push_back(row);
    if (out->size() > kMaxEnumeratedInterfaces) return ERROR_NOT_ENOUGH_MEMORY;
  }
  std::sort(out->begin(), out->end(), [](const auto& a, const auto& b) { return a.index < b.index; });
  return ERROR_SUCCESS;
}

bool close_channels(Request* request) {
  bool success = true;
  for (auto& channel : request->channels) {
    if (channel.socket == INVALID_SOCKET) continue;
    if (closesocket(channel.socket) == 0) {
      channel.socket = INVALID_SOCKET; --registry().sockets; --counters(*request).sockets;
    } else success = false;
  }
  if (success) { request->channels.clear(); request->next_channel = 0; }
  request->observed_queries.clear();
  return success;
}
bool option(SOCKET socket, int level, int name, const void* value, int length) {
  return winsock_api().setsockopt(socket, level, name, static_cast<const char*>(value), length) == 0;
}
DWORD open_channel(Request* request, const Adapter& adapter, int family,
    const std::vector<Address>& addresses = {}) {
  size_t count = registry().sockets.load();
  do {
    if (count >= kMaxSockets) return ERROR_NOT_ENOUGH_MEMORY;
  } while (!registry().sockets.compare_exchange_weak(count, count + 1));
  SOCKET socket = WSASocketW(family, SOCK_DGRAM, IPPROTO_UDP, nullptr, 0, 0);
  if (socket == INVALID_SOCKET) { --registry().sockets; return WSAGetLastError(); }
  ++counters(*request).sockets;
  // Store immediately so every subsequent partial-failure path has an owner.
  request->channels.push_back({socket, adapter, family,
      family == AF_INET ? adapter.ipv4_index : adapter.ipv6_index, nullptr, {}});
  Channel& channel = request->channels.back();
  for (const auto& address : addresses) {
    if (address.adapter == adapter && address.family == family) channel.addresses.push_back(address);
  }
  BOOL yes = TRUE;
  DWORD hops = 255, index = channel.interface_index;
  DWORD fragment = IP_PMTUDISC_DONT;
  if (!option(socket, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes))) return WSAGetLastError();
  if (family == AF_INET6 && !option(socket, IPPROTO_IPV6, IPV6_V6ONLY, &yes, sizeof(yes))) return WSAGetLastError();
  ULONG nonblocking = 1;
  if (ioctlsocket(socket, FIONBIO, &nonblocking) != 0) return WSAGetLastError();
  GROUP_REQ membership{};
  membership.gr_interface = index;
  int level = family == AF_INET ? IPPROTO_IP : IPPROTO_IPV6;
  if (family == AF_INET) {
    sockaddr_in bound{}; bound.sin_family = AF_INET; bound.sin_port = htons(5353);
    if (winsock_api().bind(socket, reinterpret_cast<sockaddr*>(&bound), sizeof(bound)) != 0) return WSAGetLastError();
    auto* group = reinterpret_cast<sockaddr_in*>(&membership.gr_group);
    group->sin_family = AF_INET; std::memcpy(&group->sin_addr, kGroup4, sizeof(kGroup4));
    DWORD network_index = htonl(index);
    if (!option(socket, level, IP_MULTICAST_IF, &network_index, sizeof(network_index)) ||
        !option(socket, level, IP_MULTICAST_TTL, &hops, sizeof(hops)) ||
        !option(socket, level, IP_MULTICAST_LOOP, &yes, sizeof(yes)) ||
        !option(socket, level, IP_PKTINFO, &yes, sizeof(yes)) ||
        !option(socket, level, IP_RECVTTL, &yes, sizeof(yes))) return WSAGetLastError();
    if (request->mode == Mode::Advertiser &&
        (!option(socket, level, IP_UNICAST_IF, &network_index, sizeof(network_index)) ||
         !option(socket, level, IP_TTL, &hops, sizeof(hops)) ||
         !option(socket, level, IP_MTU_DISCOVER, &fragment, sizeof(fragment)))) return WSAGetLastError();
  } else {
    sockaddr_in6 bound{}; bound.sin6_family = AF_INET6; bound.sin6_port = htons(5353);
    if (winsock_api().bind(socket, reinterpret_cast<sockaddr*>(&bound), sizeof(bound)) != 0) return WSAGetLastError();
    auto* group = reinterpret_cast<sockaddr_in6*>(&membership.gr_group);
    group->sin6_family = AF_INET6; group->sin6_scope_id = index;
    std::memcpy(&group->sin6_addr, kGroup6, sizeof(kGroup6));
    if (!option(socket, level, IPV6_MULTICAST_IF, &index, sizeof(index)) ||
        !option(socket, level, IPV6_MULTICAST_HOPS, &hops, sizeof(hops)) ||
        !option(socket, level, IPV6_MULTICAST_LOOP, &yes, sizeof(yes)) ||
        !option(socket, level, IPV6_PKTINFO, &yes, sizeof(yes)) ||
        !option(socket, level, IPV6_HOPLIMIT, &yes, sizeof(yes))) return WSAGetLastError();
    if (request->mode == Mode::Advertiser &&
        (!option(socket, level, IPV6_UNICAST_IF, &index, sizeof(index)) ||
         !option(socket, level, IPV6_UNICAST_HOPS, &hops, sizeof(hops)) ||
         !option(socket, level, IPV6_MTU_DISCOVER, &fragment, sizeof(fragment)))) return WSAGetLastError();
  }
  if (!option(socket, level, MCAST_JOIN_GROUP, &membership, sizeof(membership))) return WSAGetLastError();
  GUID guid = WSAID_WSARECVMSG;
  DWORD returned = 0;
  if (WSAIoctl(socket, SIO_GET_EXTENSION_FUNCTION_POINTER, &guid, sizeof(guid),
      &channel.receive, sizeof(channel.receive), &returned, nullptr, nullptr) != 0) return WSAGetLastError();
  return ERROR_SUCCESS;
}

jlong open_mode(JNIEnv* env, Mode mode) {
  if (!internet(env)) return 0;
  auto request = std::make_shared<Request>(mode);
  uintptr_t id;
  {
    std::lock_guard<std::mutex> guard(registry().lock);
    if (registry().requests.size() >= kMaxRequests || registry().next_id == 0) {
      io_error(env, "DNS-SD native request budget exceeded", ERROR_NOT_ENOUGH_MEMORY); return 0;
    }
    id = registry().next_id++;
    registry().requests.emplace(id, request);
  }
  request->initialization_error = winsock_api().error;
  if (!request->initialization_error) {
    WSADATA data{};
    request->initialization_error = WSAStartup(MAKEWORD(2, 2), &data);
    if (!request->initialization_error) request->winsock = true;
  }
  if (!request->initialization_error) {
    request->initialization_error = NotifyIpInterfaceChange(AF_UNSPEC,
        mode == Mode::Subscriber ? subscription_interface_changed : interface_changed,
        reinterpret_cast<void*>(id), FALSE, &request->interface_watch);
    if (!request->initialization_error) ++counters(*request).watches;
  }
  if (!request->initialization_error) {
    request->initialization_error = NotifyUnicastIpAddressChange(AF_UNSPEC,
        mode == Mode::Subscriber ? subscription_address_changed : address_changed,
        reinterpret_cast<void*>(id), FALSE, &request->address_watch);
    if (!request->initialization_error) ++counters(*request).watches;
  }
  // Even a partially initialized request is returned as an owned handle. Its
  // first operation reports the error; the Java finally block then closes it.
  return static_cast<jlong>(id);
}
jlong open_request(JNIEnv* env, jclass) { return open_mode(env, Mode::Resolver); }
jlong advertiser_open(JNIEnv* env, jclass) { return open_mode(env, Mode::Advertiser); }
jlong subscription_open(JNIEnv* env, jclass) { return open_mode(env, Mode::Subscriber); }
jlongArray subscription_interfaces(JNIEnv* env, jclass, jlong id) {
  if (!internet(env)) return nullptr;
  auto request = find_mode(static_cast<uintptr_t>(id), Mode::Subscriber);
  if (!usable(env, request)) return nullptr;
  std::lock_guard<std::mutex> guard(request->lock);
  if (!usable(env, request)) return nullptr;
  auto& state = *request->subscription;
  uint64_t generation = request->generation.load();
  if (!state.fence.stable(request->generation, generation)) return nullptr;
  std::vector<Adapter> rows;
  std::vector<Address> addresses;
  DWORD error = adapters(&rows, generation, false, &addresses);
  if (!state.fence.stable(request->generation, generation)) return nullptr;
  if (error) { io_error(env, "Cannot observe DNS-SD subscription scopes", error); return nullptr; }
  using Lifetimes = gesher::nsd::SubscriptionLifetimes;
  std::vector<Lifetimes::Scope> scopes;
  for (const auto& row : rows) {
    Lifetimes::Scope scope{row.luid, row.index, row.ipv4_index, row.ipv6_index, row.families, {}};
    for (const auto& address : addresses) {
      if (address.adapter == row) {
        scope.addresses.push_back({subscription_family(static_cast<ADDRESS_FAMILY>(address.family)),
            address.bytes});
      }
    }
    scopes.push_back(std::move(scope));
  }
  std::vector<uint64_t> lifetimes;
  Lifetimes::Result result;
  {
    std::lock_guard<std::mutex> observation(state.lock);
    if (!state.fence.stable(request->generation, generation)) return nullptr;
    result = state.lifetimes.snapshot(scopes, &lifetimes);
  }
  if (!state.fence.stable(request->generation, generation)) return nullptr;
  if (result == Lifetimes::Result::Retry) return nullptr;
  if (result != Lifetimes::Result::Ready) {
    error = result == Lifetimes::Result::Capacity ? ERROR_NOT_ENOUGH_MEMORY
        : result == Lifetimes::Result::Exhausted ? ERROR_ARITHMETIC_OVERFLOW : ERROR_NOT_READY;
    io_error(env, "DNS-SD subscription continuity is unavailable", error);
    return nullptr;
  }
  std::vector<jlong> values;
  for (size_t at = 0; at < rows.size(); ++at) {
    const auto& row = rows[at];
    values.insert(values.end(), {row.index, row.ipv4_index, row.ipv6_index,
        static_cast<jlong>(row.luid), static_cast<jlong>(row.generation), row.families,
        static_cast<jlong>(lifetimes[at])});
  }
  auto out = env->NewLongArray(static_cast<jsize>(values.size()));
  if (out && !values.empty()) env->SetLongArrayRegion(out, 0, values.size(), values.data());
  return out;
}
jlongArray interfaces_mode(JNIEnv* env, jlong id, Mode mode) {
  if (!internet(env)) return nullptr;
  auto request = find_mode(static_cast<uintptr_t>(id), mode);
  if (!usable(env, request)) return nullptr;
  std::lock_guard<std::mutex> guard(request->lock);
  if (!usable(env, request)) return nullptr;
  uint64_t generation = request->generation.load();
  std::vector<Adapter> rows;
  DWORD error = adapters(&rows, generation, mode == Mode::Advertiser);
  if (error) { io_error(env, "Cannot enumerate DNS-SD interfaces", error); return nullptr; }
  std::vector<jlong> values;
  for (const auto& row : rows) {
    values.insert(values.end(), {row.index, row.ipv4_index, row.ipv6_index,
        static_cast<jlong>(row.luid), static_cast<jlong>(row.generation), row.families});
    if (mode == Mode::Advertiser) values.insert(values.end(), {row.mtu4, row.mtu6});
  }
  auto out = env->NewLongArray(static_cast<jsize>(values.size()));
  if (out && !values.empty()) env->SetLongArrayRegion(out, 0, values.size(), values.data());
  return out;
}
jlongArray interfaces(JNIEnv* env, jclass, jlong id) { return interfaces_mode(env, id, Mode::Resolver); }
jlongArray advertiser_interfaces(JNIEnv* env, jclass, jlong id) {
  return interfaces_mode(env, id, Mode::Advertiser);
}
jobjectArray advertiser_addresses(JNIEnv* env, jclass, jlong id) {
  if (!internet(env)) return nullptr;
  auto request = find_mode(static_cast<uintptr_t>(id), Mode::Advertiser);
  if (!usable(env, request)) return nullptr;
  std::lock_guard<std::mutex> guard(request->lock);
  if (!usable(env, request)) return nullptr;
  std::vector<Adapter> rows;
  std::vector<Address> addresses;
  DWORD error = adapters(&rows, request->generation.load(), true, &addresses);
  if (error) { io_error(env, "Cannot enumerate DNS-SD addresses", error); return nullptr; }
  jclass cls = env->FindClass("com/gesher/net/WindowsNsdAdvertiser$Address");
  if (!cls) return nullptr;
  jmethodID constructor = env->GetMethodID(cls, "<init>", "(JII[BIIIIII)V");
  auto out = constructor ? env->NewObjectArray(addresses.size(), cls, nullptr) : nullptr;
  for (size_t i = 0; out && i < addresses.size() && !env->ExceptionCheck(); ++i) {
    const auto& address = addresses[i];
    jsize size = address.family == AF_INET ? 4 : 16;
    auto bytes = env->NewByteArray(size);
    if (!bytes) break;
    env->SetByteArrayRegion(bytes, 0, size, reinterpret_cast<const jbyte*>(address.bytes.data()));
    jobject value = !env->ExceptionCheck() ? env->NewObject(cls, constructor,
        static_cast<jlong>(address.adapter.generation), static_cast<jint>(address.adapter.index),
        address.family == AF_INET ? 4 : 6, bytes, address.prefix_length, address.flags,
        address.prefix_origin, address.suffix_origin, address.dad_state, address.skip_as_source) : nullptr;
    if (value) { env->SetObjectArrayElement(out, i, value); env->DeleteLocalRef(value); }
    env->DeleteLocalRef(bytes);
  }
  env->DeleteLocalRef(cls);
  return out;
}
jboolean configure_mode(JNIEnv* env, jlong id, jlongArray input, Mode mode) {
  if (!internet(env)) return false;
  auto request = find_mode(static_cast<uintptr_t>(id), mode);
  if (!usable(env, request)) return false;
  const size_t columns = mode == Mode::Advertiser ? 8 : 6;
  if (!input || env->GetArrayLength(input) % columns ||
      env->GetArrayLength(input) > static_cast<jsize>(kMaxInterfaces * columns)) {
    io_error(env, "Invalid DNS-SD interface snapshot", ERROR_INVALID_PARAMETER); return false;
  }
  std::vector<jlong> values(env->GetArrayLength(input));
  if (!values.empty()) env->GetLongArrayRegion(input, 0, values.size(), values.data());
  if (env->ExceptionCheck()) return false;
  std::lock_guard<std::mutex> guard(request->lock);
  if (!usable(env, request)) return false;
  if (!close_channels(request.get())) {
    io_error(env, "Cannot retire old DNS-SD sockets", WSAGetLastError()); return false;
  }
  uint64_t generation = request->generation.load();
  std::vector<Adapter> actual;
  std::vector<Address> addresses;
  DWORD error = adapters(&actual, generation, mode == Mode::Advertiser,
      mode == Mode::Advertiser ? &addresses : nullptr);
  if (error) { io_error(env, "Cannot verify DNS-SD interface snapshot", error); return false; }
  std::vector<Adapter> selected;
  for (size_t at = 0; at < values.size(); at += columns) {
    Adapter row{static_cast<ULONG>(values[at]), static_cast<ULONG>(values[at + 1]),
        static_cast<ULONG>(values[at + 2]), static_cast<ULONG>(values[at + 5]),
        static_cast<uint64_t>(values[at + 3]), static_cast<uint64_t>(values[at + 4])};
    if (mode == Mode::Advertiser) {
      for (size_t field : {size_t{0}, size_t{1}, size_t{2}, size_t{5}, size_t{6}, size_t{7}}) {
        if (values[at + field] < 0 || values[at + field] > 0xffffffffLL) {
          io_error(env, "Invalid DNS-SD interface field", ERROR_INVALID_PARAMETER); return false;
        }
      }
      row.mtu4 = static_cast<ULONG>(values[at + 6]); row.mtu6 = static_cast<ULONG>(values[at + 7]);
    }
    if (std::find(actual.begin(), actual.end(), row) == actual.end()) return false;
    if (std::find(selected.begin(), selected.end(), row) != selected.end()) {
      io_error(env, "Duplicate DNS-SD interface", ERROR_INVALID_PARAMETER); return false;
    }
    selected.push_back(row);
  }
  if (request->generation.load() != generation) return false;
  for (const auto& row : selected) {
    if (row.families & 1) error = open_channel(request.get(), row, AF_INET, addresses);
    if (!error && (row.families & 2)) error = open_channel(request.get(), row, AF_INET6, addresses);
    if (error) {
      close_channels(request.get());
      if (request->generation.load() != generation) return false;
      io_error(env, "Cannot open DNS-SD multicast socket", error); return false;
    }
  }
  if (request->generation.load() != generation) { close_channels(request.get()); return false; }
  return true;
}
jboolean configure(JNIEnv* env, jclass, jlong id, jlongArray input) {
  return configure_mode(env, id, input, Mode::Resolver);
}
jboolean advertiser_configure(JNIEnv* env, jclass, jlong id, jlongArray input) {
  return configure_mode(env, id, input, Mode::Advertiser);
}
jboolean subscription_configure(JNIEnv* env, jclass, jlong id, jlongArray input) {
  return configure_mode(env, id, input, Mode::Subscriber);
}
jlong generation_mode(JNIEnv* env, jlong id, Mode mode) {
  if (!internet(env)) return -1;
  auto request = find_mode(static_cast<uintptr_t>(id), mode);
  if (!usable(env, request)) return -1;
  return static_cast<jlong>(request->generation.load());
}
jlong generation(JNIEnv* env, jclass, jlong id) { return generation_mode(env, id, Mode::Resolver); }
jlong advertiser_generation(JNIEnv* env, jclass, jlong id) { return generation_mode(env, id, Mode::Advertiser); }
jlong subscription_generation(JNIEnv* env, jclass, jlong id) { return generation_mode(env, id, Mode::Subscriber); }
jboolean send(JNIEnv* env, jclass, jlong id, jint index, jbyteArray input) {
  if (!internet(env)) return false;
  auto request = find_mode(static_cast<uintptr_t>(id), Mode::Resolver);
  if (!usable(env, request)) return false;
  if (!input || env->GetArrayLength(input) < 12 || env->GetArrayLength(input) > 1500) {
    io_error(env, "Invalid DNS-SD query size", ERROR_INVALID_PARAMETER); return false;
  }
  std::vector<char> bytes(env->GetArrayLength(input));
  env->GetByteArrayRegion(input, 0, bytes.size(), reinterpret_cast<jbyte*>(bytes.data()));
  if (env->ExceptionCheck()) return false;
  std::lock_guard<std::mutex> guard(request->lock);
  if (!usable(env, request)) return false;
  bool sent = false;
  for (const auto& channel : request->channels) {
    if (channel.adapter.index != static_cast<ULONG>(index)) continue;
    if (channel.adapter.generation != request->generation.load()) return false;
    SOCKADDR_STORAGE storage{};
    int length;
    if (channel.family == AF_INET) {
      auto* target = reinterpret_cast<sockaddr_in*>(&storage);
      target->sin_family = AF_INET; target->sin_port = htons(5353);
      std::memcpy(&target->sin_addr, kGroup4, sizeof(kGroup4)); length = sizeof(*target);
    } else {
      auto* target = reinterpret_cast<sockaddr_in6*>(&storage);
      target->sin6_family = AF_INET6; target->sin6_port = htons(5353);
      target->sin6_scope_id = channel.interface_index;
      std::memcpy(&target->sin6_addr, kGroup6, sizeof(kGroup6)); length = sizeof(*target);
    }
    int result = winsock_api().sendto(channel.socket, bytes.data(), bytes.size(), 0,
        reinterpret_cast<sockaddr*>(&storage), length);
    if (result != static_cast<int>(bytes.size())) {
      DWORD error = WSAGetLastError();
      if (request->generation.load() != channel.adapter.generation || error == WSAEWOULDBLOCK) return false;
      io_error(env, "Cannot send DNS-SD query", error); return false;
    }
    sent = true;
    request->sent_query = true;
  }
  return sent;
}

jboolean subscription_send(JNIEnv* env, jclass, jlong id, jint index, jbyteArray input) {
  if (!internet(env)) return false;
  auto request = find_mode(static_cast<uintptr_t>(id), Mode::Subscriber);
  if (!usable(env, request)) return false;
  if (!input || env->GetArrayLength(input) < 12 || env->GetArrayLength(input) > 1500) {
    io_error(env, "Invalid DNS-SD subscription query size", ERROR_INVALID_PARAMETER); return false;
  }
  std::vector<char> bytes(env->GetArrayLength(input));
  env->GetByteArrayRegion(input, 0, bytes.size(), reinterpret_cast<jbyte*>(bytes.data()));
  if (env->ExceptionCheck()) return false;
  std::lock_guard<std::mutex> guard(request->lock);
  if (!usable(env, request)) return false;
  uint64_t generation = request->generation.load();
  unsigned expected = 0, present = 0;
  for (const auto& channel : request->channels) {
    if (channel.adapter.index != static_cast<ULONG>(index)) continue;
    unsigned family = subscription_family(static_cast<ADDRESS_FAMILY>(channel.family));
    if (!family || channel.socket == INVALID_SOCKET || channel.adapter.generation != generation ||
        (expected && expected != channel.adapter.families) || (present & family)) return false;
    expected = channel.adapter.families;
    present |= family;
  }
  // Successful transmission means every actual configured family was sent.
  // No channel or a missing family cannot be reported as success.
  if (!expected || present != expected) return false;
  unsigned sent = 0;
  for (const auto& channel : request->channels) {
    if (channel.adapter.index != static_cast<ULONG>(index)) continue;
    if (request->retiring || request->generation.load() != generation) return false;
    SOCKADDR_STORAGE storage{};
    int length;
    if (channel.family == AF_INET) {
      auto* target = reinterpret_cast<sockaddr_in*>(&storage);
      target->sin_family = AF_INET; target->sin_port = htons(5353);
      std::memcpy(&target->sin_addr, kGroup4, sizeof(kGroup4)); length = sizeof(*target);
    } else {
      auto* target = reinterpret_cast<sockaddr_in6*>(&storage);
      target->sin6_family = AF_INET6; target->sin6_port = htons(5353);
      target->sin6_scope_id = channel.interface_index;
      std::memcpy(&target->sin6_addr, kGroup6, sizeof(kGroup6)); length = sizeof(*target);
    }
    int result = winsock_api().sendto(channel.socket, bytes.data(), bytes.size(), 0,
        reinterpret_cast<sockaddr*>(&storage), length);
    if (result != static_cast<int>(bytes.size())) {
      DWORD error = result == SOCKET_ERROR ? WSAGetLastError() : WSAEMSGSIZE;
      if (request->generation.load() != generation || error == WSAEWOULDBLOCK) return false;
      io_error(env, "Cannot send DNS-SD subscription query", error); return false;
    }
    ++request->sent_datagrams;
    sent |= subscription_family(static_cast<ADDRESS_FAMILY>(channel.family));
  }
  return sent == expected && !request->retiring && request->generation.load() == generation;
}

bool unicast_address(const std::array<unsigned char, 16>& bytes, int family) {
  if (family == AF_INET) return bytes[0] != 0 && bytes[0] != 127 && bytes[0] < 224;
  if (bytes[0] == 0xff) return false;
  bool first_ten_zero = std::all_of(bytes.begin(), bytes.begin() + 10, [](auto b) { return b == 0; });
  if (first_ten_zero && bytes[10] == 0xff && bytes[11] == 0xff) return false;
  bool first_fifteen_zero = std::all_of(bytes.begin(), bytes.begin() + 15, [](auto b) { return b == 0; });
  return !first_fifteen_zero || bytes[15] > 1;
}
bool link_local6(const std::array<unsigned char, 16>& bytes) {
  return bytes[0] == 0xfe && (bytes[1] & 0xc0) == 0x80;
}
bool on_link(const Channel& channel, const std::array<unsigned char, 16>& bytes) {
  // RFC6762 5.5/11: direct unicast requires an actual local subnet/prefix.
  // Multicast receive deliberately uses a separate path for overlaid subnets.
  int limit = channel.family == AF_INET ? 32 : 128;
  for (const auto& address : channel.addresses) {
    int prefix = address.prefix_length;
    if (prefix < 0 || prefix > limit) continue; // Windows can report 255 unknown.
    size_t whole = static_cast<size_t>(prefix / 8);
    if (!std::equal(bytes.begin(), bytes.begin() + whole, address.bytes.begin())) continue;
    int remaining = prefix % 8;
    if (!remaining || ((bytes[whole] ^ address.bytes[whole]) & (0xff << (8 - remaining))) == 0) return true;
  }
  return false;
}
Endpoint endpoint(const Channel& channel, const std::array<unsigned char, 16>& bytes, USHORT port) {
  return {channel.adapter.luid, channel.adapter.generation, channel.adapter.index,
      channel.interface_index, channel.family, port, bytes};
}
void remember_query(Request* request, const Endpoint& value) {
  auto& observed = request->observed_queries;
  auto existing = std::find(observed.begin(), observed.end(), value);
  if (existing != observed.end()) observed.erase(existing);
  else if (observed.size() == kMaxObservedEndpoints) observed.erase(observed.begin());
  observed.push_back(value);
}
jlong burst_result(uint32_t sent, DWORD error) {
  return static_cast<jlong>((static_cast<uint64_t>(sent) << 32) | static_cast<uint32_t>(error));
}
uint32_t resource_records(const std::vector<char>& packet) {
  auto word = [&](size_t at) {
    return (static_cast<uint32_t>(static_cast<unsigned char>(packet[at])) << 8)
        | static_cast<unsigned char>(packet[at + 1]);
  };
  return word(6) + word(8) + word(10);
}
jlong advertiser_send(JNIEnv* env, jclass, jlong id, jint index, jlong generation,
    jint family_value, jbyteArray destination, jint port, jobjectArray input) {
  if (!internet(env)) return 0;
  auto request = find_mode(static_cast<uintptr_t>(id), Mode::Advertiser);
  if (!usable(env, request)) return 0;
  int family = family_value == 4 ? AF_INET : family_value == 6 ? AF_INET6 : AF_UNSPEC;
  jsize address_size = family == AF_INET ? 4 : 16;
  if (family == AF_UNSPEC || !destination || env->GetArrayLength(destination) != address_size ||
      index <= 0 || port <= 0 || port > 65535 || !input || env->GetArrayLength(input) <= 0 ||
      env->GetArrayLength(input) > static_cast<jsize>(kMaxBurstDatagrams)) {
    io_error(env, "Invalid DNS-SD advertisement burst", ERROR_INVALID_PARAMETER); return 0;
  }
  std::array<unsigned char, 16> target_bytes{};
  env->GetByteArrayRegion(destination, 0, address_size, reinterpret_cast<jbyte*>(target_bytes.data()));
  if (env->ExceptionCheck()) return 0;
  bool multicast = std::memcmp(target_bytes.data(), family == AF_INET ? kGroup4 : kGroup6,
      address_size) == 0;
  if ((multicast && port != 5353) || (!multicast && !unicast_address(target_bytes, family))) {
    io_error(env, "Invalid DNS-SD destination", ERROR_INVALID_PARAMETER); return 0;
  }
  const size_t headers = family == AF_INET ? 28 : 48;
  std::vector<std::vector<char>> datagrams;
  size_t total = 0;
  jsize count = env->GetArrayLength(input);
  datagrams.reserve(count);
  for (jsize i = 0; i < count; ++i) {
    auto value = static_cast<jbyteArray>(env->GetObjectArrayElement(input, i));
    if (env->ExceptionCheck()) return 0;
    jsize size = value ? env->GetArrayLength(value) : 0;
    if (size < 12 || static_cast<size_t>(size) > 9000 - headers ||
        static_cast<size_t>(size) > kMaxBurstBytes - total) {
      if (value) env->DeleteLocalRef(value);
      io_error(env, "DNS-SD datagram capacity exceeded", WSAEMSGSIZE); return 0;
    }
    datagrams.emplace_back(size);
    env->GetByteArrayRegion(value, 0, size, reinterpret_cast<jbyte*>(datagrams.back().data()));
    env->DeleteLocalRef(value);
    if (env->ExceptionCheck()) return 0;
    total += size;
  }
  std::lock_guard<std::mutex> guard(request->lock);
  if (!usable(env, request)) return 0;
  auto found = std::find_if(request->channels.begin(), request->channels.end(), [&](const auto& channel) {
    return channel.adapter.index == static_cast<ULONG>(index) && channel.family == family;
  });
  if (found == request->channels.end() || found->socket == INVALID_SOCKET) {
    io_error(env, "Missing DNS-SD advertisement channel", ERROR_NOT_FOUND); return 0;
  }
  const auto& channel = *found;
  if (channel.adapter.generation != static_cast<uint64_t>(generation) ||
      request->generation.load() != static_cast<uint64_t>(generation)) return burst_result(0, ERROR_RETRY);
  Endpoint target = endpoint(channel, target_bytes, static_cast<USHORT>(port));
  if (!multicast && std::find(request->observed_queries.begin(), request->observed_queries.end(), target)
      == request->observed_queries.end()) return burst_result(0, WSAEACCES);
  if (!multicast && !on_link(channel, target_bytes)) return burst_result(0, WSAEADDRNOTAVAIL);
  ULONG mtu = 0;
  DWORD error = interface_mtu(channel.adapter, family, &mtu);
  if (error) { io_error(env, "Cannot verify DNS-SD interface MTU", error); return 0; }
  if (!mtu || mtu <= headers) {
    io_error(env, "Unknown or unusable DNS-SD interface MTU", ERROR_INVALID_DATA); return 0;
  }
  if (mtu != (family == AF_INET ? channel.adapter.mtu4 : channel.adapter.mtu6)) return burst_result(0, ERROR_RETRY);
  size_t budget = std::min<size_t>(mtu, 9000) - headers;
  // Inspect header counts only. Java owns every record, compression dictionary
  // and layout. Validate the entire burst before its first actual send.
  for (const auto& packet : datagrams) {
    if (packet.size() > budget && resource_records(packet) > 1) {
      io_error(env, "Multiple DNS-SD records exceed actual MTU", WSAEMSGSIZE); return 0;
    }
  }
  SOCKADDR_STORAGE storage{};
  int target_length;
  if (family == AF_INET) {
    auto* to = reinterpret_cast<sockaddr_in*>(&storage);
    to->sin_family = AF_INET; to->sin_port = htons(static_cast<USHORT>(port));
    std::memcpy(&to->sin_addr, target_bytes.data(), 4); target_length = sizeof(*to);
  } else {
    auto* to = reinterpret_cast<sockaddr_in6*>(&storage);
    to->sin6_family = AF_INET6; to->sin6_port = htons(static_cast<USHORT>(port));
    if (multicast || link_local6(target_bytes)) to->sin6_scope_id = channel.interface_index;
    std::memcpy(&to->sin6_addr, target_bytes.data(), 16); target_length = sizeof(*to);
  }
  uint32_t sent = 0;
  for (const auto& packet : datagrams) {
    if (request->retiring) return burst_result(sent, ERROR_OPERATION_ABORTED);
    if (request->generation.load() != static_cast<uint64_t>(generation)) return burst_result(sent, ERROR_RETRY);
    ULONG current_mtu = 0;
    error = interface_mtu(channel.adapter, family, &current_mtu);
    if (error || current_mtu != mtu) return burst_result(sent, error ? error : ERROR_RETRY);
    int result = winsock_api().sendto(channel.socket, packet.data(), packet.size(), 0,
        reinterpret_cast<sockaddr*>(&storage), target_length);
    if (result != static_cast<int>(packet.size())) {
      error = result == SOCKET_ERROR ? WSAGetLastError() : WSAEMSGSIZE;
      return burst_result(sent, error ? error : WSAEFAULT);
    }
    ++sent;
    ++request->sent_datagrams;
  }
  return burst_result(sent, request->generation.load() == static_cast<uint64_t>(generation)
      && !request->retiring ? ERROR_SUCCESS : ERROR_RETRY);
}

jobject poll_mode(JNIEnv* env, jlong id, jint timeout, Mode mode) {
  if (!internet(env)) return nullptr;
  auto request = find_mode(static_cast<uintptr_t>(id), mode);
  if (!usable(env, request)) return nullptr;
  std::lock_guard<std::mutex> guard(request->lock);
  if (!usable(env, request)) return nullptr;
  timeout = std::clamp(timeout, 0, 50);
  if (request->channels.empty()) {
    if (timeout) Sleep(timeout);
    request->completed_poll = true;
    return nullptr;
  }
  fd_set sockets; FD_ZERO(&sockets);
  for (const auto& channel : request->channels) {
    if (channel.adapter.generation != request->generation.load()) { if (timeout) Sleep(timeout); return nullptr; }
    FD_SET(channel.socket, &sockets);
  }
  timeval wait{0, timeout * 1000};
  int ready = winsock_api().select(0, &sockets, nullptr, nullptr, &wait);
  if (ready == SOCKET_ERROR) {
    io_error(env, "Cannot wait for DNS-SD packets", WSAGetLastError()); return nullptr;
  }
  request->completed_poll = true;
  if (!ready) return nullptr;
  // A packet never enters an unbounded native queue. Round-robin selection
  // prevents a busy interface from starving another active scope.
  std::vector<char> bytes(kMaxPacket);
  for (size_t step = 0; step < request->channels.size(); ++step) {
    size_t at = (request->next_channel + step) % request->channels.size();
    const auto& channel = request->channels[at];
    if (!FD_ISSET(channel.socket, &sockets)) continue;
    SOCKADDR_STORAGE source{};
    alignas(WSACMSGHDR) char control[WSA_CMSG_SPACE(sizeof(IN6_PKTINFO)) + WSA_CMSG_SPACE(sizeof(DWORD)) + 128]{};
    WSABUF buffer{static_cast<ULONG>(bytes.size()), bytes.data()};
    WSAMSG message{};
    message.name = reinterpret_cast<sockaddr*>(&source); message.namelen = sizeof(source);
    message.lpBuffers = &buffer; message.dwBufferCount = 1;
    message.Control.buf = control; message.Control.len = sizeof(control);
    DWORD received = 0;
    int result = channel.receive(channel.socket, &message, &received, nullptr, nullptr);
    if (result == SOCKET_ERROR) {
      DWORD error = WSAGetLastError();
      if (error == WSAEWOULDBLOCK || error == WSAEMSGSIZE) continue;
      if (request->generation.load() != channel.adapter.generation) return nullptr;
      io_error(env, "Cannot receive DNS-SD packet", error); return nullptr;
    }
    if (received < 12 || received > kMaxPacket || (message.dwFlags & (MSG_TRUNC | MSG_CTRUNC))) continue;
    if (source.ss_family != channel.family) continue;
    USHORT port = channel.family == AF_INET ? reinterpret_cast<sockaddr_in*>(&source)->sin_port
        : reinterpret_cast<sockaddr_in6*>(&source)->sin6_port;
    USHORT source_port = ntohs(port);
    bool response = (static_cast<unsigned char>(bytes[2]) & 0x80) != 0;
    if ((mode != Mode::Advertiser || response) && source_port != 5353) continue;
    if (mode == Mode::Advertiser && source_port == 0) continue;
    ULONG incoming_index = 0; DWORD hops = 0; bool multicast = false, hop_known = false;
    std::array<unsigned char, 16> received_destination{}, source_address{};
    if (channel.family == AF_INET) {
      if (message.namelen < static_cast<int>(sizeof(sockaddr_in))) continue;
      std::memcpy(source_address.data(), &reinterpret_cast<sockaddr_in*>(&source)->sin_addr, 4);
    } else {
      if (message.namelen < static_cast<int>(sizeof(sockaddr_in6))) continue;
      auto* endpoint = reinterpret_cast<sockaddr_in6*>(&source);
      std::memcpy(source_address.data(), &endpoint->sin6_addr, 16);
      if (mode == Mode::Advertiser && link_local6(source_address) && endpoint->sin6_scope_id &&
          endpoint->sin6_scope_id != channel.interface_index) continue;
    }
    if (mode == Mode::Advertiser && !unicast_address(source_address, channel.family)) continue;
    for (auto* header = WSA_CMSG_FIRSTHDR(&message); header; header = WSA_CMSG_NXTHDR(&message, header)) {
      if (channel.family == AF_INET && header->cmsg_level == IPPROTO_IP && header->cmsg_type == IP_PKTINFO &&
          header->cmsg_len >= WSA_CMSG_LEN(sizeof(IN_PKTINFO))) {
        IN_PKTINFO info{}; std::memcpy(&info, WSA_CMSG_DATA(header), sizeof(info));
        incoming_index = info.ipi_ifindex;
        multicast = std::memcmp(&info.ipi_addr, kGroup4, sizeof(kGroup4)) == 0;
        std::memcpy(received_destination.data(), &info.ipi_addr, 4);
      } else if (channel.family == AF_INET6 && header->cmsg_level == IPPROTO_IPV6 && header->cmsg_type == IPV6_PKTINFO &&
          header->cmsg_len >= WSA_CMSG_LEN(sizeof(IN6_PKTINFO))) {
        IN6_PKTINFO info{}; std::memcpy(&info, WSA_CMSG_DATA(header), sizeof(info));
        incoming_index = info.ipi6_ifindex;
        multicast = std::memcmp(&info.ipi6_addr, kGroup6, sizeof(kGroup6)) == 0;
        std::memcpy(received_destination.data(), &info.ipi6_addr, 16);
      } else if (((header->cmsg_level == IPPROTO_IP &&
          (header->cmsg_type == IP_TTL || header->cmsg_type == IP_HOPLIMIT)) ||
          (header->cmsg_level == IPPROTO_IPV6 && header->cmsg_type == IPV6_HOPLIMIT)) &&
          header->cmsg_len >= WSA_CMSG_LEN(sizeof(DWORD))) {
        std::memcpy(&hops, WSA_CMSG_DATA(header), sizeof(hops));
        hop_known = true;
      }
    }
    if (incoming_index != channel.interface_index || channel.adapter.generation != request->generation.load()) continue;
    if (mode != Mode::Advertiser) {
      if (!multicast || hops != 255) continue;
    } else {
      if (multicast && (!hop_known || hops != 255)) continue;
      if (!multicast && std::none_of(channel.addresses.begin(), channel.addresses.end(), [&](const auto& address) {
        return address.bytes == received_destination;
      })) continue;
      if (!multicast && !on_link(channel, source_address)) continue;
    }
    request->next_channel = (at + 1) % request->channels.size();
    auto payload = env->NewByteArray(received);
    if (!payload) return nullptr;
    env->SetByteArrayRegion(payload, 0, received, reinterpret_cast<const jbyte*>(bytes.data()));
    jclass cls = env->FindClass(mode != Mode::Advertiser ? "com/gesher/net/WindowsNsdResolver$Packet"
        : "com/gesher/net/WindowsNsdAdvertiser$Packet");
    jmethodID constructor = cls ? env->GetMethodID(cls, "<init>", mode != Mode::Advertiser
        ? "(JIII[B)V" : "(JIII[BIZI[B)V") : nullptr;
    jobject out = nullptr;
    if (constructor && !env->ExceptionCheck()) {
      if (mode != Mode::Advertiser) out = env->NewObject(cls, constructor,
          static_cast<jlong>(channel.adapter.generation), static_cast<jint>(channel.adapter.index),
          channel.family == AF_INET ? 4 : 6, static_cast<jint>(incoming_index), payload);
      else {
        jsize address_size = channel.family == AF_INET ? 4 : 16;
        auto address = env->NewByteArray(address_size);
        if (address) {
          env->SetByteArrayRegion(address, 0, address_size, reinterpret_cast<const jbyte*>(source_address.data()));
          if (!env->ExceptionCheck()) out = env->NewObject(cls, constructor,
              static_cast<jlong>(channel.adapter.generation), static_cast<jint>(channel.adapter.index),
              channel.family == AF_INET ? 4 : 6, static_cast<jint>(incoming_index), address,
              static_cast<jint>(source_port), static_cast<jboolean>(multicast),
              hop_known ? static_cast<jint>(hops) : -1, payload);
          env->DeleteLocalRef(address);
        }
        if (out && (channel.adapter.generation != request->generation.load() || request->retiring)) {
          env->DeleteLocalRef(out); out = nullptr;
        }
        if (out && !env->ExceptionCheck() && !response &&
            (static_cast<unsigned char>(bytes[2]) & 0x78) == 0) {
          remember_query(request.get(), endpoint(channel, source_address, source_port));
        }
      }
    }
    if (mode == Mode::Subscriber && out &&
        (channel.adapter.generation != request->generation.load() || request->retiring)) {
      env->DeleteLocalRef(out); out = nullptr;
    }
    if (cls) env->DeleteLocalRef(cls);
    env->DeleteLocalRef(payload);
    return out;
  }
  return nullptr;
}
jobject poll(JNIEnv* env, jclass, jlong id, jint timeout) { return poll_mode(env, id, timeout, Mode::Resolver); }
jobject advertiser_poll(JNIEnv* env, jclass, jlong id, jint timeout) {
  return poll_mode(env, id, timeout, Mode::Advertiser);
}
jobject subscription_poll(JNIEnv* env, jclass, jlong id, jint timeout) {
  return poll_mode(env, id, timeout, Mode::Subscriber);
}

jboolean close_mode(jlong id, Mode mode) {
  auto request = find(static_cast<uintptr_t>(id));
  if (!request) return true;
  if (request->mode != mode) return false;
  request->retiring = true;
  // Several callers can request cancellation, but handles and WSAStartup's
  // matching cleanup are retired once. Callbacks never take this monitor.
  std::lock_guard<std::mutex> retiring(request->retirement_lock);
  bool closed;
  {
    std::lock_guard<std::mutex> guard(request->lock);
    closed = close_channels(request.get());
  }
  // Never call cancellation from the callback or under a lock it could need.
  // Registry leases also keep callbacks that already entered alive across erase.
  if (mode == Mode::Subscriber) {
    bool watches = gesher::nsd::retire_subscription_watches(
        request->interface_watch, request->address_watch,
        [](HANDLE watch) { return CancelMibChangeNotify2(watch) == NO_ERROR; },
        [&] { --counters(*request).watches; });
    closed = watches && closed;
  } else {
    if (request->interface_watch) {
      if (CancelMibChangeNotify2(request->interface_watch) == NO_ERROR) {
        request->interface_watch = nullptr; --counters(*request).watches;
      } else closed = false;
    }
    if (request->address_watch) {
      if (CancelMibChangeNotify2(request->address_watch) == NO_ERROR) {
        request->address_watch = nullptr; --counters(*request).watches;
      } else closed = false;
    }
  }
  if (!closed) return false;
  if (request->winsock) {
    if (WSACleanup() != 0) return false;
    request->winsock = false;
  }
  std::lock_guard<std::mutex> guard(registry().lock);
  registry().requests.erase(static_cast<uintptr_t>(id));
  return true;
}
jboolean close_request(JNIEnv*, jclass, jlong id) { return close_mode(id, Mode::Resolver); }
jboolean advertiser_close(JNIEnv*, jclass, jlong id) { return close_mode(id, Mode::Advertiser); }
jboolean subscription_close(JNIEnv*, jclass, jlong id) { return close_mode(id, Mode::Subscriber); }
jintArray active_mode(JNIEnv* env, Mode mode) {
  std::lock_guard<std::mutex> guard(registry().lock);
  jint owned = 0, polled = 0;
  uint64_t sent = 0;
  for (const auto& [id, request] : registry().requests) {
    if (request->mode != mode) continue;
    ++owned;
    polled += request->completed_poll.load() ? 1 : 0;
    sent += mode == Mode::Resolver ? (request->sent_query.load() ? 1 : 0)
        : request->sent_datagrams.load();
  }
  const auto& count = registry().modes[static_cast<unsigned>(mode)];
  // Same six-field shape; resolver progress retains its original boolean-per-
  // request semantics. Advertiser/subscriber field 5 counts completed datagrams of live
  // requests (saturating at jint's limit), not planned bursts or callbacks.
  jint values[] = {owned, static_cast<jint>(count.sockets.load()), static_cast<jint>(count.watches.load()),
      static_cast<jint>(count.callbacks.load()), polled, static_cast<jint>(std::min<uint64_t>(sent, INT_MAX))};
  auto out = env->NewIntArray(6);
  if (out) env->SetIntArrayRegion(out, 0, 6, values);
  return out;
}
jintArray active(JNIEnv* env, jclass) { return active_mode(env, Mode::Resolver); }
jintArray advertiser_active(JNIEnv* env, jclass) { return active_mode(env, Mode::Advertiser); }
jintArray subscription_active(JNIEnv* env, jclass) { return active_mode(env, Mode::Subscriber); }

const JNINativeMethod kMethods[] = {
  {"nativeOpen", "()J", NATIVEHELPER_SYSV_JNI_ENTRY(open_request)},
  {"nativeInterfaces", "(J)[J", NATIVEHELPER_SYSV_JNI_ENTRY(interfaces)},
  {"nativeConfigure", "(J[J)Z", NATIVEHELPER_SYSV_JNI_ENTRY(configure)},
  {"nativeGeneration", "(J)J", NATIVEHELPER_SYSV_JNI_ENTRY(generation)},
  {"nativePoll", "(JI)Lcom/gesher/net/WindowsNsdResolver$Packet;", NATIVEHELPER_SYSV_JNI_ENTRY(poll)},
  {"nativeSend", "(JI[B)Z", NATIVEHELPER_SYSV_JNI_ENTRY(send)},
  {"nativeClose", "(J)Z", NATIVEHELPER_SYSV_JNI_ENTRY(close_request)},
  {"nativeActive", "()[I", NATIVEHELPER_SYSV_JNI_ENTRY(active)},
};
const JNINativeMethod kSubscriptionMethods[] = {
  {"nativeSubscriptionOpen", "()J", NATIVEHELPER_SYSV_JNI_ENTRY(subscription_open)},
  {"nativeSubscriptionInterfaces", "(J)[J", NATIVEHELPER_SYSV_JNI_ENTRY(subscription_interfaces)},
  {"nativeSubscriptionConfigure", "(J[J)Z", NATIVEHELPER_SYSV_JNI_ENTRY(subscription_configure)},
  {"nativeSubscriptionGeneration", "(J)J", NATIVEHELPER_SYSV_JNI_ENTRY(subscription_generation)},
  {"nativeSubscriptionPoll", "(JI)Lcom/gesher/net/WindowsNsdResolver$Packet;", NATIVEHELPER_SYSV_JNI_ENTRY(subscription_poll)},
  {"nativeSubscriptionSend", "(JI[B)Z", NATIVEHELPER_SYSV_JNI_ENTRY(subscription_send)},
  {"nativeSubscriptionClose", "(J)Z", NATIVEHELPER_SYSV_JNI_ENTRY(subscription_close)},
  {"nativeSubscriptionActive", "()[I", NATIVEHELPER_SYSV_JNI_ENTRY(subscription_active)},
};
const JNINativeMethod kAdvertiserMethods[] = {
  {"nativeOpen", "()J", NATIVEHELPER_SYSV_JNI_ENTRY(advertiser_open)},
  {"nativeInterfaces", "(J)[J", NATIVEHELPER_SYSV_JNI_ENTRY(advertiser_interfaces)},
  {"nativeAddresses", "(J)[Lcom/gesher/net/WindowsNsdAdvertiser$Address;", NATIVEHELPER_SYSV_JNI_ENTRY(advertiser_addresses)},
  {"nativeConfigure", "(J[J)Z", NATIVEHELPER_SYSV_JNI_ENTRY(advertiser_configure)},
  {"nativeGeneration", "(J)J", NATIVEHELPER_SYSV_JNI_ENTRY(advertiser_generation)},
  {"nativePoll", "(JI)Lcom/gesher/net/WindowsNsdAdvertiser$Packet;", NATIVEHELPER_SYSV_JNI_ENTRY(advertiser_poll)},
  {"nativeSend", "(JIJI[BI[[B)J", NATIVEHELPER_SYSV_JNI_ENTRY(advertiser_send)},
  {"nativeClose", "(J)Z", NATIVEHELPER_SYSV_JNI_ENTRY(advertiser_close)},
  {"nativeActive", "()[I", NATIVEHELPER_SYSV_JNI_ENTRY(advertiser_active)},
};
jint on_load(JavaVM* vm, void*) {
  JNIEnv* env = nullptr;
  if (vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK) return JNI_ERR;
  jclass cls = env->FindClass("com/gesher/net/WindowsNsdResolver");
  if (!cls) return JNI_ERR;
  int status = env->RegisterNatives(cls, kMethods, sizeof(kMethods) / sizeof(kMethods[0]));
  if (status == JNI_OK) status = env->RegisterNatives(cls, kSubscriptionMethods,
      sizeof(kSubscriptionMethods) / sizeof(kSubscriptionMethods[0]));
  env->DeleteLocalRef(cls);
  if (status != JNI_OK) return JNI_ERR;
  cls = env->FindClass("com/gesher/net/WindowsNsdAdvertiser");
  if (!cls) return JNI_ERR;
  status = env->RegisterNatives(cls, kAdvertiserMethods,
      sizeof(kAdvertiserMethods) / sizeof(kAdvertiserMethods[0]));
  env->DeleteLocalRef(cls);
  return status == JNI_OK ? JNI_VERSION_1_6 : JNI_ERR;
}
const gesher_builtin_symbol kSymbols[] = {{"JNI_OnLoad", reinterpret_cast<void*>(&on_load)}};
const bool registered = [] {
  gesher_register_builtin_library("libgesher_nsd_resolve.so", kSymbols, 1);
  return true;
}();
}  // namespace
