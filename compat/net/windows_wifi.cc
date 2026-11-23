// Copyright (C) 2026 The Gesher Authors. SPDX-License-Identifier: Apache-2.0
// Locale-independent Native Wi-Fi boundary for com.gesher.net.WindowsWifi.
// Queries run on its single background worker; the adapter census is synchronous and the
// notification watcher owns one dedicated thread. No shell subprocess.
#include <winsock2.h>
#include <windows.h>
#include <netioapi.h>
#include <wlanapi.h>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
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
  // Optional: absent capability data means "not reported", never "supported".
  decltype(&WlanGetInterfaceCapability) capability = nullptr;
  // Connection management for user-approved requests; absent means "cannot connect".
  decltype(&WlanSetProfile) set_profile = nullptr;
  decltype(&WlanDeleteProfile) delete_profile = nullptr;
  decltype(&WlanConnect) connect = nullptr;
  decltype(&WlanDisconnect) disconnect = nullptr;
  decltype(&WlanGetProfileList) profiles = nullptr;
  decltype(&WlanSetProfilePosition) position = nullptr;
  Api() {
    // Optional on Windows Server: never turn missing WLAN support into a loader failure.
    HMODULE module = LoadLibraryExW(L"wlanapi.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!module) return;
#define LOAD(member, name) member = reinterpret_cast<decltype(member)>(GetProcAddress(module, name))
    LOAD(open, "WlanOpenHandle"); LOAD(close, "WlanCloseHandle");
    LOAD(interfaces, "WlanEnumInterfaces"); LOAD(query, "WlanQueryInterface");
    LOAD(bss, "WlanGetNetworkBssList"); LOAD(notify, "WlanRegisterNotification");
    LOAD(scan, "WlanScan"); LOAD(free, "WlanFreeMemory");
    LOAD(capability, "WlanGetInterfaceCapability");
    LOAD(set_profile, "WlanSetProfile"); LOAD(delete_profile, "WlanDeleteProfile");
    LOAD(connect, "WlanConnect"); LOAD(disconnect, "WlanDisconnect");
    LOAD(profiles, "WlanGetProfileList"); LOAD(position, "WlanSetProfilePosition");
#undef LOAD
    // The system DLL is retained for process lifetime; all function pointers remain valid.
  }
  bool ready() const { return open && close && interfaces && query && bss && notify && scan && free; }
  bool connectable() const { return ready() && set_profile && delete_profile && connect && disconnect; }
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

// Synchronous adapter census for the system feature and capability queries. An
// unavailable API, a stopped WLAN service or an open failure returns empty, which the
// Java side treats as "no Wi-Fi hardware"; it is never reported as present.
bool AuthCipherPairs(Api& api, HANDLE handle, const GUID& guid, uint32_t* auth, uint32_t* cipher) {
  Memory memory(api);
  DWORD size = 0;
  if (api.query(handle, &guid, wlan_intf_opcode_supported_infrastructure_auth_cipher_pairs, nullptr,
                &size, &memory.value, nullptr) != ERROR_SUCCESS || !memory.value) return false;
  size_t base = offsetof(WLAN_AUTH_CIPHER_PAIR_LIST, pAuthCipherPairList);
  if (size < base) return false;
  const auto* list = static_cast<const WLAN_AUTH_CIPHER_PAIR_LIST*>(memory.value);
  if (list->dwNumberOfItems > (size - base) / sizeof(DOT11_AUTH_CIPHER_PAIR)) return false;
  for (DWORD i = 0; i < list->dwNumberOfItems; i++) {
    uint32_t algorithm = static_cast<uint32_t>(list->pAuthCipherPairList[i].AuthAlgoId);
    uint32_t suite = static_cast<uint32_t>(list->pAuthCipherPairList[i].CipherAlgoId);
    if (algorithm >= 1 && algorithm <= 30) *auth |= 1u << algorithm;
    if (suite <= 30) *cipher |= 1u << suite;
    else if (suite == 0x101) *cipher |= 1u << 31; // DOT11_CIPHER_ALGO_WEP (either key length)
  }
  return true;
}

uint32_t PhyTypes(Api& api, HANDLE handle, const GUID& guid) {
  if (!api.capability) return 0;
  Memory memory(api);
  if (api.capability(handle, &guid, nullptr, reinterpret_cast<WLAN_INTERFACE_CAPABILITY**>(&memory.value))
      != ERROR_SUCCESS || !memory.value) return 0;
  const auto* capability = static_cast<const WLAN_INTERFACE_CAPABILITY*>(memory.value);
  DWORD count = capability->dwNumberOfSupportedPhys;
  if (count > WLAN_MAX_PHY_INDEX) return 0;
  uint32_t mask = 0;
  for (DWORD i = 0; i < count; i++) {
    uint32_t type = static_cast<uint32_t>(capability->dot11PhyTypes[i]);
    if (type >= 1 && type <= 30) mask |= 1u << type; // IHV-defined PHY types are not standards.
  }
  return mask;
}

std::string Hardware(Api& api) {
  if (!api.ready() || wlan_broken.load()) return {};
  Client client(api);
  DWORD version = 0;
  if (api.open(2, nullptr, &version, &client.handle) != ERROR_SUCCESS) return {};
  Memory interfaces(api);
  if (api.interfaces(client.handle, nullptr, reinterpret_cast<WLAN_INTERFACE_INFO_LIST**>(&interfaces.value)) != ERROR_SUCCESS || !interfaces.value) return {};
  const auto* list = static_cast<WLAN_INTERFACE_INFO_LIST*>(interfaces.value);
  if (list->dwNumberOfItems > 64) return {};
  std::string text = "hw\t" + std::to_string(list->dwNumberOfItems) + "\n";
  for (DWORD i = 0; i < list->dwNumberOfItems; i++) {
    const GUID& guid = list->InterfaceInfo[i].InterfaceGuid;
    uint32_t auth = 0, cipher = 0;
    if (!AuthCipherPairs(api, client.handle, guid, &auth, &cipher)) auth = cipher = 0;
    text += "cap\t" + std::to_string(PhyTypes(api, client.handle, guid)) + "\t" + std::to_string(auth)
        + "\t" + std::to_string(cipher) + "\n";
  }
  return text;
}

// WLAN notification watcher. Windows callbacks only record event classes and signal;
// the Java thread that called Watch drains them, so no JNI runs on Windows' threads.
constexpr uint32_t kWatchState = 1, kWatchScan = 2, kWatchConnection = 4, kWatchInterfaces = 8;
struct WatchState {
  std::atomic<uint32_t> pending{0};
  HANDLE event = nullptr;
  WatchState() : event(CreateEventW(nullptr, FALSE, FALSE, nullptr)) { }
  ~WatchState() { if (event) CloseHandle(event); }
};
uint32_t WatchBits(DWORD source, DWORD code) {
  if (source == WLAN_NOTIFICATION_SOURCE_ACM) {
    switch (code) {
      case wlan_notification_acm_scan_complete: return kWatchScan;
      case wlan_notification_acm_interface_arrival:
      case wlan_notification_acm_interface_removal: return kWatchInterfaces | kWatchState | kWatchConnection;
      case wlan_notification_acm_connection_complete:
      case wlan_notification_acm_connection_attempt_fail:
      case wlan_notification_acm_disconnected: return kWatchConnection;
      default: return 0;
    }
  }
  if (source == WLAN_NOTIFICATION_SOURCE_MSM) {
    switch (code) {
      case wlan_notification_msm_radio_state_change: return kWatchState | kWatchConnection;
      case wlan_notification_msm_connected:
      case wlan_notification_msm_disconnected: return kWatchConnection;
      default: return 0;
    }
  }
  return 0;
}
void WINAPI OnWatch(WLAN_NOTIFICATION_DATA* notification, void* context) {
  if (!notification || !context) return;
  uint32_t bits = WatchBits(notification->NotificationSource, notification->NotificationCode);
  if (!bits) return;
  auto* state = static_cast<WatchState*>(context);
  state->pending.fetch_or(bits);
  SetEvent(state->event);
}
// A registration silently stops when the WLAN service restarts or stops; a quiet poll
// interval therefore confirms the handle still reaches the service.
bool WatchAlive(Api& api, HANDLE handle) {
  Memory interfaces(api);
  return api.interfaces(handle, nullptr, reinterpret_cast<WLAN_INTERFACE_INFO_LIST**>(&interfaces.value))
      == ERROR_SUCCESS && interfaces.value;
}
// Runs until sink returns false (0) or WLAN cannot be watched (-1), including when the
// service behind the handle went away, so the caller reopens it. The first delivery
// follows registration, so a change racing the initial read is not lost.
int Watch(Api& api, bool (*sink)(void*, uint32_t), void* context, DWORD poll_ms) {
  if (!api.ready() || wlan_broken.load()) return -1;
  std::unique_ptr<WatchState> state(new (std::nothrow) WatchState());
  if (!state || !state->event) return -1;
  HANDLE handle = nullptr;
  DWORD version = 0;
  if (api.open(2, nullptr, &version, &handle) != ERROR_SUCCESS) return -1;
  int result = -1;
  if (api.notify(handle, WLAN_NOTIFICATION_SOURCE_ACM | WLAN_NOTIFICATION_SOURCE_MSM, FALSE, OnWatch,
                 state.get(), nullptr, nullptr) == ERROR_SUCCESS) {
    result = 0;
    bool running = sink(context, kWatchState | kWatchScan | kWatchConnection | kWatchInterfaces);
    while (running) {
      if (WaitForSingleObject(state->event, poll_ms) == WAIT_TIMEOUT && !WatchAlive(api, handle)) {
        result = -1;
        break;
      }
      running = sink(context, state->pending.exchange(0));
    }
    api.notify(handle, WLAN_NOTIFICATION_SOURCE_NONE, FALSE, nullptr, nullptr, nullptr, nullptr);
  }
  // Close drains in-flight callbacks. If it fails, retain the target as Client does.
  if (api.close(handle, nullptr) != ERROR_SUCCESS) {
    wlan_broken = true;
    state.release();
    return -1;
  }
  return result;
}

// ---- User-approved connections ---------------------------------------------------
// Outcomes use WifiManager.STATUS_LOCAL_ONLY_CONNECTION_FAILURE_* values; the extra
// values are internal. A connection is reported only after Windows' own completion
// notification for this attempt's profile on this interface.
constexpr int kConnected = -1, kFailUnknown = 0, kFailAssociation = 1, kFailAuthentication = 2,
    kFailNotFound = 4, kFailNoResponse = 5, kCancelled = 100;

int ConnectFailure(DWORD reason) {
  if (reason == WLAN_REASON_CODE_NOT_VISIBLE || reason == WLAN_REASON_CODE_NETWORK_NOT_AVAILABLE) return kFailNotFound;
  if (reason == WLAN_REASON_CODE_ASSOCIATION_FAILURE || reason == WLAN_REASON_CODE_ASSOCIATION_TIMEOUT) return kFailAssociation;
  if (reason == WLAN_REASON_CODE_START_SECURITY_FAILURE || reason == WLAN_REASON_CODE_SECURITY_FAILURE
      || reason == WLAN_REASON_CODE_SECURITY_TIMEOUT
      || (reason >= WLAN_REASON_CODE_MSMSEC_BASE && reason <= WLAN_REASON_CODE_MSMSEC_END)) return kFailAuthentication;
  return kFailUnknown;
}

struct ConnectWait {
  GUID guid{};
  WCHAR profile[WLAN_MAX_NAME_LENGTH] = {};
  std::atomic<int> outcome{-2}; // -2 pending, else a value above
  HANDLE event = nullptr;
  ConnectWait() : event(CreateEventW(nullptr, FALSE, FALSE, nullptr)) { }
  ~ConnectWait() { if (event) CloseHandle(event); }
};

void WINAPI OnConnect(WLAN_NOTIFICATION_DATA* notification, void* context) {
  if (!notification || !context || notification->NotificationSource != WLAN_NOTIFICATION_SOURCE_ACM) return;
  DWORD code = notification->NotificationCode;
  if (code != wlan_notification_acm_connection_complete && code != wlan_notification_acm_connection_attempt_fail) return;
  auto* wait = static_cast<ConnectWait*>(context);
  if (memcmp(&wait->guid, &notification->InterfaceGuid, sizeof(GUID)) != 0) return;
  if (!notification->pData || notification->dwDataSize < offsetof(WLAN_CONNECTION_NOTIFICATION_DATA, strProfileXml)) return;
  const auto* data = static_cast<const WLAN_CONNECTION_NOTIFICATION_DATA*>(notification->pData);
  // Another profile's completion (an earlier connection) is not this attempt's outcome.
  if (wcsncmp(data->strProfileName, wait->profile, WLAN_MAX_NAME_LENGTH) != 0) return;
  int outcome = data->wlanReasonCode == WLAN_REASON_CODE_SUCCESS && code == wlan_notification_acm_connection_complete
      ? kConnected : ConnectFailure(data->wlanReasonCode);
  int pending = -2;
  if (wait->outcome.compare_exchange_strong(pending, outcome)) SetEvent(wait->event);
}

bool InterfaceGuid(Api& api, HANDLE handle, ULONG if_index, GUID* guid) {
  Memory interfaces(api);
  if (api.interfaces(handle, nullptr, reinterpret_cast<WLAN_INTERFACE_INFO_LIST**>(&interfaces.value)) != ERROR_SUCCESS || !interfaces.value) return false;
  const auto* list = static_cast<WLAN_INTERFACE_INFO_LIST*>(interfaces.value);
  if (list->dwNumberOfItems > 64) return false;
  for (DWORD i = 0; i < list->dwNumberOfItems; i++) {
    if (Index(list->InterfaceInfo[i].InterfaceGuid) == if_index) { *guid = list->InterfaceInfo[i].InterfaceGuid; return true; }
  }
  return false;
}

// Stores the per-user, manual-connection profile and waits (bounded) for its outcome.
int Connect(Api& api, ULONG if_index, const WCHAR* xml, const WCHAR* profile, const unsigned char* bssid,
            bool (*active)(void*), void* context, DWORD timeout_ms) {
  if (!api.connectable() || wlan_broken.load() || wcslen(profile) >= WLAN_MAX_NAME_LENGTH) return kFailUnknown;
  HANDLE handle = nullptr;
  DWORD version = 0;
  if (api.open(2, nullptr, &version, &handle) != ERROR_SUCCESS) return kFailUnknown;
  std::unique_ptr<ConnectWait> wait(new (std::nothrow) ConnectWait());
  int result = kFailUnknown;
  bool registered = false;
  if (wait && wait->event && InterfaceGuid(api, handle, if_index, &wait->guid)) {
    wcsncpy(wait->profile, profile, WLAN_MAX_NAME_LENGTH - 1);
    registered = api.notify(handle, WLAN_NOTIFICATION_SOURCE_ACM, FALSE, OnConnect, wait.get(), nullptr, nullptr) == ERROR_SUCCESS;
    DWORD reason = 0;
    if (registered && active(context)
        && api.set_profile(handle, &wait->guid, WLAN_PROFILE_USER, xml, nullptr, TRUE, nullptr, &reason) == ERROR_SUCCESS) {
      DOT11_BSSID_LIST desired{};
      WLAN_CONNECTION_PARAMETERS parameters{};
      parameters.wlanConnectionMode = wlan_connection_mode_profile;
      parameters.strProfile = profile;
      parameters.dot11BssType = dot11_BSS_type_infrastructure;
      if (bssid) {
        // Pin the user-selected access point, as Android pins the request's BSSID.
        desired.Header.Type = NDIS_OBJECT_TYPE_DEFAULT;
        desired.Header.Revision = DOT11_BSSID_LIST_REVISION_1;
        desired.Header.Size = sizeof(DOT11_BSSID_LIST);
        desired.uNumOfEntries = desired.uTotalNumOfEntries = 1;
        memcpy(desired.BSSIDs[0], bssid, 6);
        parameters.pDesiredBssidList = &desired;
      }
      if (api.connect(handle, &wait->guid, &parameters, nullptr) == ERROR_SUCCESS) {
        ULONGLONG deadline = GetTickCount64() + timeout_ms;
        result = kFailNoResponse;
        for (;;) {
          int outcome = wait->outcome.load();
          if (outcome != -2) { result = outcome; break; }
          if (!active(context)) { result = kCancelled; break; }
          ULONGLONG now = GetTickCount64();
          if (now >= deadline) break;
          WaitForSingleObject(wait->event, static_cast<DWORD>(deadline - now < 250 ? deadline - now : 250));
        }
      }
    }
  }
  if (registered) api.notify(handle, WLAN_NOTIFICATION_SOURCE_NONE, FALSE, nullptr, nullptr, nullptr, nullptr);
  if (api.close(handle, nullptr) != ERROR_SUCCESS) {
    wlan_broken = true;
    wait.release();
  }
  return result;
}

// Ends a Gesher connection: disconnect, delete the temporary profile and, when the
// interface had a previous connection, ask Windows to return to that profile.
bool Disconnect(Api& api, ULONG if_index, const WCHAR* remove, const WCHAR* restore) {
  if (!api.connectable() || wlan_broken.load()) return false;
  Client client(api);
  DWORD version = 0;
  GUID guid{};
  if (api.open(2, nullptr, &version, &client.handle) != ERROR_SUCCESS
      || !InterfaceGuid(api, client.handle, if_index, &guid)) return false;
  bool ok = api.disconnect(client.handle, &guid, nullptr) == ERROR_SUCCESS;
  if (remove && *remove) ok = api.delete_profile(client.handle, &guid, remove, nullptr) == ERROR_SUCCESS && ok;
  if (restore && *restore) {
    WLAN_CONNECTION_PARAMETERS parameters{};
    parameters.wlanConnectionMode = wlan_connection_mode_profile;
    parameters.strProfile = restore;
    parameters.dot11BssType = dot11_BSS_type_infrastructure;
    ok = api.connect(client.handle, &guid, &parameters, nullptr) == ERROR_SUCCESS && ok;
  }
  return ok;
}

// Moves a stored profile to the end of the interface's preference list. Windows adds a
// new profile at the top, above every network the user saved; an app's suggestion must
// not outrank them (Android scores user-saved networks above suggestions).
bool LowestPriority(Api& api, HANDLE handle, const GUID& guid, const WCHAR* profile) {
  if (!api.profiles || !api.position) return false;
  Memory memory(api);
  if (api.profiles(handle, &guid, nullptr, reinterpret_cast<WLAN_PROFILE_INFO_LIST**>(&memory.value)) != ERROR_SUCCESS
      || !memory.value) return false;
  DWORD count = static_cast<const WLAN_PROFILE_INFO_LIST*>(memory.value)->dwNumberOfItems;
  return count > 0 && api.position(handle, &guid, profile, count - 1, nullptr) == ERROR_SUCCESS;
}

// Stores (or removes, with xml == nullptr) a per-user profile without connecting. A stored
// profile ranks below the user's own; if Windows cannot place it there it is removed.
bool StoreProfile(Api& api, ULONG if_index, const WCHAR* xml, const WCHAR* profile) {
  if (!api.connectable() || wlan_broken.load()) return false;
  Client client(api);
  DWORD version = 0, reason = 0;
  GUID guid{};
  if (api.open(2, nullptr, &version, &client.handle) != ERROR_SUCCESS
      || !InterfaceGuid(api, client.handle, if_index, &guid)) return false;
  if (!xml) return api.delete_profile(client.handle, &guid, profile, nullptr) == ERROR_SUCCESS;
  if (api.set_profile(client.handle, &guid, WLAN_PROFILE_USER, xml, nullptr, TRUE, nullptr, &reason) != ERROR_SUCCESS) {
    return false;
  }
  if (LowestPriority(api, client.handle, guid, profile)) return true;
  api.delete_profile(client.handle, &guid, profile, nullptr);
  return false;
}

// Names of this user's profiles on the interface that start with prefix, '\n'-separated.
std::wstring Profiles(Api& api, ULONG if_index, const WCHAR* prefix) {
  if (!api.ready() || !api.profiles || wlan_broken.load()) return {};
  Client client(api);
  DWORD version = 0;
  GUID guid{};
  if (api.open(2, nullptr, &version, &client.handle) != ERROR_SUCCESS
      || !InterfaceGuid(api, client.handle, if_index, &guid)) return {};
  Memory memory(api);
  if (api.profiles(client.handle, &guid, nullptr, reinterpret_cast<WLAN_PROFILE_INFO_LIST**>(&memory.value)) != ERROR_SUCCESS
      || !memory.value) return {};
  const auto* list = static_cast<const WLAN_PROFILE_INFO_LIST*>(memory.value);
  std::wstring names;
  size_t prefix_length = wcslen(prefix);
  for (DWORD i = 0; i < list->dwNumberOfItems && i < 4096; i++) {
    WCHAR name[WLAN_MAX_NAME_LENGTH];
    memcpy(name, list->ProfileInfo[i].strProfileName, sizeof(name));
    name[WLAN_MAX_NAME_LENGTH - 1] = 0;
    // Only per-user profiles: Gesher never stores all-user ones, and never removes them.
    if ((list->ProfileInfo[i].dwFlags & WLAN_PROFILE_USER) && wcsncmp(name, prefix, prefix_length) == 0) {
      names += name;
      names += L'\n';
    }
  }
  return names;
}

// The connected profile's name on this interface, or empty when not connected.
std::wstring CurrentProfile(Api& api, ULONG if_index) {
  if (!api.ready() || wlan_broken.load()) return {};
  Client client(api);
  DWORD version = 0;
  GUID guid{};
  if (api.open(2, nullptr, &version, &client.handle) != ERROR_SUCCESS
      || !InterfaceGuid(api, client.handle, if_index, &guid)) return {};
  WLAN_CONNECTION_ATTRIBUTES connection = {};
  if (!Query(api, client.handle, guid, wlan_intf_opcode_current_connection, &connection)
      || connection.isState != wlan_interface_state_connected) return {};
  connection.strProfileName[WLAN_MAX_NAME_LENGTH - 1] = 0;
  return connection.strProfileName;
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
// Copies a Java string to a NUL-terminated wide string; false for embedded NULs.
bool Wide(JNIEnv* env, jstring text, std::wstring* out, jsize maximum) {
  if (!text) return false;
  jsize length = env->GetStringLength(text);
  if (length <= 0 || length > maximum) return false;
  std::wstring value(static_cast<size_t>(length), L'\0');
  env->GetStringRegion(text, 0, length, reinterpret_cast<jchar*>(&value[0]));
  if (env->ExceptionCheck() || value.find(L'\0') != std::wstring::npos) return false;
  *out = std::move(value);
  return true;
}
jint WindowsWifi_nativeConnect(JNIEnv* env, jclass, jint if_index, jstring xml, jstring profile,
                               jbyteArray bssid, jobject active, jint timeout_ms) {
  std::wstring wide_xml, wide_profile;
  if (if_index <= 0 || timeout_ms <= 0 || !active || !Wide(env, xml, &wide_xml, 64 * 1024)
      || !Wide(env, profile, &wide_profile, WLAN_MAX_NAME_LENGTH - 1)) return kFailUnknown;
  unsigned char mac[6];
  bool pinned = bssid != nullptr;
  if (pinned) {
    if (env->GetArrayLength(bssid) != 6) return kFailUnknown;
    env->GetByteArrayRegion(bssid, 0, 6, reinterpret_cast<jbyte*>(mac));
  }
  jclass type = env->GetObjectClass(active);
  if (!type) return kFailUnknown;
  jmethodID method = env->GetMethodID(type, "getAsBoolean", "()Z");
  env->DeleteLocalRef(type);
  if (!method) return kFailUnknown;
  Eligibility gate{env, active, method};
  int result = Connect(Wlan(), static_cast<ULONG>(if_index), wide_xml.c_str(), wide_profile.c_str(),
                       pinned ? mac : nullptr, Eligible, &gate, static_cast<DWORD>(timeout_ms));
  // The profile text may carry a passphrase: clear the native copy.
  SecureZeroMemory(&wide_xml[0], wide_xml.size() * sizeof(WCHAR));
  if (env->ExceptionCheck()) return kFailUnknown;
  return result;
}
jboolean WindowsWifi_nativeDisconnect(JNIEnv* env, jclass, jint if_index, jstring remove, jstring restore) {
  std::wstring wide_remove, wide_restore;
  if (if_index <= 0) return JNI_FALSE;
  if (remove && !Wide(env, remove, &wide_remove, WLAN_MAX_NAME_LENGTH - 1)) return JNI_FALSE;
  if (restore && !Wide(env, restore, &wide_restore, WLAN_MAX_NAME_LENGTH - 1)) return JNI_FALSE;
  return Disconnect(Wlan(), static_cast<ULONG>(if_index), wide_remove.c_str(), wide_restore.c_str())
      ? JNI_TRUE : JNI_FALSE;
}
jboolean WindowsWifi_nativeStoreProfile(JNIEnv* env, jclass, jint if_index, jstring xml, jstring profile) {
  std::wstring wide_xml, wide_profile;
  if (if_index <= 0 || !Wide(env, profile, &wide_profile, WLAN_MAX_NAME_LENGTH - 1)) return JNI_FALSE;
  if (xml && !Wide(env, xml, &wide_xml, 64 * 1024)) return JNI_FALSE;
  bool ok = StoreProfile(Wlan(), static_cast<ULONG>(if_index), xml ? wide_xml.c_str() : nullptr, wide_profile.c_str());
  if (!wide_xml.empty()) SecureZeroMemory(&wide_xml[0], wide_xml.size() * sizeof(WCHAR));
  return ok ? JNI_TRUE : JNI_FALSE;
}
jstring WindowsWifi_nativeCurrentProfile(JNIEnv* env, jclass, jint if_index) {
  if (if_index <= 0) return nullptr;
  std::wstring name = CurrentProfile(Wlan(), static_cast<ULONG>(if_index));
  if (name.empty()) return nullptr;
  return env->NewString(reinterpret_cast<const jchar*>(name.c_str()), static_cast<jsize>(name.size()));
}
jstring WindowsWifi_nativeProfiles(JNIEnv* env, jclass, jint if_index, jstring prefix) {
  std::wstring wide_prefix;
  if (if_index <= 0 || !Wide(env, prefix, &wide_prefix, 64)) return nullptr;
  std::wstring names = Profiles(Wlan(), static_cast<ULONG>(if_index), wide_prefix.c_str());
  return env->NewString(reinterpret_cast<const jchar*>(names.c_str()), static_cast<jsize>(names.size()));
}
jstring WindowsWifi_nativeHardware(JNIEnv* env, jclass) {
  std::string text = Hardware(Wlan());
  return text.empty() ? nullptr : env->NewStringUTF(text.c_str());
}
struct WatchSink {
  JNIEnv* env;
  jobject predicate;
  jmethodID method;
};
bool DeliverWatch(void* context, uint32_t bits) {
  auto* sink = static_cast<WatchSink*>(context);
  if (sink->env->ExceptionCheck()) return false;
  jboolean running = sink->env->CallBooleanMethod(sink->predicate, sink->method, static_cast<jint>(bits));
  return !sink->env->ExceptionCheck() && running == JNI_TRUE;
}
jint WindowsWifi_nativeWatch(JNIEnv* env, jclass, jobject predicate, jint poll_ms) {
  if (!predicate || poll_ms <= 0) return -1;
  jclass type = env->GetObjectClass(predicate);
  if (!type) return -1;
  jmethodID method = env->GetMethodID(type, "test", "(I)Z");
  env->DeleteLocalRef(type);
  if (!method) return -1;
  WatchSink sink{env, predicate, method};
  int result = Watch(Wlan(), DeliverWatch, &sink, static_cast<DWORD>(poll_ms));
  return env->ExceptionCheck() ? -1 : result;
}
const JNINativeMethod methods[] = {
    {"nativeQuery", "(ZLjava/util/function/BooleanSupplier;)Ljava/lang/String;", NATIVEHELPER_SYSV_JNI_ENTRY(WindowsWifi_nativeQuery)},
    {"nativeHardware", "()Ljava/lang/String;", NATIVEHELPER_SYSV_JNI_ENTRY(WindowsWifi_nativeHardware)},
    {"nativeWatch", "(Ljava/util/function/IntPredicate;I)I", NATIVEHELPER_SYSV_JNI_ENTRY(WindowsWifi_nativeWatch)},
    {"nativeConnect", "(ILjava/lang/String;Ljava/lang/String;[BLjava/util/function/BooleanSupplier;I)I", NATIVEHELPER_SYSV_JNI_ENTRY(WindowsWifi_nativeConnect)},
    {"nativeDisconnect", "(ILjava/lang/String;Ljava/lang/String;)Z", NATIVEHELPER_SYSV_JNI_ENTRY(WindowsWifi_nativeDisconnect)},
    {"nativeStoreProfile", "(ILjava/lang/String;Ljava/lang/String;)Z", NATIVEHELPER_SYSV_JNI_ENTRY(WindowsWifi_nativeStoreProfile)},
    {"nativeCurrentProfile", "(I)Ljava/lang/String;", NATIVEHELPER_SYSV_JNI_ENTRY(WindowsWifi_nativeCurrentProfile)},
    {"nativeProfiles", "(ILjava/lang/String;)Ljava/lang/String;", NATIVEHELPER_SYSV_JNI_ENTRY(WindowsWifi_nativeProfiles)},
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
