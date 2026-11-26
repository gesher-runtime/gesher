// Copyright (C) 2026 The Gesher Authors. SPDX-License-Identifier: Apache-2.0
// Mobile Broadband (WWAN) boundary for com.gesher.server.WindowsCellular.
//
// Snapshot reads Windows' Mobile Broadband API (mbnapi, the WwanSvc COM server) on the
// caller's thread: interface capabilities, ready state, subscriber information,
// home provider, registration, signal, packet-connection and radio state, and the
// connection mode (auto, auto-home or manual) of the SIM's default connection profile.
// A machine without the WWAN service or without a cellular interface reports "none"
// rather than failing the runtime.
//
// Configure is the only write, made for Android's mobile data and data roaming
// switches (docs/telephony.md): it sets the ConnectionMode element of that same default
// profile (IMbnConnectionProfile::UpdateProfile, the Mobile Broadband profile schema's
// "manual", "auto" or "auto-home") and then makes the packet connection follow it
// (IMbnConnection::Disconnect or Connect with that profile). Nothing else is ever
// written: no PIN, registration mode, preferred providers, SMS, other profile elements
// or radio state.
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
// {DCCCCAB6-201F-4BBB-AAEE-338E368AF6FA}
const IID kRadio = {0xDCCCCAB6, 0x201F, 0x4BBB, {0xAA, 0xEE, 0x33, 0x8E, 0x36, 0x8A, 0xF6, 0xFA}};
// CLSID_MbnConnectionProfileManager {BDFEE05A-4418-11DD-90ED-001C257CCFF1}
const CLSID kProfileManagerClass = {0xBDFEE05A, 0x4418, 0x11DD, {0x90, 0xED, 0x00, 0x1C, 0x25, 0x7C, 0xCF, 0xF1}};
// IID_IMbnConnectionProfileManager {DCBBBAB6-200F-4BBB-AAEE-338E368AF6FA}
const IID kProfileManager = {0xDCBBBAB6, 0x200F, 0x4BBB, {0xAA, 0xEE, 0x33, 0x8E, 0x36, 0x8A, 0xF6, 0xFA}};

// MBN_CONNECTION_MODE_PROFILE: Connect's profile argument is a stored profile's name.
constexpr ULONG kConnectWithProfile = 0;
// MBN_ACTIVATION_STATE.
constexpr ULONG kActivated = 1, kActivating = 2, kDeactivated = 3;
// MBN_REGISTER_STATE.
constexpr ULONG kRegisterHome = 3, kRegisterRoaming = 4, kRegisterPartner = 5;

