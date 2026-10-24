// Gesher: Windows' networks as Android's connectivity sees them
// (com.gesher.net.WindowsNetworks's natives, in the built-in library
// libgesher_net.so; docs/network.md, ADR 0037).
//
// A snapshot lists the adapters that are up and can reach other networks (a
// gateway, or the route Windows would take to the internet), each with its
// kind (Wi-Fi, Ethernet, cellular, VPN), addresses, gateways, DNS servers and
// suffix, MTU and link speeds, and from the Network List Manager whether it
// reaches the internet and whether it is metered or roaming. Changes of
// interfaces, addresses and routes (NotifyIpInterfaceChange and friends) set
// an event that nativeWaitForChange waits on.
//
// Win32 side (Windows' headers) but for the JNI table, which needs only jni.h.

#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <netioapi.h>
#include <objbase.h>
#include <netlistmgr.h>
#include <windows.h>

#include <stdio.h>
#include <string.h>
#include <wchar.h>
#include <wctype.h>

#include <map>
#include <mutex>
#include <string>
#include <vector>

#include <gesher/builtin_library.h>
#include <gesher/inet.h>
#include <jni.h>
#include <nativehelper/jni_macros.h>

namespace {

// The Network List Manager's ids (netlistmgr.h), spelled out so no import
// library has to provide them.
const CLSID kClsidNetworkListManager = {
    0xDCB00C01, 0x570F, 0x4A9B, {0x8D, 0x69, 0x19, 0x9F, 0xDB, 0xA5, 0x72, 0x3B}};
const IID kIidNetworkListManager = {
    0xDCB00000, 0x570F, 0x4A9B, {0x8D, 0x69, 0x19, 0x9F, 0xDB, 0xA5, 0x72, 0x3B}};
const IID kIidNetworkConnectionCost = {
    0xDCB0000A, 0x570F, 0x4A9B, {0x8D, 0x69, 0x19, 0x9F, 0xDB, 0xA5, 0x72, 0x3B}};

std::string Narrow(const wchar_t* w) {
  if (w == nullptr || *w == 0) return std::string();
  int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
  std::string s(static_cast<size_t>(n > 0 ? n - 1 : 0), '\0');
  if (n > 1) WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
  return s;
}

// Tabs and line ends would break the snapshot's lines.
std::string Field(std::string s) {
  for (char& c : s) {
    if (c == '\t' || c == '\n' || c == '\r') c = ' ';
  }
  return s;
}

std::string Address(const SOCKADDR* sa) {
  char text[INET6_ADDRSTRLEN] = {};
  if (sa->sa_family == AF_INET) {
    InetNtopA(AF_INET, &reinterpret_cast<const sockaddr_in*>(sa)->sin_addr, text, sizeof(text));
  } else if (sa->sa_family == AF_INET6) {
    InetNtopA(AF_INET6, &reinterpret_cast<const sockaddr_in6*>(sa)->sin6_addr, text, sizeof(text));
  }
  return text;
}

// What the Network List Manager says of an adapter.
struct NlmInfo {
  bool internet = false;
  bool metered = false;
  bool roaming = false;
  bool congested = false;
};

// By adapter GUID; empty when the Network List Manager cannot be asked.
std::map<GUID, NlmInfo, bool (*)(const GUID&, const GUID&)> QueryNlm(bool* machine_internet,
                                                                    bool* ok) {
  std::map<GUID, NlmInfo, bool (*)(const GUID&, const GUID&)> out(
      [](const GUID& a, const GUID& b) { return memcmp(&a, &b, sizeof(GUID)) < 0; });
  *ok = false;
  HRESULT init = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  INetworkListManager* nlm = nullptr;
  if (SUCCEEDED(CoCreateInstance(kClsidNetworkListManager, nullptr, CLSCTX_ALL,
                                 kIidNetworkListManager, reinterpret_cast<void**>(&nlm)))) {
    NLM_CONNECTIVITY connectivity = NLM_CONNECTIVITY_DISCONNECTED;
    if (SUCCEEDED(nlm->GetConnectivity(&connectivity))) {
      *machine_internet =
          (connectivity & (NLM_CONNECTIVITY_IPV4_INTERNET | NLM_CONNECTIVITY_IPV6_INTERNET)) != 0;
      *ok = true;
    }
    IEnumNetworkConnections* connections = nullptr;
    if (SUCCEEDED(nlm->GetNetworkConnections(&connections)) && connections != nullptr) {
      INetworkConnection* c = nullptr;
      ULONG fetched = 0;
      while (connections->Next(1, &c, &fetched) == S_OK && fetched == 1) {
        GUID adapter;
        NLM_CONNECTIVITY cc = NLM_CONNECTIVITY_DISCONNECTED;
        if (SUCCEEDED(c->GetAdapterId(&adapter)) && SUCCEEDED(c->GetConnectivity(&cc))) {
          NlmInfo& info = out[adapter];
          info.internet =
              (cc & (NLM_CONNECTIVITY_IPV4_INTERNET | NLM_CONNECTIVITY_IPV6_INTERNET)) != 0;
          INetworkConnectionCost* cost = nullptr;
          if (SUCCEEDED(c->QueryInterface(kIidNetworkConnectionCost,
                                          reinterpret_cast<void**>(&cost)))) {
            DWORD value = 0;
            if (SUCCEEDED(cost->GetCost(&value))) {
              info.metered = (value & (NLM_CONNECTION_COST_FIXED | NLM_CONNECTION_COST_VARIABLE)) != 0;
              info.roaming = (value & NLM_CONNECTION_COST_ROAMING) != 0;
              info.congested = (value & NLM_CONNECTION_COST_CONGESTED) != 0;
            }
            cost->Release();
          }
        }
        c->Release();
      }
      connections->Release();
    }
    nlm->Release();
  }
  if (SUCCEEDED(init)) CoUninitialize();
  return out;
}

// The interface Windows routes `address` (a public one) through, or 0.
DWORD BestInterface(int family) {
  sockaddr_storage dest = {};
  if (family == AF_INET) {
    auto* in = reinterpret_cast<sockaddr_in*>(&dest);
    in->sin_family = AF_INET;
    InetPtonA(AF_INET, "8.8.8.8", &in->sin_addr);
  } else {
    auto* in6 = reinterpret_cast<sockaddr_in6*>(&dest);
    in6->sin6_family = AF_INET6;
    InetPtonA(AF_INET6, "2001:4860:4860::8888", &in6->sin6_addr);
  }
  DWORD index = 0;
  if (GetBestInterfaceEx(reinterpret_cast<sockaddr*>(&dest), &index) != NO_ERROR) return 0;
  return index;
}

bool Contains(const wchar_t* haystack, const wchar_t* needle) {
  if (haystack == nullptr) return false;
  std::wstring h(haystack), n(needle);
  for (auto& c : h) c = static_cast<wchar_t>(towlower(c));
  for (auto& c : n) c = static_cast<wchar_t>(towlower(c));
  return h.find(n) != std::wstring::npos;
}

const char* Kind(const IP_ADAPTER_ADDRESSES* a) {
  // Tunnels, PPP and virtual adapters of VPN clients are VPNs.
  static const wchar_t* const kVpnNames[] = {L"vpn", L"tap-windows", L"wireguard", L"wintun",
                                             L"openvpn", L"anyconnect", L"fortinet", L"pangp",
                                             L"globalprotect", L"zerotier", L"tailscale"};
  bool vpn = a->IfType == IF_TYPE_PPP || a->IfType == IF_TYPE_PROP_VIRTUAL ||
             a->IfType == IF_TYPE_TUNNEL;
  for (const wchar_t* name : kVpnNames) {
    vpn = vpn || Contains(a->Description, name) || Contains(a->FriendlyName, name);
  }
  if (vpn) return "vpn";
  switch (a->IfType) {
    case IF_TYPE_IEEE80211:
      return "wifi";
    case IF_TYPE_ETHERNET_CSMACD:
      return "ethernet";
    case IF_TYPE_WWANPP:
    case IF_TYPE_WWANPP2:
      return "cellular";
    default:
      return "other";
  }
}

std::string Snapshot() {
  bool machine_internet = false, nlm_ok = false;
  auto nlm = QueryNlm(&machine_internet, &nlm_ok);
  DWORD best4 = BestInterface(AF_INET), best6 = BestInterface(AF_INET6);

  ULONG size = 32 * 1024;
  std::vector<unsigned char> buffer;
  ULONG result = ERROR_BUFFER_OVERFLOW;
  for (int attempt = 0; attempt < 4 && result == ERROR_BUFFER_OVERFLOW; attempt++) {
    buffer.resize(size);
    result = GetAdaptersAddresses(AF_UNSPEC,
                                  GAA_FLAG_INCLUDE_GATEWAYS | GAA_FLAG_SKIP_ANYCAST |
                                      GAA_FLAG_SKIP_MULTICAST,
                                  nullptr, reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()),
                                  &size);
  }
  std::string out;
  char line[512];
  snprintf(line, sizeof(line), "default\t%lu\t%lu\ninternet\t%d\t%d\n", best4, best6,
           machine_internet ? 1 : 0, nlm_ok ? 1 : 0);
  out += line;
  if (result != NO_ERROR) return out;
  for (auto* a = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()); a != nullptr;
       a = a->Next) {
    if (a->OperStatus != IfOperStatusUp || a->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
    DWORD index = a->IfIndex != 0 ? a->IfIndex : a->Ipv6IfIndex;
    bool is_default = index == best4 || index == best6 ||
                      (a->Ipv6IfIndex != 0 && a->Ipv6IfIndex == best6);
    if (a->FirstGatewayAddress == nullptr && !is_default) continue;  // a host-only network
    NlmInfo info;
    bool known = false;
    GUID guid;
    std::wstring name(a->AdapterName != nullptr ? std::wstring(a->AdapterName,
                                                               a->AdapterName + strlen(a->AdapterName))
                                                : std::wstring());
    if (!name.empty() && SUCCEEDED(CLSIDFromString(name.c_str(), &guid))) {
      auto it = nlm.find(guid);
      if (it != nlm.end()) {
        info = it->second;
        known = true;
      }
    }
    // Without the Network List Manager's word, a gateway is taken for the internet.
    bool internet = known ? info.internet : (!nlm_ok && a->FirstGatewayAddress != nullptr);
    snprintf(line, sizeof(line), "net\t%lu\t%s\t%lu\t%llu\t%llu\t%d\t%d\t%d\t%d\t%d\t",
             index, Kind(a), a->Mtu, static_cast<unsigned long long>(a->ReceiveLinkSpeed),
             static_cast<unsigned long long>(a->TransmitLinkSpeed), info.metered ? 1 : 0,
             info.roaming ? 1 : 0, info.congested ? 1 : 0, internet ? 1 : 0,
             is_default ? 1 : 0);
    out += line;
    out += Field(Narrow(a->FriendlyName)) + "\t" + Field(Narrow(a->Description)) + "\n";
    for (auto* u = a->FirstUnicastAddress; u != nullptr; u = u->Next) {
      snprintf(line, sizeof(line), "addr\t%lu\t%s\t%u\n", index,
               Address(u->Address.lpSockaddr).c_str(), u->OnLinkPrefixLength);
      out += line;
    }
    for (auto* g = a->FirstGatewayAddress; g != nullptr; g = g->Next) {
      snprintf(line, sizeof(line), "gw\t%lu\t%s\n", index, Address(g->Address.lpSockaddr).c_str());
      out += line;
    }
    for (auto* d = a->FirstDnsServerAddress; d != nullptr; d = d->Next) {
      std::string dns = Address(d->Address.lpSockaddr);
      // Windows' site-local placeholders (fec0:0:0:ffff::1..3) are not servers.
      if (dns.rfind("fec0:0:0:ffff::", 0) == 0) continue;
      snprintf(line, sizeof(line), "dns\t%lu\t%s\n", index, dns.c_str());
      out += line;
    }
    std::string suffix = Field(Narrow(a->DnsSuffix));
    if (!suffix.empty()) out += "suffix\t" + std::to_string(index) + "\t" + suffix + "\n";
  }
  return out;
}

