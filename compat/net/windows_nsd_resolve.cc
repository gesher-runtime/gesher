// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
// Binary-preserving DNS-SD query transport. Windows callbacks only invalidate
// interface generations; the Java worker owns socket I/O and terminal cleanup.
#define FD_SETSIZE 128
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mswsock.h>
#include <windows.h>
#include <iphlpapi.h>
#include <netioapi.h>
#include <gesher/builtin_library.h>
#include <gesher/inet.h>
#include <jni.h>
#include <nativehelper/jni_macros.h>

#include <algorithm>
#include <atomic>
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
constexpr unsigned char kGroup4[] = {224, 0, 0, 251};
constexpr unsigned char kGroup6[] = {0xff, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xfb};

struct Adapter {
  ULONG index = 0, ipv4_index = 0, ipv6_index = 0;
  ULONG families = 0;
  uint64_t luid = 0, generation = 0;
  bool operator==(const Adapter&) const = default;
};
struct Channel {
  SOCKET socket = INVALID_SOCKET;
  Adapter adapter;
  int family = 0;
  ULONG interface_index = 0;
  LPFN_WSARECVMSG receive = nullptr;
};
struct Request {
  std::mutex lock;
  std::atomic<uint64_t> generation{1};
  std::atomic<bool> retiring{false};
  // Bounded request-owned progress diagnostics: registry allocation alone does
  // not prove that a scope was reconciled, a poll completed or a query was sent.
  std::atomic<bool> completed_poll{false}, sent_query{false};
  DWORD initialization_error = 0;
  bool winsock = false;
  HANDLE interface_watch = nullptr, address_watch = nullptr;
  std::vector<Channel> channels;
  size_t next_channel = 0;
};
struct Registry {
  std::mutex lock;
  std::map<uintptr_t, std::shared_ptr<Request>> requests;
  uintptr_t next_id = 1;
  std::atomic<size_t> sockets{0}, watches{0}, callbacks{0};
};
// Only the empty registry shell is process-owned. A failed cancellation keeps
// its bounded request entry until a worker observes actual cancellation success.
Registry& registry() { static auto* value = new Registry; return *value; }
std::shared_ptr<Request> find(uintptr_t id) {
  std::lock_guard<std::mutex> guard(registry().lock);
  auto it = registry().requests.find(id);
  return it == registry().requests.end() ? nullptr : it->second;
}

void changed(void* context) {
  auto request = find(reinterpret_cast<uintptr_t>(context));
  if (!request) return;
  ++registry().callbacks;
  // No JNI, application reference, socket access or worker lock in a callback.
  // A loss and return with the same index still advances this generation.
  request->generation.fetch_add(1);
  --registry().callbacks;
}
void CALLBACK interface_changed(void* context, PMIB_IPINTERFACE_ROW, MIB_NOTIFICATION_TYPE) {
  changed(context);
}
void CALLBACK address_changed(void* context, PMIB_UNICASTIPADDRESS_ROW, MIB_NOTIFICATION_TYPE) {
  changed(context);
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

DWORD adapters(std::vector<Adapter>* out, uint64_t generation) {
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
    for (auto* address = value->FirstUnicastAddress; address; address = address->Next) {
      if (!address->Address.lpSockaddr || (address->DadState != IpDadStatePreferred &&
          address->DadState != IpDadStateDeprecated)) continue;
      if (address->Address.lpSockaddr->sa_family == AF_INET && row.ipv4_index) row.families |= 1;
      if (address->Address.lpSockaddr->sa_family == AF_INET6 && row.ipv6_index) row.families |= 2;
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
      channel.socket = INVALID_SOCKET; --registry().sockets;
    } else success = false;
  }
  if (success) { request->channels.clear(); request->next_channel = 0; }
  return success;
}
bool option(SOCKET socket, int level, int name, const void* value, int length) {
  return winsock_api().setsockopt(socket, level, name, static_cast<const char*>(value), length) == 0;
}
DWORD open_channel(Request* request, const Adapter& adapter, int family) {
  size_t count = registry().sockets.load();
  do {
    if (count >= kMaxSockets) return ERROR_NOT_ENOUGH_MEMORY;
  } while (!registry().sockets.compare_exchange_weak(count, count + 1));
  SOCKET socket = WSASocketW(family, SOCK_DGRAM, IPPROTO_UDP, nullptr, 0, 0);
  if (socket == INVALID_SOCKET) { --registry().sockets; return WSAGetLastError(); }
  // Store immediately so every subsequent partial-failure path has an owner.
  request->channels.push_back({socket, adapter, family,
      family == AF_INET ? adapter.ipv4_index : adapter.ipv6_index, nullptr});
  Channel& channel = request->channels.back();
  BOOL yes = TRUE;
  DWORD hops = 255, index = channel.interface_index;
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
  }
  if (!option(socket, level, MCAST_JOIN_GROUP, &membership, sizeof(membership))) return WSAGetLastError();
  GUID guid = WSAID_WSARECVMSG;
  DWORD returned = 0;
  if (WSAIoctl(socket, SIO_GET_EXTENSION_FUNCTION_POINTER, &guid, sizeof(guid),
      &channel.receive, sizeof(channel.receive), &returned, nullptr, nullptr) != 0) return WSAGetLastError();
  return ERROR_SUCCESS;
}

