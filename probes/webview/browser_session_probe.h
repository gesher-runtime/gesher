// Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0
// A diagnostic of documented WebView2 COM session access, not a worker/Fetch test.
#pragma once

#include <algorithm>
#include <map>
#include <memory>
#include <set>
#include <string_view>

namespace browser_session_probe {
using namespace gesher::webview;
constexpr size_t kMaxJson = 65536;

// Bounded JSON for the few CDP responses below. Keep targetId and type in the
// same parsed object; substring/regex matching could attach an unrelated target.
struct Json {
  wchar_t kind = 0;
  std::wstring text;
  std::map<std::wstring, Json> fields;
  std::vector<Json> items;
  const Json* Get(const wchar_t* key, wchar_t type) const {
    auto found = fields.find(key);
    return kind == L'{' && found != fields.end() && found->second.kind == type
               ? &found->second : nullptr;
  }
};

class JsonReader {
 public:
  explicit JsonReader(std::wstring_view input) : input_(input) {}
  bool Read(Json* value) {
    return input_.size() <= kMaxJson && Value(value, 0) && (Space(), at_ == input_.size());
  }
 private:
  void Space() { while (at_ < input_.size() && (input_[at_] == L' ' || input_[at_] == L'\t' ||
      input_[at_] == L'\r' || input_[at_] == L'\n')) ++at_; }
  bool Take(wchar_t c) { Space(); if (at_ == input_.size() || input_[at_] != c) return false; ++at_; return true; }
  bool String(std::wstring* out) {
    if (!Take(L'"')) return false;
    while (at_ < input_.size()) {
      wchar_t c = input_[at_++];
      if (c == L'"') return true;
      if (c < 0x20) return false;
      if (c == L'\\') {
        if (at_ == input_.size()) return false;
        c = input_[at_++];
        switch (c) {
          case L'"': case L'\\': case L'/': break;
          case L'b': c = L'\b'; break; case L'f': c = L'\f'; break;
          case L'n': c = L'\n'; break; case L'r': c = L'\r'; break; case L't': c = L'\t'; break;
          case L'u': {
            unsigned code = 0;
            for (int i = 0; i < 4; ++i) {
              if (at_ == input_.size()) return false;
              wchar_t digit = input_[at_++];
              int n = digit >= L'0' && digit <= L'9' ? digit - L'0' :
                      digit >= L'a' && digit <= L'f' ? digit - L'a' + 10 :
                      digit >= L'A' && digit <= L'F' ? digit - L'A' + 10 : -1;
              if (n < 0) return false;
              code = code * 16 + n;
            }
            c = static_cast<wchar_t>(code); break;
          }
          default: return false;
        }
      }
      out->push_back(c);
    }
    return false;
  }
  bool Value(Json* value, unsigned depth) {
    Space();
    if (depth > 16 || ++nodes_ > 2048 || at_ == input_.size()) return false;
    value->kind = input_[at_];
    if (value->kind == L'"') return String(&value->text);
    if (value->kind == L'{' || value->kind == L'[') {
      const bool object = input_[at_++] == L'{';
      const wchar_t end = object ? L'}' : L']';
      if (Take(end)) return true;
      do {
        Json member; std::wstring key;
        if (object && (!String(&key) || !Take(L':'))) return false;
        if (!Value(&member, depth + 1)) return false;
        if (object) {
          if (!value->fields.emplace(std::move(key), std::move(member)).second) return false;
        } else value->items.push_back(std::move(member));
        if (Take(end)) return true;
      } while (Take(L','));
      return false;
    }
    for (auto literal : {L"true", L"false", L"null"}) {
      std::wstring_view word(literal);
      if (input_.substr(at_, word.size()) == word) { at_ += word.size(); return true; }
    }
    value->kind = L'#';
    size_t start = at_;
    if (input_[at_] == L'-') ++at_;
    if (at_ == input_.size()) return false;
    if (input_[at_] == L'0') ++at_;
    else {
      if (input_[at_] < L'1' || input_[at_] > L'9') return false;
      while (at_ < input_.size() && input_[at_] >= L'0' && input_[at_] <= L'9') ++at_;
    }
    for (wchar_t marker : {L'.', L'e'}) {
      if (at_ == input_.size() || (input_[at_] != marker && !(marker == L'e' && input_[at_] == L'E'))) continue;
      ++at_;
      if (marker == L'e' && at_ < input_.size() && (input_[at_] == L'+' || input_[at_] == L'-')) ++at_;
      size_t digits = at_;
      while (at_ < input_.size() && input_[at_] >= L'0' && input_[at_] <= L'9') ++at_;
      if (at_ == digits) return false;
    }
    value->text = input_.substr(start, at_ - start);
    return true;
  }
  std::wstring_view input_;
  size_t at_ = 0;
  unsigned nodes_ = 0;
};

inline std::wstring Quote(const std::wstring& value) {
  std::wstring result = L"\"";
  for (wchar_t c : value) {
    if (c == L'"' || c == L'\\') { result += L'\\'; result += c; }
    else if (c < 0x20) { wchar_t escape[7]; swprintf(escape, 7, L"\\u%04x", unsigned(c)); result += escape; }
    else result += c;
  }
  return result + L'"';
}

inline bool PumpUntil(ULONGLONG deadline, const std::function<bool()>& done) {
  while (!done()) {
    if (GetTickCount64() >= deadline) return false;
    MsgWaitForMultipleObjects(0, nullptr, FALSE, 10, QS_ALLINPUT);
    MSG message;
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
      TranslateMessage(&message); DispatchMessageW(&message);
      if (done()) return true;
      if (GetTickCount64() >= deadline) return false;
    }
  }
  return true;
}

