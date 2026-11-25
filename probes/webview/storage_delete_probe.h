// Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0
// Actual Host::ClearOriginData qualification. CDP seeds and observes browser
// data; only the public production Host method performs deletion.
#pragma once

#include <mutex>

#include "environment_options.h"
#include "quota_registry_probe.h"
#include "worker_response_server.h"

namespace storage_delete_probe {
using namespace gesher::webview;
namespace quota = quota_registry_probe;
using browser_session_probe::Json;
using browser_session_probe::PumpUntil;
using browser_session_probe::Quote;

inline void Log(const wchar_t* level, const std::wstring& message) {
  wprintf(L"%ls  storage-delete: %ls\n", level, message.c_str());
  fflush(stdout);
}

struct Case {
  quota::Observation observed;
  std::wstring top;
  std::wstring middle;  // A -> B -> A exposes the real implicit-site ^31 key.
  bool removed = false;
};

// Keep the browser's actual IndexedDB transaction/readback fixture. Add data
// outside Android's quota deletion mask and an authenticated nested-frame route.
// No expected storage key, inventory, deletion or quota is supplied by this JS.
inline std::string Page(const std::wstring& origin) {
  std::string page = quota::Page(origin);
  const size_t end = page.rfind("</script></body>");
  if (end == std::string::npos) return {};
  page.insert(end, R"JS(
const underlyingOperation = operate;
async function fileSystem(mode, label) {
  if (!['A-first-party', 'B-first-party'].includes(label)) return {state:'not-tested', bytes:0};
  if (typeof navigator.storage?.getDirectory !== 'function') throw Error('real OPFS interface unavailable');
  const root = await navigator.storage.getDirectory();
  const name = 'gesher-storage-delete-' + label;
  if (mode === 'absent') {
    try { await root.getFileHandle(name); }
    catch (error) {
      if (error.name === 'NotFoundError') return {state:'absent', bytes:0};
      throw error;
    }
    throw Error('deleted OPFS file still exists');
  }
  const seed = Array.from(label).reduce((n, c) => n + c.charCodeAt(0), 0);
  const handle = await root.getFileHandle(name, {create:mode === 'write'});
  if (mode === 'write') {
    const data = new Uint8Array(8192);
    for (let i = 0; i < data.length; ++i) data[i] = (i * 29 + seed) % 251;
    const stream = await handle.createWritable();
    try { await stream.write(data); await stream.close(); }
    catch (error) { await stream.abort().catch(() => {}); throw error; }
  }
  const data = new Uint8Array(await (await handle.getFile()).arrayBuffer());
  if (data.length !== 8192) throw Error('OPFS length mismatch');
  for (let i = 0; i < data.length; ++i) {
    if (data[i] !== (i * 29 + seed) % 251) throw Error('OPFS byte mismatch at ' + i);
  }
  return {state:mode === 'write' ? 'written' : 'preserved', bytes:data.length};
}
operate = async (mode, label) => {
  const key = 'gesher-preserve-' + label;
  const firstParty = self === top;
  if (mode === 'write') {
    localStorage.setItem(key, label);
    if (firstParty) document.cookie = key + '=' + label + '; Path=/; Max-Age=3600; SameSite=Lax' +
        (location.protocol === 'https:' ? '; Secure' : '');
  }
  const value = await underlyingOperation(mode, label);
  if (localStorage.getItem(key) !== label) throw Error('localStorage sentinel was removed: ' + label);
  if (firstParty && !document.cookie.split(';').some(part => part.trim() === key + '=' + label))
    throw Error('persistent cookie sentinel was removed: ' + label);
  return {...value, localStoragePreserved:true, cookieChecked:firstParty, fileSystem:await fileSystem(mode, label)};
};
window.operate = operate;
window.withNested = (middle, origin, mode, label) => new Promise((resolve, reject) => {
  document.getElementById('quota-child')?.remove();
  const frame = document.createElement('iframe'); frame.id = 'quota-child';
  const nonce = crypto.randomUUID();
  const listener = event => {
    if (event.source !== frame.contentWindow || event.origin !== middle || event.data?.nonce !== nonce) return;
    window.removeEventListener('message', listener);
    if (event.data.error) reject(Error(event.data.error)); else resolve(event.data.value);
  };
  window.addEventListener('message', listener);
  frame.onload = () => frame.contentWindow.postMessage({kind:'quota-nested-fixture', nonce, origin, mode, label}, middle);
  frame.onerror = () => { window.removeEventListener('message', listener); reject(Error('nested iframe load failed')); };
  frame.src = middle + '/storage.html'; document.body.appendChild(frame);
});
window.addEventListener('message', event => {
  if (event.source !== parent || !event.data || event.data.kind !== 'quota-nested-fixture') return;
  const {nonce, origin, mode, label} = event.data;
  if (!['write', 'read', 'absent'].includes(mode) || typeof label !== 'string' || typeof origin !== 'string') return;
  window.withFrame(origin, mode, label).then(
    value => event.source.postMessage({nonce, value}, event.origin),
    error => event.source.postMessage({nonce, error:String(error)}, event.origin));
});
)JS");
  return page;
}

inline bool StartOrdinary(const std::shared_ptr<quota::State>& state, HWND parent,
                          const std::wstring& profile, const std::vector<std::wstring>& origins) {
  // Use the actual production options when sharing its live user-data folder.
  Com<ICoreWebView2EnvironmentOptions> options(new EnvironmentOptions(HostBrowserArguments(L"")), false);
  if (!quota::Start(state, parent, profile, origins, options.get())) return false;
  Com<ICoreWebView2_13> core13;
  Com<ICoreWebView2Profile> actual;
  CoString name;
  BOOL in_private = TRUE;
  if (!state->core.As(IID_ICoreWebView2_13, &core13) ||
      !state->Check(core13->get_Profile(actual.Put()), L"fixture actual profile") || !actual ||
      !state->Check(actual->get_ProfileName(name.Put()), L"fixture profile name") ||
      !state->Check(actual->get_IsInPrivateModeEnabled(&in_private), L"fixture ordinary profile") || in_private) {
    return state->Fail(L"fixture did not open the actual ordinary application profile");
  }
  // The actual default profile has a valid empty name. Keep it unchanged.
  Log(L"INFO", L"ordinary-profile=\"" + name.str() + L"\" path=" + state->profile_path);
  for (const auto& origin : origins) {
    std::string page = Page(origin);
    if (page.empty()) return state->Fail(L"storage fixture page construction failed");
    state->pages[origin + L"/storage.html"] = std::move(page);
  }
  return true;
}

inline bool Visit(const std::shared_ptr<quota::State>& state, const std::wstring& origin) {
  const std::wstring url = origin + L"/storage.html";
  CoString source;
  if (!state->Check(state->core->get_Source(source.Put()), L"fixture current document")) return false;
  return source.str() == url || quota::Navigate(state, url);
}

inline bool Content(const std::shared_ptr<quota::State>& state, const Case& item,
                    const wchar_t* mode) {
  if (!Visit(state, item.top)) return false;
  const bool first_party = item.middle.empty() && item.top == item.observed.origin;
  const std::wstring arguments = Quote(mode) + L"," + Quote(item.observed.label);
  const std::wstring expression = !item.middle.empty()
      ? L"window.withNested(" + Quote(item.middle) + L"," + Quote(item.observed.origin) + L"," + arguments + L")"
      : first_party ? L"window.operate(" + arguments + L")"
      : L"window.withFrame(" + Quote(item.observed.origin) + L"," + arguments + L")";
  Json value;
  if (!quota::Evaluate(state, expression, &value)) return false;
  const Json* origin = value.Get(L"origin", L'"');
  const Json* label = value.Get(L"label", L'"');
  const Json* bytes = value.Get(L"bytes", L'#');
  const Json* file = value.Get(L"fileSystem", L'{');
  const Json* file_state = file ? file->Get(L"state", L'"') : nullptr;
  const Json* file_bytes = file ? file->Get(L"bytes", L'#') : nullptr;
  const bool absent = std::wstring_view(mode) == L"absent";
  const bool file_tested = item.observed.label == L"A-first-party" || item.observed.label == L"B-first-party";
  const std::wstring file_expected = !file_tested ? L"not-tested" : absent ? L"absent" :
      std::wstring_view(mode) == L"write" ? L"written" : L"preserved";
  if (!origin || origin->text != item.observed.origin || !label || label->text != item.observed.label ||
      !value.Get(L"secure", L't') || !value.Get(L"closed", L't') || !bytes ||
      bytes->text != (absent ? L"0" : std::to_wstring(quota::kPayloadSize)) ||
      !value.Get(L"absent", absent ? L't' : L'f') || !value.Get(L"localStoragePreserved", L't') ||
      !value.Get(L"cookieChecked", first_party ? L't' : L'f') || !file_state || !file_bytes ||
      file_state->text != file_expected || file_bytes->text != (file_tested && !absent ? L"8192" : L"0")) {
    return state->Fail(L"invalid actual storage/cookie evidence for " + item.observed.label);
  }
  Log(L"PASS", std::wstring(mode) + L" " + item.observed.label + L" bytes=" + bytes->text +
      L" localStorage=preserved cookies=" + (first_party ? L"preserved" : L"not-tested") + L" closed=true");
  if (file_tested) Log(L"PASS", L"filesystem " + std::wstring(mode) + L" " + item.observed.label +
      L" bytes=" + file_bytes->text + L" writable-closed=true");
  return !state->failed;
}

inline bool Seed(const std::shared_ptr<quota::State>& state, Case* item) {
  if (!Content(state, *item, L"write")) return false;
  if (!item->middle.empty()) return true;  // The real registry supplies the nested key below.
  return quota::CaptureKey(state, &item->observed, item->top != item->observed.origin);
}

inline bool Measure(const std::shared_ptr<quota::State>& state, const wchar_t* phase,
                    const std::vector<Case>& cases, size_t count) {
  if (!quota::Navigate(state, L"edge://quota-internals/", true)) return false;
  std::vector<quota::Observation> positive, removed;
  for (size_t i = 0; i < count; ++i) {
    (cases[i].removed ? removed : positive).push_back(cases[i].observed);
  }
  if (!quota::Measure(state, phase, positive, removed)) return false;
  Log(L"PASS", std::wstring(phase) + L" positive=" + std::to_wstring(positive.size()) +
      L" removed=" + std::to_wstring(removed.size()));
  return true;
}

inline bool CaptureNestedKey(const std::shared_ptr<quota::State>& state,
                             const std::vector<Case>& before, Case* nested) {
  if (!quota::Navigate(state, L"edge://quota-internals/", true)) return false;
  std::set<std::wstring> known;
  for (const auto& item : before) known.insert(item.observed.key);
  const ULONGLONG end = state->CallDeadline();
  do {
    std::vector<quota::Bucket> buckets;
    if (!quota::Registry(state, &buckets)) return false;
    std::set<std::wstring> added;
    for (const auto& bucket : buckets) {
      if (!known.contains(bucket.key) && bucket.usage > 0) added.insert(bucket.key);
    }
    if (!added.empty()) {
      // Validate the browser's observed serialization; retain that exact key.
      // No frame ID, key, registry row or positive usage is manufactured here.
      if (added.size() != 1 || *added.begin() != nested->observed.origin + L"/^31") {
        return state->Fail(L"nested A-to-B-to-A did not yield one distinct implicit-site storage key");
      }
      nested->observed.key = *added.begin();
      Log(L"INFO", L"observed nested registry key=" + nested->observed.key);
      return true;
    }
    PumpUntil(std::min(end, GetTickCount64() + 100), [] { return false; });
  } while (GetTickCount64() < end);
  return state->Fail(L"nested IndexedDB storage key did not appear in the live registry");
}

struct Completion {
  int32_t type;
  int64_t id;
  int32_t hr;
  DWORD thread;
  bool empty;
  uint64_t time_ms;
  HRESULT apartment_result;
  APTTYPE apartment;
  std::wstring detail;
};
inline std::mutex g_completion_mutex;
inline std::vector<Completion> g_completions;
inline bool g_completion_overflow = false;

inline uint64_t UtcMillis() {
  FILETIME value;
  GetSystemTimePreciseAsFileTime(&value);
  ULARGE_INTEGER ticks;
  ticks.LowPart = value.dwLowDateTime;
  ticks.HighPart = value.dwHighDateTime;
  return (ticks.QuadPart - 116444736000000000ULL) / 10000;
}

inline void Completed(int32_t type, int64_t id, int32_t hr, const std::wstring& value) {
  APTTYPE apartment = APTTYPE_CURRENT;
  APTTYPEQUALIFIER qualifier = APTTYPEQUALIFIER_NONE;
  const HRESULT apartment_result = CoGetApartmentType(&apartment, &qualifier);
  std::lock_guard<std::mutex> lock(g_completion_mutex);
  if (g_completions.size() == 16) { g_completion_overflow = true; return; }
  g_completions.push_back({type, id, hr, GetCurrentThreadId(), value.empty(), UtcMillis(), apartment_result,
      apartment, value.substr(0, 512)});
}

inline uint64_t CompletionTime(int64_t id) {
  std::lock_guard<std::mutex> lock(g_completion_mutex);
  for (const auto& result : g_completions) if (result.id == id) return result.time_ms;
  return 0;
}

inline bool Completions(const std::shared_ptr<quota::State>& state,
                        const std::vector<int64_t>& expected, bool wait) {
  if (wait && !PumpUntil(std::min(state->deadline, GetTickCount64() + 250000), [&] {
        std::lock_guard<std::mutex> lock(g_completion_mutex);
        return g_completions.size() >= expected.size() || g_completion_overflow;
      })) return state->Fail(L"production Host completion exceeded the qualification deadline");
  std::lock_guard<std::mutex> lock(g_completion_mutex);
  if (g_completion_overflow || g_completions.size() != expected.size()) {
    return state->Fail(L"production Host did not complete each admitted operation exactly once");
  }
  DWORD owner = 0;
  for (size_t i = 0; i < expected.size(); ++i) {
    const auto& result = g_completions[i];
    if (result.type != kGlobalDone || result.id != expected[i] || result.hr != S_OK || !result.empty ||
        result.thread == GetCurrentThreadId() || (owner && owner != result.thread) ||
        result.apartment_result != S_OK || result.apartment != APTTYPE_STA) {
      return state->Fail(L"unexpected production Host completion id=" + std::to_wstring(result.id) +
          L" hr=" + std::to_wstring(result.hr) + L" apartment=" + std::to_wstring(result.apartment) +
          L" detail=" + result.detail);
    }
    owner = result.thread;
  }
  Log(L"PASS", L"production callbacks=" + std::to_wstring(expected.size()) + L" ordered=true exactly-once=true owner-STA=true");
  return true;
}

inline bool NetworkControl(const std::shared_ptr<quota::State>& state,
                           worker_response_probe::LoopbackServer& server, unsigned expected,
                           const wchar_t* phase) {
  const std::wstring filter = server.Origin() + L"/*";
  if (!state->Check(state->core->RemoveWebResourceRequestedFilter(filter.c_str(),
      COREWEBVIEW2_WEB_RESOURCE_CONTEXT_ALL), L"open loopback positive control")) return false;
  const std::wstring url = server.Origin() + L"/network/storage-delete-control";
  Json value;
  const bool observed = quota::Navigate(state, url) &&
      quota::Evaluate(state, L"({body: document.body.innerText.trim()})", &value);
  const bool restored = state->Check(state->core->AddWebResourceRequestedFilter(filter.c_str(),
      COREWEBVIEW2_WEB_RESOURCE_CONTEXT_ALL), L"restore owned loopback fixture responses");
  const Json* body = value.Get(L"body", L'"');
  if (!observed || !restored || !body || body->text != L"origin:storage-delete-control" ||
      server.Count("/network/storage-delete-control") != expected || server.Connections() < expected || !server.Healthy()) {
    return state->Fail(L"independent loopback network counter positive control failed");
  }
  Log(L"PASS", L"network " + std::wstring(phase) + L" positive-control requests=" + std::to_wstring(expected) +
      L" accepted-connections=" + std::to_wstring(server.Connections()));
  return true;
}

// A second server/controller exercises real ordinary-profile networking while
// the InPrivate production resolver applies its target network conditions. It
// never visits a seeded storage origin, opens IndexedDB or uses storage APIs.
inline bool StartOrdinaryNetwork(const std::shared_ptr<quota::State>& state,
                                worker_response_probe::LoopbackServer& server) {
  const RECT bounds{0, 0, 400, 300};
  if (!state->Check(state->controller->put_Bounds(bounds), L"ordinary network controller bounds") ||
      !state->Check(state->controller->put_IsVisible(TRUE), L"ordinary network controller active") ||
      !quota::Navigate(state, server.Origin() + L"/network/storage-network-control")) return false;
  static const wchar_t* script = LR"JS((async () => {
    if (globalThis.__gesherOrdinaryNetwork) throw Error('network control already exists');
    const state = {running:true, stopped:false, successes:0, failures:0, times:[], firstError:''};
    globalThis.__gesherOrdinaryNetwork = state;
    let ready;
    const started = new Promise(resolve => { ready = resolve; });
    const step = async () => {
      if (!state.running) { state.stopped = true; return; }
      const abort = new AbortController();
      const timer = setTimeout(() => abort.abort(), 10000);
      try {
        const response = await fetch('/network/storage-delete-ping', {cache:'no-store', signal:abort.signal});
        if (response.status !== 200 || await response.text() !== 'origin:storage-delete-ping')
          throw Error('ordinary origin response changed');
        if (state.times.length >= 30000) throw Error('network observation bound exceeded');
        state.times.push(Date.now()); ++state.successes;
      } catch (error) {
        ++state.failures;
        if (!state.firstError) state.firstError = String(error).slice(0, 512);
      } finally {
        clearTimeout(timer);
        if (ready) { ready(); ready = null; }
        if (state.running) setTimeout(step, 25); else state.stopped = true;
      }
    };
    step(); await started;
    return {started:true, successes:String(state.successes), failures:String(state.failures)};
  })())JS";
  Json value;
  uint64_t successes = 0, failures = 0;
  if (!quota::Evaluate(state, script, &value) || !value.Get(L"started", L't') ||
      !quota::Unsigned(value.Get(L"successes", L'"'), &successes) || successes == 0 ||
      !quota::Unsigned(value.Get(L"failures", L'"'), &failures) || failures != 0 || !server.Healthy() ||
      server.Count("/network/storage-network-control") != 1) {
    return state->Fail(L"ordinary-profile network control did not start with a real successful fetch");
  }
  Log(L"PASS", L"ordinary-network control started on unrelated origin with real no-store fetch");
  return true;
}