jlong open_request(JNIEnv* env, jclass) {
  if (!internet(env)) return 0;
  auto request = std::make_shared<Request>();
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
    request->initialization_error = NotifyIpInterfaceChange(AF_UNSPEC, interface_changed,
        reinterpret_cast<void*>(id), FALSE, &request->interface_watch);
    if (!request->initialization_error) ++registry().watches;
  }
  if (!request->initialization_error) {
    request->initialization_error = NotifyUnicastIpAddressChange(AF_UNSPEC, address_changed,
        reinterpret_cast<void*>(id), FALSE, &request->address_watch);
    if (!request->initialization_error) ++registry().watches;
  }
  // Even a partially initialized request is returned as an owned handle. Its
  // first operation reports the error; the Java finally block then closes it.
  return static_cast<jlong>(id);
}
jlongArray interfaces(JNIEnv* env, jclass, jlong id) {
  if (!internet(env)) return nullptr;
  auto request = find(static_cast<uintptr_t>(id));
  if (!usable(env, request)) return nullptr;
  std::lock_guard<std::mutex> guard(request->lock);
  uint64_t generation = request->generation.load();
  std::vector<Adapter> rows;
  DWORD error = adapters(&rows, generation);
  if (error) { io_error(env, "Cannot enumerate DNS-SD interfaces", error); return nullptr; }
  std::vector<jlong> values;
  for (const auto& row : rows) {
    values.insert(values.end(), {row.index, row.ipv4_index, row.ipv6_index,
        static_cast<jlong>(row.luid), static_cast<jlong>(row.generation), row.families});
  }
  auto out = env->NewLongArray(static_cast<jsize>(values.size()));
  if (out && !values.empty()) env->SetLongArrayRegion(out, 0, values.size(), values.data());
  return out;
}
jboolean configure(JNIEnv* env, jclass, jlong id, jlongArray input) {
  if (!internet(env)) return false;
  auto request = find(static_cast<uintptr_t>(id));
  if (!usable(env, request)) return false;
  if (!input || env->GetArrayLength(input) % 6 || env->GetArrayLength(input) > static_cast<jsize>(kMaxInterfaces * 6)) {
    io_error(env, "Invalid DNS-SD interface snapshot", ERROR_INVALID_PARAMETER); return false;
  }
  std::vector<jlong> values(env->GetArrayLength(input));
  if (!values.empty()) env->GetLongArrayRegion(input, 0, values.size(), values.data());
  if (env->ExceptionCheck()) return false;
  std::lock_guard<std::mutex> guard(request->lock);
  if (!close_channels(request.get())) {
    io_error(env, "Cannot retire old DNS-SD sockets", WSAGetLastError()); return false;
  }
  uint64_t generation = request->generation.load();
  std::vector<Adapter> actual;
  DWORD error = adapters(&actual, generation);
  if (error) { io_error(env, "Cannot verify DNS-SD interface snapshot", error); return false; }
  std::vector<Adapter> selected;
  for (size_t at = 0; at < values.size(); at += 6) {
    Adapter row{static_cast<ULONG>(values[at]), static_cast<ULONG>(values[at + 1]),
        static_cast<ULONG>(values[at + 2]), static_cast<ULONG>(values[at + 5]),
        static_cast<uint64_t>(values[at + 3]), static_cast<uint64_t>(values[at + 4])};
    if (std::find(actual.begin(), actual.end(), row) == actual.end()) return false;
    if (std::find(selected.begin(), selected.end(), row) != selected.end()) {
      io_error(env, "Duplicate DNS-SD interface", ERROR_INVALID_PARAMETER); return false;
    }
    selected.push_back(row);
  }
  if (request->generation.load() != generation) return false;
  for (const auto& row : selected) {
    if (row.families & 1) error = open_channel(request.get(), row, AF_INET);
    if (!error && (row.families & 2)) error = open_channel(request.get(), row, AF_INET6);
    if (error) {
      close_channels(request.get());
      if (request->generation.load() != generation) return false;
      io_error(env, "Cannot open DNS-SD multicast socket", error); return false;
    }
  }
  if (request->generation.load() != generation) { close_channels(request.get()); return false; }
  return true;
}
jlong generation(JNIEnv* env, jclass, jlong id) {
  if (!internet(env)) return -1;
  auto request = find(static_cast<uintptr_t>(id));
  if (!usable(env, request)) return -1;
  return static_cast<jlong>(request->generation.load());
}
jboolean send(JNIEnv* env, jclass, jlong id, jint index, jbyteArray input) {
  if (!internet(env)) return false;
  auto request = find(static_cast<uintptr_t>(id));
  if (!usable(env, request)) return false;
  if (!input || env->GetArrayLength(input) < 12 || env->GetArrayLength(input) > 1500) {
    io_error(env, "Invalid DNS-SD query size", ERROR_INVALID_PARAMETER); return false;
  }
  std::vector<char> bytes(env->GetArrayLength(input));
  env->GetByteArrayRegion(input, 0, bytes.size(), reinterpret_cast<jbyte*>(bytes.data()));
  if (env->ExceptionCheck()) return false;
  std::lock_guard<std::mutex> guard(request->lock);
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

jobject poll(JNIEnv* env, jclass, jlong id, jint timeout) {
  if (!internet(env)) return nullptr;
  auto request = find(static_cast<uintptr_t>(id));
  if (!usable(env, request)) return nullptr;
  std::lock_guard<std::mutex> guard(request->lock);
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
    if (ntohs(port) != 5353) continue;
    ULONG incoming_index = 0; DWORD hops = 0; bool multicast = false;
    for (auto* header = WSA_CMSG_FIRSTHDR(&message); header; header = WSA_CMSG_NXTHDR(&message, header)) {
      if (header->cmsg_level == IPPROTO_IP && header->cmsg_type == IP_PKTINFO &&
          header->cmsg_len >= WSA_CMSG_LEN(sizeof(IN_PKTINFO))) {
        IN_PKTINFO info{}; std::memcpy(&info, WSA_CMSG_DATA(header), sizeof(info));
        incoming_index = info.ipi_ifindex;
        multicast = std::memcmp(&info.ipi_addr, kGroup4, sizeof(kGroup4)) == 0;
      } else if (header->cmsg_level == IPPROTO_IPV6 && header->cmsg_type == IPV6_PKTINFO &&
          header->cmsg_len >= WSA_CMSG_LEN(sizeof(IN6_PKTINFO))) {
        IN6_PKTINFO info{}; std::memcpy(&info, WSA_CMSG_DATA(header), sizeof(info));
        incoming_index = info.ipi6_ifindex;
        multicast = std::memcmp(&info.ipi6_addr, kGroup6, sizeof(kGroup6)) == 0;
      } else if (((header->cmsg_level == IPPROTO_IP &&
          (header->cmsg_type == IP_TTL || header->cmsg_type == IP_HOPLIMIT)) ||
          (header->cmsg_level == IPPROTO_IPV6 && header->cmsg_type == IPV6_HOPLIMIT)) &&
          header->cmsg_len >= WSA_CMSG_LEN(sizeof(DWORD))) {
        std::memcpy(&hops, WSA_CMSG_DATA(header), sizeof(hops));
      }
    }
    if (!multicast || hops != 255 || incoming_index != channel.interface_index ||
        channel.adapter.generation != request->generation.load()) continue;
    request->next_channel = (at + 1) % request->channels.size();
    auto payload = env->NewByteArray(received);
    if (!payload) return nullptr;
    env->SetByteArrayRegion(payload, 0, received, reinterpret_cast<const jbyte*>(bytes.data()));
    jclass cls = env->FindClass("com/gesher/net/WindowsNsdResolver$Packet");
    jmethodID constructor = cls ? env->GetMethodID(cls, "<init>", "(JIII[B)V") : nullptr;
    jobject out = constructor ? env->NewObject(cls, constructor, static_cast<jlong>(channel.adapter.generation),
        static_cast<jint>(channel.adapter.index), channel.family == AF_INET ? 4 : 6,
        static_cast<jint>(incoming_index), payload) : nullptr;
    if (cls) env->DeleteLocalRef(cls);
    env->DeleteLocalRef(payload);
    return out;
  }
  return nullptr;
}