// --- change notifications -------------------------------------------------------

HANDLE g_changed = nullptr;
std::once_flag g_watch_once;

void CALLBACK OnInterfaceChange(PVOID, PMIB_IPINTERFACE_ROW, MIB_NOTIFICATION_TYPE) {
  SetEvent(g_changed);
}
void CALLBACK OnAddressChange(PVOID, PMIB_UNICASTIPADDRESS_ROW, MIB_NOTIFICATION_TYPE) {
  SetEvent(g_changed);
}
void CALLBACK OnRouteChange(PVOID, PMIB_IPFORWARD_ROW2, MIB_NOTIFICATION_TYPE) {
  SetEvent(g_changed);
}

void StartWatching() {
  g_changed = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  HANDLE h = nullptr;
  NotifyIpInterfaceChange(AF_UNSPEC, OnInterfaceChange, nullptr, FALSE, &h);
  h = nullptr;
  NotifyUnicastIpAddressChange(AF_UNSPEC, OnAddressChange, nullptr, FALSE, &h);
  h = nullptr;
  NotifyRouteChange2(AF_UNSPEC, OnRouteChange, nullptr, FALSE, &h);
}

// --- JNI ---------------------------------------------------------------------------

jstring WindowsNetworks_nativeSnapshot(JNIEnv* env, jclass) {
  return env->NewStringUTF(Snapshot().c_str());
}

