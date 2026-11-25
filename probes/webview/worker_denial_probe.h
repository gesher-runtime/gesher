// Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0
// Test-only scope qualification of the measured deferred URI denial candidate.
#pragma once
#include <array>
#include <optional>

namespace worker_denial_probe {
using namespace gesher::webview;
using browser_session_probe::Json;
using browser_session_probe::JsonReader;
using browser_session_probe::PumpUntil;
using worker_response_probe::LoopbackServer;
using worker_response_probe::RequestRecord;

constexpr const wchar_t* kNames[] = {L"post-pass", L"post-deny", L"cors-pass", L"cors-deny", L"redirect-pass",
    L"redirect-deny", L"parallel-allow", L"parallel-deny", L"sequential-pass", L"teardown-deny", L"teardown-followup"};
constexpr size_t kCount = 11;
static_assert(std::size(kNames) == kCount);
inline void Hr(const wchar_t* name, HRESULT hr) {
  wprintf(L"INFO  worker-denial: %ls hr=0x%08lx\n", name, static_cast<unsigned long>(hr)); fflush(stdout);
}
inline bool Text(const Json& value, const wchar_t* key, const std::wstring& expected) {
  const Json* field = value.Get(key, L'"'); return field && field->text == expected;
}
inline bool Integer(const Json& value, const wchar_t* key, uint64_t* out) {
  const Json* field = value.Get(key, L'#');
  if (!field || field->text.empty() || field->text.size() > 7) return false;
  *out = 0;
  for (wchar_t c : field->text) { if (c < L'0' || c > L'9') return false; *out = *out * 10 + c - L'0'; }
  return true;
}
inline std::string Narrow(const std::wstring& value) { return {value.begin(), value.end()}; }
inline unsigned Count(const std::vector<RequestRecord>& rows, const std::string& path, const std::string& method = {}) {
  unsigned result = 0;
  for (const auto& row : rows) if (row.path == path && (method.empty() || row.method == method)) ++result;
  return result;
}
inline std::string Payload(const std::wstring& name, const std::wstring& identity) {
  std::string result = Narrow(name + L"|" + identity + L"|"); result.resize(32768, 'P'); return result;
}

struct View {
  Com<ICoreWebView2Controller> controller;
  Com<ICoreWebView2> core;
  Com<ICoreWebView2_18> external;
  Com<ICoreWebView2_22> filter;
  EventRegistrationToken message_token{}, request_token{}, external_token{};
  bool done = false, closed = false, messages = false, resources = false, guarded = false;
  unsigned filters = 0;
};
struct Pending {
  Com<ICoreWebView2WebResourceRequestedEventArgs> args;
  Com<ICoreWebView2WebResourceRequest> request;
  Com<ICoreWebView2Deferral> deferral;
  std::wstring name, original;
  enum Decision { kUndecided, kAllowed, kDenied } decision = kUndecided;
  bool completed = false;
};
struct State {
  Com<ICoreWebView2Environment> environment;
  View owner, observer;
  std::wstring origin, other, denied_uri, page_url, observer_url, identity;
  std::array<unsigned, kCount> intercepted{};
  std::array<bool, kCount> rejected{}, request_identity{}, timing_identity{};
  std::vector<Pending> pending, parallel;
  std::optional<Pending> late;
  unsigned observations = 0, external_launches = 0, late_rejections = 0, restore_calls = 0;
  unsigned parallel_completed = 0, denied = 0;
  bool environment_done = false, closed = false, failed = false, main_ready = false, observer_ready = false;
  bool main_complete = false, observer_navigated = false, complete = false, in_request = false;
  bool redirect_target_seen = false, terminal_before_close = false;