inline bool OrdinaryNetworkProgress(const std::shared_ptr<quota::State>& state,
                                    worker_response_probe::LoopbackServer& server,
                                    const wchar_t* phase, uint64_t began, uint64_t ended) {
  Json value;
  const std::wstring script = L"(()=>{const s=globalThis.__gesherOrdinaryNetwork;"
      L"if(!s)throw Error('missing network control');return {successes:String(s.successes),"
      L"failures:String(s.failures),during:String(s.times.filter(t=>t>=" + std::to_wstring(began) +
      L"&&t<=" + std::to_wstring(ended) + L").length),firstError:s.firstError}})()";
  uint64_t successes = 0, failures = 0, during = 0;
  if (!began || ended < began || !quota::Evaluate(state, script, &value) ||
      !quota::Unsigned(value.Get(L"successes", L'"'), &successes) ||
      !quota::Unsigned(value.Get(L"failures", L'"'), &failures) ||
      !quota::Unsigned(value.Get(L"during", L'"'), &during) || failures != 0 || during == 0 ||
      server.Count("/network/storage-delete-ping") < successes || !server.Healthy()) {
    const Json* error = value.Get(L"firstError", L'"');
    return state->Fail(L"ordinary networking failed or made no progress during production " +
        std::wstring(phase) + L" deletion: " + (error ? error->text : L"missing evidence"));
  }
  Log(L"PASS", L"ordinary-network phase=" + std::wstring(phase) + L" failures=0 completed-during=" +
      std::to_wstring(during) + L" independently-counted=true");
  return true;
}

