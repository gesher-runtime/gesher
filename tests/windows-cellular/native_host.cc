// Copyright (C) 2026 The Gesher Authors. SPDX-License-Identifier: Apache-2.0
// The production Mobile Broadband adapter runs unchanged against an explicit COM
// boundary: synthetic WWAN interfaces exist only in this host fixture.
#include <cassert>
#include <cstdlib>
#include <iostream>
#include <map>
#include <string>
#include <vector>
#ifndef GESHER_CELLULAR_SOURCE
#define GESHER_CELLULAR_SOURCE "../../compat/net/windows_cellular.cc"
#endif
#include GESHER_CELLULAR_SOURCE

namespace fixture {
int checks = 0;
void check(bool value, const char* reason) {
  if (!value) { std::cerr << reason << "\n"; std::abort(); }
  ++checks;
}

// ---- BSTR and SAFEARRAY bookkeeping: every allocation must be freed exactly once. ----
std::map<const WCHAR*, size_t> strings;
BSTR make(const std::u16string& text) {
  auto* raw = new WCHAR[text.size() + 1];
  std::memcpy(raw, text.data(), text.size() * sizeof(WCHAR));
  raw[text.size()] = 0;
  strings[raw] = text.size();
  return raw;
}
int live_objects = 0, initialized = 0, uninitialized = 0;
HRESULT initialize_result = S_OK, create_result = S_OK, interfaces_result = S_OK;
bool no_interface_array = false;
}  // namespace fixture

struct SAFEARRAY {
  bool objects;
  std::vector<IUnknown*> items;
  std::vector<std::u16string> texts;
};
int live_arrays = 0;

void SysFreeString(BSTR value) {
  auto it = fixture::strings.find(value);
  fixture::check(it != fixture::strings.end(), "Freed a string that is not live");
  fixture::strings.erase(it);
  delete[] value;
}
UINT SysStringLen(BSTR value) {
  auto it = fixture::strings.find(value);
  fixture::check(it != fixture::strings.end(), "Measured a string that is not live");
  return static_cast<UINT>(it->second);
}
HRESULT SafeArrayGetLBound(SAFEARRAY*, UINT dimension, LONG* out) {
  fixture::check(dimension == 1, "Arrays are one-dimensional");
  *out = 0;
  return S_OK;
}
HRESULT SafeArrayGetUBound(SAFEARRAY* array, UINT dimension, LONG* out) {
  fixture::check(dimension == 1, "Arrays are one-dimensional");
  *out = static_cast<LONG>(array->objects ? array->items.size() : array->texts.size()) - 1;
  return S_OK;
}
HRESULT SafeArrayGetElement(SAFEARRAY* array, LONG* index, void* out) {
  if (array->objects) {
    IUnknown* item = array->items.at(*index);
    item->AddRef();
    *static_cast<IUnknown**>(out) = item;
  } else {
    *static_cast<BSTR*>(out) = fixture::make(array->texts.at(*index));
  }
  return S_OK;
}
HRESULT SafeArrayDestroy(SAFEARRAY* array) {
  for (IUnknown* item : array->items) item->Release();
  --live_arrays;
  delete array;
  return S_OK;
}

namespace fixture {
template <class Base>
struct Object : Base {
  ULONG refs = 1;
  Object() { ++live_objects; }
  virtual ~Object() { --live_objects; }
  ULONG STDMETHODCALLTYPE AddRef() override { return ++refs; }
  ULONG STDMETHODCALLTYPE Release() override {
    check(refs > 0, "Released a dead object");
    ULONG left = --refs;
    if (left == 0) delete this;
    return left;
  }
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void**) override { return E_NOINTERFACE; }
};

struct Spec {
  std::u16string id = u"{0D7F7A43-0000-4000-8000-00000000C311}";
  ULONG ready = 1;
  bool caps = true, subscriber = true, registration = true, signal = true, connection = true;
  std::u16string device = u"356938035643809", manufacturer = u"Fixture", model = u"WWAN 1",
                 firmware = u"FW 1.0";
  std::u16string imsi = u"425010123456789", iccid = u"8997201000000000001F";
  std::vector<std::u16string> numbers = {u"+972501234567"};
  std::u16string home_id = u"42501", home_name = u"פלאפון";
  ULONG register_state = 3, data_class = 0x20, signal_value = 20, signal_error = 99, activation = 1;
  std::u16string reg_id = u"42501", reg_name = u"Partner \U0001F4F6", roaming = u"";
};