inline size_t JsonLength(LPCWSTR json) {
  size_t size = 0;
  while (json && size <= kMaxJson && json[size]) ++size;
  return size;
}

inline void Result(const wchar_t* operation, const wchar_t* phase, HRESULT hr, LPCWSTR json = L"") {
  const wchar_t* value = json ? json : L"<null>";
  size_t size = JsonLength(value);
  wprintf(L"INFO  browser-session: %ls %ls hr=0x%08lx json=%.*ls%ls\n", operation, phase,
          static_cast<unsigned long>(hr), static_cast<int>(std::min(size, kMaxJson)), value,
          size > kMaxJson ? L" [over JSON limit]" : L"");
  fflush(stdout);
}

struct State {
  Com<ICoreWebView2Environment> environment;
  Com<ICoreWebView2Controller> controller;
  Com<ICoreWebView2> core;
  Com<ICoreWebView2_11> sessions;
  Com<ICoreWebView2DevToolsProtocolEventReceiver> receiver;
  EventRegistrationToken event_token{};
  bool environment_done = false, controller_done = false, subscribed = false, closed = false;
  bool failed = false, discovery_requested = false;
  std::set<std::wstring> browsers;
  std::vector<std::wstring> attached;
  void Failure(const wchar_t* reason) {
    failed = true; wprintf(L"FAIL  browser-session: %ls\n", reason); fflush(stdout);
  }
  bool Observe(const Json& info) {
    const Json* type = info.Get(L"type", L'"');
    const Json* id = info.Get(L"targetId", L'"');
    if (!type || !id || id->text.empty() || id->text.size() > 1024) return false;
    if (type->text == L"browser" && !browsers.contains(id->text)) {
      if (browsers.size() == 16) return false;
      browsers.insert(id->text);
    }
    return true;
  }
  void Close() {
    closed = true;
    if (subscribed) {
      HRESULT hr = receiver->remove_DevToolsProtocolEventReceived(event_token);
      Result(L"remove targetCreated", L"immediate", hr);
      if (FAILED(hr)) Failure(L"event removal failed");
      subscribed = false;
    }
    if (controller) {
      HRESULT hr = controller->Close(); Result(L"controller.Close", L"immediate", hr);
      if (FAILED(hr)) Failure(L"controller closure failed");
    }
    receiver.Reset(); sessions.Reset(); core.Reset(); controller.Reset(); environment.Reset();
  }
};