inline bool StopOrdinaryNetwork(const std::shared_ptr<quota::State>& state,
                               worker_response_probe::LoopbackServer& server) {
  Json value;
  static const wchar_t* script = LR"JS((async () => {
    const state = globalThis.__gesherOrdinaryNetwork;
    if (!state) throw Error('missing network control');
    state.running = false;
    await new Promise(resolve => {
      const done = () => state.stopped ? resolve() : setTimeout(done, 10); done();
    });
    return {stopped:state.stopped, successes:String(state.successes), failures:String(state.failures)};
  })())JS";
  uint64_t successes = 0, failures = 0;
  if (!quota::Evaluate(state, script, &value) || !value.Get(L"stopped", L't') ||
      !quota::Unsigned(value.Get(L"successes", L'"'), &successes) ||
      !quota::Unsigned(value.Get(L"failures", L'"'), &failures) || failures != 0 ||
      server.Count("/network/storage-delete-ping") != successes || !server.Healthy()) {
    return state->Fail(L"ordinary network control failed or disagreed with the independent origin counter");
  }
  Log(L"PASS", L"ordinary-network stopped failures=0 successful-fetches=" + std::to_wstring(successes) +
      L" origin-requests=" + std::to_wstring(successes));
  return true;
}

// Close both same-profile observer controllers before waiting for their shared
// browser process. State::Close subsequently verifies the retained exit handle.
inline void ReleaseOrdinaryNetwork(const std::shared_ptr<quota::State>& state) {
  state->closed = true;
  if (state->subscribed && state->core) {
    state->Check(state->core->remove_WebResourceRequested(state->resources), L"remove ordinary network handler");
    state->subscribed = false;
  }
  if (state->controller) state->Check(state->controller->Close(), L"close ordinary network controller");
  state->core.Reset(); state->controller.Reset(); state->environment.Reset();
}