struct Subscriber : Object<mbn::SubscriberInformation> {
  const Spec& s;
  explicit Subscriber(const Spec& s) : s(s) { }
  HRESULT STDMETHODCALLTYPE get_SubscriberID(BSTR* out) override { *out = make(s.imsi); return S_OK; }
  HRESULT STDMETHODCALLTYPE get_SimIccID(BSTR* out) override { *out = make(s.iccid); return S_OK; }
  HRESULT STDMETHODCALLTYPE get_TelephoneNumbers(SAFEARRAY** out) override {
    *out = new SAFEARRAY{false, {}, s.numbers};
    ++live_arrays;
    return S_OK;
  }
};
struct Connection : Object<mbn::Connection> {
  const Spec& s;
  explicit Connection(const Spec& s) : s(s) { }
  HRESULT STDMETHODCALLTYPE get_ConnectionID(BSTR*) override { return E_FAIL; }
  HRESULT STDMETHODCALLTYPE get_InterfaceID(BSTR*) override { return E_FAIL; }
  HRESULT STDMETHODCALLTYPE Connect(ULONG, LPCWSTR, ULONG*) override {
    check(false, "The adapter must never connect");
    return E_FAIL;
  }
  HRESULT STDMETHODCALLTYPE Disconnect(ULONG*) override {
    check(false, "The adapter must never disconnect");
    return E_FAIL;
  }
  HRESULT STDMETHODCALLTYPE GetConnectionState(ULONG* state, BSTR* profile) override {
    *state = s.activation;
    *profile = make(u"profile");
    return S_OK;
  }
};
struct Registration : Object<mbn::Registration> {
  const Spec& s;
  explicit Registration(const Spec& s) : s(s) { }
  HRESULT STDMETHODCALLTYPE GetRegisterState(ULONG* out) override { *out = s.register_state; return S_OK; }
  HRESULT STDMETHODCALLTYPE GetRegisterMode(ULONG* out) override { *out = 0; return S_OK; }
  HRESULT STDMETHODCALLTYPE GetProviderID(BSTR* out) override { *out = make(s.reg_id); return S_OK; }
  HRESULT STDMETHODCALLTYPE GetProviderName(BSTR* out) override { *out = make(s.reg_name); return S_OK; }
  HRESULT STDMETHODCALLTYPE GetRoamingText(BSTR* out) override { *out = make(s.roaming); return S_OK; }
  HRESULT STDMETHODCALLTYPE GetAvailableDataClasses(ULONG* out) override { *out = 0x3F; return S_OK; }
  HRESULT STDMETHODCALLTYPE GetCurrentDataClass(ULONG* out) override { *out = s.data_class; return S_OK; }
};
struct Signal : Object<mbn::Signal> {
  const Spec& s;
  explicit Signal(const Spec& s) : s(s) { }
  HRESULT STDMETHODCALLTYPE GetSignalStrength(ULONG* out) override { *out = s.signal_value; return S_OK; }
  HRESULT STDMETHODCALLTYPE GetSignalError(ULONG* out) override { *out = s.signal_error; return S_OK; }
};
struct Interface : Object<mbn::Interface> {
  Spec s;
  explicit Interface(Spec spec) : s(std::move(spec)) { }
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
    if (iid == mbn::kRegistration && s.registration) { *out = static_cast<mbn::Registration*>(new Registration(s)); return S_OK; }
    if (iid == mbn::kSignal && s.signal) { *out = static_cast<mbn::Signal*>(new Signal(s)); return S_OK; }
    *out = nullptr;
    return E_NOINTERFACE;
  }
  HRESULT STDMETHODCALLTYPE get_InterfaceID(BSTR* out) override { *out = make(s.id); return S_OK; }
  HRESULT STDMETHODCALLTYPE GetInterfaceCapability(mbn::InterfaceCaps* caps) override {
    if (!s.caps) return static_cast<HRESULT>(0x80070015);  // ERROR_NOT_READY
    caps->cellularClass = 1; caps->voiceClass = 1; caps->dataClass = 0x3F; caps->smsCaps = 3;
    caps->customDataClass = make(u""); caps->customBandClass = make(u"");
    caps->deviceID = make(s.device); caps->manufacturer = make(s.manufacturer);
    caps->model = make(s.model); caps->firmwareInfo = make(s.firmware);
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE GetSubscriberInformation(mbn::SubscriberInformation** out) override {
    if (!s.subscriber) return static_cast<HRESULT>(0x80548202);  // E_MBN_SIM_NOT_INSERTED
    *out = new Subscriber(s);
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE GetReadyState(ULONG* out) override { *out = s.ready; return S_OK; }
  HRESULT STDMETHODCALLTYPE InEmergencyMode(VARIANT_BOOL*) override { return E_FAIL; }
  HRESULT STDMETHODCALLTYPE GetHomeProvider(mbn::Provider* out) override {
    if (!s.subscriber) return static_cast<HRESULT>(0x80548202);
    out->providerID = make(s.home_id); out->providerName = make(s.home_name);
    out->providerState = 0; out->dataClass = 0;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE GetPreferredProviders(SAFEARRAY**) override { check(false, "Unused read"); return E_FAIL; }
  HRESULT STDMETHODCALLTYPE SetPreferredProviders(SAFEARRAY*, ULONG*) override { check(false, "Never writes"); return E_FAIL; }
  HRESULT STDMETHODCALLTYPE GetVisibleProviders(ULONG*, SAFEARRAY**) override { check(false, "Never scans"); return E_FAIL; }
  HRESULT STDMETHODCALLTYPE ScanNetwork(ULONG*) override { check(false, "Never scans"); return E_FAIL; }
  HRESULT STDMETHODCALLTYPE GetConnection(mbn::Connection** out) override {
    if (!s.connection) return static_cast<HRESULT>(0x80548212);  // E_MBN_NOT_REGISTERED
    *out = new Connection(s);
    return S_OK;
  }
};
std::vector<Spec> modems;
struct Manager : Object<mbn::Manager> {
  HRESULT STDMETHODCALLTYPE GetInterface(LPCWSTR, mbn::Interface**) override { return E_FAIL; }
  HRESULT STDMETHODCALLTYPE GetInterfaces(SAFEARRAY** out) override {
    if (FAILED(interfaces_result)) return interfaces_result;
    if (no_interface_array) { *out = nullptr; return S_OK; }
    auto* array = new SAFEARRAY{true, {}, {}};
    ++live_arrays;
    for (const Spec& spec : modems) array->items.push_back(new Interface(spec));
    *out = array;
    return S_OK;
  }
};

void reset() {
  initialize_result = S_OK; create_result = S_OK; interfaces_result = S_OK;
  no_interface_array = false;
  initialized = uninitialized = 0;
  modems.clear();
}
void balanced() {
  check(strings.empty(), "Every BSTR is freed");
  check(live_objects == 0, "Every COM object is released");
  check(live_arrays == 0, "Every SAFEARRAY is destroyed");
  check(initialized == uninitialized || initialize_result == RPC_E_CHANGED_MODE,
        "COM initialization is balanced");
}
std::string hex(const std::string& utf8) {
  static const char digits[] = "0123456789abcdef";
  std::string out;
  for (unsigned char c : utf8) { out += digits[c >> 4]; out += digits[c & 15]; }
  return out;
}
std::vector<std::string> split(const std::string& line) {
  std::vector<std::string> fields;
  size_t start = 0;
  while (true) {
    size_t tab = line.find('\t', start);
    fields.push_back(line.substr(start, tab == std::string::npos ? std::string::npos : tab - start));
    if (tab == std::string::npos) return fields;
    start = tab + 1;
  }
}
std::vector<std::string> lines(const std::string& text) {
  std::vector<std::string> out;
  size_t start = 0;
  while (start < text.size()) {
    size_t end = text.find('\n', start);
    out.push_back(text.substr(start, end - start));
    start = end + 1;
  }
  return out;
}
}  // namespace fixture

HRESULT CoInitializeEx(LPVOID, DWORD) {
  if (SUCCEEDED(fixture::initialize_result)) ++fixture::initialized;
  return fixture::initialize_result;
}
void CoUninitialize() { ++fixture::uninitialized; }
HRESULT CoCreateInstance(REFCLSID clsid, LPUNKNOWN, DWORD, REFIID iid, LPVOID* out) {
  fixture::check(clsid == mbn::kManagerClass && iid == mbn::kManager, "Creates only the interface manager");
  if (FAILED(fixture::create_result)) return fixture::create_result;
  *out = static_cast<mbn::Manager*>(new fixture::Manager());
  return S_OK;
}

int main() {
  using namespace fixture;
  Api api;

  // No WWAN service on this machine: "none", not an error, and COM stays balanced.
  reset();
  create_result = REGDB_E_CLASSNOTREG;
  check(Snapshot(api) == "status\tnone\t80040154\n", "Missing WwanSvc reports no modem");
  balanced();

  reset();
  interfaces_result = HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
  check(Snapshot(api) == "status\tnone\t80070490\n", "No WWAN interface reports no modem");
  balanced();

  reset();
  interfaces_result = E_ACCESSDENIED;
  check(Snapshot(api) == "status\terror\t80070005\n", "A denial is an error, not an absent modem");
  balanced();

  reset();
  initialize_result = static_cast<HRESULT>(0x8007000E);
  check(Snapshot(api) == "status\terror\t8007000e\n", "Failed COM initialization is reported");
  balanced();

  // A thread already in another apartment still reads, without uninitializing it.
  reset();
  initialize_result = RPC_E_CHANGED_MODE;
  no_interface_array = true;
  check(Snapshot(api) == "status\tok\t00000000\n", "Existing apartment is usable");
  check(uninitialized == 0, "Another apartment is never uninitialized");
  balanced();

  // A registered LTE modem with SIM: every field, Unicode names as UTF-8 hex.
  reset();
  modems.push_back(Spec());
  {
    auto out = lines(Snapshot(api));
    check(out.size() == 2 && out[0] == "status\tok\t00000000", "One modem line");
    auto f = split(out[1]);
    check(f.size() == 24, "Twenty-four modem fields");
    check(f[0] == "modem" && f[1] == hex("{0D7F7A43-0000-4000-8000-00000000C311}"), "Interface ID");
    check(f[2] == "1" && f[3] == "1" && f[4] == "1" && f[5] == "63" && f[6] == "3", "Ready state and caps");
    check(f[7] == hex("356938035643809") && f[8] == hex("Fixture") && f[9] == hex("WWAN 1")
          && f[10] == hex("FW 1.0"), "Device identity");
    check(f[11] == hex("425010123456789") && f[12] == hex("8997201000000000001F"), "Subscriber IDs");
    check(f[13] == hex("+972501234567"), "Telephone numbers");
    check(f[14] == hex("42501") && f[15] == hex("\xd7\xa4\xd7\x9c\xd7\x90\xd7\xa4\xd7\x95\xd7\x9f"),
          "Hebrew home provider name is exact UTF-8");
    check(f[16] == "3" && f[17] == hex("42501") && f[18] == hex("Partner \xf0\x9f\x93\xb6")
          && f[19] == "", "Registration with a surrogate pair");
    check(f[20] == "32" && f[21] == "20" && f[22] == "99" && f[23] == "1", "Data class, signal, activation");
  }
  balanced();

  // No SIM: subscriber and home provider unavailable ('-'), no packet connection.
  reset();
  {
    Spec spec;
    spec.ready = 2; spec.subscriber = false; spec.connection = false; spec.register_state = 2;
    modems.push_back(spec);
  }
  {
    auto f = split(lines(Snapshot(api))[1]);
    check(f[2] == "2" && f[11] == "-" && f[12] == "-" && f[13] == "-" && f[14] == "-" && f[15] == "-",
          "SIM-dependent fields are unreported, not empty");
    check(f[16] == "2" && f[23] == "-", "Searching, no connection object");
  }
  balanced();

  // Interfaces that cannot be queried leave unreported fields; IIDs fail closed.
  reset();
  {
    Spec spec;
    spec.caps = false; spec.registration = false; spec.signal = false;
    modems.push_back(spec);
  }
  {
    auto f = split(lines(Snapshot(api))[1]);
    for (int i = 3; i <= 10; i++) check(f[i] == "-", "Capabilities unavailable");
    for (int i = 16; i <= 22; i++) check(f[i] == "-", "Registration and signal unavailable");
  }
  balanced();

  // Bounds: eight interfaces, 256 UTF-16 units per string, eight numbers; an
  // unpaired surrogate becomes U+FFFD rather than invalid UTF-8.
  reset();
  for (int i = 0; i < 10; i++) {
    Spec spec;
    spec.id = u"if" + std::u16string(1, static_cast<char16_t>(u'0' + i));
    spec.model = std::u16string(400, u'm');
    spec.manufacturer = std::u16string(1, static_cast<char16_t>(0xD800)) + u"x";
    spec.numbers.assign(12, u"1");
    modems.push_back(spec);
  }
  {
    auto out = lines(Snapshot(api));
    check(out.size() == 9, "At most eight interfaces");
    auto f = split(out[1]);
    check(f[9] == hex(std::string(256, 'm')), "Strings are bounded");
    check(f[8] == hex("\xef\xbf\xbdx"), "Unpaired surrogate replaced");
    size_t commas = 0;
    for (char c : f[13]) commas += c == ',';
    check(commas == 7, "At most eight numbers");
  }
  balanced();

  std::cout << "Windows cellular native contracts passed: " << checks << " checks\n";
  return 0;
}