enum class CallResult { kSuccess, kDenied, kFailure };
inline CallResult Call(const std::shared_ptr<State>& state, ULONGLONG deadline,
                       const wchar_t* method, const std::wstring& parameters, Json* parsed,
                       const std::wstring& session = L"") {
  if (GetTickCount64() >= deadline) {
    state->Failure(L"diagnostic deadline expired before a CDP call"); return CallResult::kFailure;
  }
  struct Pending { bool done = false; HRESULT hr = E_PENDING; std::wstring json; };
  auto pending = std::make_shared<Pending>();
  auto callback = GESHER_CALLBACK(ICoreWebView2CallDevToolsProtocolMethodCompletedHandler,
      [pending, operation = std::wstring(method)](HRESULT hr, LPCWSTR json) -> HRESULT {
        Result(operation.c_str(), L"completion", hr, json);
        pending->hr = hr;
        pending->json.assign(json ? json : L"", JsonLength(json));
        pending->done = true; return S_OK;
      });
  HRESULT hr = session.empty()
      ? state->core->CallDevToolsProtocolMethod(method, parameters.c_str(), callback.get())
      : state->sessions->CallDevToolsProtocolMethodForSession(session.c_str(), method,
                                                             parameters.c_str(), callback.get());
  Result(method, L"immediate", hr);
  if (hr == E_ACCESSDENIED || hr == E_NOTIMPL) return CallResult::kDenied;
  if (FAILED(hr)) { state->Failure(L"CDP call was not admitted"); return CallResult::kFailure; }
  if (!PumpUntil(deadline, [&] { return pending->done; })) {
    state->Failure(L"CDP completion exceeded the diagnostic deadline"); return CallResult::kFailure;
  }
  if (pending->hr == E_ACCESSDENIED || pending->hr == E_NOTIMPL) return CallResult::kDenied;
  if (!JsonReader(pending->json).Read(parsed) || parsed->kind != L'{') {
    state->Failure(L"malformed CDP response"); return CallResult::kFailure;
  }
  const Json* error = parsed->Get(L"error", L'{');
  if (parsed->fields.contains(L"error") && !error) {
    state->Failure(L"malformed CDP error object"); return CallResult::kFailure;
  }
  if (!error && (parsed->fields.contains(L"code") || parsed->fields.contains(L"message"))) error = parsed;
  if (error) {
    const Json* code = error->Get(L"code", L'#');
    const Json* message = error->Get(L"message", L'"');
    if (code && message && (code->text == L"-32601" ||
        (code->text == L"-32000" && (message->text == L"Not allowed" ||
         message->text == L"Not supported" ||
         message->text == L"Browser context management is not supported."))))
      return CallResult::kDenied;
    state->Failure(L"CDP returned an error other than explicit unsupported/access denial");
    return CallResult::kFailure;
  }
  if (FAILED(pending->hr)) { state->Failure(L"CDP failed without an explicit protocol error"); return CallResult::kFailure; }
  return CallResult::kSuccess;
}

