// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
// Windows DNS-SD browse transport. Windows callbacks enqueue owned values only;
// Java pulls them on its own thread. No JNI call runs on a DNS client callback.
#ifndef UNICODE
#define UNICODE
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <windns.h>
#include <iphlpapi.h>
#include <gesher/builtin_library.h>
#include <gesher/inet.h>
#include <jni.h>
#include <nativehelper/jni_macros.h>
#include "nsd_browse_records.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace {
using BrowseFn = DNS_STATUS(WINAPI*)(PMDNS_QUERY_REQUEST, PMDNS_QUERY_HANDLE);
using CancelFn = DNS_STATUS(WINAPI*)(PMDNS_QUERY_HANDLE);
using FreeFn = VOID(WINAPI*)(PVOID, DNS_FREE_TYPE);

struct Api {
  HMODULE dll = LoadLibraryExW(L"dnsapi.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
  BrowseFn browse = dll ? reinterpret_cast<BrowseFn>(GetProcAddress(dll, "DnsStartMulticastQuery")) : nullptr;
  CancelFn cancel = dll ? reinterpret_cast<CancelFn>(GetProcAddress(dll, "DnsStopMulticastQuery")) : nullptr;
  FreeFn free = dll ? reinterpret_cast<FreeFn>(GetProcAddress(dll, "DnsFree")) : nullptr;
  bool supported() const { return browse && cancel && free; }
  // Process lifetime: outstanding DNS callbacks may still refer to this DLL.
};
Api& api() { static Api* a = new Api; return *a; }

struct Event {
  DWORD status;
  std::wstring name;
  DWORD ttl;
  int64_t service_ttl = -1;
  bool goodbye = false;
  WORD type = DNS_TYPE_PTR;
  ULONGLONG received = GetTickCount64();
};
std::atomic<size_t>& live_queries() { static auto* count = new std::atomic<size_t>(0); return *count; }
std::atomic<uint64_t>& base_goodbyes() { static auto* count = new std::atomic<uint64_t>(0); return *count; }
std::atomic<DWORD>& last_ptr_ttl() { static auto* ttl = new std::atomic<DWORD>(0); return *ttl; }
struct Diagnostics {
  std::atomic<uint64_t> callbacks{0}, records{0}, membership{0}, errors{0};
  std::atomic<DWORD> last_status{0};
};
Diagnostics& diagnostics() { static auto* counters = new Diagnostics; return *counters; }
struct WireQuery {
  MDNS_QUERY_REQUEST request{};
  MDNS_QUERY_HANDLE handle{};
  bool active = false;
  bool retired = false;
};
struct Browse {
  explicit Browse(size_t slots) : slots(slots) { live_queries() += slots; }
  ~Browse() { live_queries() -= slots; }
  const size_t slots;
  std::mutex lock;
  std::condition_variable changed;
  std::wstring query;
  std::wstring base_query;
  WireQuery primary;
  WireQuery base;
  std::deque<Event> events;
  bool cancel_requested = false;
  bool base_starting = false;
  bool terminal = false;
  bool overflow = false;
};
struct Registry {
  std::mutex lock;
  std::map<uintptr_t, std::shared_ptr<Browse>> entries;
  uintptr_t next_id = 1;
};
// Keep only the empty registry shell for process lifetime: DNS callbacks may
// race with CRT static destructors at process shutdown. Individual operations
// are erased after DnsStopMulticastQuery succeeds and entered callbacks release
// their shared owners. The Java listener is never stored here.
Registry& state() { static Registry* value = new Registry; return *value; }
constexpr size_t kMaxEvents = 1024;
constexpr size_t kMaxQueries = 512;

using gesher::nsd::same_dns_name;

bool instance_of_type(const wchar_t* name, const std::wstring& type) {
  size_t length = wcslen(name);
  if (length && name[length - 1] == L'.') --length;
  return length > type.size() + 1 && name[length - type.size() - 1] == L'.' &&
      same_dns_name(name + length - type.size(), type.c_str());
}

std::shared_ptr<Browse> find(uintptr_t id) {
  std::lock_guard<std::mutex> guard(state().lock);
  auto it = state().entries.find(id);
  return it == state().entries.end() ? nullptr : it->second;
}

// The context is a monotonically allocated integer, never a freed memory address.
// A callback racing with Java's release either holds its own shared_ptr through
// its complete invocation or sees an absent id. In the latter case the callback
// never dereferences the handle supplied by Windows (intentionally unused below).
// Stop success does not prove entered callbacks have returned: their shared
// owners keep the request and handle storage alive until each invocation returns.
void queue_records(PVOID context, PDNS_QUERY_RESULT result, bool companion) {
  auto op = find(reinterpret_cast<uintptr_t>(context));
  DWORD status = result ? result->QueryStatus : ERROR_INVALID_DATA;
  auto* records = result ? result->pQueryRecords : nullptr;
  ++diagnostics().callbacks;
  diagnostics().last_status = status;
  if (status != ERROR_SUCCESS) ++diagnostics().errors;
  if (op) {
    std::lock_guard<std::mutex> guard(op->lock);
    const auto& wire = companion ? op->base : op->primary;
    if (!op->terminal && !wire.retired) {
      if (status == ERROR_CANCELLED && op->cancel_requested) {
        // Windows promises no final cancellation callback for this API.
      } else if (status != ERROR_SUCCESS) {
        // ERROR_CANCELLED in the queue is a private transport token emitted
        // only by cancel() after a successful DnsStopMulticastQuery call.
        if (status == ERROR_CANCELLED) status = ERROR_OPERATION_ABORTED;
        if (op->events.size() < kMaxEvents) op->events.push_back({status, {}, 0});
        else op->overflow = true;
      } else {
        for (auto* record = records; record; record = record->pNext) {
          ++diagnostics().records;
          if ((record->wType != DNS_TYPE_PTR && record->wType != DNS_TYPE_SRV) || !record->pName) continue;
          if (companion && record->wType != DNS_TYPE_PTR) continue;
          // The request is Unicode. Reject unexpected encodings rather than
          // reinterpret char pointers as WCHAR records.
          if (record->Flags.S.CharSet != DnsCharSetUnicode) {
            if (op->events.size() < kMaxEvents) op->events.push_back({ERROR_INVALID_DATA, {}, 0});
            else op->overflow = true;
            continue;
          }
          if (record->wType == DNS_TYPE_SRV) {
            if (!instance_of_type(record->pName, op->base_query)) continue;
            if (op->events.size() >= kMaxEvents) { op->overflow = true; break; }
            DWORD ttl = record->Flags.S.Delete ? 0 : record->dwTtl;
            op->events.push_back({ERROR_SUCCESS, record->pName, ttl, ttl, ttl == 0, DNS_TYPE_SRV});
            continue;
          }
          if (!record->Data.PTR.pNameHost) continue;
          const auto observation = gesher::nsd::classify_ptr(record->pName, op->query,
              op->base_query, record->Flags.S.Delete || record->dwTtl == 0, companion);
          if (observation == gesher::nsd::PtrObservation::Ignore) continue;
          const bool base_goodbye = observation == gesher::nsd::PtrObservation::Goodbye;
          if (base_goodbye) ++base_goodbyes();
          if (!record->Flags.S.Delete && record->dwTtl > 0) last_ptr_ttl() = record->dwTtl;
          if (op->events.size() >= kMaxEvents) {
            op->overflow = true;
            break;
          }
          int64_t service_ttl = -1;
          bool goodbye = base_goodbye;
          bool base_loss_in_batch = false;
          for (auto* extra = records; extra; extra = extra->pNext) {
            if (extra->wType == DNS_TYPE_SRV && extra->pName &&
                extra->Flags.S.CharSet == DnsCharSetUnicode &&
                same_dns_name(extra->pName, record->Data.PTR.pNameHost)) {
              service_ttl = extra->Flags.S.Delete ? 0 : extra->dwTtl;
              if (service_ttl == 0) goodbye = true;
            } else if (!base_goodbye && op->slots == 2 &&
                extra->wType == DNS_TYPE_PTR && extra->pName &&
                extra->Data.PTR.pNameHost && extra->Flags.S.CharSet == DnsCharSetUnicode &&
                same_dns_name(extra->pName, op->base_query.c_str()) &&
                same_dns_name(extra->Data.PTR.pNameHost, record->Data.PTR.pNameHost) &&
                (extra->Flags.S.Delete || extra->dwTtl == 0)) {
              base_loss_in_batch = true;
            }
          }
          // The companion owns base PTR loss. A subtype PTR paired with that
          // loss cannot start or refresh membership from the same packet.
          if (base_loss_in_batch) continue;
          if (!base_goodbye) ++diagnostics().membership;
          op->events.push_back({ERROR_SUCCESS, record->Data.PTR.pNameHost,
                               record->Flags.S.Delete ? 0 : record->dwTtl, service_ttl, goodbye});
        }
      }
      op->changed.notify_all();
    }
  }
  if (records) api().free(records, DnsFreeRecordList);
}

void WINAPI on_browse(PVOID context, PMDNS_QUERY_HANDLE, PDNS_QUERY_RESULT result) {
  queue_records(context, result, false);
}
void WINAPI on_base_loss(PVOID context, PMDNS_QUERY_HANDLE, PDNS_QUERY_RESULT result) {
  // Both callbacks append under the same Browse mutex. A base deletion and
  // subtype PTR therefore reach Java in the native FIFO's observation order.
  queue_records(context, result, true);
}

void prepare(WireQuery& wire, const std::wstring& query, ULONG index, uintptr_t id,
             PMDNS_QUERY_CALLBACK callback) {
  wire.request.Version = DNS_QUERY_REQUEST_VERSION1;
  wire.request.InterfaceIndex = index;
  wire.request.Query = query.c_str();
  wire.request.QueryType = DNS_TYPE_PTR;
  wire.request.QueryOptions = DNS_QUERY_STANDARD;
  wire.request.pQueryCallback = callback;
  wire.request.pQueryContext = reinterpret_cast<void*>(id);
}

void throw_error(JNIEnv* env, const char* type, const char* message) {
  jclass c = env->FindClass(type);
  if (c) { env->ThrowNew(c, message); env->DeleteLocalRef(c); }
}
bool internet(JNIEnv* env) {
  if (gesher_inet_allowed()) return true;
  throw_error(env, "java/lang/SecurityException", "NsdService requires android.permission.INTERNET");
  return false;
}

jboolean supported(JNIEnv* env, jclass) {
  return internet(env) && api().supported();
}
jintArray interfaces(JNIEnv* env, jclass) {
  if (!internet(env)) return nullptr;
  ULONG bytes = 16384;
  std::vector<unsigned char> buffer(bytes);
  DWORD status;
  for (;;) {
    status = GetAdaptersAddresses(AF_UNSPEC, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST |
        GAA_FLAG_SKIP_DNS_SERVER, nullptr, reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()), &bytes);
    if (status != ERROR_BUFFER_OVERFLOW) break;
    buffer.resize(bytes);
  }
  if (status != ERROR_SUCCESS && status != ERROR_NO_DATA) {
    throw_error(env, "java/io/IOException", "Cannot enumerate DNS-SD interfaces");
    return nullptr;
  }
  std::vector<jint> indices;
  if (status == ERROR_SUCCESS) {
    for (auto* a = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()); a; a = a->Next) {
      if (a->OperStatus != IfOperStatusUp || (a->Flags & IP_ADAPTER_NO_MULTICAST) ||
          a->IfType == IF_TYPE_SOFTWARE_LOOPBACK || !a->FirstUnicastAddress) continue;
      ULONG index = a->IfIndex ? a->IfIndex : a->Ipv6IfIndex;
      if (index && index <= 0x7fffff9bUL) indices.push_back(static_cast<jint>(index));
    }
  }
  auto out = env->NewIntArray(static_cast<jsize>(indices.size()));
  if (out && !indices.empty()) env->SetIntArrayRegion(out, 0, indices.size(), indices.data());
  return out;
}