inline int Run(HWND parent) {
  // Five production calls (two queued duplicates, sentinel, HTTPS, HTTP), three
  // browser lifetimes and full payload readback have an independent process
  // budget. A failure cannot consume the original visual probe's deadline.
  const ULONGLONG end = GetTickCount64() + 660000, work_end = end - 40000;
  wchar_t temp[MAX_PATH]{}, id[40]{}; GUID guid{};
  DWORD length = GetTempPathW(MAX_PATH, temp);
  if (!length || length >= MAX_PATH || FAILED(CoCreateGuid(&guid)) || !StringFromGUID2(guid, id, 40)) {
    Log(L"FAIL", L"cannot create a fresh profile identity"); return 1;
  }
  std::wstring nonce(id);
  nonce.erase(std::remove_if(nonce.begin(), nonce.end(), [](wchar_t c) { return c == L'{' || c == L'}' || c == L'-'; }), nonce.end());
  for (wchar_t& c : nonce) if (c >= L'A' && c <= L'F') c += L'a' - L'A';
  const std::wstring profile = std::wstring(temp) + L"gesher-storage-delete-" + nonce;
  if (!CreateDirectoryW(profile.c_str(), nullptr)) { Log(L"FAIL", L"fresh profile creation failed"); return 1; }
  worker_response_probe::LoopbackServer server, ordinary_server;
  if (!server.Start(end) || !ordinary_server.Start(end)) { Log(L"FAIL", L"loopback isolation/control server failed to start"); return 1; }
  const std::wstring a = L"https://storage-a-" + nonce + L".invalid";
  const std::wstring b = L"https://storage-b-" + nonce + L".invalid";
  const std::wstring c = L"https://storage-c-" + nonce + L".invalid";
  const std::wstring d = L"https://storage-d-" + nonce + L".invalid";
  const std::wstring e = L"https://storage-e-" + nonce + L".invalid";
  const std::wstring subdomain = L"https://child.storage-a-" + nonce + L".invalid";
  const std::wstring port = a + L":8443";
  const std::wstring http = server.Origin(), https = L"https" + http.substr(4);
  const std::vector<std::wstring> origins{a, b, c, d, e, subdomain, port, http, https};
  std::vector<Case> cases{
    {{L"A-first-party", a, L""}, a, L""}, {{L"C-under-A", c, L""}, a, L""},
    {{L"B-first-party", b, L""}, b, L""}, {{L"C-under-B", c, L""}, b, L""},
    {{L"A-under-B", a, L""}, b, L""}, {{L"subdomain-first-party", subdomain, L""}, subdomain, L""},
    {{L"D-under-subdomain", d, L""}, subdomain, L""}, {{L"port-first-party", port, L""}, port, L""},
    {{L"E-under-port", e, L""}, port, L""}, {{L"HTTP-first-party", http, L""}, http, L""},
    {{L"C-under-HTTP", c, L""}, http, L""}, {{L"HTTPS-first-party", https, L""}, https, L""},
    {{L"D-under-HTTPS", d, L""}, https, L""}, {{L"A-through-B-under-A", a, L""}, a, b}};
  auto seed = std::make_shared<quota::State>(work_end);
  auto expanded = std::make_shared<quota::State>(work_end);
  auto inspect = std::make_shared<quota::State>(work_end);
  auto ordinary_network = std::make_shared<quota::State>(work_end);
  bool complete = false, host_started = false, host_joined = false;
  unsigned network_before = 0;
  const std::string sdk = GESHER_WEBVIEW2_PROBE_SDK_VERSION, loader = LoaderKind();
  Log(L"INFO", L"sdk=" + std::wstring(sdk.begin(), sdk.end()) + L" loader=" + std::wstring(loader.begin(), loader.end()) + L" profile=" + profile);
  do {
    if (!StartOrdinary(seed, parent, profile, origins) || !NetworkControl(seed, server, 1, L"before")) break;
    bool seeded = true;
    for (size_t i = 0; i < 5; ++i) if (!Seed(seed, &cases[i])) { seeded = false; break; }
    if (!seeded || !seed->Close(std::min(end, GetTickCount64() + 20000))) break;
    if (!StartOrdinary(expanded, parent, profile, origins) || expanded->profile_path != seed->profile_path) {
      if (!expanded->failed) expanded->Fail(L"persisted fixture reopened a different profile"); break;
    }
    if (!quota::InitializeIndexedDb(expanded) || !Measure(expanded, L"persisted-baseline", cases, 5)) break;
    if (expanded->served != 0) { expanded->Fail(L"persisted baseline revisited a fixture origin"); break; }
    Log(L"PASS", L"five persisted keys discovered after browser exit before origin revisit");
    for (size_t i = 5; i < 13; ++i) if (!Seed(expanded, &cases[i])) { seeded = false; break; }
    if (!seeded || !Measure(expanded, L"expanded-origins", cases, 13)) break;
    const std::vector<Case> before_nested(cases.begin(), cases.begin() + 13);
    if (!Seed(expanded, &cases[13]) || !CaptureNestedKey(expanded, before_nested, &cases[13]) ||
        !Measure(expanded, L"all-written", cases, cases.size()) ||
        !expanded->Close(std::min(end, GetTickCount64() + 20000))) break;
    // This unrelated ordinary network controller starts a fresh browser but
    // does not revisit storage origins or initialize IndexedDB. The production
    // Host receives no application Host, controller, frame ID or inventory.
    if (!StartOrdinary(ordinary_network, parent, profile, {}) || ordinary_network->profile_path != seed->profile_path ||
        !StartOrdinaryNetwork(ordinary_network, ordinary_server)) {
      if (!ordinary_network->failed) ordinary_network->Fail(L"ordinary network control used a different application profile"); break;
    }
    network_before = server.Connections();
    Host::Configure(profile, L"");
    Host::SetGlobalCallback(Completed);
    host_started = true;
    const uint64_t a_began = UtcMillis();
    if (!Host::ClearOriginData(a, 101) || !Host::ClearOriginData(a, 102)) {
      inspect->Fail(L"production Host rejected a valid origin before admission"); break;
    }
    if (!Completions(inspect, {101, 102}, true)) break;
    if (!OrdinaryNetworkProgress(ordinary_network, ordinary_server, L"A", a_began, CompletionTime(102))) break;
    if (!StartOrdinary(inspect, parent, profile, origins) || inspect->profile_path != seed->profile_path ||
        inspect->browser_pid != ordinary_network->browser_pid) {
      if (!inspect->failed) inspect->Fail(L"production result observer opened a different profile"); break;
    }
    if (!quota::InitializeIndexedDb(inspect)) break;
    for (size_t i : {size_t(0), size_t(1), size_t(6), size_t(8), size_t(13)}) cases[i].removed = true;
    if (!Measure(inspect, L"after-A", cases, cases.size())) break;
    if (inspect->served != 0) { inspect->Fail(L"ordinary origin was revisited before production result inventory"); break; }
    Log(L"PASS", L"production discovered and deleted persisted partitions before ordinary origin revisit");
    const uint64_t sentinel_began = UtcMillis();
    if (!Host::ClearOriginData(L"https://gesher-storage-resolver.invalid", 103)) {
      inspect->Fail(L"production Host rejected its first sentinel as a legitimate requested origin"); break;
    }
    if (!Completions(inspect, {101, 102, 103}, true) ||
        !OrdinaryNetworkProgress(ordinary_network, ordinary_server, L"sentinel", sentinel_began, CompletionTime(103)) ||
        !Measure(inspect, L"sentinel-preserved", cases, cases.size())) break;
    const uint64_t https_began = UtcMillis();
    if (!Host::ClearOriginData(https, 104)) { inspect->Fail(L"production Host rejected HTTPS loopback origin"); break; }
    if (!Completions(inspect, {101, 102, 103, 104}, true)) break;
    if (!OrdinaryNetworkProgress(ordinary_network, ordinary_server, L"HTTPS", https_began, CompletionTime(104))) break;
    cases[11].removed = cases[12].removed = true;
    if (!Measure(inspect, L"after-HTTPS", cases, cases.size()) ||
        !Content(inspect, cases[9], L"read") || !Content(inspect, cases[10], L"read")) break;
    if (!quota::Navigate(inspect, L"edge://quota-internals/", true)) break;
    const uint64_t http_began = UtcMillis();
    if (!Host::ClearOriginData(http, 105)) { inspect->Fail(L"production Host rejected HTTP loopback origin"); break; }
    // Admission closes immediately, while the last real deletion is pending.
    // The Host must drain it, deliver its callback and release its owner STA.
    if (CompletionTime(105) != 0) { inspect->Fail(L"pending deletion was not observed before shutdown admission closed"); break; }
    const ULONGLONG shutdown_end = std::min(work_end, GetTickCount64() + 130000);
    for (;;) {
      const ULONGLONG now = GetTickCount64();
      if (now >= shutdown_end) break;
      const int shutdown = Host::ShutdownIdle(static_cast<uint32_t>(std::min<ULONGLONG>(1000, shutdown_end - now)));
      if (shutdown == 1) { host_joined = true; break; }
      if (shutdown != 2) break;
      // These ordinary observers belong to this STA. Keep their COM pump live
      // while the independent production STA drains and joins.
      PumpUntil(std::min(shutdown_end, GetTickCount64() + 10), [] { return false; });
    }
    if (!host_joined || !Completions(inspect, {101, 102, 103, 104, 105}, false)) {
      inspect->Fail(L"production Host did not drain accepted deletion and join cleanly"); break;
    }
    if (!OrdinaryNetworkProgress(ordinary_network, ordinary_server, L"HTTP", http_began, CompletionTime(105))) break;
    if (Host::ClearOriginData(a, 106)) { inspect->Fail(L"production Host accepted work after shutdown"); break; }
    Log(L"PASS", L"shutdown drained accepted deletion pending-observed=true joined=true later-admission=rejected");
    cases[9].removed = cases[10].removed = true;
    if (!Measure(inspect, L"after-HTTP", cases, cases.size())) break;
    bool content = true;
    for (const auto& item : cases) {
      if (!Content(inspect, item, item.removed ? L"absent" : L"read")) { content = false; break; }
    }
    if (!content || !Completions(inspect, {101, 102, 103, 104, 105}, false)) break;
    if (!server.Healthy() || server.Connections() != network_before) {
      inspect->Fail(L"production resolver or observer escaped the owned resource isolation"); break;
    }
    Log(L"PASS", L"resolver-origin-network-connections=0 independent-positive-control=true");
    // The normal application profile must still reach its real origin after
    // every InPrivate resolver is closed and the production STA has joined.
    if (!NetworkControl(inspect, server, 2, L"after")) break;
    if (!StopOrdinaryNetwork(ordinary_network, ordinary_server)) break;
    complete = true;
  } while (false);
  if (host_started && !host_joined) {
    for (;;) {
      const ULONGLONG now = GetTickCount64();
      if (now >= end) break;
      const int result = Host::ShutdownIdle(static_cast<uint32_t>(std::min<ULONGLONG>(1000, end - now)));
      if (result == 1) { host_joined = true; break; }
      if (result != 2) break;
      PumpUntil(std::min(end, GetTickCount64() + 10), [] { return false; });
    }
    if (!host_joined) inspect->Fail(L"production Host cleanup did not join");
  }
  seed->Close(end);
  expanded->Close(end);
  ReleaseOrdinaryNetwork(ordinary_network);
  inspect->Close(end);
  ordinary_network->Close(end);
  if (!server.Healthy()) inspect->Fail(L"loopback sentinel failed before browser exit");
  if (!server.Stop()) inspect->Fail(L"loopback sentinel did not stop");
  if (!ordinary_server.Stop()) ordinary_network->Fail(L"ordinary network origin did not stop");
  if (!complete || seed->failed || expanded->failed || inspect->failed ||
      ordinary_network->failed || seed->unavailable || expanded->unavailable || inspect->unavailable ||
      ordinary_network->unavailable) {
    Log(L"FAIL", L"actual production deletion qualification failed; no support inferred from an unavailable route");
    return 1;
  }
  Log(L"PASS", L"QUALIFIED production=Host::ClearOriginData persisted_baseline=5 full_keys=14 deleted=9 preserved=5 localStorage=14 cookies=6 fileSystems=2 callbacks=5 browser_exit=true");
  return 0;
}
}  // namespace storage_delete_probe
