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

// The connection profiles WwanSvc stores, per interface ID, as XML; and the writes the
// adapter makes. Writes are allowed only inside Configure.
std::map<std::u16string, std::vector<std::u16string>> profiles;
HRESULT profile_manager_result = S_OK, profiles_result = S_OK, update_result = S_OK,
        disconnect_result = S_OK, connect_result = S_OK, state_result = S_OK;
// Updates after this many succeed fail with update_result (the first ones succeed).
size_t updates_succeeding = 0;
bool writes_allowed = false;
std::vector<std::u16string> updates, connects;
int disconnects = 0;

std::u16string profile(const std::u16string& mode, const std::u16string& imsi = u"425010123456789",
                       bool isDefault = true, const std::u16string& name = u"Partner &amp; Co") {
  return u"<?xml version=\"1.0\"?><MBNProfile xmlns=\"http://www.microsoft.com/networking/WWAN/profile/v1\">"
         u"<Name>" + name + u"</Name><IsDefault>" + (isDefault ? u"true" : u"false")
         + u"</IsDefault><ProfileCreationType>DeviceProvisioned</ProfileCreationType><SubscriberID>" + imsi
         + u"</SubscriberID><SimIccID>8997201000000000001F</SimIccID><HomeProviderName>Partner</HomeProviderName>"
           u"<AutoConnectOnInternet>false</AutoConnectOnInternet>"
         + (mode.empty() ? u"" : u"<ConnectionMode>" + mode + u"</ConnectionMode>")
         + u"<Context><AccessString>internet</AccessString><UserLogonCred><UserName>user</UserName>"
           u"</UserLogonCred></Context></MBNProfile>";
}
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
  bool caps = true, subscriber = true, registration = true, signal = true, connection = true, radio = true;
  bool radio_readable = true;
  std::u16string device = u"356938035643809", manufacturer = u"Fixture", model = u"WWAN 1",
                 firmware = u"FW 1.0";
  std::u16string imsi = u"425010123456789", iccid = u"8997201000000000001F";
  std::vector<std::u16string> numbers = {u"+972501234567"};
  std::u16string home_id = u"42501", home_name = u"פלאפון";
  ULONG register_state = 3, data_class = 0x20, signal_value = 20, signal_error = 99, activation = 1;
  mbn::MBN_RADIO software_radio = mbn::MBN_RADIO_ON, hardware_radio = mbn::MBN_RADIO_ON;
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
  HRESULT STDMETHODCALLTYPE Connect(ULONG mode, LPCWSTR profile, ULONG* request) override {
    check(writes_allowed, "Only Configure connects");
    check(mode == 0, "Connects with a stored profile (MBN_CONNECTION_MODE_PROFILE)");
    connects.push_back(profile);
    *request = 7;
    return connect_result;
  }
  HRESULT STDMETHODCALLTYPE Disconnect(ULONG* request) override {
    check(writes_allowed, "Only Configure disconnects");
    ++disconnects;
    *request = 8;
    return disconnect_result;
  }
  HRESULT STDMETHODCALLTYPE GetConnectionState(ULONG* state, BSTR* profile) override {
    if (FAILED(state_result)) return state_result;
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
struct Radio : Object<mbn::Radio> {
  const Spec& s;
  explicit Radio(const Spec& s) : s(s) { }
  HRESULT STDMETHODCALLTYPE get_SoftwareRadioState(mbn::MBN_RADIO* out) override {
    if (!s.radio_readable) return static_cast<HRESULT>(0x80548210);  // E_MBN_FAILURE
    *out = s.software_radio;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE get_HardwareRadioState(mbn::MBN_RADIO* out) override {
    if (!s.radio_readable) return static_cast<HRESULT>(0x80548210);
    *out = s.hardware_radio;
    return S_OK;
  }
};
struct Interface : Object<mbn::Interface> {
  Spec s;
  explicit Interface(Spec spec) : s(std::move(spec)) { }
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
    if (iid == mbn::kRegistration && s.registration) { *out = static_cast<mbn::Registration*>(new Registration(s)); return S_OK; }
    if (iid == mbn::kSignal && s.signal) { *out = static_cast<mbn::Signal*>(new Signal(s)); return S_OK; }
    if (iid == mbn::kRadio && s.radio) { *out = static_cast<mbn::Radio*>(new Radio(s)); return S_OK; }
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
struct Profile : Object<mbn::ConnectionProfile> {
  std::u16string id;
  size_t index;
  Profile(std::u16string id, size_t index) : id(std::move(id)), index(index) { }
  HRESULT STDMETHODCALLTYPE GetProfileXmlData(BSTR* out) override {
    *out = make(profiles[id].at(index));
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE UpdateProfile(LPCWSTR xml) override {
    check(writes_allowed, "Only Configure updates a profile");
    updates.push_back(xml);
    HRESULT result = updates.size() > updates_succeeding ? update_result : S_OK;
    if (SUCCEEDED(result)) profiles[id].at(index) = xml;
    return result;
  }
};
struct ProfileManager : Object<mbn::ProfileManager> {
  HRESULT STDMETHODCALLTYPE GetConnectionProfiles(mbn::Interface* modem, SAFEARRAY** out) override {
    if (FAILED(profiles_result)) return profiles_result;
    BSTR id = nullptr;
    check(SUCCEEDED(modem->get_InterfaceID(&id)), "Profiles of an interface");
    std::u16string key(id, strings[id]);
    SysFreeString(id);
    auto* array = new SAFEARRAY{true, {}, {}};
    ++live_arrays;
    for (size_t i = 0; i < profiles[key].size(); i++) array->items.push_back(new Profile(key, i));
    *out = array;
    return S_OK;
  }
};
struct Manager : Object<mbn::Manager> {
  HRESULT STDMETHODCALLTYPE GetInterface(LPCWSTR id, mbn::Interface** out) override {
    for (const Spec& spec : modems) {
      if (spec.id == id) { *out = new Interface(spec); return S_OK; }
    }
    return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
  }
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
  profiles.clear();
  profiles[Spec().id] = {profile(u"auto")};
  profile_manager_result = profiles_result = update_result = disconnect_result = connect_result = S_OK;
  state_result = S_OK;
  updates_succeeding = 0;
  writes_allowed = false;
  updates.clear(); connects.clear(); disconnects = 0;
}
std::string configure(Api& api, const char16_t* id, const char16_t* mode) {
  writes_allowed = true;
  std::string out = Configure(api, id, mode);
  writes_allowed = false;
  return out;
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
  if (clsid == mbn::kProfileManagerClass) {
    fixture::check(iid == mbn::kProfileManager, "The profile manager by its IID");
    if (FAILED(fixture::profile_manager_result)) return fixture::profile_manager_result;
    *out = static_cast<mbn::ProfileManager*>(new fixture::ProfileManager());
    return S_OK;
  }
  fixture::check(clsid == mbn::kManagerClass && iid == mbn::kManager,
                 "Creates only the interface and profile managers");
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
    check(f.size() == 27, "Twenty-seven modem fields");
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
    check(f[24] == "1" && f[25] == "1", "Software and hardware radio on");
    check(f[26] == hex("auto"), "The default profile's connection mode");
  }
  check(updates.empty() && connects.empty() && disconnects == 0, "A snapshot writes nothing");
  balanced();

  // The SIM's default profile: IsDefault and this SIM's SubscriberID; the schema's default
  // mode, manual, without the element; unreadable profiles are unreported, not a mode.
  struct ModeCase { std::vector<std::u16string> stored; const char* expected; const char* reason; };
  const ModeCase modes[] = {
      {{profile(u"auto", u"999990000000001"), profile(u"manual", u"425010123456789", false),
        profile(u"auto-home")}, "auto-home", "Only this SIM's default profile counts"},
      {{profile(u"")}, "manual", "No ConnectionMode element: the schema's manual"},
      {{profile(u"auto", u"999990000000001")}, "", "No default profile for this SIM"},
      {{}, "", "No stored profile"},
      {{u"<MBNProfile><Name>x</Name><IsDefault>true</IsDefault><SubscriberID>425010123456789</SubscriberID>"
        u"<ConnectionMode mode=\"x\">auto</ConnectionMode></MBNProfile>"}, "-", "A form the edit does not handle"},
      {{u"<MBNProfile><Name>x</Name><IsDefault>true</IsDefault><SubscriberID>425010123456789</SubscriberID>"
        u"<ConnectionMode>auto</MBNProfile>"}, "-", "An unterminated element"},
  };
  for (const ModeCase& c : modes) {
    reset();
    modems.push_back(Spec());
    profiles[Spec().id] = c.stored;
    auto f = split(lines(Snapshot(api))[1]);
    check(f[26] == (c.expected[0] == '-' ? std::string("-") : hex(c.expected)), c.reason);
    balanced();
  }
  // A profile without SubscriberID (v4 MBNProfileExt) belongs to the SIM by its SimIccID;
  // one with another SubscriberID does not, whatever its SimIccID.
  const std::u16string noSubscriber =
      u"<MBNProfileExt xmlns=\"http://www.microsoft.com/networking/WWAN/profile/v4\"><Name>e</Name>"
      u"<IsDefault>true</IsDefault><SimIccID>8997201000000000001</SimIccID>"
      u"<ConnectionMode>auto-home</ConnectionMode></MBNProfileExt>";
  struct SelectionCase { std::vector<std::u16string> stored; const char* expected; const char* reason; };
  std::vector<std::u16string> many(16, profile(u"auto", u"999990000000001"));
  many.push_back(profile(u"manual"));
  const SelectionCase selections[] = {
      {{noSubscriber}, "auto-home", "SimIccID (without its F padding) when there is no SubscriberID"},
      {{profile(u"auto", u"999990000000001")}, "", "Another SubscriberID with the same SimIccID is not this SIM's"},
      {{u"<v1:MBNProfile xmlns:v1=\"x\"><v1:Name>p</v1:Name><v1:IsDefault>true</v1:IsDefault>"
        u"<v1:SubscriberID>425010123456789</v1:SubscriberID><v1:ConnectionMode>auto</v1:ConnectionMode>"
        u"</v1:MBNProfile>"}, "-", "Prefixed elements are unreadable, not absent"},
      {many, "", "At most sixteen profiles are read"},
  };
  for (const SelectionCase& c : selections) {
    reset();
    modems.push_back(Spec());
    profiles[Spec().id] = c.stored;
    auto f = split(lines(Snapshot(api))[1]);
    check(f[26] == (c.expected[0] == '-' ? std::string("-") : hex(c.expected)), c.reason);
    balanced();
  }

  reset();
  modems.push_back(Spec());
  profile_manager_result = REGDB_E_CLASSNOTREG;
  check(split(lines(Snapshot(api))[1])[26] == "-", "No profile manager: mode unreported");
  balanced();
  reset();
  modems.push_back(Spec());
  profiles_result = E_ACCESSDENIED;
  check(split(lines(Snapshot(api))[1])[26] == "-", "Unreadable profiles: mode unreported");
  balanced();
  reset();
  modems.push_back(Spec());
  profiles_result = HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
  check(split(lines(Snapshot(api))[1])[26] == "", "No profiles stored (ERROR_NOT_FOUND)");
  balanced();

  // Configure: the default profile's ConnectionMode, then the connection follows it.
  const std::u16string id = Spec().id;
  reset();
  modems.push_back(Spec());  // home, connected
  check(configure(api, id.c_str(), u"manual") == "ok\tdisconnect\t00000000", "Data off disconnects");
  check(updates.size() == 1 && profiles[id][0] == profile(u"manual"), "Only ConnectionMode changes");
  check(disconnects == 1 && connects.empty(), "One disconnect");
  balanced();

  reset();
  {
    Spec roaming;
    roaming.register_state = 4;
    modems.push_back(roaming);
  }
  check(configure(api, id.c_str(), u"auto-home") == "ok\tdisconnect\t00000000", "Roaming off disconnects while roaming");
  check(profiles[id][0] == profile(u"auto-home"), "auto-home stored");
  balanced();

  reset();
  modems.push_back(Spec());
  check(configure(api, id.c_str(), u"auto-home") == "ok\tnone\t00000000", "auto-home at home keeps the connection");
  check(disconnects == 0 && connects.empty(), "No connection change at home");
  balanced();

  reset();
  {
    Spec down;
    down.activation = 3;
    modems.push_back(down);
  }
  profiles[id] = {profile(u"manual")};
  check(configure(api, id.c_str(), u"auto") == "ok\tconnect\t00000000", "Data on connects");
  check(connects.size() == 1 && connects[0] == u"Partner & Co", "Connects with the decoded profile name");
  balanced();

  reset();
  {
    Spec down;
    down.activation = 3;
    modems.push_back(down);
  }
  profiles[id] = {profile(u"manual")};
  connect_result = static_cast<HRESULT>(0x80548212);  // E_MBN_NOT_REGISTERED
  check(configure(api, id.c_str(), u"auto") == "ok\tconnect\t80548212", "A refused connect is reported, not an error");
  check(profiles[id][0] == profile(u"auto"), "The mode stays set");
  balanced();

  reset();
  modems.push_back(Spec());
  check(configure(api, id.c_str(), u"auto") == "ok\tunchanged\t00000000", "Unchanged mode, already connected");
  check(updates.empty(), "An unchanged mode is not rewritten");
  balanced();

  // An unchanged mode changes nothing (DataSettingsManager: an unchanged switch is a
  // no-op): a connection made by hand in manual mode stays, and a connection dropped by
  // hand in auto mode is not reconnected.
  reset();
  modems.push_back(Spec());  // connected by hand
  profiles[id] = {profile(u"manual")};
  check(configure(api, id.c_str(), u"manual") == "ok\tunchanged\t00000000"
        && disconnects == 0 && updates.empty(), "A repeated off leaves a hand-made connection");
  balanced();
  reset();
  {
    Spec down;
    down.activation = 3;  // disconnected by hand
    modems.push_back(down);
  }
  check(configure(api, id.c_str(), u"auto") == "ok\tunchanged\t00000000" && connects.empty(),
        "A repeated on does not reconnect");
  balanced();

  // Registration: a partner network is roaming; an unregistered modem is not connected;
  // a connection that was never activated (MBN_ACTIVATION_STATE_NONE) is connected.
  reset();
  {
    Spec partner;
    partner.register_state = 5;
    modems.push_back(partner);
  }
  check(configure(api, id.c_str(), u"auto-home") == "ok\tdisconnect\t00000000", "A partner network is roaming");
  balanced();
  reset();
  {
    Spec searching;
    searching.register_state = 2;
    searching.activation = 3;
    modems.push_back(searching);
  }
  profiles[id] = {profile(u"manual")};
  check(configure(api, id.c_str(), u"auto") == "ok\tnone\t00000000" && connects.empty(),
        "No connect while unregistered");
  balanced();
  reset();
  {
    Spec never;
    never.activation = 0;
    modems.push_back(never);
  }
  profiles[id] = {profile(u"manual")};
  check(configure(api, id.c_str(), u"auto") == "ok\tconnect\t00000000" && connects.size() == 1,
        "A never-activated connection is connected");
  balanced();

  // The connection state cannot be read for a disconnect: an error, and the previous
  // mode is restored.
  reset();
  modems.push_back(Spec());
  state_result = E_ACCESSDENIED;
  check(configure(api, id.c_str(), u"manual") == "error\tstate\t80070005", "An unreadable connection state is an error");
  check(updates.size() == 2 && profiles[id][0] == profile(u"auto"), "and restores the previous mode");
  balanced();

  // A refused disconnect whose restore is refused too: reported as its own error, with the
  // new mode left stored while the connection is still up.
  reset();
  modems.push_back(Spec());
  disconnect_result = E_ACCESSDENIED;
  update_result = static_cast<HRESULT>(0x80070020);  // ERROR_SHARING_VIOLATION
  updates_succeeding = 1;
  check(configure(api, id.c_str(), u"manual") == "error\trollback\t80070020", "A refused restore is reported");
  check(profiles[id][0] == profile(u"manual"), "The new mode stayed stored");
  balanced();

  reset();
  modems.push_back(Spec());
  profiles[id] = {profile(u"")};
  check(configure(api, id.c_str(), u"auto-home") == "ok\tnone\t00000000", "Absent element is inserted");
  {
    std::u16string expected = profile(u"");
    std::u16string anchor = u"<AutoConnectOnInternet>false</AutoConnectOnInternet>";
    expected.insert(expected.find(anchor) + anchor.size(), u"<ConnectionMode>auto-home</ConnectionMode>");
    check(profiles[id][0] == expected, "In its schema position");
  }
  balanced();

  reset();
  modems.push_back(Spec());
  disconnect_result = E_ACCESSDENIED;
  check(configure(api, id.c_str(), u"manual") == "error\tdisconnect\t80070005", "A refused disconnect is an error");
  check(updates.size() == 2 && profiles[id][0] == profile(u"auto"), "A refused disconnect restores the previous profile");
  balanced();

  reset();
  modems.push_back(Spec());
  disconnect_result = HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
  check(configure(api, id.c_str(), u"manual") == "ok\tdisconnect\t80070490", "Already disconnected");
  balanced();

  reset();
  modems.push_back(Spec());
  update_result = E_ACCESSDENIED;
  check(configure(api, id.c_str(), u"manual") == "error\tupdate\t80070005", "A refused update is an error");
  check(disconnects == 0, "Nothing disconnects after a refused update");
  balanced();

  reset();
  modems.push_back(Spec());
  check(configure(api, id.c_str(), u"off") == "error\tmode\t80070057", "Only the schema's modes");
  check(initialized == 0, "An invalid mode reaches no COM");
  check(configure(api, u"{other}", u"manual") == "error\tinterface\t80070490", "Unknown interface");
  profiles[id] = {profile(u"auto", u"999990000000001")};
  check(configure(api, id.c_str(), u"manual") == "error\tprofile\t80070490", "No default profile");
  check(updates.empty() && disconnects == 0, "Errors write nothing");
  balanced();

  reset();
  {
    Spec noSim;
    noSim.subscriber = false;
    modems.push_back(noSim);
  }
  check(configure(api, id.c_str(), u"manual") == "error\tsubscriber\t80548202", "No SIM, no profile");
  balanced();

  // A write from a thread already in another apartment leaves that apartment alone.
  reset();
  modems.push_back(Spec());
  initialize_result = RPC_E_CHANGED_MODE;
  check(configure(api, id.c_str(), u"manual") == "ok\tdisconnect\t00000000", "Writes in an existing apartment");
  check(uninitialized == 0, "A write never uninitializes another apartment");
  balanced();
  reset();
  initialize_result = static_cast<HRESULT>(0x8007000E);
  check(configure(api, id.c_str(), u"manual") == "error\tcom\t8007000e", "Failed COM initialization");
  balanced();

  // Airplane mode or Windows' cellular switch (software) and a hardware switch are reported
  // separately and in order; an unreadable radio is unreported, not off.
  reset();
  {
    Spec off;
    off.software_radio = mbn::MBN_RADIO_OFF;
    modems.push_back(off);
    Spec hardware;
    hardware.id = u"{0D7F7A43-0000-4000-8000-00000000C312}";
    hardware.hardware_radio = mbn::MBN_RADIO_OFF;
    modems.push_back(hardware);
    Spec unreadable;
    unreadable.id = u"{0D7F7A43-0000-4000-8000-00000000C313}";
    unreadable.radio_readable = false;
    modems.push_back(unreadable);
    Spec missing;
    missing.id = u"{0D7F7A43-0000-4000-8000-00000000C314}";
    missing.radio = false;
    modems.push_back(missing);
  }
  {
    auto out = lines(Snapshot(api));
    check(out.size() == 5, "Four modem lines");
    auto software = split(out[1]), hardware = split(out[2]), unreadable = split(out[3]), missing = split(out[4]);
    check(software[24] == "0" && software[25] == "1", "Software radio states are reported in order");
    check(hardware[24] == "1" && hardware[25] == "0", "Hardware radio states are reported in order");
    check(unreadable[24] == "-" && unreadable[25] == "-", "A failed radio read is unreported");
    check(missing[24] == "-" && missing[25] == "-", "No IMbnRadio: radio unreported");
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
