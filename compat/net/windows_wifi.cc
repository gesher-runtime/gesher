// Copyright (C) 2026 The Gesher Authors. SPDX-License-Identifier: Apache-2.0
// Locale-independent Native Wi-Fi boundary for com.gesher.net.WindowsWifi.
// All calls run on its single background worker. No shell subprocess or profile mutation.
#include <winsock2.h>
#include <windows.h>
#include <netioapi.h>
#include <wlanapi.h>
#include <atomic>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <string>
#include <memory>
#include <new>
#include <vector>
#include <gesher/builtin_library.h>
#include <jni.h>
#include <nativehelper/jni_macros.h>

namespace {
struct Api {
  decltype(&WlanOpenHandle) open = nullptr;
  decltype(&WlanCloseHandle) close = nullptr;
  decltype(&WlanEnumInterfaces) interfaces = nullptr;
  decltype(&WlanQueryInterface) query = nullptr;
  decltype(&WlanGetNetworkBssList) bss = nullptr;
  decltype(&WlanRegisterNotification) notify = nullptr;
  decltype(&WlanScan) scan = nullptr;
  decltype(&WlanFreeMemory) free = nullptr;
  Api() {
    // Optional on Windows Server: never turn missing WLAN support into a loader failure.
    HMODULE module = LoadLibraryExW(L"wlanapi.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!module) return;
#define LOAD(member, name) member = reinterpret_cast<decltype(member)>(GetProcAddress(module, name))
    LOAD(open, "WlanOpenHandle"); LOAD(close, "WlanCloseHandle");
    LOAD(interfaces, "WlanEnumInterfaces"); LOAD(query, "WlanQueryInterface");
    LOAD(bss, "WlanGetNetworkBssList"); LOAD(notify, "WlanRegisterNotification");
    LOAD(scan, "WlanScan"); LOAD(free, "WlanFreeMemory");
#undef LOAD
    // The system DLL is retained for process lifetime; all function pointers remain valid.
  }
  bool ready() const { return open && close && interfaces && query && bss && notify && scan && free; }
};
Api& Wlan() { static Api api; return api; }

struct Memory {
  Api& api;
  void* value = nullptr;
  explicit Memory(Api& api) : api(api) { }
  ~Memory() { if (value) api.free(value); }
};
std::string Hex(const unsigned char* bytes, size_t length) {
  constexpr char hex[] = "0123456789abcdef";
  std::string result;
  result.reserve(length * 2);
  for (size_t i = 0; i < length; i++) { result += hex[bytes[i] >> 4]; result += hex[bytes[i] & 15]; }
  return result;
}
std::string Mac(const unsigned char* bytes) {
  char result[18];
  snprintf(result, sizeof(result), "%02x:%02x:%02x:%02x:%02x:%02x", bytes[0], bytes[1], bytes[2], bytes[3], bytes[4], bytes[5]);
  return result;
}
ULONG Index(const GUID& guid) {
  NET_LUID luid = {};
  NET_IFINDEX index = 0;
  if (ConvertInterfaceGuidToLuid(&guid, &luid) != NO_ERROR || ConvertInterfaceLuidToIndex(&luid, &index) != NO_ERROR) return 0;
  return index;
}

template<class T>
bool Query(Api& api, HANDLE handle, const GUID& guid, WLAN_INTF_OPCODE operation, T* out) {
  Memory memory(api);
  DWORD size = 0;
  if (api.query(handle, &guid, operation, nullptr, &size, &memory.value, nullptr) != ERROR_SUCCESS
      || !memory.value || size < sizeof(T)) return false;
  memcpy(out, memory.value, sizeof(T));
  return true;
}

int Radio(Api& api, HANDLE handle, const WLAN_INTERFACE_INFO& info) {
  if (info.isState == wlan_interface_state_connected) return 1;
  WLAN_RADIO_STATE radio = {};
  if (!Query(api, handle, info.InterfaceGuid, wlan_intf_opcode_radio_state, &radio)
      || radio.dwNumberOfPhys == 0 || radio.dwNumberOfPhys > WLAN_MAX_PHY_INDEX) return -1;
  bool unknown = false;
  for (DWORD i = 0; i < radio.dwNumberOfPhys; i++) {
    auto& phy = radio.PhyRadioState[i];
    if (phy.dot11SoftwareRadioState == dot11_radio_state_on && phy.dot11HardwareRadioState == dot11_radio_state_on) return 1;
    if (phy.dot11SoftwareRadioState == dot11_radio_state_unknown || phy.dot11HardwareRadioState == dot11_radio_state_unknown) unknown = true;
  }
  return unknown ? -1 : 0;
}

struct ScanItem {
  GUID guid;
  std::atomic<int> state{-2}; // not-started=-2, pending=0, completed=1, failed=-1
};
struct ScanWait {
  ScanItem items[64];
  size_t count = 0;
  HANDLE event = nullptr;
  ScanWait() : event(CreateEventW(nullptr, FALSE, FALSE, nullptr)) { }
  ~ScanWait() { if (event) CloseHandle(event); }
};
// A failed close cannot prove that callbacks have stopped. Retain at most one
// context in that exceptional case and disable further WLAN operations for this
// process, rather than risk freeing a live callback target or accumulating leaks.
std::atomic<bool> wlan_broken{false};
struct Client {
  Api& api;
  HANDLE handle = nullptr;
  std::unique_ptr<ScanWait> wait;
  explicit Client(Api& api) : api(api) { }
  ~Client() {
    if (handle && api.close(handle, nullptr) != ERROR_SUCCESS) {
      wlan_broken = true;
      wait.release();
    }
  }
};

void WINAPI OnScan(WLAN_NOTIFICATION_DATA* notification, void* context) {
  if (!notification || notification->NotificationSource != WLAN_NOTIFICATION_SOURCE_ACM) return;
  int outcome;
  if (notification->NotificationCode == wlan_notification_acm_scan_complete) outcome = 1;
  else if (notification->NotificationCode == wlan_notification_acm_scan_fail) outcome = -1;
  else return;
  auto* wait = static_cast<ScanWait*>(context);
  for (size_t i = 0; i < wait->count; i++) {
    if (memcmp(&wait->items[i].guid, &notification->InterfaceGuid, sizeof(GUID)) == 0) {
      int pending = 0;
      wait->items[i].state.compare_exchange_strong(pending, outcome);
      SetEvent(wait->event);
      return;
    }
  }
}

bool Scan(Api& api, HANDLE handle, const WLAN_INTERFACE_INFO_LIST& list, ScanWait& wait,
          bool (*eligible)(void*), void* context, bool* denied) {
  if (!wait.event || list.dwNumberOfItems > 64) return false;
  for (DWORD i = 0; i < list.dwNumberOfItems; i++) {
    if (Radio(api, handle, list.InterfaceInfo[i]) == 1) wait.items[wait.count++].guid = list.InterfaceInfo[i].InterfaceGuid;
  }
  if (wait.count == 0 || api.notify(handle, WLAN_NOTIFICATION_SOURCE_ACM, FALSE, OnScan, &wait, nullptr, nullptr) != ERROR_SUCCESS) return false;
  // Registration precedes initiation so fast completion cannot be lost. The immutable
  // GUID set is fully populated before any callback can access it.
  for (size_t i = 0; i < wait.count; i++) {
    if (!eligible(context)) { wait.items[i].state = -1; continue; }
    wait.items[i].state = 0;
    DWORD result = api.scan(handle, &wait.items[i].guid, nullptr, nullptr, nullptr);
    if (result == ERROR_ACCESS_DENIED) *denied = true;
    if (result != ERROR_SUCCESS) wait.items[i].state = -1;
  }
  ULONGLONG deadline = GetTickCount64() + 5000;
  for (;;) {
    bool done = true;
    for (size_t i = 0; i < wait.count; i++) done = done && wait.items[i].state.load() != 0;
    if (done) break;
    ULONGLONG now = GetTickCount64();
    if (now >= deadline || WaitForSingleObject(wait.event, static_cast<DWORD>(deadline - now)) != WAIT_OBJECT_0) break;
  }
  // No locks are held: unregister/close waits for in-flight callbacks. ScanWait
  // remains owned by Client until close has drained outstanding callbacks.
  api.notify(handle, WLAN_NOTIFICATION_SOURCE_NONE, FALSE, nullptr, nullptr, nullptr, nullptr);
  for (size_t i = 0; i < wait.count; i++) if (wait.items[i].state.load() != 1) return false;
  return true;
}

struct Bss {
  std::string row;
  std::string mac;
  int frequency;
};

bool ReadBss(Api& api, HANDLE handle, const GUID& guid, std::vector<Bss>* result, size_t* remaining, bool* denied) {
  Memory memory(api);
  auto** out = reinterpret_cast<WLAN_BSS_LIST**>(&memory.value);
  DWORD status = api.bss(handle, &guid, nullptr, dot11_BSS_type_any, FALSE, nullptr, out);
  if (status == ERROR_ACCESS_DENIED) *denied = true;
  if (status != ERROR_SUCCESS || !memory.value) return false;
  const auto* list = static_cast<WLAN_BSS_LIST*>(memory.value);
  size_t total = list->dwTotalSize, base = offsetof(WLAN_BSS_LIST, wlanBssEntries);
  if (total < base || total > 4 * 1024 * 1024 || list->dwNumberOfItems > (total - base) / sizeof(WLAN_BSS_ENTRY)) return false;
  // Android elapsedRealtime uses this interrupt clock, including suspend. Sample
  // it before wall time so a scheduling delay can only make data look older.
  // Convert the observation once: later WLAN RPC/close delays must not shift it.
  ULONGLONG elapsed_ticks;
  QueryInterruptTimePrecise(&elapsed_ticks);
  FILETIME filetime;
  GetSystemTimeAsFileTime(&filetime);
  ULONGLONG now = (static_cast<ULONGLONG>(filetime.dwHighDateTime) << 32) | filetime.dwLowDateTime;
  for (DWORD i = 0; i < list->dwNumberOfItems; i++) {
    const WLAN_BSS_ENTRY& item = list->wlanBssEntries[i];
    size_t entryOffset = base + static_cast<size_t>(i) * sizeof(WLAN_BSS_ENTRY);
    if (item.dot11Ssid.uSSIDLength > sizeof(item.dot11Ssid.ucSSID)
        || item.ulIeOffset > total - entryOffset || item.ulIeSize > total - entryOffset - item.ulIeOffset
        || item.ulIeSize > 65535 || (item.ulIeSize && entryOffset + item.ulIeOffset < base + static_cast<size_t>(list->dwNumberOfItems) * sizeof(WLAN_BSS_ENTRY))) return false;
    // FILETIME is wall-clock 100ns since 1601; the wire timestamp is microseconds
    // since boot, exactly the ScanResult domain. Ignore future/pre-boot samples.
    if (item.ullHostTimestamp == 0 || item.ullHostTimestamp > now) continue;
    ULONGLONG age_ticks = now - item.ullHostTimestamp;
    if (age_ticks > elapsed_ticks) continue;
    ULONGLONG observed_us = (elapsed_ticks - age_ticks) / 10;
    if (item.ulChCenterFrequency < 1000 || item.lRssi < -127 || item.lRssi > 0) continue;
    // Bound the aggregate serialized payload before allocating/hex-encoding it.
    // Per-adapter allocation bounds alone are insufficient on multi-adapter hosts.
    size_t required = static_cast<size_t>(item.ulIeSize) * 2 + item.dot11Ssid.uSSIDLength * 2 + 160;
    if (required > *remaining) return false;
    const auto* ies = reinterpret_cast<const unsigned char*>(&item) + item.ulIeOffset;
    Bss bss;
    bss.mac = Mac(item.dot11Bssid);
    bss.frequency = static_cast<int>(item.ulChCenterFrequency / 1000);
    bss.row = "bss\t" + Hex(item.dot11Ssid.ucSSID, item.dot11Ssid.uSSIDLength) + "\t" + bss.mac
        + "\t" + std::to_string(bss.frequency) + "\t" + std::to_string(item.lRssi)
        + "\t" + std::to_string(observed_us) + "\t" + std::to_string(item.dot11BssPhyType)
        + "\t" + std::to_string(item.usCapabilityInformation) + "\t" + Hex(ies, item.ulIeSize) + "\n";
    *remaining -= bss.row.size();
    result->push_back(std::move(bss));
  }
  return true;
}

std::string Snapshot(Api& api, bool request_scan, bool (*eligible)(void*), void* context) {
  if (!api.ready()) return {}; // An unavailable API cannot establish that radios are off.
  if (wlan_broken.load()) return {};
  Client client(api);
  DWORD version = 0;
  if (api.open(2, nullptr, &version, &client.handle) != ERROR_SUCCESS) return {};
  Memory interfaces(api);
  if (api.interfaces(client.handle, nullptr, reinterpret_cast<WLAN_INTERFACE_INFO_LIST**>(&interfaces.value)) != ERROR_SUCCESS || !interfaces.value) return {};
  auto* list = static_cast<WLAN_INTERFACE_INFO_LIST*>(interfaces.value);
  if (list->dwNumberOfItems > 64) return {};
  bool scans_valid = !request_scan;
  bool denied = false;
  if (request_scan) {
    client.wait.reset(new (std::nothrow) ScanWait());
    scans_valid = client.wait && Scan(api, client.handle, *list, *client.wait, eligible, context, &denied);
  }
  std::string interfaces_text, scans_text;
  size_t remaining = 4 * 1024 * 1024 - 5;
  for (DWORD i = 0; i < list->dwNumberOfItems; i++) {
    const auto& item = list->InterfaceInfo[i];
    std::vector<Bss> bss;
    bool bss_valid = ReadBss(api, client.handle, item.InterfaceGuid, &bss, &remaining, &denied);
    scans_valid = scans_valid && bss_valid;
    for (const Bss& entry : bss) scans_text += entry.row;
    // WLAN scan data is keyed by radio GUID and does not require an IP interface
    // mapping. Losing that mapping can only omit the connection overlay.
    ULONG index = Index(item.InterfaceGuid);
    if (index == 0 || index > 0x7fffff9bUL) {
      std::string row = "radio\t" + std::to_string(Radio(api, client.handle, item)) + "\n";
      if (row.size() > remaining) return {};
      remaining -= row.size();
      interfaces_text += row;
      continue;
    }
    WLAN_CONNECTION_ATTRIBUTES connection = {};
    bool connected = Query(api, client.handle, item.InterfaceGuid, wlan_intf_opcode_current_connection, &connection)
        && connection.isState == wlan_interface_state_connected
        && connection.wlanAssociationAttributes.dot11Ssid.uSSIDLength <= 32;
    int radio = Radio(api, client.handle, item);
    std::string ssid, mac;
    int frequency = -1, rssi = -127;
    ULONG rx = 0, tx = 0, phy = 0, auth = 0;
    if (connected) {
      auto& association = connection.wlanAssociationAttributes;
      ssid = Hex(association.dot11Ssid.ucSSID, association.dot11Ssid.uSSIDLength);
      mac = Mac(association.dot11Bssid);
      rx = association.ulRxRate; tx = association.ulTxRate;
      phy = association.dot11PhyType;
      auth = connection.wlanSecurityAttributes.dot11AuthAlgorithm;
      // OPEN with a WEP cipher is not an unencrypted network.
      if (auth == 1 && connection.wlanSecurityAttributes.bSecurityEnabled) auth = 2;
      LONG signal = -127;
      if (Query(api, client.handle, item.InterfaceGuid, wlan_intf_opcode_rssi, &signal) && signal >= -127 && signal <= 0) rssi = signal;
      for (const Bss& entry : bss) if (entry.mac == mac) { frequency = entry.frequency; break; }
    }
    std::string row = "if\t" + std::to_string(index) + "\t" + std::to_string(radio) + "\t" + (connected ? "1" : "0")
        + "\t" + ssid + "\t" + mac + "\t" + std::to_string(frequency) + "\t" + std::to_string(rssi)
        + "\t" + std::to_string(rx) + "\t" + std::to_string(tx) + "\t" + std::to_string(phy)
        + "\t" + std::to_string(auth) + "\t1\n";
    if (row.size() > remaining) return {};
    remaining -= row.size();
    interfaces_text += row;
  }
  return std::string(denied ? "ok\t2\n" : (scans_valid ? "ok\t1\n" : "ok\t0\n")) + interfaces_text + (scans_valid ? scans_text : "");
}

struct Eligibility {
  JNIEnv* env;
  jobject supplier;
  jmethodID method;
};
bool Eligible(void* context) {
  auto* gate = static_cast<Eligibility*>(context);
  if (!gate->method || gate->env->ExceptionCheck()) return false;
  jboolean accepted = gate->env->CallBooleanMethod(gate->supplier, gate->method);
  return !gate->env->ExceptionCheck() && accepted == JNI_TRUE;
}
jstring WindowsWifi_nativeQuery(JNIEnv* env, jclass, jboolean scan, jobject eligible) {
  if (!eligible) return nullptr;
  jclass type = env->GetObjectClass(eligible);
  if (!type) return nullptr;
  jmethodID method = env->GetMethodID(type, "getAsBoolean", "()Z");
  env->DeleteLocalRef(type);
  if (!method) return nullptr;
  Eligibility gate{env, eligible, method};
  std::string text = Snapshot(Wlan(), scan == JNI_TRUE, Eligible, &gate);
  if (env->ExceptionCheck()) return nullptr;
  return text.empty() ? nullptr : env->NewStringUTF(text.c_str());
}
const JNINativeMethod methods[] = {
    {"nativeQuery", "(ZLjava/util/function/BooleanSupplier;)Ljava/lang/String;", NATIVEHELPER_SYSV_JNI_ENTRY(WindowsWifi_nativeQuery)},
};
jint OnLoad(JavaVM* vm, void*) {
  JNIEnv* env = nullptr;
  if (vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK) return JNI_ERR;
  jclass type = env->FindClass("com/gesher/net/WindowsWifi");
  if (!type) return JNI_ERR;
  int result = env->RegisterNatives(type, methods, sizeof(methods) / sizeof(methods[0]));
  env->DeleteLocalRef(type);
  return result == JNI_OK ? JNI_VERSION_1_6 : JNI_ERR;
}
const gesher_builtin_symbol symbols[] = {{"JNI_OnLoad", reinterpret_cast<void*>(&OnLoad)}};
const bool registered = [] {
  gesher_register_builtin_library("libgesher_wifi.so", symbols, sizeof(symbols) / sizeof(symbols[0]));
  return true;
}();
} // namespace