jboolean WindowsNetworks_nativeWaitForChange(JNIEnv*, jclass, jint timeout_ms) {
  std::call_once(g_watch_once, StartWatching);
  if (g_changed == nullptr) {
    Sleep(static_cast<DWORD>(timeout_ms));
    return JNI_FALSE;
  }
  DWORD r = WaitForSingleObject(g_changed, timeout_ms < 0 ? INFINITE : static_cast<DWORD>(timeout_ms));
  if (r != WAIT_OBJECT_0) return JNI_FALSE;
  // Changes come in bursts (an interface, then its addresses and routes): one wake-up.
  Sleep(300);
  ResetEvent(g_changed);
  return JNI_TRUE;
}

void WindowsNetworks_nativeSetInetAllowed(JNIEnv*, jclass, jboolean allowed) {
  gesher_set_inet_allowed(allowed ? 1 : 0);
}

jboolean WindowsNetworks_nativeInetAllowed(JNIEnv*, jclass) {
  return gesher_inet_allowed() ? JNI_TRUE : JNI_FALSE;
}

const JNINativeMethod kMethods[] = {
    {"nativeSnapshot", "()Ljava/lang/String;",
     NATIVEHELPER_SYSV_JNI_ENTRY(WindowsNetworks_nativeSnapshot)},
    {"nativeWaitForChange", "(I)Z",
     NATIVEHELPER_SYSV_JNI_ENTRY(WindowsNetworks_nativeWaitForChange)},
    {"nativeSetInetAllowed", "(Z)V",
     NATIVEHELPER_SYSV_JNI_ENTRY(WindowsNetworks_nativeSetInetAllowed)},
    {"nativeInetAllowed", "()Z", NATIVEHELPER_SYSV_JNI_ENTRY(WindowsNetworks_nativeInetAllowed)},
};

jint OnLoad(JavaVM* vm, void*) {
  JNIEnv* env = nullptr;
  if (vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK) return JNI_ERR;
  jclass c = env->FindClass("com/gesher/net/WindowsNetworks");
  if (c == nullptr) return JNI_ERR;
  if (env->RegisterNatives(c, kMethods, sizeof(kMethods) / sizeof(kMethods[0])) != JNI_OK) {
    return JNI_ERR;
  }
  env->DeleteLocalRef(c);
  return JNI_VERSION_1_6;
}

const gesher_builtin_symbol kGesherNet[] = {
    {"JNI_OnLoad", reinterpret_cast<void*>(&OnLoad)},
};

const bool registered = [] {
  gesher_register_builtin_library("libgesher_net.so", kGesherNet,
                                  sizeof(kGesherNet) / sizeof(kGesherNet[0]));
  return true;
}();

}  // namespace