  void Failure(const wchar_t* why) { failed = true; wprintf(L"FAIL  worker-denial: %ls\n", why); fflush(stdout); }
  std::wstring Url(unsigned i) const {
    return (i == 2 || i == 3 ? other : origin) + L"/denial/network/" +
           (i >= 6 && i <= 8 ? L"concurrent" : kNames[i]);
  }
  void Message(const Json& value, bool from_observer) {
    if (Text(value, L"event", L"matrix-ready")) {
      bool& ready = from_observer ? observer_ready : main_ready;
      if (ready || !value.Get(L"secure", L't') || !value.Get(L"observer", from_observer ? L't' : L'f') ||
          !Text(value, L"script", origin + L"/denial/sw.js") || !Text(value, L"scope", origin + L"/denial/"))
        Failure(L"worker registration or observer identity mismatch");
      ready = true; return;
    }
    if (Text(value, L"event", L"matrix-main-complete")) {
      if (from_observer || main_complete || observations != 9) Failure(L"main cases incomplete or duplicated");
      main_complete = true; return;
    }
    if (Text(value, L"event", L"matrix-complete")) {
      if (!from_observer || complete || observations != kCount || !value.Get(L"unregistered", L't'))
        Failure(L"observer cases or unregister incomplete");
      complete = true; return;
    }
    if (!Text(value, L"event", L"matrix-case") || observations >= kCount ||
        !Text(value, L"name", kNames[observations]) || from_observer != (observations >= 9) ||
        !(from_observer ? observer_ready : main_ready)) { Failure(L"unexpected or unordered worker result"); return; }
    const unsigned i = observations++;
    const Json* id = value.Get(L"workerIdentity", L'"');
    const Json* timings = value.Get(L"timings", L'[');
    uint64_t status = 0, bytes = 0, upload = 0;
    if (!id || id->text.empty() || id->text.size() > 64 || !timings || timings->items.size() > 16 ||
        !Integer(value, L"status", &status) || status > 599 || !Integer(value, L"bytes", &bytes) || bytes > 262144 ||
        !Integer(value, L"uploadBytes", &upload) || !value.Get(L"timedOut", L'f') || !value.Get(L"aborted", L'f') ||
        !value.Get(L"errorName", L'"') || !value.Get(L"error", L'"') || !value.Get(L"text", L'"') ||
        !value.Get(L"requestUrl", L'"') || !value.Get(L"finalRequestUrl", L'"') ||
        !value.Get(L"method", L'"') || !value.Get(L"finalMethod", L'"') ||
        !value.Get(L"responseUrl", L'"') || !Text(value, L"workerScript", origin + L"/denial/sw.js")) {
      Failure(L"malformed, timed-out or aborted worker result"); return;
    }
    if (i == 0) identity = id->text;
    if (id->text != identity) { Failure(L"worker instance changed during the matrix"); return; }
    const std::wstring method = i == 4 || i == 5 || i == 10 ? L"GET" : L"POST";
    if (upload != (method == L"POST" ? 32768u : 0u)) Failure(L"upload fixture was not constructed completely");
    request_identity[i] = Text(value, L"requestUrl", Url(i)) && Text(value, L"finalRequestUrl", Url(i)) &&
                          Text(value, L"method", method) && Text(value, L"finalMethod", method);
    timing_identity[i] = timings->items.size() == (i == 6 || i == 7 ? 2u : 1u);
    for (const Json& timing : timings->items) {
      if (!timing.Get(L"name", L'"') || !timing.Get(L"type", L'"')) Failure(L"malformed timing entry");
      timing_identity[i] = timing_identity[i] && Text(timing, L"name", Url(i)) && Text(timing, L"type", L"fetch");
    }
    const bool control = i == 0 || i == 2 || i == 4 || i == 6 || i == 8 || i == 10;
    if (control) {
      const std::wstring suffix = i == 4 ? L"redirect-pass-target" : i == 6 || i == 8 ? L"concurrent" : kNames[i];
      if (!Text(value, L"fetch", L"resolved") || !Text(value, L"body", L"complete") || status != 200 ||
          !Text(value, L"text", L"origin:" + suffix) || bytes != 7 + suffix.size() || !Text(value, L"originHeader", L"origin") ||
          !Text(value, L"errorName", L"") || !Text(value, L"error", L"") ||
          !Text(value, L"responseUrl", i == 4 ? other + L"/denial/network/redirect-pass-target" : Url(i)) ||
          !value.Get(L"redirected", i == 4 ? L't' : L'f') || !request_identity[i] || !timing_identity[i])
        Failure(L"ordinary POST, CORS, redirect, concurrent or follow-up control failed");
    } else if (Text(value, L"fetch", L"rejected")) {
      if (status || bytes || !Text(value, L"body", L"none") || !Text(value, L"responseUrl", L"") ||
          !Text(value, L"text", L"") || Text(value, L"error", L"")) Failure(L"inconsistent initial rejection");
      rejected[i] = Text(value, L"errorName", L"TypeError");
    } else if (!Text(value, L"fetch", L"resolved") ||
               (!Text(value, L"body", L"complete") && !Text(value, L"body", L"rejected")))
      Failure(L"unrecognized denial observation");
  }