enum MBN_RADIO { MBN_RADIO_OFF = 0, MBN_RADIO_ON = MBN_RADIO_OFF + 1 };

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
// SetSoftwareRadioState follows; it is never called and not declared.
struct Radio : IUnknown {
  virtual HRESULT STDMETHODCALLTYPE get_SoftwareRadioState(MBN_RADIO* state) = 0;
  virtual HRESULT STDMETHODCALLTYPE get_HardwareRadioState(MBN_RADIO* state) = 0;
};
// IMbnConnectionProfile; Delete follows and is never called or declared.
struct ConnectionProfile : IUnknown {
  virtual HRESULT STDMETHODCALLTYPE GetProfileXmlData(BSTR* xml) = 0;
  virtual HRESULT STDMETHODCALLTYPE UpdateProfile(LPCWSTR xml) = 0;
};
// IMbnConnectionProfileManager; GetConnectionProfile and CreateConnectionProfile follow.
struct ProfileManager : IUnknown {
  virtual HRESULT STDMETHODCALLTYPE GetConnectionProfiles(Interface* modem, SAFEARRAY** profiles) = 0;
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

using Text = std::basic_string<WCHAR>;

constexpr LONG kMaxProfiles = 16;
constexpr UINT kMaxProfileUnits = 65536;

Text Wide(const char* ascii) {
  Text text;
  while (*ascii) text += static_cast<WCHAR>(*ascii++);
  return text;
}
bool Space(WCHAR c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }
Text Trim(const Text& text) {
  size_t start = 0, end = text.size();
  while (start < end && Space(text[start])) start++;
  while (end > start && Space(text[end - 1])) end--;
  return text.substr(start, end - start);
}

// The profile XML is edited as text: elements are found by their exact, unprefixed
// tags, as the Mobile Broadband profile schema's own elements are written. Anything
// else (attributes, a namespace prefix, a self-closing or unterminated element, markup
// in a value) is malformed, and the profile is then neither read nor written.
enum class Found { kAbsent, kFound, kMalformed };
struct Span { size_t open = 0, start = 0, end = 0, close = 0; };

Found Locate(const Text& xml, const char* name, Span* span) {
  Text open = Wide("<") + Wide(name), close = Wide("</") + Wide(name) + Wide(">");
  // "<prefix:Name": the element under a namespace prefix (its closing tag needs an opening
  // one), which this edit does not read; an empty prefix ("<:Name") is malformed too. Only
  // a tag counts: the prefix is a name right after "<", so text such as
  // "http://h/x:Name" in a value is not one.
  auto nameChar = [](WCHAR c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')
        || c == '-' || c == '_' || c == '.';
  };
  for (size_t at = xml.find(Wide(":") + Wide(name)); at != Text::npos;
       at = xml.find(Wide(":") + Wide(name), at + 1)) {
    size_t start = at;
    while (start > 0 && nameChar(xml[start - 1])) start--;
    size_t after = at + 1 + std::char_traits<char>::length(name);
    bool tag = start > 0 && xml[start - 1] == '<'
        && after < xml.size() && (xml[after] == '>' || Space(xml[after]) || xml[after] == '/');
    if (tag) return Found::kMalformed;
  }
  for (size_t at = xml.find(open); at != Text::npos; at = xml.find(open, at + 1)) {
    size_t after = at + open.size();
    if (after >= xml.size()) return Found::kMalformed;
    WCHAR c = xml[after];
    if (c == '>') {
      size_t end = xml.find(close, after + 1);
      if (end == Text::npos) return Found::kMalformed;
      if (xml.find(static_cast<WCHAR>('<'), after + 1) != end) return Found::kMalformed;
      *span = {at, after + 1, end, end + close.size()};
      return Found::kFound;
    }
    // "<NameX": another element; "<Name " or "<Name/": a form this edit does not handle.
    bool other = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')
        || c == '-' || c == '_' || c == '.';
    if (!other) return Found::kMalformed;
  }
  return Found::kAbsent;
}

// XML character and entity references in an element value.
bool Decode(const Text& raw, Text* out) {
  out->clear();
  for (size_t i = 0; i < raw.size(); i++) {
    if (raw[i] != '&') { *out += raw[i]; continue; }
    size_t semi = raw.find(static_cast<WCHAR>(';'), i);
    if (semi == Text::npos || semi - i > 10) return false;
    Text entity = raw.substr(i + 1, semi - i - 1);
    if (entity == Wide("amp")) *out += static_cast<WCHAR>('&');
    else if (entity == Wide("lt")) *out += static_cast<WCHAR>('<');
    else if (entity == Wide("gt")) *out += static_cast<WCHAR>('>');
    else if (entity == Wide("quot")) *out += static_cast<WCHAR>('"');
    else if (entity == Wide("apos")) *out += static_cast<WCHAR>('\'');
    else if (entity.size() > 1 && entity[0] == '#') {
      bool hex = entity[1] == 'x';
      uint32_t value = 0;
      size_t digits = 0;
      for (size_t k = hex ? 2 : 1; k < entity.size(); k++, digits++) {
        WCHAR c = entity[k];
        int digit = c >= '0' && c <= '9' ? c - '0'
            : hex && c >= 'a' && c <= 'f' ? c - 'a' + 10 : hex && c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
        if (digit < 0) return false;
        value = value * (hex ? 16 : 10) + digit;
        if (value > 0xFFFF) return false;  // Names and tokens here stay in the BMP.
      }
      if (digits == 0 || (value >= 0xD800 && value <= 0xDFFF)) return false;
      *out += static_cast<WCHAR>(value);
    } else {
      return false;
    }
    i = semi;
  }
  return true;
}

Found Value(const Text& xml, const char* name, Text* value) {
  Span span;
  Found found = Locate(xml, name, &span);
  if (found != Found::kFound) return found;
  return Decode(xml.substr(span.start, span.end - span.start), value) ? Found::kFound : Found::kMalformed;
}

bool KnownMode(const Text& mode) {
  return mode == Wide("auto") || mode == Wide("auto-home") || mode == Wide("manual");
}

// The profile's connection mode: the schema's default, "manual", when the element is
// absent. False when the profile cannot be read this way.
bool ConnectionMode(const Text& xml, Text* mode) {
  Text value;
  Found found = Value(xml, "ConnectionMode", &value);
  if (found == Found::kMalformed) return false;
  *mode = found == Found::kAbsent ? Wide("manual") : Trim(value);
  return true;
}

// The profile with ConnectionMode set; the element goes in its schema position (after
// SubscriberID, SimIccID, HomeProviderName and AutoConnectOnInternet) when absent.
bool WithConnectionMode(const Text& xml, const Text& mode, Text* out) {
  Text element = Wide("<ConnectionMode>") + mode + Wide("</ConnectionMode>");
  Span span;
  Found found = Locate(xml, "ConnectionMode", &span);
  if (found == Found::kMalformed) return false;
  if (found == Found::kFound) {
    *out = xml.substr(0, span.start) + mode + xml.substr(span.end);
    return true;
  }
  for (const char* before : {"AutoConnectOnInternet", "HomeProviderName", "SimIccID", "SubscriberID"}) {
    found = Locate(xml, before, &span);
    if (found == Found::kMalformed) return false;
    if (found == Found::kFound) {
      *out = xml.substr(0, span.close) + element + xml.substr(span.close);
      return true;
    }
  }
  return false;
}

// UTF-16 to UTF-8, bounded; unpaired surrogates become U+FFFD. Output is hex so
// operator names and numbers cross the JNI text boundary without any escaping.
std::string Hex(const WCHAR* text, UINT length) {
  if (!text) return "";
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
std::string Hex(Api& api, BSTR text) {
  return text ? Hex(text, api.string_length(text)) : std::string();
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

// The subscriber ID (IMSI) and ICCID go to *imsi and *iccid when they were read:
// profiles are per SIM.
void AppendSubscriber(Api& api, mbn::Interface* modem, std::string& line, Text* imsi, Text* iccidOut) {
  Ref<mbn::SubscriberInformation> info;
  HRESULT result = modem->GetSubscriberInformation(&info.value);
  if (FAILED(result) || !info.value) {
    line += "\t-\t-\t-";
    return;
  }
  String subscriber(api), iccid(api);
  HRESULT read = info.value->get_SubscriberID(&subscriber.value);
  line += '\t' + Field(api, read, subscriber.value);
  if (SUCCEEDED(read) && subscriber.value) *imsi = Text(subscriber.value, api.string_length(subscriber.value));
  read = info.value->get_SimIccID(&iccid.value);
  line += '\t' + Field(api, read, iccid.value);
  if (SUCCEEDED(read) && iccid.value) *iccidOut = Text(iccid.value, api.string_length(iccid.value));
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
  // Each call completes before its value is formatted: the order in which a call's
  // arguments are evaluated is unspecified (GCC reads `state` before the call).
  ULONG state = 0, data = 0;
  HRESULT read = registration.value->GetRegisterState(&state);
  line += '\t' + Number(read, state);
  String id(api), name(api), roaming(api);
  read = registration.value->GetProviderID(&id.value);
  line += '\t' + Field(api, read, id.value);
  read = registration.value->GetProviderName(&name.value);
  line += '\t' + Field(api, read, name.value);
  read = registration.value->GetRoamingText(&roaming.value);
  line += '\t' + Field(api, read, roaming.value);
  read = registration.value->GetCurrentDataClass(&data);
  line += '\t' + Number(read, data);
}

void AppendSignal(Api& api, mbn::Interface* modem, std::string& line) {
  Ref<mbn::Signal> signal;
  if (FAILED(modem->QueryInterface(mbn::kSignal, reinterpret_cast<void**>(&signal.value)))
      || !signal.value) {
    line += "\t-\t-";
    return;
  }
  ULONG strength = 0, error = 0;
  HRESULT read = signal.value->GetSignalStrength(&strength);
  line += '\t' + Number(read, strength);
  read = signal.value->GetSignalError(&error);
  line += '\t' + Number(read, error);
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
  HRESULT read = connection.value->GetConnectionState(&state, &profile.value);
  line += '\t' + Number(read, state);
}

// IMbnRadio: the software (Windows' cellular switch, airplane mode) and hardware radio
// states. Read only; SetSoftwareRadioState is never reached.
void AppendRadio(mbn::Interface* modem, std::string& line) {
  Ref<mbn::Radio> radio;
  if (FAILED(modem->QueryInterface(mbn::kRadio, reinterpret_cast<void**>(&radio.value))) || !radio.value) {
    line += "\t-\t-";
    return;
  }
  mbn::MBN_RADIO software = mbn::MBN_RADIO_OFF, hardware = mbn::MBN_RADIO_OFF;
  HRESULT read = radio.value->get_SoftwareRadioState(&software);
  line += '\t' + Number(read, static_cast<ULONG>(software));
  read = radio.value->get_HardwareRadioState(&hardware);
  line += '\t' + Number(read, static_cast<ULONG>(hardware));
}

// The default connection profile of the modem's SIM: IsDefault true (at most one per
// SIM, as the schema requires) and SubscriberID equal to the SIM's IMSI. S_OK with the
// profile and its XML, S_FALSE when the SIM has none, or the failure.
// An ICCID as profiles and the modem write it: trimmed, without the SIM's 'F' padding,
// case-insensitive.
Text Iccid(const Text& raw) {
  Text id = Trim(raw);
  while (!id.empty() && (id.back() == 'F' || id.back() == 'f')) id.pop_back();
  for (WCHAR& c : id) {
    if (c >= 'a' && c <= 'z') c = static_cast<WCHAR>(c - 'a' + 'A');
  }
  return id;
}

struct Sim {
  Text imsi, iccid;
};

// Whether a profile belongs to the SIM: its SubscriberID is the SIM's IMSI or, in a profile
// without one (the v4 MBNProfileExt schema makes it optional, SimIccID mandatory), its
// SimIccID is the SIM's ICCID.
Found ForSim(const Text& xml, const Sim& sim) {
  Text subscriber, iccid;
  Found found = Value(xml, "SubscriberID", &subscriber);
  if (found == Found::kMalformed) return found;
  if (found == Found::kFound) {
    return !sim.imsi.empty() && Trim(subscriber) == Trim(sim.imsi) ? Found::kFound : Found::kAbsent;
  }
  found = Value(xml, "SimIccID", &iccid);
  if (found != Found::kFound) return found;
  return !sim.iccid.empty() && Iccid(iccid) == Iccid(sim.iccid) ? Found::kFound : Found::kAbsent;
}

HRESULT DefaultProfile(Api& api, mbn::ProfileManager* profiles, mbn::Interface* modem, const Sim& sim,
                       Ref<mbn::ConnectionProfile>* found, Text* xml) {
  Array array(api);
  LONG low = 0, high = -1;
  HRESULT result = profiles->GetConnectionProfiles(modem, &array.value);
  // No stored profile at all.
  if (result == HRESULT_FROM_WIN32(ERROR_NOT_FOUND)) return S_FALSE;
  if (FAILED(result)) return result;
  if (!array.value) return S_FALSE;
  if (FAILED(api.lower(array.value, 1, &low)) || FAILED(api.upper(array.value, 1, &high))) return E_FAIL;
  // A profile that cannot be read this way might be the SIM's default: then the answer
  // is unknown, not "no default profile".
  bool unreadable = false;
  for (LONG i = low, n = 0; i <= high && n < kMaxProfiles; i++, n++) {
    Ref<mbn::ConnectionProfile> profile;
    if (FAILED(api.element(array.value, &i, &profile.value)) || !profile.value) { unreadable = true; continue; }
    String data(api);
    if (FAILED(profile.value->GetProfileXmlData(&data.value)) || !data.value) { unreadable = true; continue; }
    UINT length = api.string_length(data.value);
    if (length > kMaxProfileUnits) { unreadable = true; continue; }
    Text text(data.value, length), isDefault;
    Found match = Value(text, "IsDefault", &isDefault);
    if (match == Found::kMalformed) { unreadable = true; continue; }
    if (match == Found::kAbsent) continue;
    isDefault = Trim(isDefault);
    if (isDefault != Wide("true") && isDefault != Wide("1")) continue;
    match = ForSim(text, sim);
    if (match == Found::kMalformed) { unreadable = true; continue; }
    if (match == Found::kAbsent) continue;
    found->value = profile.value;
    profile.value = nullptr;
    *xml = text;
    return S_OK;
  }
  return unreadable ? E_UNEXPECTED : S_FALSE;
}

// The default profile's connection mode: the mode, empty when the SIM has no default
// profile (or no SIM is readable), "-" when the profiles or that profile cannot be read.
void AppendProfile(Api& api, mbn::ProfileManager* profiles, mbn::Interface* modem, const Sim& sim,
                   std::string& line) {
  if (!profiles) {
    line += "\t-";
    return;
  }
  if (sim.imsi.empty() && sim.iccid.empty()) {
    line += '\t';
    return;
  }
  Ref<mbn::ConnectionProfile> profile;
  Text xml, mode;
  HRESULT result = DefaultProfile(api, profiles, modem, sim, &profile, &xml);
  if (FAILED(result)) {
    line += "\t-";
  } else if (result == S_FALSE) {
    line += '\t';
  } else if (!ConnectionMode(xml, &mode)) {
    line += "\t-";
  } else {
    line += '\t' + Hex(mode.data(), static_cast<UINT>(mode.size()));
  }
}

// One line per interface, tab-separated, after a status line:
//   status <ok|none|error> <hresult>
//   modem <id> <ready> <cellular> <voice> <data> <sms> <device> <manufacturer> <model>
//         <firmware> <imsi> <iccid> <numbers> <home-id> <home-name> <register> <reg-id>
//         <reg-name> <roaming-text> <data-class> <signal> <signal-error> <activation>
//         <software-radio> <hardware-radio> <connection-mode>
// HRESULTs are printed as their 32 bits (through uint32_t), so a 64-bit unsigned
// long (the Linux host fixture) does not sign-extend them.
std::string Snapshot(Api& api) {
  HRESULT initialized = api.initialize(nullptr, COINIT_MULTITHREADED);
  // RPC_E_CHANGED_MODE: the thread is already in an apartment; COM is usable.
  bool balance = SUCCEEDED(initialized);
  if (FAILED(initialized) && initialized != RPC_E_CHANGED_MODE) {
    char status[48];
    snprintf(status, sizeof(status), "status\terror\t%08lx\n",
             static_cast<unsigned long>(static_cast<uint32_t>(initialized)));
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
    // Profiles are read only when there are interfaces; a failure leaves the mode unreported.
    Ref<mbn::ProfileManager> profiles;
    if (SUCCEEDED(result) && interfaces.value
        && FAILED(api.create(mbn::kProfileManagerClass, nullptr, CLSCTX_ALL, mbn::kProfileManager,
                             reinterpret_cast<void**>(&profiles.value)))) {
      profiles.value = nullptr;
    }
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
               static_cast<unsigned long>(static_cast<uint32_t>(result)));
      out = status;
    } else {
      snprintf(status, sizeof(status), "status\tok\t%08lx\n",
               static_cast<unsigned long>(static_cast<uint32_t>(result)));
      out = status;
      for (LONG i = low, n = 0; interfaces.value && i <= high && n < kMaxInterfaces; i++, n++) {
        Ref<mbn::Interface> modem;
        if (FAILED(api.element(interfaces.value, &i, &modem.value)) || !modem.value) continue;
        String id(api);
        if (FAILED(modem.value->get_InterfaceID(&id.value)) || !id.value) continue;
        std::string line = "modem\t" + Hex(api, id.value);
        ULONG ready = 0;
        HRESULT read = modem.value->GetReadyState(&ready);
        line += '\t' + Number(read, ready);
        AppendCaps(api, modem.value, line);
        Sim sim;
        AppendSubscriber(api, modem.value, line, &sim.imsi, &sim.iccid);
        AppendHome(api, modem.value, line);
        AppendRegistration(api, modem.value, line);
        AppendSignal(api, modem.value, line);
        AppendConnection(api, modem.value, line);
        AppendRadio(modem.value, line);
        AppendProfile(api, profiles.value, modem.value, sim, line);
        out += line + '\n';
      }
    }
  }
  if (balance) api.uninitialize();
  return out;
}

// Makes the packet connection follow a connection mode, from its state and registration.
template <class Line>
std::string Enforce(Api& api, mbn::Interface* modem, const Text& mode, const Text& xml, Line line) {
  ULONG registerState = 0;
  bool registered = false, roaming = false;
  {
    Ref<mbn::Registration> registration;
    if (SUCCEEDED(modem->QueryInterface(mbn::kRegistration, reinterpret_cast<void**>(&registration.value)))
        && registration.value && SUCCEEDED(registration.value->GetRegisterState(&registerState))) {
      roaming = registerState == mbn::kRegisterRoaming || registerState == mbn::kRegisterPartner;
      registered = roaming || registerState == mbn::kRegisterHome;
    }
  }
  bool wanted = mode == Wide("auto") ? registered : mode == Wide("auto-home") ? registered && !roaming : false;
  Ref<mbn::Connection> connection;
  HRESULT result = modem->GetConnection(&connection.value);
  if (FAILED(result) || !connection.value) {
    // No connection object (not registered): nothing is connected, nothing can connect.
    return line("ok", wanted ? "unavailable" : "none", FAILED(result) ? result : S_OK);
  }
  ULONG state = 0;
  String profileName(api);
  result = connection.value->GetConnectionState(&state, &profileName.value);
  if (FAILED(result)) return wanted ? line("ok", "unavailable", result) : line("error", "state", result);
  bool up = state == mbn::kActivated || state == mbn::kActivating;
  ULONG request = 0;
  if (!wanted && up) {
    result = connection.value->Disconnect(&request);
    // ERROR_NOT_FOUND: already disconnected.
    if (FAILED(result) && result != HRESULT_FROM_WIN32(ERROR_NOT_FOUND)) return line("error", "disconnect", result);
    return line("ok", "disconnect", result);
  }
  if (wanted && (state == mbn::kDeactivated || state == 0)) {
    Text name;
    if (Value(xml, "Name", &name) != Found::kFound) return line("ok", "unavailable", E_UNEXPECTED);
    name = Trim(name);
    return line("ok", "connect", connection.value->Connect(mbn::kConnectWithProfile, name.c_str(), &request));
  }
  return line("ok", "none", S_OK);
}

// Sets the connection mode of the SIM's default profile and makes the packet connection
// follow it: "manual" disconnects, "auto-home" disconnects while roaming and connects
// at home, "auto" connects while registered. A mode that is already set changes nothing,
// as DataSettingsManager does nothing for an unchanged switch: a connection made or
// dropped by hand in Windows stays as it is. The result is one line:
//   ok <unchanged|none|connect|disconnect|unavailable> <hresult of that step>
//   error <stage> <hresult>
// The profile update and a needed disconnect must succeed. When the disconnect (or
// reading the connection state for it) fails, the previous profile is restored; if the
// restore fails too, the stage is "rollback" with the restore's result: the new mode is
// then stored while the connection is still up. A connect that Windows refuses is
// reported, not an error: the mode is set and Windows keeps connecting by it.
std::string Configure(Api& api, const Text& interfaceId, const Text& mode) {
  auto line = [](const char* outcome, const char* step, HRESULT result) {
    char text[64];
    snprintf(text, sizeof(text), "%s\t%s\t%08lx", outcome, step,
             static_cast<unsigned long>(static_cast<uint32_t>(result)));
    return std::string(text);
  };
  if (!KnownMode(mode)) return line("error", "mode", E_INVALIDARG);
  HRESULT initialized = api.initialize(nullptr, COINIT_MULTITHREADED);
  bool balance = SUCCEEDED(initialized);
  if (FAILED(initialized) && initialized != RPC_E_CHANGED_MODE) return line("error", "com", initialized);
  std::string out;
  {
    Ref<mbn::Manager> manager;
    Ref<mbn::Interface> modem;
    Ref<mbn::SubscriberInformation> info;
    Ref<mbn::ProfileManager> profiles;
    Ref<mbn::ConnectionProfile> profile;
    String imsiText(api), iccidText(api);
    Text xml, previous, updated;
    Sim sim;
    HRESULT result = api.create(mbn::kManagerClass, nullptr, CLSCTX_ALL, mbn::kManager,
                                reinterpret_cast<void**>(&manager.value));
    if (SUCCEEDED(result)) result = manager.value->GetInterface(interfaceId.c_str(), &modem.value);
    if (FAILED(result) || !modem.value) {
      out = line("error", "interface", FAILED(result) ? result : E_FAIL);
    } else if (FAILED(result = modem.value->GetSubscriberInformation(&info.value)) || !info.value) {
      out = line("error", "subscriber", FAILED(result) ? result : E_FAIL);
    } else if (FAILED(result = api.create(mbn::kProfileManagerClass, nullptr, CLSCTX_ALL, mbn::kProfileManager,
                                          reinterpret_cast<void**>(&profiles.value))) || !profiles.value) {
      out = line("error", "profiles", FAILED(result) ? result : E_FAIL);
    } else {
      if (SUCCEEDED(info.value->get_SubscriberID(&imsiText.value)) && imsiText.value) {
        sim.imsi = Text(imsiText.value, api.string_length(imsiText.value));
      }
      if (SUCCEEDED(info.value->get_SimIccID(&iccidText.value)) && iccidText.value) {
        sim.iccid = Text(iccidText.value, api.string_length(iccidText.value));
      }
      result = sim.imsi.empty() && sim.iccid.empty() ? E_FAIL
          : DefaultProfile(api, profiles.value, modem.value, sim, &profile, &xml);
      if (sim.imsi.empty() && sim.iccid.empty()) {
        out = line("error", "subscriber", E_FAIL);
      } else if (result != S_OK) {
        out = line("error", "profile", FAILED(result) ? result : HRESULT_FROM_WIN32(ERROR_NOT_FOUND));
      } else if (!ConnectionMode(xml, &previous) || !WithConnectionMode(xml, mode, &updated)) {
        out = line("error", "profile", E_UNEXPECTED);
      } else if (previous == mode) {
        out = line("ok", "unchanged", S_OK);
      } else if (FAILED(result = profile.value->UpdateProfile(updated.c_str()))) {
        out = line("error", "update", result);
      } else {
        out = Enforce(api, modem.value, mode, xml, line);
        // A refused disconnect leaves the connection up: the previous mode is restored.
        if (out.rfind("error", 0) == 0 && FAILED(result = profile.value->UpdateProfile(xml.c_str()))) {
          out = line("error", "rollback", result);
        }
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
Text FromJava(JNIEnv* env, jstring value) {
  Text text;
  if (!value) return text;
  jsize length = env->GetStringLength(value);
  const jchar* chars = env->GetStringChars(value, nullptr);
  if (!chars) return text;
  for (jsize i = 0; i < length; i++) text += static_cast<WCHAR>(chars[i]);
  env->ReleaseStringChars(value, chars);
  return text;
}
jstring WindowsCellular_nativeConfigure(JNIEnv* env, jclass, jstring interfaceId, jstring mode) {
  static Api api;
  std::string text = Configure(api, FromJava(env, interfaceId), FromJava(env, mode));
  return env->NewStringUTF(text.c_str());
}
const JNINativeMethod methods[] = {
    {"nativeSnapshot", "()Ljava/lang/String;", NATIVEHELPER_SYSV_JNI_ENTRY(WindowsCellular_nativeSnapshot)},
    {"nativeConfigure", "(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;",
     NATIVEHELPER_SYSV_JNI_ENTRY(WindowsCellular_nativeConfigure)},
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