inline bool Run(HWND parent) {
  // Original required probe waits total about 153 seconds; reserve at most 15
  // more, including cleanup, under the existing 180-second CI process budget.
  const ULONGLONG deadline = GetTickCount64() + 15000, work_deadline = deadline - 3000;
  auto state = std::make_shared<State>();
  const std::string sdk = GESHER_WEBVIEW2_PROBE_SDK_VERSION, loader = LoaderKind();
  wprintf(L"INFO  browser-session: sdk=%ls loader=%ls (no Fetch or worker activation)\n",
          std::wstring(sdk.begin(), sdk.end()).c_str(), std::wstring(loader.begin(), loader.end()).c_str());
  wchar_t temp[MAX_PATH]{}, guid_text[40]{}; GUID guid{};
  DWORD length = GetTempPathW(MAX_PATH, temp);
  if (!length || length >= MAX_PATH || FAILED(CoCreateGuid(&guid)) || !StringFromGUID2(guid, guid_text, 40)) {
    state->Failure(L"cannot create a fresh profile identity"); return false;
  }
  const std::wstring profile = std::wstring(temp) + L"gesher-browser-session-" + guid_text;
  if (!CreateDirectoryW(profile.c_str(), nullptr)) { state->Failure(L"fresh profile directory creation failed"); return false; }
  wprintf(L"INFO  browser-session: fresh-profile=%ls\n", profile.c_str());
  bool available = false;
  const wchar_t* unavailable = L"no browser target observed";
  do {
    HRESULT hr = CreateEnvironment(nullptr, profile.c_str(), nullptr,
        GESHER_CALLBACK(ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler,
          [state](HRESULT result, ICoreWebView2Environment* environment) -> HRESULT {
            Result(L"CreateEnvironment", L"completion", result);
            if (!state->closed) {
              if (SUCCEEDED(result) && environment) state->environment = Com<ICoreWebView2Environment>(environment);
              state->environment_done = true;
            }
            return S_OK;
          }).get());
    Result(L"CreateEnvironment", L"immediate", hr);
    if (FAILED(hr) || !PumpUntil(work_deadline, [&] { return state->environment_done; }) || !state->environment) {
      state->Failure(L"environment setup failed or timed out"); break;
    }
    Com<ICoreWebView2Environment7> env7; CoString actual_profile, version;
    hr = state->environment->get_BrowserVersionString(version.Put());
    Result(L"runtime version", L"immediate", hr, version.str().c_str());
    if (FAILED(hr) || !state->environment.As(IID_ICoreWebView2Environment7, &env7) ||
        FAILED(env7->get_UserDataFolder(actual_profile.Put())) ||
        CompareStringOrdinal(profile.c_str(), -1, actual_profile.str().c_str(), -1, TRUE) != CSTR_EQUAL) {
      state->Failure(L"runtime version or fresh profile isolation could not be verified"); break;
    }
    hr = state->environment->CreateCoreWebView2Controller(parent,
        GESHER_CALLBACK(ICoreWebView2CreateCoreWebView2ControllerCompletedHandler,
          [state](HRESULT result, ICoreWebView2Controller* controller) -> HRESULT {
            Result(L"CreateController", L"completion", result);
            if (state->closed) {
              if (controller) {
                HRESULT hr = controller->Close(); Result(L"late controller.Close", L"immediate", hr);
                if (FAILED(hr)) state->Failure(L"late controller closure failed");
              }
              return S_OK;
            }
            if (SUCCEEDED(result) && controller) state->controller = Com<ICoreWebView2Controller>(controller);
            state->controller_done = true; return S_OK;
          }).get());
    Result(L"CreateController", L"immediate", hr);
    if (FAILED(hr) || !PumpUntil(work_deadline, [&] { return state->controller_done; }) || !state->controller ||
        FAILED(state->controller->put_IsVisible(FALSE)) || FAILED(state->controller->get_CoreWebView2(state->core.Put())) || !state->core) {
      state->Failure(L"controller setup failed or timed out"); break;
    }
    hr = state->core->QueryInterface(IID_ICoreWebView2_11, reinterpret_cast<void**>(state->sessions.Put()));
    Result(L"ICoreWebView2_11", L"QueryInterface", hr);
    if (hr == E_NOINTERFACE) { unavailable = L"session COM interface unavailable"; break; }
    if (FAILED(hr) || !state->sessions) { state->Failure(L"session interface query failed"); break; }
    hr = state->core->GetDevToolsProtocolEventReceiver(L"Target.targetCreated", state->receiver.Put());
    Result(L"targetCreated receiver", L"immediate", hr);
    if (FAILED(hr) || !state->receiver) { state->Failure(L"target event receiver failed"); break; }
    std::weak_ptr<State> weak = state;
    hr = state->receiver->add_DevToolsProtocolEventReceived(
        GESHER_CALLBACK(ICoreWebView2DevToolsProtocolEventReceivedEventHandler,
          [weak](ICoreWebView2*, ICoreWebView2DevToolsProtocolEventReceivedEventArgs* args) -> HRESULT {
            auto state = weak.lock(); if (!state || state->closed || state->failed) return S_OK;
            LPWSTR raw = nullptr;
            HRESULT hr = args ? args->get_ParameterObjectAsJson(&raw) : E_POINTER;
            Result(L"Target.targetCreated", L"event", hr, raw);
            std::wstring body(raw ? raw : L"", JsonLength(raw));
            CoTaskMemFree(raw);
            Json event;
            if (FAILED(hr) || !JsonReader(body).Read(&event)) state->Failure(L"malformed target event");
            else { const Json* info = event.Get(L"targetInfo", L'{');
              if (!info || !state->Observe(*info)) state->Failure(L"malformed target identity"); }
            return S_OK;
          }).get(), &state->event_token);
    Result(L"targetCreated subscription", L"immediate", hr);
    if (FAILED(hr)) { state->Failure(L"target event subscription failed"); break; }
    state->subscribed = true;
    Json enumeration;
    CallResult status = Call(state, work_deadline, L"Target.getTargets", L"{\"filter\":[{\"type\":\"browser\"}]}", &enumeration);
    if (status == CallResult::kFailure) break;
    if (status == CallResult::kSuccess) {
      const Json* infos = enumeration.Get(L"targetInfos", L'[');
      if (!infos) { state->Failure(L"target enumeration has no targetInfos array"); break; }
      for (const auto& info : infos->items) {
        if (!state->Observe(info)) { state->Failure(L"malformed enumerated target identity"); break; }
      }
      if (state->failed) break;
    }
    Json discovery; state->discovery_requested = true;
    status = Call(state, work_deadline, L"Target.setDiscoverTargets", L"{\"discover\":true,\"filter\":[{\"type\":\"browser\"}]}", &discovery);
    if (status == CallResult::kDenied) state->discovery_requested = false;
    if (status == CallResult::kFailure) break;
    if (status == CallResult::kSuccess) {
      if (!discovery.fields.empty()) { state->Failure(L"malformed discovery acknowledgement"); break; }
      PumpUntil(std::min(work_deadline, GetTickCount64() + 750), [] { return false; });
      if (GetTickCount64() >= work_deadline) { state->Failure(L"target observation exceeded the diagnostic deadline"); break; }
    }
    if (state->failed) break;
    // Only IDs structurally observed as browser targets above reach attach.
    for (const auto& id : state->browsers) {
      Json attached;
      status = Call(state, work_deadline, L"Target.attachToTarget",
                    L"{\"targetId\":" + Quote(id) + L",\"flatten\":true}", &attached);
      if (status == CallResult::kFailure) break;
      if (status == CallResult::kDenied) { unavailable = L"observed browser target refused attachment"; continue; }
      const Json* session = attached.Get(L"sessionId", L'"');
      if (!session || session->text.empty()) { state->Failure(L"attachment has no nonempty sessionId"); break; }
      state->attached.push_back(session->text);
      Json contexts;
      status = Call(state, work_deadline, L"Target.getBrowserContexts", L"{}", &contexts, session->text);
      if (status == CallResult::kFailure) break;
      if (status == CallResult::kDenied) { unavailable = L"browser-scope proof unavailable for attached target"; continue; }
      const Json* ids = contexts.Get(L"browserContextIds", L'[');
      if (!ids || std::any_of(ids->items.begin(), ids->items.end(), [](const Json& item) { return item.kind != L'"'; })) {
        state->Failure(L"browser-scope response has no valid context array"); break;
      }
      available = true; break;
    }
  } while (false);
  // Close still runs after timeout. Late setup callbacks retain state and close
  // a late controller; command callbacks own only their bounded response state.
  if (state->core) {
    for (const auto& session : state->attached) {
      Json ignored;
      if (Call(state, deadline, L"Target.detachFromTarget", L"{\"sessionId\":" + Quote(session) + L"}", &ignored) != CallResult::kSuccess || !ignored.fields.empty())
        state->Failure(L"session detach did not complete successfully");
    }
    if (state->discovery_requested) {
      Json ignored;
      if (Call(state, deadline, L"Target.setDiscoverTargets", L"{\"discover\":false}", &ignored) != CallResult::kSuccess || !ignored.fields.empty())
        state->Failure(L"discovery disable did not complete successfully");
    }
  }
  state->Close();
  wprintf(L"%ls  browser-session: %ls%ls; no Fetch or worker capability established\n",
      state->failed ? L"FAIL" : L"INFO", state->failed ? L"harness failure" :
      available ? L"browser scope verified through documented COM" : L"unavailable: ",
      state->failed || available ? L"" : unavailable);
  fflush(stdout);
  return !state->failed;
}
}  // namespace browser_session_probe
