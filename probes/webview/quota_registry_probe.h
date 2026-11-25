// Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0
// Qualification of the browser's live quota registry. This is an isolated
// diagnostic, not a production WebStorage backend or a capability declaration.
#pragma once

#include <limits>

#include "browser_session_probe.h"
#include "../../compat/webview/storage_registry_scripts.h"

namespace quota_registry_probe {
using namespace gesher::webview;
using browser_session_probe::Json;
using browser_session_probe::JsonLength;
using browser_session_probe::JsonReader;
using browser_session_probe::PumpUntil;
using browser_session_probe::Quote;

constexpr ULONGLONG kCallMs = 10000;
constexpr size_t kPayloadSize = 262144;

inline void Log(const wchar_t* level, const std::wstring& message) {
  wprintf(L"%ls  quota-registry: %ls\n", level, message.c_str());
  fflush(stdout);
}

inline void Result(const wchar_t* method, HRESULT hr, LPCWSTR json = L"") {
  size_t length = JsonLength(json);
  wprintf(L"INFO  quota-registry: %ls hr=0x%08lx json=%.*ls%ls\n", method,
      static_cast<unsigned long>(hr), static_cast<int>(std::min(length, browser_session_probe::kMaxJson)),
      json ? json : L"", length > browser_session_probe::kMaxJson ? L" [over limit]" : L"");
  fflush(stdout);
}

struct Observation {
  std::wstring label;
  std::wstring origin;
  std::wstring key;  // Exact Storage.getStorageKeyForFrame result, never synthesized.
};

struct Bucket {
  std::wstring key, id, name;
  uint64_t usage = 0;
};

inline bool Unsigned(const Json* value, uint64_t* number) {
  if (!value || value->kind != L'"' || value->text.empty() || value->text.size() > 19) return false;
  uint64_t result = 0;
  for (wchar_t c : value->text) {
    if (c < L'0' || c > L'9' || result > (uint64_t(INT64_MAX) - (c - L'0')) / 10) return false;
    result = result * 10 + (c - L'0');
  }
  *number = result;
  return true;
}

inline bool ReadBuckets(const Json& value, std::vector<Bucket>* buckets) {
  const Json* entries = value.Get(L"entries", L'[');
  if (!entries || entries->items.size() > 64) return false;
  std::set<std::wstring> ids;
  for (const Json& entry : entries->items) {
    const Json* key = entry.Get(L"storageKey", L'"');
    const Json* id = entry.Get(L"bucketId", L'"');
    const Json* name = entry.Get(L"name", L'"');
    uint64_t usage = 0, number = 0;
    if (!key || key->text.empty() || key->text.size() > 4096 || !name || name->text.empty() ||
        !Unsigned(id, &number) || number == 0 || !ids.insert(id->text).second ||
        !Unsigned(entry.Get(L"usage", L'"'), &usage)) return false;
    buckets->push_back({key->text, id->text, name->text, usage});
  }
  return true;
}

// A fresh, exclusively owned fixture profile has exactly the captured keys.
// Check identity and live measurements, not merely a nonempty registry response.
inline bool InventoryMatches(const std::vector<Bucket>& buckets,
                             const std::vector<Observation>& positive,
                             const std::vector<Observation>& removed) {
  std::map<std::wstring, uint64_t> totals;
  std::set<std::wstring> allowed;
  for (const auto& item : positive) {
    if (item.key.empty() || !allowed.insert(item.key).second) return false;
  }
  for (const auto& item : removed) {
    if (item.key.empty() || !allowed.insert(item.key).second) return false;
  }
  for (const auto& bucket : buckets) {
    if (!allowed.contains(bucket.key) || totals[bucket.key] > uint64_t(INT64_MAX) - bucket.usage) return false;
    totals[bucket.key] += bucket.usage;
  }
  for (const auto& item : positive) if (totals[item.key] == 0) return false;
  for (const auto& item : removed) if (totals[item.key] != 0) return false;
  return true;
}

inline std::wstring NodeId(const Json* value) {
  if (!value || value->kind != L'#' || value->text.empty() || value->text.size() > 10) return {};
  uint64_t number = 0;
  for (wchar_t c : value->text) {
    if (c < L'0' || c > L'9') return {};
    number = number * 10 + (c - L'0');
  }
  return number > 0 && number <= INT32_MAX ? value->text : std::wstring();
}

inline bool FrameOwnerId(const Json& result, const std::wstring& expected_node,
                         const std::wstring& expected_url, std::wstring* frame_id) {
  const Json* node = result.Get(L"node", L'{');
  const Json* name = node ? node->Get(L"nodeName", L'"') : nullptr;
  const Json* type = node ? node->Get(L"nodeType", L'#') : nullptr;
  const Json* frame = node ? node->Get(L"frameId", L'"') : nullptr;
  const Json* attrs = node ? node->Get(L"attributes", L'[') : nullptr;
  if (!node || NodeId(node->Get(L"nodeId", L'#')) != expected_node || expected_node.empty() ||
      !name || name->text != L"IFRAME" || !type || type->text != L"1" ||
      !frame || frame->text.empty() || frame->text.size() > 128 ||
      !attrs || attrs->items.size() > 64 || attrs->items.size() % 2 != 0) return false;
  std::map<std::wstring, std::wstring> attributes;
  for (size_t i = 0; i < attrs->items.size(); i += 2) {
    const Json& key = attrs->items[i]; const Json& value = attrs->items[i + 1];
    if (key.kind != L'"' || value.kind != L'"' ||
        !attributes.emplace(key.text, value.text).second) return false;
  }
  if (attributes[L"id"] != L"quota-child" || attributes[L"src"] != expected_url) return false;
  *frame_id = frame->text;
  return true;
}

struct State {
  Com<ICoreWebView2Environment> environment;
  Com<ICoreWebView2Controller> controller;
  Com<ICoreWebView2> core;
  HANDLE browser_process = nullptr;
  UINT32 browser_pid = 0;
  EventRegistrationToken resources{};
  bool subscribed = false, environment_done = false, controller_done = false, closed = false;
  bool failed = false, unavailable = false;
  unsigned served = 0;
  std::wstring profile_path, reason;
  std::map<std::wstring, std::string> pages;
  ULONGLONG deadline;
  explicit State(ULONGLONG end) : deadline(end) {}
  ~State() { if (browser_process) CloseHandle(browser_process); }
  bool Fail(const std::wstring& message) { failed = true; reason = message; Log(L"FAIL", message); return false; }
  bool Unavailable(const std::wstring& message) { unavailable = true; reason = message; Log(L"INFO", L"unavailable: " + message); return false; }
  ULONGLONG CallDeadline() const { return std::min(deadline, GetTickCount64() + kCallMs); }
  bool Check(HRESULT hr, const wchar_t* operation) {
    Result(operation, hr);
    return SUCCEEDED(hr) || Fail(std::wstring(operation) + L" failed");
  }
  bool Close(ULONGLONG end) {
    closed = true;
    if (subscribed && core) {
      Check(core->remove_WebResourceRequested(resources), L"remove resource fixture");
      subscribed = false;
    }
    if (controller) Check(controller->Close(), L"controller.Close");
    core.Reset(); controller.Reset(); environment.Reset();
    if (browser_process) {
      if (!PumpUntil(end, [&] { return WaitForSingleObject(browser_process, 0) != WAIT_TIMEOUT; }) ||
          WaitForSingleObject(browser_process, 0) != WAIT_OBJECT_0) {
        return Fail(L"owned browser process did not exit within the cleanup deadline");
      }
      Log(L"INFO", L"browser process exited pid=" + std::to_wstring(browser_pid));
      CloseHandle(browser_process); browser_process = nullptr;
    }
    return !failed;
  }
};

inline bool Call(const std::shared_ptr<State>& state, const wchar_t* method,
                 const std::wstring& params, Json* value, bool capability = false) {
  if (GetTickCount64() >= state->deadline) return state->Fail(L"work deadline expired before CDP call");
  struct Pending { bool done = false; HRESULT hr = E_PENDING; std::wstring json; };
  auto pending = std::make_shared<Pending>();
  const ULONGLONG end = state->CallDeadline();
  HRESULT hr = state->core->CallDevToolsProtocolMethod(method, params.c_str(),
      GESHER_CALLBACK(ICoreWebView2CallDevToolsProtocolMethodCompletedHandler,
        [pending, method = std::wstring(method)](HRESULT result, LPCWSTR json) -> HRESULT {
          Result(method.c_str(), result, json);
          pending->hr = result;
          pending->json.assign(json ? json : L"", JsonLength(json));
          pending->done = true; return S_OK;
        }).get());
  Result(method, hr);
  if (capability && (hr == E_ACCESSDENIED || hr == E_NOTIMPL)) return state->Unavailable(L"CDP entry explicitly denied");
  if (FAILED(hr)) return state->Fail(L"CDP operation was not admitted");
  if (!PumpUntil(end, [&] { return pending->done; }) || GetTickCount64() >= end) return state->Fail(L"CDP operation exceeded its deadline");
  if (capability && (pending->hr == E_ACCESSDENIED || pending->hr == E_NOTIMPL)) return state->Unavailable(L"CDP completion explicitly denied");
  if (!JsonReader(pending->json).Read(value) || value->kind != L'{') return state->Fail(L"malformed CDP result");
  if (FAILED(pending->hr) || value->fields.contains(L"error") || value->fields.contains(L"code")) return state->Fail(L"CDP returned an operation error");
  return true;
}

inline bool Evaluate(const std::shared_ptr<State>& state, const std::wstring& script,
                     Json* value, bool capability = false) {
  Json result;
  if (!Call(state, L"Runtime.evaluate", L"{\"expression\":" + Quote(script) +
      L",\"awaitPromise\":true,\"returnByValue\":true}", &result, capability)) return false;
  if (result.fields.contains(L"exceptionDetails")) return state->Fail(L"browser JavaScript rejected the operation");
  const Json* object = result.Get(L"result", L'{');
  const Json* content = object ? object->Get(L"value", L'{') : nullptr;
  if (!content) return state->Fail(L"JavaScript did not return a structured result");
  *value = *content;
  return true;
}

inline bool Navigate(const std::shared_ptr<State>& state, const std::wstring& url, bool capability = false) {
  struct Pending { bool done = false; BOOL success = FALSE; COREWEBVIEW2_WEB_ERROR_STATUS error{}; HRESULT hr = E_PENDING; };
  auto pending = std::make_shared<Pending>();
  EventRegistrationToken token{};
  if (!state->Check(state->core->add_NavigationCompleted(
      GESHER_CALLBACK(ICoreWebView2NavigationCompletedEventHandler,
        [pending](ICoreWebView2*, ICoreWebView2NavigationCompletedEventArgs* args) -> HRESULT {
          pending->hr = args ? args->get_IsSuccess(&pending->success) : E_POINTER;
          if (SUCCEEDED(pending->hr)) pending->hr = args->get_WebErrorStatus(&pending->error);
          pending->done = true; return S_OK;
        }).get(), &token), L"subscribe navigation")) return false;
  const ULONGLONG end = state->CallDeadline();
  HRESULT hr = state->core->Navigate(url.c_str());
  Result(L"Navigate", hr, url.c_str());
  bool complete = SUCCEEDED(hr) && PumpUntil(end, [&] { return pending->done; }) && GetTickCount64() < end;
  if (!state->Check(state->core->remove_NavigationCompleted(token), L"remove navigation")) return false;
  if (!complete || FAILED(pending->hr)) return state->Fail(L"navigation setup or completion failed/timed out");
  if (!pending->success) {
    std::wstring reason = L"navigation rejected url=" + url + L" status=" + std::to_wstring(pending->error);
    return capability ? state->Unavailable(reason) : state->Fail(reason);
  }
  CoString source;
  if (!state->Check(state->core->get_Source(source.Put()), L"navigation source")) return false;
  Log(L"INFO", L"navigated " + source.str());
  if (source.str() != url) return capability ? state->Unavailable(L"internal URL did not remain the requested document") : state->Fail(L"fixture origin/navigation changed");
  return true;
}

// The only supplied content is a genuine HTTPS document fixture. IndexedDB,
// origin/security checks, transactions and storage partitioning are browser code.
inline std::string Page(const std::wstring& origin) {
  std::string expected(origin.begin(), origin.end());
  return std::string(R"HTML(<!doctype html><meta charset="utf-8"><title>quota registry fixture</title><body>
<script>
'use strict';
const expectedOrigin = ')HTML") + expected + R"HTML(';
const dbName = 'gesher-quota-registry';
function operate(mode, label) {
  if (location.origin !== expectedOrigin || !isSecureContext) throw Error('wrong origin/security context');
  return new Promise((resolve, reject) => {
    let db, absent = false, finished = false;
    const finish = (error, extra = {}) => {
      if (finished) return; finished = true;
      if (db) db.close();
      if (error) reject(error);
      else resolve({origin: location.origin, secure: true, closed: true, label, ...extra});
    };
    const open = indexedDB.open(dbName, 1);
    open.onblocked = () => finish(Error('database open blocked'));
    open.onupgradeneeded = () => {
      if (mode !== 'write') { absent = true; open.transaction.abort(); }
      else open.result.createObjectStore('values');
    };
    open.onerror = () => {
      if (absent && mode === 'absent' && open.error?.name === 'AbortError') finish(null, {absent: true, bytes: 0});
      else finish(Error('database open failed: ' + open.error));
    };
    open.onsuccess = () => {
      db = open.result;
      db.onversionchange = () => db.close();
      if (mode === 'absent') { finish(Error('deleted database still exists')); return; }
      let bytes = 0;
      const seed = Array.from(label).reduce((n, c) => n + c.charCodeAt(0), 0);
      const transaction = db.transaction('values', mode === 'write' ? 'readwrite' : 'readonly');
      transaction.onerror = () => finish(Error('transaction error: ' + transaction.error));
      transaction.onabort = () => finish(Error('transaction aborted'));
      transaction.oncomplete = () => finish(null, {absent: false, bytes});
      if (mode === 'write') {
        const payload = new Uint8Array(262144);
        for (let i = 0; i < payload.length; ++i) payload[i] = (i * 37 + seed) % 251;
        transaction.objectStore('values').put(payload, 'sentinel'); bytes = payload.length;
      } else {
        const read = transaction.objectStore('values').get('sentinel');
        read.onsuccess = () => {
          const payload = read.result;
          if (!(payload instanceof Uint8Array) || payload.length !== 262144) { finish(Error('missing/full-length sentinel')); return; }
          for (let i = 0; i < payload.length; ++i) {
            if (payload[i] !== (i * 37 + seed) % 251) { finish(Error('sentinel byte mismatch at ' + i)); return; }
          }
          bytes = payload.length;
        };
      }
    };
  });
}
window.operate = operate;
window.withFrame = (origin, mode, label) => new Promise((resolve, reject) => {
  document.getElementById('quota-child')?.remove();
  const frame = document.createElement('iframe'); frame.id = 'quota-child';
  const nonce = crypto.randomUUID();
  const listener = event => {
    if (event.source !== frame.contentWindow || event.origin !== origin || event.data?.nonce !== nonce) return;
    window.removeEventListener('message', listener);
    if (event.data.error) reject(Error(event.data.error)); else resolve(event.data.value);
  };
  window.addEventListener('message', listener);
  frame.onload = () => frame.contentWindow.postMessage({kind: 'quota-fixture', nonce, mode, label}, origin);
  frame.onerror = () => { window.removeEventListener('message', listener); reject(Error('iframe load failed')); };
  frame.src = origin + '/storage.html'; document.body.appendChild(frame);
});
window.addEventListener('message', event => {
  if (event.source !== parent || !event.data || event.data.kind !== 'quota-fixture') return;
  const {nonce, mode, label} = event.data;
  if (!['write', 'read', 'absent'].includes(mode) || typeof label !== 'string') return;
  Promise.resolve().then(() => operate(mode, label)).then(
    value => event.source.postMessage({nonce, value}, event.origin),
    error => event.source.postMessage({nonce, error: String(error)}, event.origin));
});
</script></body>)HTML";
}

inline bool Start(const std::shared_ptr<State>& state, HWND parent, const std::wstring& profile,
                  const std::vector<std::wstring>& origins,
                  ICoreWebView2EnvironmentOptions* environment_options = nullptr) {
  HRESULT hr = CreateEnvironment(nullptr, profile.c_str(), environment_options,
      GESHER_CALLBACK(ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler,
        [state](HRESULT result, ICoreWebView2Environment* environment) -> HRESULT {
          Result(L"environment completion", result);
          if (!state->closed) {
            if (SUCCEEDED(result) && environment) state->environment = Com<ICoreWebView2Environment>(environment);
            state->environment_done = true;
          }
          return S_OK;
        }).get());
  if (!state->Check(hr, L"CreateEnvironment") ||
      !PumpUntil(std::min(state->deadline, GetTickCount64() + 20000), [&] { return state->environment_done; }) ||
      !state->environment) return state->Fail(L"environment setup failed or timed out");
  Com<ICoreWebView2Environment7> env7; CoString actual, version;
  if (!state->environment.As(IID_ICoreWebView2Environment7, &env7) ||
      !state->Check(env7->get_UserDataFolder(actual.Put()), L"environment profile") ||
      CompareStringOrdinal(profile.c_str(), -1, actual.str().c_str(), -1, TRUE) != CSTR_EQUAL ||
      !state->Check(state->environment->get_BrowserVersionString(version.Put()), L"runtime version")) {
    return state->Fail(L"environment identity was not verified");
  }
  Log(L"INFO", L"runtime=" + version.str() + L" user-data=" + actual.str());
  hr = state->environment->CreateCoreWebView2Controller(parent,
      GESHER_CALLBACK(ICoreWebView2CreateCoreWebView2ControllerCompletedHandler,
        [state](HRESULT result, ICoreWebView2Controller* controller) -> HRESULT {
          Result(L"controller completion", result);
          if (state->closed) {
            if (controller) state->Check(controller->Close(), L"late controller.Close");
            return S_OK;
          }
          if (SUCCEEDED(result) && controller) state->controller = Com<ICoreWebView2Controller>(controller);
          state->controller_done = true; return S_OK;
        }).get());
  if (!state->Check(hr, L"CreateController") ||
      !PumpUntil(std::min(state->deadline, GetTickCount64() + 20000), [&] { return state->controller_done; }) ||
      !state->controller || !state->Check(state->controller->put_IsVisible(FALSE), L"hide controller") ||
      !state->Check(state->controller->get_CoreWebView2(state->core.Put()), L"get core") || !state->core) {
    return state->Fail(L"controller setup failed or timed out");
  }
  if (!state->Check(state->core->get_BrowserProcessId(&state->browser_pid), L"browser PID")) return false;
  state->browser_process = OpenProcess(SYNCHRONIZE, FALSE, state->browser_pid);
  if (!state->browser_process) return state->Fail(L"could not retain owned browser exit handle");
  Log(L"INFO", L"browser pid=" + std::to_wstring(state->browser_pid));
  Com<ICoreWebView2_13> core13; Com<ICoreWebView2Profile> actual_profile;
  CoString profile_path;
  if (!state->core.As(IID_ICoreWebView2_13, &core13) ||
      !state->Check(core13->get_Profile(actual_profile.Put()), L"get actual profile") || !actual_profile ||
      !state->Check(actual_profile->get_ProfilePath(profile_path.Put()), L"actual profile path")) {
    return state->Fail(L"controller profile could not be identified");
  }
  state->profile_path = profile_path.str();
  Log(L"INFO", L"profile-path=" + state->profile_path);
  for (const auto& origin : origins) {
    state->pages.emplace(origin + L"/storage.html", Page(origin));
    if (!state->Check(state->core->AddWebResourceRequestedFilter((origin + L"/*").c_str(),
        COREWEBVIEW2_WEB_RESOURCE_CONTEXT_ALL), L"add exact fixture origin filter")) return false;
  }
  std::weak_ptr<State> weak = state;
  hr = state->core->add_WebResourceRequested(
      GESHER_CALLBACK(ICoreWebView2WebResourceRequestedEventHandler,
        [weak](ICoreWebView2*, ICoreWebView2WebResourceRequestedEventArgs* args) -> HRESULT {
          auto state = weak.lock(); if (!state || state->closed) return S_OK;
          Com<ICoreWebView2WebResourceRequest> request; CoString uri, method;
          if (!args || FAILED(args->get_Request(request.Put())) || !request ||
              FAILED(request->get_Uri(uri.Put())) || FAILED(request->get_Method(method.Put()))) {
            state->Fail(L"resource fixture could not read the actual request"); return E_FAIL;
          }
          auto found = state->pages.find(uri.str());
          const bool fixture = found != state->pages.end() && method.str() == L"GET";
          const bool icon = uri.str().ends_with(L"/favicon.ico") && method.str() == L"GET";
          if (!fixture && !icon) state->Fail(L"unexpected fixture request " + method.str() + L" " + uri.str());
          std::string body = fixture ? found->second : std::string();
          Com<IStream> stream(SHCreateMemStream(reinterpret_cast<const BYTE*>(body.data()), static_cast<UINT>(body.size())), false);
          Com<ICoreWebView2WebResourceResponse> response;
          HRESULT hr = state->environment->CreateWebResourceResponse(stream.get(), fixture ? 200 : 404,
              fixture ? L"OK" : L"Not Found", L"Content-Type: text/html; charset=utf-8\r\nCache-Control: no-store\r\n", response.Put());
          if ((fixture && !stream) || FAILED(hr) || !response || FAILED(args->put_Response(response.get()))) {
            state->Fail(L"fixture response failed"); return E_FAIL;
          }
          if (fixture) ++state->served;
          return S_OK;
        }).get(), &state->resources);
  state->subscribed = SUCCEEDED(hr);
  return state->Check(hr, L"subscribe resource fixture");
}

inline bool Content(const std::shared_ptr<State>& state, const Observation& item,
                    const wchar_t* mode, bool frame) {
  Json value;
  const std::wstring script = frame
      ? L"window.withFrame(" + Quote(item.origin) + L"," + Quote(mode) + L"," + Quote(item.label) + L")"
      : L"window.operate(" + Quote(mode) + L"," + Quote(item.label) + L")";
  if (!Evaluate(state, script, &value)) return false;
  const Json* origin = value.Get(L"origin", L'"');
  const Json* label = value.Get(L"label", L'"');
  const Json* secure = value.Get(L"secure", L't');
  const Json* closed = value.Get(L"closed", L't');
  const Json* bytes = value.Get(L"bytes", L'#');
  const bool absent = std::wstring_view(mode) == L"absent";
  if (!origin || origin->text != item.origin || !label || label->text != item.label || !secure || !closed ||
      !bytes || bytes->text != (absent ? L"0" : std::to_wstring(kPayloadSize)) ||
      !value.Get(L"absent", absent ? L't' : L'f')) return state->Fail(L"invalid content evidence for " + item.label);
  Log(L"PASS", std::wstring(mode) + L" " + item.label + L" origin=" + item.origin + L" bytes=" + bytes->text + L" closed=true secure=true");
  return !state->failed;
}

inline void FindFrames(const Json& tree, const std::wstring& url, std::vector<std::wstring>* ids) {
  const Json* frame = tree.Get(L"frame", L'{');
  const Json* address = frame ? frame->Get(L"url", L'"') : nullptr;
  const Json* id = frame ? frame->Get(L"id", L'"') : nullptr;
  if (address && id && address->text == url && !id->text.empty()) ids->push_back(id->text);
  const Json* children = tree.Get(L"childFrames", L'[');
  if (children) for (const auto& child : children->items) FindFrames(child, url, ids);
}

inline bool CaptureKey(const std::shared_ptr<State>& state, Observation* item, bool frame = false) {
  Json key;
  std::wstring frame_id;
  if (frame) {
    // Page.getFrameTree is renderer-scoped and can omit an out-of-process
    // child. Its iframe owner remains in the parent DOM and carries the real
    // child frame token, including for OOPIFs; never synthesize a storage key.
    Json document, selected, described;
    if (!Call(state, L"DOM.getDocument", L"{\"depth\":0}", &document)) return false;
    const Json* root = document.Get(L"root", L'{');
    const std::wstring root_id = NodeId(root ? root->Get(L"nodeId", L'#') : nullptr);
    if (root_id.empty()) return state->Fail(L"missing actual DOM document for " + item->label);
    if (!Call(state, L"DOM.querySelectorAll", L"{\"nodeId\":" + root_id +
        L",\"selector\":\"iframe#quota-child\"}", &selected)) return false;
    const Json* owners = selected.Get(L"nodeIds", L'[');
    if (!owners || owners->items.size() != 1) return state->Fail(L"expected exactly one actual iframe owner for " + item->label);
    const std::wstring node_id = NodeId(&owners->items[0]);
    if (node_id.empty()) return state->Fail(L"missing actual iframe owner for " + item->label);
    if (!Call(state, L"DOM.describeNode", L"{\"nodeId\":" + node_id + L",\"depth\":0}", &described)) return false;
    if (!FrameOwnerId(described, node_id, item->origin + L"/storage.html", &frame_id)) {
      return state->Fail(L"invalid actual iframe owner for " + item->label);
    }
  } else {
    Json frames;
    if (!Call(state, L"Page.getFrameTree", L"{}", &frames)) return false;
    const Json* tree = frames.Get(L"frameTree", L'{');
    std::vector<std::wstring> ids;
    if (tree) FindFrames(*tree, item->origin + L"/storage.html", &ids);
    if (ids.size() != 1) return state->Fail(L"expected exactly one real frame for " + item->label);
    frame_id = ids[0];
  }
  if (!Call(state, L"Storage.getStorageKeyForFrame", L"{\"frameId\":" + Quote(frame_id) + L"}", &key)) return false;
  const Json* result = key.Get(L"storageKey", L'"');
  if (!result || result->text.empty() || result->text.size() > 4096) return state->Fail(L"missing actual storage key");
  item->key = result->text;
  Log(L"INFO", L"captured " + item->label + L" key=" + item->key);
  return true;
}

inline bool InitializeIndexedDb(const std::shared_ptr<State>& state) {
  // The quota DB can enumerate persisted buckets before the IndexedDB client's
  // lazy disk discovery. Until then that client reports zero for unknown IDs.
  // This real WebUI control waits for InitializeFromFilesIfNeeded; it does not
  // navigate to any fixture origin or substitute metadata for quota usage.
  if (!Navigate(state, L"edge://indexeddb-internals/", true)) return false;
  const wchar_t* initialization = storage_registry_scripts::kInitializeIndexedDb;
  Json value;
  if (!Evaluate(state, initialization, &value, true)) return false;
  const Json* unavailable = value.Get(L"unavailable", L'"');
  if (unavailable) return state->Unavailable(unavailable->text);
  uint64_t partitions = 0;
  if (!value.Get(L"initialized", L't') || !Unsigned(value.Get(L"partitions", L'"'), &partitions) ||
      partitions == 0 || partitions > 64) return state->Fail(L"malformed IndexedDB discovery completion");
  Log(L"INFO", L"IndexedDB discovery barrier completed partitions=" + std::to_wstring(partitions));
  return true;
}

inline bool Registry(const std::shared_ptr<State>& state, std::vector<Bucket>* buckets) {
  // The 131 proxy accepts StorageType.TEMPORARY=0; newer proxies ignore that
  // extra JS argument. Invoke the runtime's own module and generated bindings.
  // The global-usage barrier opens/bootstrap-orders the live manager before the
  // table read; RetrieveBucketsTable alone can otherwise return an unopened DB.
  const wchar_t* script = storage_registry_scripts::kReadRegistry;
  Json value;
  if (!Evaluate(state, script, &value, true)) return false;
  const Json* unavailable = value.Get(L"unavailable", L'"');
  if (unavailable) return state->Unavailable(unavailable->text);
  uint64_t usage = 0;
  if (!Unsigned(value.Get(L"globalUsage", L'"'), &usage) || !ReadBuckets(value, buckets)) return state->Fail(L"malformed live quota inventory/usage");
  Log(L"INFO", L"live registry buckets=" + std::to_wstring(buckets->size()) + L" globalUsage=" + std::to_wstring(usage));
  for (const auto& item : *buckets) Log(L"INFO", L"bucket=" + item.id + L" key=" + item.key + L" name=" + item.name + L" usage=" + std::to_wstring(item.usage));
  return true;
}

inline bool Measure(const std::shared_ptr<State>& state, const wchar_t* phase,
                    const std::vector<Observation>& positive,
                    const std::vector<Observation>& removed = {}) {
  const ULONGLONG end = std::min(state->deadline, GetTickCount64() + 10000);
  do {
    std::vector<Bucket> buckets;
    if (!Registry(state, &buckets)) return false;
    if (GetTickCount64() >= end) return state->Fail(L"registry convergence exceeded deadline");
    if (InventoryMatches(buckets, positive, removed)) {
      Log(L"PASS", std::wstring(phase) + L" complete keys=" + std::to_wstring(positive.size()) +
          L" removed=" + std::to_wstring(removed.size()));
      return true;
    }
    PumpUntil(std::min(end, GetTickCount64() + 100), [] { return false; });
  } while (GetTickCount64() < end);
  return state->Fail(std::wstring(phase) + L" did not contain the expected complete live measurements");
}

inline bool Delete(const std::shared_ptr<State>& state, const Observation& item) {
  Json result;
  if (item.key.empty()) return state->Fail(L"deletion requires an observed key");
  if (!Call(state, L"Storage.clearDataForStorageKey", L"{\"storageKey\":" + Quote(item.key) +
      L",\"storageTypes\":\"file_systems,indexeddb\"}", &result)) return false;
  if (!result.fields.empty()) return state->Fail(L"malformed deletion acknowledgement");
  Log(L"INFO", L"deletion acknowledged " + item.label + L" key=" + item.key);
  return true;
}

inline int Run(HWND parent) {
  // A separate CLI mode/outer process budget preserves the existing probe's
  // deadlines. Reserve cleanup time even when a browser promise never settles.
  const ULONGLONG end = GetTickCount64() + 160000, work_end = end - 20000;
  wchar_t temp[MAX_PATH]{}, id[40]{}; GUID guid{};
  DWORD length = GetTempPathW(MAX_PATH, temp);
  if (!length || length >= MAX_PATH || FAILED(CoCreateGuid(&guid)) || !StringFromGUID2(guid, id, 40)) {
    Log(L"FAIL", L"cannot create a fresh profile identity"); return 1;
  }
  std::wstring nonce(id);
  nonce.erase(std::remove_if(nonce.begin(), nonce.end(), [](wchar_t c) { return c == L'{' || c == L'}' || c == L'-'; }), nonce.end());
  for (wchar_t& c : nonce) if (c >= L'A' && c <= L'F') c += L'a' - L'A';
  const std::wstring profile = std::wstring(temp) + L"gesher-quota-registry-" + nonce;
  if (!CreateDirectoryW(profile.c_str(), nullptr)) { Log(L"FAIL", L"fresh profile creation failed"); return 1; }
  const std::wstring a = L"https://quota-a-" + nonce + L".invalid";
  const std::wstring b = L"https://quota-b-" + nonce + L".invalid";
  const std::wstring c = L"https://quota-c-" + nonce + L".invalid";
  Observation first{L"A-first-party", a, L""}, embedded_a{L"C-under-A", c, L""},
      live{L"B-first-party", b, L""}, embedded_b{L"C-under-B", c, L""}, a_under_b{L"A-under-B", a, L""};
  const std::string sdk = GESHER_WEBVIEW2_PROBE_SDK_VERSION, loader = LoaderKind();
  Log(L"INFO", L"sdk=" + std::wstring(sdk.begin(), sdk.end()) + L" loader=" + std::wstring(loader.begin(), loader.end()) + L" profile=" + profile);
  auto seed = std::make_shared<State>(work_end);
  auto inspect = std::make_shared<State>(work_end);
  bool complete = false;
  do {
    if (!Start(seed, parent, profile, {a, b, c}) || !Navigate(seed, a + L"/storage.html") ||
        !Content(seed, first, L"write", false) || !CaptureKey(seed, &first) ||
        !Content(seed, embedded_a, L"write", true) || !CaptureKey(seed, &embedded_a, true)) break;
    // No registry API ran in the writer environment. Observe full process exit,
    // then discover persisted entries before any fixture origin is revisited.
    if (!seed->Close(std::min(end, GetTickCount64() + 20000))) break;
    if (!Start(inspect, parent, profile, {a, b, c}) || inspect->profile_path != seed->profile_path) {
      if (!inspect->failed) inspect->Fail(L"reopened a different browser profile"); break;
    }
    if (!InitializeIndexedDb(inspect) || !Navigate(inspect, L"edge://quota-internals/", true) ||
        !Measure(inspect, L"persisted-before-revisit", {first, embedded_a})) break;
    if (inspect->served != 0) { inspect->Fail(L"persisted inventory revisited an origin before discovery"); break; }
    Log(L"PASS", L"persisted discovery after browser exit without revisiting fixture origins");
    if (!Navigate(inspect, b + L"/storage.html") || !Content(inspect, live, L"write", false) || !CaptureKey(inspect, &live) ||
        !Content(inspect, embedded_b, L"write", true) || !CaptureKey(inspect, &embedded_b, true) ||
        !Content(inspect, a_under_b, L"write", true) || !CaptureKey(inspect, &a_under_b, true)) break;
    if (embedded_a.key == embedded_b.key || first.key == a_under_b.key) {
      inspect->Unavailable(L"runtime did not expose distinct first-/third-party storage keys"); break;
    }
    if (!Navigate(inspect, L"edge://quota-internals/", true) ||
        !Measure(inspect, L"live-update", {first, embedded_a, live, embedded_b, a_under_b})) break;
    // Android's default trusted-origin deletion selects A first-party plus C
    // under A, leaving A-under-B and C-under-B intact. These keys came from real
    // frame contexts, not a fabricated/visited-origin inventory.
    if (!Delete(inspect, first) || !Delete(inspect, embedded_a) ||
        !Measure(inspect, L"post-delete", {live, embedded_b, a_under_b}, {first, embedded_a})) break;
    // The fresh zero/absence observation precedes any attempted DB reopening.
    if (!Navigate(inspect, a + L"/storage.html") || !Content(inspect, first, L"absent", false) ||
        !Content(inspect, embedded_a, L"absent", true) || !Navigate(inspect, b + L"/storage.html") ||
        !Content(inspect, live, L"read", false) || !Content(inspect, embedded_b, L"read", true) ||
        !Content(inspect, a_under_b, L"read", true)) break;
    complete = true;
  } while (false);
  seed->Close(end);
  inspect->Close(end);
  if (seed->failed || inspect->failed) {
    Log(L"FAIL", L"qualification failed; production enumeration remains unavailable"); return 1;
  }
  if (!complete) {
    Log(L"INFO", L"UNAVAILABLE " + (inspect->reason.empty() ? seed->reason : inspect->reason) + L"; production support not established"); return 3;
  }
  Log(L"PASS", L"QUALIFIED persisted=2 live=5 deleted=2 preserved=3 payload_bytes=262144; production API still gated");
  return 0;
}
}  // namespace quota_registry_probe