jlong start(JNIEnv* env, jclass, jstring query, jint index) {
  if (!internet(env)) return -ERROR_ACCESS_DENIED;
  if (!api().supported()) return -ERROR_NOT_SUPPORTED;
  if (!query || index <= 0) return -ERROR_INVALID_PARAMETER;
  const jchar* chars = env->GetStringChars(query, nullptr);
  if (!chars) return -ERROR_NOT_ENOUGH_MEMORY;
  std::wstring name(reinterpret_cast<const wchar_t*>(chars), env->GetStringLength(query));
  env->ReleaseStringChars(query, chars);
  if (name.find(L'\0') != std::wstring::npos) return -ERROR_INVALID_PARAMETER;
  const auto subtype = name.find(L"._sub.");
  size_t slots = subtype == std::wstring::npos ? 1 : 2;
  std::shared_ptr<Browse> op;
  uintptr_t id;
  {
    std::lock_guard<std::mutex> guard(state().lock);
    // Count stopped requests still leased by entered callbacks and operations
    // retained after native Stop failures. Listener reuse cannot evade this cap.
    if (live_queries() > kMaxQueries - slots || state().next_id == 0 ||
        state().next_id > static_cast<uintptr_t>(INT64_MAX)) return -ERROR_TOO_MANY_CMDS;
    op = std::make_shared<Browse>(slots);
    id = state().next_id++;
    state().entries.emplace(id, op);
  }
  op->query = std::move(name);
  op->base_query = subtype == std::wstring::npos ? op->query : op->query.substr(subtype + 6);
  prepare(op->primary, op->query, static_cast<ULONG>(index), id, on_browse);
  last_ptr_ttl() = 0;
  DWORD status = api().browse(&op->primary.request, &op->primary.handle);
  if (status != ERROR_SUCCESS) {
    std::lock_guard<std::mutex> guard(state().lock);
    state().entries.erase(id);
    return -static_cast<jlong>(status);
  }
  {
    std::lock_guard<std::mutex> guard(op->lock);
    op->primary.active = true;
  }
  return static_cast<jlong>(id);
}