jboolean close_request(JNIEnv*, jclass, jlong id) {
  auto request = find(static_cast<uintptr_t>(id));
  if (!request) return true;
  request->retiring = true;
  bool closed;
  {
    std::lock_guard<std::mutex> guard(request->lock);
    closed = close_channels(request.get());
  }
  // Never call cancellation from the callback or under a lock it could need.
  // Registry leases also keep callbacks that already entered alive across erase.
  if (request->interface_watch) {
    if (CancelMibChangeNotify2(request->interface_watch) == NO_ERROR) {
      request->interface_watch = nullptr; --registry().watches;
    } else closed = false;
  }
  if (request->address_watch) {
    if (CancelMibChangeNotify2(request->address_watch) == NO_ERROR) {
      request->address_watch = nullptr; --registry().watches;
    } else closed = false;
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
jintArray active(JNIEnv* env, jclass) {
  std::lock_guard<std::mutex> guard(registry().lock);
  jint polled = 0, sent = 0;
  for (const auto& [id, request] : registry().requests) {
    polled += request->completed_poll.load() ? 1 : 0;
    sent += request->sent_query.load() ? 1 : 0;
  }
  jint values[] = {static_cast<jint>(registry().requests.size()), static_cast<jint>(registry().sockets.load()),
      static_cast<jint>(registry().watches.load()), static_cast<jint>(registry().callbacks.load()), polled, sent};
  auto out = env->NewIntArray(6);
  if (out) env->SetIntArrayRegion(out, 0, 6, values);
  return out;
}

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
jint on_load(JavaVM* vm, void*) {
  JNIEnv* env = nullptr;
  if (vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK) return JNI_ERR;
  jclass cls = env->FindClass("com/gesher/net/WindowsNsdResolver");
  if (!cls) return JNI_ERR;
  int status = env->RegisterNatives(cls, kMethods, sizeof(kMethods) / sizeof(kMethods[0]));
  env->DeleteLocalRef(cls);
  return status == JNI_OK ? JNI_VERSION_1_6 : JNI_ERR;
}
const gesher_builtin_symbol kSymbols[] = {{"JNI_OnLoad", reinterpret_cast<void*>(&on_load)}};
const bool registered = [] {
  gesher_register_builtin_library("libgesher_nsd_resolve.so", kSymbols, 1);
  return true;
}();
}  // namespace