  HRESULT Request(ICoreWebView2WebResourceRequestedEventArgs* args) {
    Com<ICoreWebView2WebResourceRequestedEventArgs2> source;
    Com<ICoreWebView2WebResourceRequest> request;
    CoString uri, method;
    COREWEBVIEW2_WEB_RESOURCE_REQUEST_SOURCE_KINDS kind{};
    if (!args || FAILED(args->QueryInterface(IID_ICoreWebView2WebResourceRequestedEventArgs2,
        reinterpret_cast<void**>(source.Put()))) || FAILED(source->get_RequestedSourceKind(&kind)) ||
        kind != COREWEBVIEW2_WEB_RESOURCE_REQUEST_SOURCE_KINDS_SERVICE_WORKER ||
        FAILED(args->get_Request(request.Put())) || !request || FAILED(request->get_Uri(uri.Put())) ||
        FAILED(request->get_Method(method.Put()))) { Failure(L"request source or fields unavailable"); return S_OK; }
    const std::wstring url = uri.str(), verb = method.str();
    unsigned i = kCount;
    for (unsigned n = 0; n < kCount; ++n) if (url == Url(n)) { i = n; break; }
    const bool target_pass = url == other + L"/denial/network/redirect-pass-target";
    const bool target_deny = url == other + L"/denial/network/redirect-deny-target";
    if (target_pass) i = 4;
    if (target_deny) { i = 5; redirect_target_seen = true; }
    if (i == 6) {
      Com<ICoreWebView2HttpRequestHeaders> headers; CoString tag;
      if (FAILED(request->get_Headers(headers.Put())) || !headers ||
          FAILED(headers->GetHeader(L"X-Gesher-Diagnostic", tag.Put()))) { Failure(L"concurrent request tag missing"); return S_OK; }
      for (unsigned n = 6; n <= 8; ++n) if (tag.str() == kNames[n]) i = n;
      if (tag.str() != kNames[i]) { Failure(L"concurrent request tag invalid"); return S_OK; }
    }
    wprintf(L"INFO  worker-denial: request uri=%ls method=%ls source=SERVICE_WORKER\n", url.c_str(), verb.c_str());
    if (i == kCount) { Failure(L"unrecognized intercepted URL"); return S_OK; }
    // Observe preflights without denying or replacing them. Independent server
    // OPTIONS counters decide whether a denied CORS request leaked a preflight.
    if (verb == L"OPTIONS" && (i == 2 || i == 3)) return S_OK;
    if (verb != (i == 4 || i == 5 || i == 10 ? L"GET" : L"POST")) { Failure(L"request method changed"); return S_OK; }
    ++intercepted[i];
    const bool block = i == 1 || i == 3 || (i == 5 && target_deny) || i == 7 || i == 9;
    if (!block && i != 6) return S_OK;
    Com<ICoreWebView2WebResourceResponse> response;
    if (FAILED(args->get_Response(response.Put())) || response) { Failure(L"request already has a replacement response"); return S_OK; }
    Pending item; item.name = kNames[i]; item.original = url;
    item.args = Com<ICoreWebView2WebResourceRequestedEventArgs>(args); item.request = request;
    HRESULT hr = args->GetDeferral(item.deferral.Put()); Hr(L"GetDeferral", hr);
    if (FAILED(hr) || !item.deferral) { Failure(L"request deferral failed"); return S_OK; }
    if (i == 6 || i == 7) {
      if (parallel.size() >= 2) { Failure(L"duplicate concurrent deferral"); return S_OK; }
      parallel.push_back(std::move(item));
    } else pending.push_back(std::move(item));
    return S_OK;
  }
  bool Deny(Pending& item) {
    CoString original, rewritten;
    if (in_request || item.completed || item.decision != Pending::kUndecided ||
        FAILED(item.request->get_Uri(original.Put())) || original.str() != item.original) {
      Failure(L"terminal denial lost request ownership or STA ordering"); return false;
    }
    HRESULT hr = item.request->put_Uri(denied_uri.c_str()); Hr(L"terminal put_Uri", hr);
    if (FAILED(hr) || FAILED(item.request->get_Uri(rewritten.Put())) || rewritten.str() != denied_uri) {
      Failure(L"non-network denial mutation failed"); return false;
    }
    item.decision = Pending::kDenied; ++denied;
    wprintf(L"INFO  worker-denial: terminal-denial case=%ls original=%ls rewritten=%ls\n",
            item.name.c_str(), item.original.c_str(), rewritten.str().c_str());
    return true;
  }
  bool Allow(Pending& item, bool restore_original = false) {
    if (item.completed || item.decision != Pending::kUndecided) {
      ++late_rejections;
      wprintf(L"INFO  worker-denial: late-allow rejected case=%ls terminal=denied original-restores=%u\n",
              item.name.c_str(), restore_calls); return false;
    }
    if (restore_original) {
      ++restore_calls;
      HRESULT hr = item.request->put_Uri(item.original.c_str()); Hr(L"late callback original URI restore", hr);
      if (FAILED(hr)) { Failure(L"late callback attempted an original URI restore"); return false; }
    }
    // An undecided ordinary request retains its original URI. Denied requests
    // never reach an API that could restore that URI or clear a denial response.
    item.decision = Pending::kAllowed; return true;
  }
  void Complete(Pending& item, bool after_close = false) {
    if (item.completed || item.decision == Pending::kUndecided) { Failure(L"duplicate or undecided completion"); return; }
    item.completed = true;
    HRESULT hr = item.deferral->Complete(); Hr(after_close ? L"Complete after owner closure" : L"Complete", hr);
    if (FAILED(hr) && !(after_close && hr == HRESULT_FROM_WIN32(ERROR_INVALID_STATE))) Failure(L"deferral completion failed");
  }
  void CloseView(View& view) {
    if (view.closed) return;
    view.closed = true;
    for (unsigned i = 0; i < view.filters; ++i) {
      HRESULT hr = view.filter->RemoveWebResourceRequestedFilterWithRequestSourceKinds(
          ((i == 0 ? origin : other) + L"/denial/network/*").c_str(), COREWEBVIEW2_WEB_RESOURCE_CONTEXT_ALL,
          COREWEBVIEW2_WEB_RESOURCE_REQUEST_SOURCE_KINDS_SERVICE_WORKER);
      Hr(L"remove source filter", hr); if (FAILED(hr)) Failure(L"source filter removal failed");
    }
    if (view.resources) { HRESULT hr = view.core->remove_WebResourceRequested(view.request_token); Hr(L"remove request callback", hr); if (FAILED(hr)) Failure(L"request callback removal failed"); }
    if (view.messages) { HRESULT hr = view.core->remove_WebMessageReceived(view.message_token); Hr(L"remove message callback", hr); if (FAILED(hr)) Failure(L"message callback removal failed"); }
    if (view.controller) { HRESULT hr = view.controller->Close(); Hr(L"controller.Close", hr); if (FAILED(hr)) Failure(L"controller close failed"); }
    if (view.guarded) { HRESULT hr = view.external->remove_LaunchingExternalUriScheme(view.external_token); Hr(L"remove external guard", hr); if (FAILED(hr) && hr != HRESULT_FROM_WIN32(ERROR_INVALID_STATE)) Failure(L"external guard removal failed"); }
  }
  void Drain() {
    if (closed || failed || in_request) return;
    if (late) {
      if (Allow(*late, true)) Failure(L"late callback restored a terminal denial");
      late.reset();
    }
    if (parallel.size() == 2) {
      for (auto it = parallel.rbegin(); it != parallel.rend(); ++it) {
        wprintf(L"INFO  worker-denial: parallel reverse-complete case=%ls\n", it->name.c_str());
        if (it->name == L"parallel-deny" ? Deny(*it) : Allow(*it)) { Complete(*it); ++parallel_completed; }
        if (failed) return;
      }
      parallel.clear();
    }
    for (auto& item : pending) {
      if (!Deny(item)) return;
      if (item.name == L"teardown-deny") {
        // The terminal decision is applied before owner closure. The observer
        // and the same-profile worker remain alive to report the real outcome.
        HRESULT hr = item.request->put_Content(nullptr); Hr(L"clear upload before owner closure", hr);
        if (FAILED(hr)) { Failure(L"pending upload scrub failed"); return; }
        terminal_before_close = item.decision == Pending::kDenied && !owner.closed;
        CloseView(owner);
        Complete(item, true);
        late = std::move(item);  // A later STA turn tries, and must reject, allow.
      } else Complete(item);
      if (failed) return;
    }
    pending.clear();
    if (main_complete && !observer_navigated && !failed) {
      observer_navigated = true;
      HRESULT hr = observer.core->Navigate(observer_url.c_str()); Hr(L"observer Navigate", hr);
      if (FAILED(hr)) Failure(L"observer navigation failed");
    }
  }
  void Close() {
    closed = true;
    // Never intentionally release an undecided pending upload during cleanup.
    for (auto& item : pending) if (item.decision == Pending::kUndecided) Deny(item);
    for (auto& item : parallel) if (item.decision == Pending::kUndecided) Deny(item);
    CloseView(owner); CloseView(observer);
    pending.clear(); parallel.clear(); late.reset();
    owner = {}; observer = {}; environment.Reset();
  }
};

inline bool MakeView(const std::shared_ptr<State>& state, View& view, HWND parent, ULONGLONG deadline, bool observer) {
  HRESULT hr = state->environment->CreateCoreWebView2Controller(parent,
      GESHER_CALLBACK(ICoreWebView2CreateCoreWebView2ControllerCompletedHandler,
        [state, &view](HRESULT result, ICoreWebView2Controller* controller) -> HRESULT {
          if (state->closed) { if (controller) controller->Close(); return S_OK; }
          Hr(L"CreateController completion", result);
          if (SUCCEEDED(result) && controller) view.controller = Com<ICoreWebView2Controller>(controller);
          view.done = true; return S_OK;
        }).get());
  Hr(L"CreateController immediate", hr);
  if (FAILED(hr) || !PumpUntil(deadline, [&] { return view.done; }) || !view.controller ||
      FAILED(view.controller->put_IsVisible(FALSE)) || FAILED(view.controller->get_CoreWebView2(view.core.Put())) || !view.core ||
      !view.core.As(IID_ICoreWebView2_18, &view.external)) { state->Failure(L"controller or external guard unavailable"); return false; }
  std::weak_ptr<State> weak = state;
  hr = view.external->add_LaunchingExternalUriScheme(
      GESHER_CALLBACK(ICoreWebView2LaunchingExternalUriSchemeEventHandler,
        [weak](ICoreWebView2*, ICoreWebView2LaunchingExternalUriSchemeEventArgs* args) -> HRESULT {
          HRESULT hr = args ? args->put_Cancel(TRUE) : E_POINTER; Hr(L"external launch cancellation", hr);
          if (auto state = weak.lock()) { ++state->external_launches; state->Failure(L"external launch attempted; denial not qualified"); }
          return hr;
        }).get(), &view.external_token);
  if (FAILED(hr)) { state->Failure(L"external guard subscription failed"); return false; }
  view.guarded = true;
  hr = view.core->add_WebMessageReceived(
      GESHER_CALLBACK(ICoreWebView2WebMessageReceivedEventHandler,
        [weak, observer](ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT {
          auto state = weak.lock(); if (!state || state->closed || state->failed) return S_OK;
          CoString source; LPWSTR raw = nullptr;
          HRESULT hr = args ? args->get_Source(source.Put()) : E_POINTER;
          if (SUCCEEDED(hr)) hr = args->get_WebMessageAsJson(&raw);
          size_t length = browser_session_probe::JsonLength(raw);
          wprintf(L"INFO  worker-denial: message observer=%d hr=0x%08lx json=%.*ls\n", observer,
                  static_cast<unsigned long>(hr), static_cast<int>(std::min<size_t>(length, 8192)), raw ? raw : L"");
          Json value;
          if (FAILED(hr) || !raw || length > 8192 || source.str() != (observer ? state->observer_url : state->page_url) ||
              !JsonReader(std::wstring_view(raw, length)).Read(&value)) state->Failure(L"page source or result JSON invalid");
          else state->Message(value, observer);
          CoTaskMemFree(raw); return S_OK;
        }).get(), &view.message_token);
  if (FAILED(hr)) { state->Failure(L"message subscription failed"); return false; }
  view.messages = true;
  if (observer) return true;
  if (!view.core.As(IID_ICoreWebView2_22, &view.filter)) { state->Failure(L"source filter unavailable"); return false; }
  hr = view.core->add_WebResourceRequested(
      GESHER_CALLBACK(ICoreWebView2WebResourceRequestedEventHandler,
        [weak](ICoreWebView2*, ICoreWebView2WebResourceRequestedEventArgs* args) -> HRESULT {
          auto state = weak.lock(); if (!state || state->closed || state->failed) return S_OK;
          state->in_request = true; HRESULT hr = state->Request(args); state->in_request = false; return hr;
        }).get(), &view.request_token);
  if (FAILED(hr)) { state->Failure(L"request subscription failed"); return false; }
  view.resources = true;
  for (const auto& origin : {state->origin, state->other}) {
    hr = view.filter->AddWebResourceRequestedFilterWithRequestSourceKinds((origin + L"/denial/network/*").c_str(),
        COREWEBVIEW2_WEB_RESOURCE_CONTEXT_ALL, COREWEBVIEW2_WEB_RESOURCE_REQUEST_SOURCE_KINDS_SERVICE_WORKER);
    if (FAILED(hr)) { state->Failure(L"source filter admission failed"); return false; }
    ++view.filters;
  }
  return true;
}

inline int Run(HWND parent, ULONGLONG deadline) {
  const ULONGLONG work_deadline = deadline - 6000;
  auto state = std::make_shared<State>(); LoopbackServer first, second;
  wchar_t temp[MAX_PATH]{}, identity[40]{}; GUID guid{};
  DWORD length = GetTempPathW(MAX_PATH, temp);
  if (!parent || !length || length >= MAX_PATH || FAILED(CoCreateGuid(&guid)) || !StringFromGUID2(guid, identity, 40) ||
      !first.Start(deadline) || !second.Start(deadline) || !first.SetRedirectOrigin(second.Origin())) {
    state->Failure(L"fresh profile or two loopback origins could not be created"); return 1;
  }
  const std::wstring profile = std::wstring(temp) + L"gesher-worker-denial-scope-" + identity;
  if (!CreateDirectoryW(profile.c_str(), nullptr)) { state->Failure(L"fresh profile creation failed"); return 1; }
  std::wstring scheme = L"gesher-denial-";
  for (wchar_t c : std::wstring(identity)) {
    if (c >= L'0' && c <= L'9') scheme += c;
    else if (c >= L'A' && c <= L'F') scheme += c - L'A' + L'a';
    else if (c >= L'a' && c <= L'f') scheme += c;
  }
  for (REGSAM view : {KEY_WOW64_32KEY, KEY_WOW64_64KEY}) {
    HKEY key = nullptr; LSTATUS status = RegOpenKeyExW(HKEY_CLASSES_ROOT, scheme.c_str(), 0, KEY_READ | view, &key);
    if (key) RegCloseKey(key);
    if (scheme.size() != 46 || (status != ERROR_FILE_NOT_FOUND && status != ERROR_PATH_NOT_FOUND)) {
      state->Failure(L"fresh denial scheme registration check failed"); return 1;
    }
  }
  state->denied_uri = scheme + L":blocked"; state->origin = first.Origin(); state->other = second.Origin();
  state->page_url = state->origin + L"/denial/index.html?other=" + state->other;
  state->observer_url = state->origin + L"/denial/observer.html?other=" + state->other;
  wprintf(L"INFO  worker-denial: fresh-profile=%ls origin=%ls other=%ls\n", profile.c_str(), state->origin.c_str(), state->other.c_str());
  do {
    HRESULT hr = CreateEnvironment(nullptr, profile.c_str(), nullptr,
        GESHER_CALLBACK(ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler,
          [state](HRESULT result, ICoreWebView2Environment* environment) -> HRESULT {
            Hr(L"CreateEnvironment completion", result);
            if (!state->closed) { if (SUCCEEDED(result) && environment) state->environment = Com<ICoreWebView2Environment>(environment); state->environment_done = true; }
            return S_OK;
          }).get());
    if (FAILED(hr) || !PumpUntil(work_deadline, [&] { return state->environment_done; }) || !state->environment) {
      state->Failure(L"environment setup failed"); break;
    }
    Com<ICoreWebView2Environment7> env7; CoString actual, runtime;
    if (!state->environment.As(IID_ICoreWebView2Environment7, &env7) || FAILED(env7->get_UserDataFolder(actual.Put())) ||
        CompareStringOrdinal(profile.c_str(), -1, actual.str().c_str(), -1, TRUE) != CSTR_EQUAL ||
        FAILED(state->environment->get_BrowserVersionString(runtime.Put()))) { state->Failure(L"profile/runtime verification failed"); break; }
    wprintf(L"INFO  worker-denial: runtime=%ls sdk=%hs loader=%hs\n", runtime.str().c_str(), GESHER_WEBVIEW2_PROBE_SDK_VERSION, LoaderKind());
    if (!MakeView(state, state->owner, parent, work_deadline, false) || !MakeView(state, state->observer, parent, work_deadline, true)) break;
    hr = state->owner.core->Navigate(state->page_url.c_str()); Hr(L"owner Navigate", hr);
    if (FAILED(hr) || !PumpUntil(work_deadline, [&] { state->Drain(); return state->failed || state->complete; }))
      state->Failure(L"matrix did not finish within the shared diagnostic budget");
  } while (false);
  state->Close();
  PumpUntil(std::min(deadline - 5000, GetTickCount64() + 250), [] { return false; });
  if (!first.Stop() || !second.Stop() || !first.Healthy() || !second.Healthy()) state->Failure(L"loopback origin failed or did not stop");
  std::vector<RequestRecord> a, b;
  for (const auto& row : first.Requests()) if (row.path.starts_with("/denial/network/")) a.push_back(row);
  for (const auto& row : second.Requests()) if (row.path.starts_with("/denial/network/")) b.push_back(row);
  for (unsigned side = 0; side < 2; ++side) for (const auto& row : side == 0 ? a : b)
    wprintf(L"INFO  worker-denial: origin=%lc method=%hs path=%hs body-bytes=%llu complete=%ls\n", side == 0 ? L'A' : L'B',
            row.method.c_str(), row.path.c_str(), static_cast<unsigned long long>(row.body.size()), row.complete ? L"yes" : L"no");
  const unsigned page_requests = first.Count(Narrow(L"/denial/index.html?other=" + state->other));
  const unsigned observer_requests = first.Count(Narrow(L"/denial/observer.html?other=" + state->other));
  const unsigned script_requests = first.Count("/denial/sw.js");
  const unsigned known_first = static_cast<unsigned>(a.size()) + page_requests + observer_requests + script_requests + first.Count("/favicon.ico");
  unsigned unexpected = first.TotalCount() - known_first + second.TotalCount() - static_cast<unsigned>(b.size());
  for (unsigned side = 0; side < 2; ++side) for (const auto& row : side == 0 ? a : b) {
    const std::vector<std::string> names = side == 0 ?
        std::vector<std::string>{"post-pass", "post-deny", "redirect-pass", "redirect-deny", "concurrent", "teardown-deny", "teardown-followup"} :
        std::vector<std::string>{"cors-pass", "cors-deny", "redirect-pass-target", "redirect-deny-target"};
    bool known = false;
    for (const auto& name : names) known = known || row.path == "/denial/network/" + name;
    if (!known) ++unexpected;
  }
  wprintf(L"INFO  worker-denial: unknown-origin-requests=%u origin-a-connections=%u origin-b-connections=%u\n",
          unexpected, first.Connections(), second.Connections());
  wprintf(L"INFO  worker-denial: received-http-bytes-a=%llu received-http-bytes-b=%llu unattributed-http-bytes=%llu\n",
          static_cast<unsigned long long>(first.ReceivedBytes()), static_cast<unsigned long long>(second.ReceivedBytes()),
          static_cast<unsigned long long>(first.UnattributedBytes() + second.UnattributedBytes()));
  if (unexpected) state->Failure(L"independent origin received an unexpected alternate request");
  if (!state->failed) {
    const auto path = [](const char* name) { return std::string("/denial/network/") + name; };
    const auto payloads = [&](const std::vector<RequestRecord>& rows, const char* route, const std::vector<std::wstring>& names) {
      std::vector<std::string> actual, expected;
      for (const auto& row : rows) if (row.path == path(route) && row.method == "POST") {
        if (!row.complete) return false;
        actual.push_back(row.body);
      }
      for (const auto& name : names) expected.push_back(Payload(name, state->identity));
      std::sort(actual.begin(), actual.end()); std::sort(expected.begin(), expected.end()); return actual == expected;
    };
    const auto concurrent_count = [&](const std::wstring& name) {
      unsigned count = 0;
      for (const auto& row : a) if (row.path == path("concurrent") && row.method == "POST" &&
          row.complete && row.body == Payload(name, state->identity)) ++count;
      return count;
    };
    const unsigned concurrent_denied = Count(a, path("concurrent")) - concurrent_count(L"parallel-allow") - concurrent_count(L"sequential-pass");
    if (!state->complete || page_requests != 1 || observer_requests != 1 || !script_requests ||
        !payloads(a, "post-pass", {L"post-pass"}) || !payloads(b, "cors-pass", {L"cors-pass"}) ||
        Count(b, path("cors-pass"), "OPTIONS") != 1 || Count(a, path("redirect-pass"), "GET") != 1 ||
        Count(b, path("redirect-pass-target"), "GET") != 1 || Count(a, path("redirect-deny"), "GET") != 1 ||
        concurrent_count(L"parallel-allow") != 1 || concurrent_count(L"sequential-pass") != 1 ||
        Count(a, path("teardown-followup"), "GET") != 1 || state->parallel_completed != 2 ||
        state->external_launches || !state->terminal_before_close || state->late_rejections != 1 || state->restore_calls ||
        state->denied != (state->redirect_target_seen ? 5u : 4u))
      state->Failure(L"independent controls, complete upload contents or terminal ownership failed");
    for (unsigned i = 0; i < kCount; ++i) {
      wprintf(L"INFO  worker-denial: case=%ls intercepted=%u\n", kNames[i], state->intercepted[i]);
      const bool valid = i == 4 ? state->intercepted[i] == 1 || state->intercepted[i] == 2 :
          state->intercepted[i] == (i == 5 ? state->redirect_target_seen ? 2u : 1u : i == 10 ? 0u : 1u);
      if (!valid) state->Failure(L"request source filters missed or duplicated a required fixture");
    }
    for (unsigned i : {1u, 3u, 5u, 7u, 9u}) {
      const unsigned preflight = i == 3 ? Count(b, path("cors-deny"), "OPTIONS") : 0;
      const unsigned original = i == 3 ? Count(b, path("cors-deny")) - preflight : i == 5 ? Count(b, path("redirect-deny-target")) :
          i == 7 ? concurrent_denied : Count(a, path(i == 1 ? "post-deny" : "teardown-deny"));
      const bool observed = state->rejected[i] && state->request_identity[i] && state->timing_identity[i] && original == 0 && preflight == 0 &&
                            (i != 5 || state->redirect_target_seen) && !state->failed;
      wprintf(L"INFO  worker-denial: result case=%ls initial-typeerror=%ls request-identity=%ls timing-identity=%ls target-network=%u preflight-network=%u outcome=%ls\n",
              kNames[i], state->rejected[i] ? L"yes" : L"no", state->request_identity[i] ? L"yes" : L"no",
              state->timing_identity[i] ? L"yes" : L"no", original, preflight, observed ? L"network-rejection-observed" : L"not-qualified");
    }
    wprintf(L"INFO  worker-denial: parallel-reverse-completions=%u terminal-before-close=%ls late-allow-rejections=%u original-restores=%u external-launches=%u\n",
            state->parallel_completed, state->terminal_before_close ? L"yes" : L"no", state->late_rejections, state->restore_calls, state->external_launches);
    wprintf(L"INFO  worker-denial: redirect-start-network=%u redirect-target-callback=%ls; observed WebView2 routing only; Android redirect parity unqualified\n",
            Count(a, path("redirect-deny"), "GET"), state->redirect_target_seen ? L"yes" : L"no");
  }
  wprintf(L"%ls  worker-denial: %ls; no production worker capability enabled\n", state->failed ? L"FAIL" : L"PASS",
          state->failed ? L"harness failed; no scope qualification" : L"scope controls and independent request bodies verified; outcomes recorded");
  fflush(stdout); return state->failed ? 1 : 0;
}
}  // namespace worker_denial_probe