jint attach_base_loss(JNIEnv* env, jclass, jlong id) {
  if (!internet(env)) return ERROR_ACCESS_DENIED;
  auto op = find(static_cast<uintptr_t>(id));
  if (!op) return ERROR_INVALID_HANDLE;
  {
    std::lock_guard<std::mutex> guard(op->lock);
    if (op->slots != 2 || op->terminal || !op->primary.active) return ERROR_INVALID_PARAMETER;
    if (op->cancel_requested || op->base_starting || op->base.active) return ERROR_BUSY;
    op->base_starting = true;
    prepare(op->base, op->base_query, op->primary.request.InterfaceIndex,
            static_cast<uintptr_t>(id), on_base_loss);
  }
  DWORD status = api().browse(&op->base.request, &op->base.handle);
  {
    std::lock_guard<std::mutex> guard(op->lock);
    op->base.active = status == ERROR_SUCCESS;
    op->base_starting = false;
  }
  return static_cast<jint>(status);
}

jobjectArray poll(JNIEnv* env, jclass, jlong id, jint timeout) {
  auto op = find(static_cast<uintptr_t>(id));
  if (!op) return nullptr;
  std::deque<Event> events;
  {
    std::unique_lock<std::mutex> guard(op->lock);
    if (op->events.empty() && !op->overflow) {
      op->changed.wait_for(guard, std::chrono::milliseconds(timeout < 0 ? 0 : timeout),
                          [&] { return !op->events.empty() || op->overflow; });
    }
    events.swap(op->events);
    if (op->overflow) {
      events.push_back({ERROR_NOT_ENOUGH_MEMORY, {}, 0});
      op->overflow = false;
    }
  }
  if (events.empty()) return nullptr;
  jclass strings = env->FindClass("java/lang/String");
  if (!strings) return nullptr;
  jobjectArray out = env->NewObjectArray(events.size() * 7, strings, nullptr);
  env->DeleteLocalRef(strings);
  if (!out) return nullptr;
  jsize i = 0;
  for (const auto& event : events) {
    for (const auto& value : {std::to_wstring(event.status), event.name, std::to_wstring(event.ttl),
                              std::to_wstring(GetTickCount64() - event.received),
                              std::to_wstring(event.service_ttl),
                              std::to_wstring(event.goodbye ? 1 : 0), std::to_wstring(event.type)}) {
      auto str = env->NewString(reinterpret_cast<const jchar*>(value.data()), value.size());
      if (!str) return nullptr;
      env->SetObjectArrayElement(out, i++, str);
      env->DeleteLocalRef(str);
      if (env->ExceptionCheck()) return nullptr;
    }
  }
  return out;
}
jint cancel(JNIEnv*, jclass, jlong id) {
  auto op = find(static_cast<uintptr_t>(id));
  if (!op) return ERROR_INVALID_HANDLE;
  bool primary, base;
  {
    std::lock_guard<std::mutex> guard(op->lock);
    if (op->terminal) return ERROR_SUCCESS;
    if (op->cancel_requested || op->base_starting) return ERROR_BUSY;
    op->cancel_requested = true;
    primary = op->primary.active;
    base = op->base.active;
  }
  // A synchronous callback is allowed; never hold op->lock across the API.
  DWORD primary_status = primary ? api().cancel(&op->primary.handle) : ERROR_SUCCESS;
  DWORD base_status = base ? api().cancel(&op->base.handle) : ERROR_SUCCESS;
  {
    std::lock_guard<std::mutex> guard(op->lock);
    if (primary && primary_status == ERROR_SUCCESS) {
      op->primary.active = false;
      op->primary.retired = true;
    }
    if (base && base_status == ERROR_SUCCESS) {
      op->base.active = false;
      op->base.retired = true;
    }
    if (op->primary.active || op->base.active) {
      op->cancel_requested = false;
    } else {
      op->terminal = true;
      op->events.clear();
      op->overflow = false;
      op->events.push_back({ERROR_CANCELLED, {}, 0});
      op->changed.notify_all();
    }
  }
  return static_cast<jint>(primary_status != ERROR_SUCCESS ? primary_status : base_status);
}
jboolean release(JNIEnv*, jclass, jlong id) {
  std::lock_guard<std::mutex> registry_guard(state().lock);
  auto it = state().entries.find(static_cast<uintptr_t>(id));
  if (it == state().entries.end()) return true;
  // Keep an owner until after the guard releases the operation mutex.
  auto keep = it->second;
  std::lock_guard<std::mutex> guard(keep->lock);
  if (!keep->terminal) return false;
  state().entries.erase(it);
  return true;
}
jint active(JNIEnv*, jclass) {
  return static_cast<jint>(live_queries().load());
}
jlong goodbyes(JNIEnv*, jclass) { return static_cast<jlong>(base_goodbyes().load()); }
jlong ptr_ttl(JNIEnv*, jclass) { return static_cast<jlong>(last_ptr_ttl().load()); }
jlongArray diagnostic_snapshot(JNIEnv* env, jclass) {
  const jlong values[] = {static_cast<jlong>(diagnostics().callbacks.load()),
      static_cast<jlong>(diagnostics().records.load()),
      static_cast<jlong>(diagnostics().membership.load()),
      static_cast<jlong>(base_goodbyes().load()),
      static_cast<jlong>(diagnostics().errors.load()),
      static_cast<jlong>(diagnostics().last_status.load())};
  auto out = env->NewLongArray(6);
  if (out) env->SetLongArrayRegion(out, 0, 6, values);
  return out;
}
const JNINativeMethod kMethods[] = {
  {"nativeSupported", "()Z", NATIVEHELPER_SYSV_JNI_ENTRY(supported)},
  {"nativeInterfaces", "()[I", NATIVEHELPER_SYSV_JNI_ENTRY(interfaces)},
  {"nativeStart", "(Ljava/lang/String;I)J", NATIVEHELPER_SYSV_JNI_ENTRY(start)},
  {"nativeAttachBaseLoss", "(J)I", NATIVEHELPER_SYSV_JNI_ENTRY(attach_base_loss)},
  {"nativePoll", "(JI)[Ljava/lang/String;", NATIVEHELPER_SYSV_JNI_ENTRY(poll)},
  {"nativeCancel", "(J)I", NATIVEHELPER_SYSV_JNI_ENTRY(cancel)},
  {"nativeRelease", "(J)Z", NATIVEHELPER_SYSV_JNI_ENTRY(release)},
  {"nativeActive", "()I", NATIVEHELPER_SYSV_JNI_ENTRY(active)},
  {"nativeBaseGoodbyes", "()J", NATIVEHELPER_SYSV_JNI_ENTRY(goodbyes)},
  {"nativeLastPtrTtl", "()J", NATIVEHELPER_SYSV_JNI_ENTRY(ptr_ttl)},
  {"nativeDiagnostics", "()[J", NATIVEHELPER_SYSV_JNI_ENTRY(diagnostic_snapshot)},
};
jint on_load(JavaVM* vm, void*) {
  JNIEnv* env = nullptr;
  if (vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK) return JNI_ERR;
  jclass c = env->FindClass("com/gesher/net/WindowsNsd");
  if (!c) return JNI_ERR;
  int result = env->RegisterNatives(c, kMethods, sizeof(kMethods) / sizeof(kMethods[0]));
  env->DeleteLocalRef(c);
  return result == JNI_OK ? JNI_VERSION_1_6 : JNI_ERR;
}
const gesher_builtin_symbol kSymbols[] = {{"JNI_OnLoad", reinterpret_cast<void*>(&on_load)}};
const bool registered = [] {
  gesher_register_builtin_library("libgesher_nsd.so", kSymbols, 1);
  return true;
}();
}  // namespace
