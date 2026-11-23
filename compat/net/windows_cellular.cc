// Copyright (C) 2026 The Gesher Authors. SPDX-License-Identifier: Apache-2.0
// Read-only Mobile Broadband (WWAN) boundary for com.gesher.server.WindowsCellular.
//
// Reads Windows' Mobile Broadband API (mbnapi, the WwanSvc COM server) on the
// caller's thread: interface capabilities, ready state, subscriber information,
// home provider, registration, signal and packet-connection state. Nothing is
// written to the modem: no connect/disconnect, PIN, registration mode, SMS or
// provider changes. A machine without the WWAN service or without a cellular
// interface reports "none" rather than failing the runtime.
//
// The interface declarations below are written from the documented mbnapi.idl
// (Windows SDK); llvm-mingw does not ship that header. Only the methods up to the
// last one used are declared, in the IDL's vtable order. Interfaces are reached by
// IID through CoCreateInstance and QueryInterface, so a mismatched IID fails closed.
#include <windows.h>
#include <objbase.h>
#include <oleauto.h>
#include <cstdint>
#include <cstdio>
#include <string>
#include <gesher/builtin_library.h>
#include <jni.h>
#include <nativehelper/jni_macros.h>

namespace {
namespace mbn {
// {BDFEE05B-4418-11DD-90ED-001C257CCFF1}
const CLSID kManagerClass = {0xBDFEE05B, 0x4418, 0x11DD, {0x90, 0xED, 0x00, 0x1C, 0x25, 0x7C, 0xCF, 0xF1}};
// {DCBBBAB6-201B-4BBB-AAEE-338E368AF6FA}
const IID kManager = {0xDCBBBAB6, 0x201B, 0x4BBB, {0xAA, 0xEE, 0x33, 0x8E, 0x36, 0x8A, 0xF6, 0xFA}};
// {DCBBBAB6-2009-4BBB-AAEE-338E368AF6FA}
const IID kRegistration = {0xDCBBBAB6, 0x2009, 0x4BBB, {0xAA, 0xEE, 0x33, 0x8E, 0x36, 0x8A, 0xF6, 0xFA}};
// {DCBBBAB6-2003-4BBB-AAEE-338E368AF6FA}
const IID kSignal = {0xDCBBBAB6, 0x2003, 0x4BBB, {0xAA, 0xEE, 0x33, 0x8E, 0x36, 0x8A, 0xF6, 0xFA}};

struct InterfaceCaps {
  ULONG cellularClass;  // MBN_CELLULAR_CLASS
  ULONG voiceClass;     // MBN_VOICE_CLASS
  ULONG dataClass;
  BSTR customDataClass;
  ULONG gsmBandClass;
  ULONG cdmaBandClass;
  BSTR customBandClass;
  ULONG smsCaps;
  ULONG controlCaps;
  BSTR deviceID;
  BSTR manufacturer;
  BSTR model;
  BSTR firmwareInfo;
};
struct Provider {
  BSTR providerID;
  ULONG providerState;
  BSTR providerName;
  ULONG dataClass;
};

struct Connection : IUnknown {
  virtual HRESULT STDMETHODCALLTYPE get_ConnectionID(BSTR* id) = 0;
  virtual HRESULT STDMETHODCALLTYPE get_InterfaceID(BSTR* id) = 0;
  virtual HRESULT STDMETHODCALLTYPE Connect(ULONG mode, LPCWSTR profile, ULONG* request) = 0;
  virtual HRESULT STDMETHODCALLTYPE Disconnect(ULONG* request) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetConnectionState(ULONG* state, BSTR* profileName) = 0;
};
struct SubscriberInformation : IUnknown {
  virtual HRESULT STDMETHODCALLTYPE get_SubscriberID(BSTR* id) = 0;
  virtual HRESULT STDMETHODCALLTYPE get_SimIccID(BSTR* id) = 0;
  virtual HRESULT STDMETHODCALLTYPE get_TelephoneNumbers(SAFEARRAY** numbers) = 0;
};
struct Interface : IUnknown {
  virtual HRESULT STDMETHODCALLTYPE get_InterfaceID(BSTR* id) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetInterfaceCapability(InterfaceCaps* caps) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetSubscriberInformation(SubscriberInformation** info) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetReadyState(ULONG* state) = 0;
  virtual HRESULT STDMETHODCALLTYPE InEmergencyMode(VARIANT_BOOL* emergency) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetHomeProvider(Provider* provider) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetPreferredProviders(SAFEARRAY** providers) = 0;
  virtual HRESULT STDMETHODCALLTYPE SetPreferredProviders(SAFEARRAY* providers, ULONG* request) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetVisibleProviders(ULONG* age, SAFEARRAY** providers) = 0;
  virtual HRESULT STDMETHODCALLTYPE ScanNetwork(ULONG* request) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetConnection(Connection** connection) = 0;
};
struct Manager : IUnknown {
  virtual HRESULT STDMETHODCALLTYPE GetInterface(LPCWSTR id, Interface** result) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetInterfaces(SAFEARRAY** interfaces) = 0;
};
struct Registration : IUnknown {
  virtual HRESULT STDMETHODCALLTYPE GetRegisterState(ULONG* state) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetRegisterMode(ULONG* mode) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetProviderID(BSTR* id) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetProviderName(BSTR* name) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetRoamingText(BSTR* text) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetAvailableDataClasses(ULONG* classes) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetCurrentDataClass(ULONG* dataClass) = 0;
};
struct Signal : IUnknown {
  virtual HRESULT STDMETHODCALLTYPE GetSignalStrength(ULONG* strength) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetSignalError(ULONG* error) = 0;
};
}  // namespace mbn

// The Windows calls this adapter makes, replaceable only by the native test host.
struct Api {
  HRESULT (*initialize)(LPVOID, DWORD) = CoInitializeEx;
  void (*uninitialize)() = CoUninitialize;
  HRESULT (*create)(REFCLSID, LPUNKNOWN, DWORD, REFIID, LPVOID*) = CoCreateInstance;
  void (*free_string)(BSTR) = SysFreeString;
  UINT (*string_length)(BSTR) = SysStringLen;
  HRESULT (*lower)(SAFEARRAY*, UINT, LONG*) = SafeArrayGetLBound;
  HRESULT (*upper)(SAFEARRAY*, UINT, LONG*) = SafeArrayGetUBound;
  HRESULT (*element)(SAFEARRAY*, LONG*, void*) = SafeArrayGetElement;
  HRESULT (*destroy)(SAFEARRAY*) = SafeArrayDestroy;
};

constexpr LONG kMaxInterfaces = 8;
constexpr LONG kMaxNumbers = 8;
constexpr UINT kMaxStringUnits = 256;

template <class T>
struct Ref {
  T* value = nullptr;
  ~Ref() { if (value) value->Release(); }
};
struct String {
  Api& api;
  BSTR value = nullptr;
  explicit String(Api& api) : api(api) { }
  ~String() { if (value) api.free_string(value); }
};
struct Array {
  Api& api;
  SAFEARRAY* value = nullptr;
  explicit Array(Api& api) : api(api) { }
  ~Array() { if (value) api.destroy(value); }
};

// UTF-16 to UTF-8, bounded; unpaired surrogates become U+FFFD. Output is hex so
// operator names and numbers cross the JNI text boundary without any escaping.
std::string Hex(Api& api, BSTR text) {
  if (!text) return "";
  UINT length = api.string_length(text);
  if (length > kMaxStringUnits) length = kMaxStringUnits;
  std::string utf8;
  for (UINT i = 0; i < length; i++) {
    uint32_t c = static_cast<uint16_t>(text[i]);
    if (c >= 0xD800 && c <= 0xDBFF && i + 1 < length) {
      uint32_t low = static_cast<uint16_t>(text[i + 1]);
      if (low >= 0xDC00 && low <= 0xDFFF) {
        c = 0x10000 + ((c - 0xD800) << 10) + (low - 0xDC00);
        i++;
      } else {
        c = 0xFFFD;
      }
    } else if (c >= 0xD800 && c <= 0xDFFF) {
      c = 0xFFFD;
    }
    if (c < 0x80) {
      utf8 += static_cast<char>(c);
    } else if (c < 0x800) {
      utf8 += static_cast<char>(0xC0 | (c >> 6));
      utf8 += static_cast<char>(0x80 | (c & 0x3F));
    } else if (c < 0x10000) {
      utf8 += static_cast<char>(0xE0 | (c >> 12));
      utf8 += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
      utf8 += static_cast<char>(0x80 | (c & 0x3F));
    } else {
      utf8 += static_cast<char>(0xF0 | (c >> 18));
      utf8 += static_cast<char>(0x80 | ((c >> 12) & 0x3F));
      utf8 += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
      utf8 += static_cast<char>(0x80 | (c & 0x3F));
    }
  }
  constexpr char digits[] = "0123456789abcdef";
  std::string hex;
  hex.reserve(utf8.size() * 2);
  for (unsigned char b : utf8) { hex += digits[b >> 4]; hex += digits[b & 15]; }
  return hex;
}
// A field the modem did not report is "-", distinct from a reported empty value.
std::string Field(Api& api, HRESULT result, BSTR text) {
  return SUCCEEDED(result) ? Hex(api, text) : std::string("-");
}
std::string Number(HRESULT result, ULONG value) {
  if (FAILED(result)) return "-";
  char buffer[16];
  snprintf(buffer, sizeof(buffer), "%lu", static_cast<unsigned long>(value));
  return buffer;
}

void AppendCaps(Api& api, mbn::Interface* modem, std::string& line) {
  mbn::InterfaceCaps caps = {};
  HRESULT result = modem->GetInterfaceCapability(&caps);
  line += '\t' + Number(result, caps.cellularClass);
  line += '\t' + Number(result, caps.voiceClass);
  line += '\t' + Number(result, caps.dataClass);
  line += '\t' + Number(result, caps.smsCaps);
  line += '\t' + Field(api, result, caps.deviceID);
  line += '\t' + Field(api, result, caps.manufacturer);
  line += '\t' + Field(api, result, caps.model);
  line += '\t' + Field(api, result, caps.firmwareInfo);
  // The structure's strings belong to the caller even when some are empty.
  for (BSTR text : {caps.customDataClass, caps.customBandClass, caps.deviceID, caps.manufacturer,
                    caps.model, caps.firmwareInfo}) {
    if (text) api.free_string(text);
  }
}

void AppendSubscriber(Api& api, mbn::Interface* modem, std::string& line) {
  Ref<mbn::SubscriberInformation> info;
  HRESULT result = modem->GetSubscriberInformation(&info.value);
  if (FAILED(result) || !info.value) {
    line += "\t-\t-\t-";
    return;
  }
  String subscriber(api), iccid(api);
  line += '\t' + Field(api, info.value->get_SubscriberID(&subscriber.value), subscriber.value);
  line += '\t' + Field(api, info.value->get_SimIccID(&iccid.value), iccid.value);
  Array numbers(api);
  LONG low = 0, high = -1;
  if (FAILED(info.value->get_TelephoneNumbers(&numbers.value)) || !numbers.value
      || FAILED(api.lower(numbers.value, 1, &low)) || FAILED(api.upper(numbers.value, 1, &high))) {
    line += "\t-";
    return;
  }
  std::string list;
  for (LONG i = low, n = 0; i <= high && n < kMaxNumbers; i++, n++) {
    String number(api);
    if (FAILED(api.element(numbers.value, &i, &number.value))) continue;
    std::string hex = Hex(api, number.value);
    if (hex.empty()) continue;
    if (!list.empty()) list += ',';
    list += hex;
  }
  line += '\t' + list;
}

void AppendHome(Api& api, mbn::Interface* modem, std::string& line) {
  mbn::Provider home = {};
  HRESULT result = modem->GetHomeProvider(&home);
  line += '\t' + Field(api, result, home.providerID);
  line += '\t' + Field(api, result, home.providerName);
  if (home.providerID) api.free_string(home.providerID);
  if (home.providerName) api.free_string(home.providerName);
}

void AppendRegistration(Api& api, mbn::Interface* modem, std::string& line) {
  Ref<mbn::Registration> registration;
  if (FAILED(modem->QueryInterface(mbn::kRegistration, reinterpret_cast<void**>(&registration.value)))
      || !registration.value) {
    line += "\t-\t-\t-\t-\t-";
    return;
  }
  ULONG state = 0, data = 0;
  line += '\t' + Number(registration.value->GetRegisterState(&state), state);
  String id(api), name(api), roaming(api);
  line += '\t' + Field(api, registration.value->GetProviderID(&id.value), id.value);
  line += '\t' + Field(api, registration.value->GetProviderName(&name.value), name.value);
  line += '\t' + Field(api, registration.value->GetRoamingText(&roaming.value), roaming.value);
  line += '\t' + Number(registration.value->GetCurrentDataClass(&data), data);
}

void AppendSignal(Api& api, mbn::Interface* modem, std::string& line) {
  Ref<mbn::Signal> signal;
  if (FAILED(modem->QueryInterface(mbn::kSignal, reinterpret_cast<void**>(&signal.value)))
      || !signal.value) {
    line += "\t-\t-";
    return;
  }
  ULONG strength = 0, error = 0;
  line += '\t' + Number(signal.value->GetSignalStrength(&strength), strength);
  line += '\t' + Number(signal.value->GetSignalError(&error), error);
}

void AppendConnection(Api& api, mbn::Interface* modem, std::string& line) {
  Ref<mbn::Connection> connection;
  if (FAILED(modem->GetConnection(&connection.value)) || !connection.value) {
    // E_MBN_NOT_REGISTERED and similar: there is no packet connection object.
    line += "\t-";
    return;
  }
  ULONG state = 0;
  String profile(api);
  line += '\t' + Number(connection.value->GetConnectionState(&state, &profile.value), state);
}

// One line per interface, tab-separated, after a status line:
//   status <ok|none|error> <hresult>
//   modem <id> <ready> <cellular> <voice> <data> <sms> <device> <manufacturer> <model>
//         <firmware> <imsi> <iccid> <numbers> <home-id> <home-name> <register> <reg-id>
//         <reg-name> <roaming-text> <data-class> <signal> <signal-error> <activation>
std::string Snapshot(Api& api) {
  HRESULT initialized = api.initialize(nullptr, COINIT_MULTITHREADED);
  // RPC_E_CHANGED_MODE: the thread is already in an apartment; COM is usable.
  bool balance = SUCCEEDED(initialized);
  if (FAILED(initialized) && initialized != RPC_E_CHANGED_MODE) {
    char status[48];
    snprintf(status, sizeof(status), "status\terror\t%08lx\n", static_cast<unsigned long>(initialized));
    return status;
  }
  std::string out;
  {
    Ref<mbn::Manager> manager;
    HRESULT result = api.create(mbn::kManagerClass, nullptr, CLSCTX_ALL, mbn::kManager,
                                reinterpret_cast<void**>(&manager.value));
    Array interfaces(api);
    LONG low = 0, high = -1;
    if (SUCCEEDED(result) && manager.value) result = manager.value->GetInterfaces(&interfaces.value);
    if (SUCCEEDED(result) && interfaces.value) {
      if (FAILED(api.lower(interfaces.value, 1, &low)) || FAILED(api.upper(interfaces.value, 1, &high))) {
        result = E_FAIL;
      }
    }
    char status[48];
    if (FAILED(result) || !manager.value) {
      // No WwanSvc (REGDB_E_CLASSNOTREG and similar) or no interface
      // (HRESULT_FROM_WIN32(ERROR_NOT_FOUND)) both mean no usable modem.
      bool absent = result == REGDB_E_CLASSNOTREG || result == HRESULT_FROM_WIN32(ERROR_NOT_FOUND)
          || result == HRESULT_FROM_WIN32(ERROR_SERVICE_DISABLED)
          || result == HRESULT_FROM_WIN32(ERROR_SERVICE_DOES_NOT_EXIST);
      snprintf(status, sizeof(status), "status\t%s\t%08lx\n", absent ? "none" : "error",
               static_cast<unsigned long>(result));
      out = status;
    } else {
      snprintf(status, sizeof(status), "status\tok\t%08lx\n", static_cast<unsigned long>(result));
      out = status;
      for (LONG i = low, n = 0; interfaces.value && i <= high && n < kMaxInterfaces; i++, n++) {
        Ref<mbn::Interface> modem;
        if (FAILED(api.element(interfaces.value, &i, &modem.value)) || !modem.value) continue;
        String id(api);
        if (FAILED(modem.value->get_InterfaceID(&id.value)) || !id.value) continue;
        std::string line = "modem\t" + Hex(api, id.value);
        ULONG ready = 0;
        line += '\t' + Number(modem.value->GetReadyState(&ready), ready);
        AppendCaps(api, modem.value, line);
        AppendSubscriber(api, modem.value, line);
        AppendHome(api, modem.value, line);
        AppendRegistration(api, modem.value, line);
        AppendSignal(api, modem.value, line);
        AppendConnection(api, modem.value, line);
        out += line + '\n';
      }
    }
  }
  if (balance) api.uninitialize();
  return out;
}

jstring WindowsCellular_nativeSnapshot(JNIEnv* env, jclass) {
  static Api api;
  std::string text = Snapshot(api);
  return env->NewStringUTF(text.c_str());
}
const JNINativeMethod methods[] = {
    {"nativeSnapshot", "()Ljava/lang/String;", NATIVEHELPER_SYSV_JNI_ENTRY(WindowsCellular_nativeSnapshot)},
};
jint OnLoad(JavaVM* vm, void*) {
  JNIEnv* env = nullptr;
  if (vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK) return JNI_ERR;
  jclass type = env->FindClass("com/gesher/server/WindowsCellular");
  if (!type) return JNI_ERR;
  int result = env->RegisterNatives(type, methods, sizeof(methods) / sizeof(methods[0]));
  env->DeleteLocalRef(type);
  return result == JNI_OK ? JNI_VERSION_1_6 : JNI_ERR;
}
const gesher_builtin_symbol symbols[] = {{"JNI_OnLoad", reinterpret_cast<void*>(&OnLoad)}};
const bool registered = [] {
  gesher_register_builtin_library("libgesher_cellular.so", symbols, sizeof(symbols) / sizeof(symbols[0]));
  return true;
}();
}  // namespace
