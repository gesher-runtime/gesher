// Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0
// Test-only qualification of WebView2 response errors in REAL service workers.
// No Gesher worker API is enabled. Caller supplies an STA and an owner HWND.
#pragma once

#include <array>
#include <cstdio>
#include <iterator>
#include <mutex>

#include "com_util.h"
#include "owned_stream.h"
#include "browser_session_probe.h"
#include "worker_response_server.h"
#include "worker_denial_probe.h"

namespace worker_response_probe {
using namespace gesher::webview;
using browser_session_probe::Json;
using browser_session_probe::JsonReader;
using browser_session_probe::PumpUntil;

constexpr const wchar_t* kCases[] = {L"pass", L"fulfill", L"http403", L"first-read", L"first-abort", L"mid-body",
                                    L"uri-deferred", L"invoke-abort", L"followup"};
constexpr unsigned kUriDeferred = 6, kInvokeAbort = 7, kFollowup = 8, kCaseCount = 9;
static_assert(std::size(kCases) == kCaseCount);
constexpr uint64_t kBodySize = 131072, kMidFault = 65536;

inline void Hr(const wchar_t* operation, HRESULT hr) {
  wprintf(L"INFO  worker-response: %ls hr=0x%08lx\n", operation, static_cast<unsigned long>(hr));
  fflush(stdout);
}

struct StreamStats {
  std::atomic<unsigned> reads{0}, faults{0}, live{0};
  std::atomic<uint64_t> bytes{0};
};

// Immutable, fully allocated content with an intentional storage read failure.
// This does not model a progressive producer. It retains no controller/state.
class FaultStream final : public IStream, public IAgileObject {
 public:
  static HRESULT Create(SharedBody body, uint64_t fault_at, HRESULT error,
                        const std::shared_ptr<StreamStats>& stats, IStream** out,
                        uint64_t position = 0) {
    if (!out) return E_POINTER;
    *out = nullptr;
    auto* stream = new (std::nothrow) FaultStream(std::move(body), fault_at, error, stats);
    if (!stream) return E_OUTOFMEMORY;
    HRESULT hr = OwnedStream::Create(stream->body_, stream->source_.Put());
    LARGE_INTEGER offset{}; offset.QuadPart = static_cast<LONGLONG>(position);
    if (SUCCEEDED(hr)) hr = stream->Seek(offset, STREAM_SEEK_SET, nullptr);
    if (SUCCEEDED(hr)) hr = CoCreateFreeThreadedMarshaler(static_cast<IStream*>(stream), &stream->marshaler_);
    if (FAILED(hr)) { stream->Release(); return hr; }
    *out = stream; return S_OK;
  }
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
    if (!out) return E_POINTER;
    *out = nullptr;
    if (IsEqualIID(iid, IID_IUnknown) || IsEqualIID(iid, IID_IStream) || IsEqualIID(iid, IID_ISequentialStream))
      *out = static_cast<IStream*>(this);
    else if (IsEqualIID(iid, IID_IAgileObject)) *out = static_cast<IAgileObject*>(this);
    else if (IsEqualIID(iid, IID_IMarshal) && marshaler_) return marshaler_->QueryInterface(iid, out);
    else return E_NOINTERFACE;
    AddRef(); return S_OK;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
  ULONG STDMETHODCALLTYPE Release() override { ULONG left = --refs_; if (!left) delete this; return left; }
  HRESULT STDMETHODCALLTYPE Read(void* data, ULONG count, ULONG* read) override {
    if (read) *read = 0;
    if (!data && count) return STG_E_INVALIDPOINTER;
    std::lock_guard<std::mutex> lock(mutex_);
    ++stats_->reads;
    if (!count) return S_OK;
    if (position_ >= fault_at_) { ++stats_->faults; return error_; }
    const ULONG take = static_cast<ULONG>(std::min<uint64_t>(count, fault_at_ - position_));
    ULONG got = 0; HRESULT hr = source_->Read(data, take, &got);
    position_ += got; stats_->bytes += got;
    if (read) *read = got;
    if (FAILED(hr)) return hr;
    // A read spanning the failure returns the successfully copied prefix AND
    // the error, as IStream permits. Never invent EOF to avoid the error.
    if (take < count) { ++stats_->faults; return error_; }
    return hr;
  }
  HRESULT STDMETHODCALLTYPE Write(const void*, ULONG, ULONG* written) override {
    if (written) *written = 0; return STG_E_ACCESSDENIED;
  }
  HRESULT STDMETHODCALLTYPE Seek(LARGE_INTEGER move, DWORD origin, ULARGE_INTEGER* result) override {
    std::lock_guard<std::mutex> lock(mutex_); ULARGE_INTEGER position{};
    HRESULT hr = source_->Seek(move, origin, &position);
    if (SUCCEEDED(hr)) { position_ = position.QuadPart; if (result) *result = position; }
    return hr;
  }
  HRESULT STDMETHODCALLTYPE SetSize(ULARGE_INTEGER) override { return STG_E_ACCESSDENIED; }
  HRESULT STDMETHODCALLTYPE CopyTo(IStream* target, ULARGE_INTEGER count, ULARGE_INTEGER* read,
                                    ULARGE_INTEGER* written) override {
    if (read) read->QuadPart = 0;
    if (written) written->QuadPart = 0;
    if (!target) return STG_E_INVALIDPOINTER;
    uint64_t total = 0, copied = 0; unsigned char bytes[16384];
    while (total < count.QuadPart) {
      ULONG got = 0, put = 0;
      HRESULT hr = Read(bytes, static_cast<ULONG>(std::min<uint64_t>(sizeof(bytes), count.QuadPart - total)), &got);
      total += got; if (read) read->QuadPart = total;
      if (got) {
        HRESULT write_hr = target->Write(bytes, got, &put);
        if (put > got) return STG_E_WRITEFAULT;
        copied += put; if (written) written->QuadPart = copied;
        if (FAILED(write_hr)) return write_hr;
        if (put != got) return STG_E_WRITEFAULT;
      }
      if (FAILED(hr)) return hr;
      if (!got || hr == S_FALSE) break;
    }
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE Commit(DWORD) override { return S_OK; }
  HRESULT STDMETHODCALLTYPE Revert() override { return S_OK; }
  HRESULT STDMETHODCALLTYPE LockRegion(ULARGE_INTEGER, ULARGE_INTEGER, DWORD) override { return STG_E_INVALIDFUNCTION; }
  HRESULT STDMETHODCALLTYPE UnlockRegion(ULARGE_INTEGER, ULARGE_INTEGER, DWORD) override { return STG_E_INVALIDFUNCTION; }
  HRESULT STDMETHODCALLTYPE Stat(STATSTG* stat, DWORD flags) override { return source_->Stat(stat, flags); }
  HRESULT STDMETHODCALLTYPE Clone(IStream** out) override {
    uint64_t position;
    { std::lock_guard<std::mutex> lock(mutex_); position = position_; }
    return Create(body_, fault_at_, error_, stats_, out, position);
  }
 private:
  FaultStream(SharedBody body, uint64_t fault_at, HRESULT error, std::shared_ptr<StreamStats> stats)
      : body_(std::move(body)), fault_at_(fault_at), error_(error), stats_(std::move(stats)) { ++stats_->live; }
  ~FaultStream() { if (marshaler_) marshaler_->Release(); --stats_->live; }
  std::atomic<ULONG> refs_{1};
  const SharedBody body_;
  const uint64_t fault_at_;
  const HRESULT error_;
  const std::shared_ptr<StreamStats> stats_;
  Com<IStream> source_;
  IUnknown* marshaler_ = nullptr;
  std::mutex mutex_;
  uint64_t position_ = 0;
};

inline const Json* Field(const Json& object, const wchar_t* name, wchar_t type) { return object.Get(name, type); }
inline bool Text(const Json& object, const wchar_t* name, const wchar_t* value) {
  const Json* field = Field(object, name, L'"'); return field && field->text == value;
}
inline bool Number(const Json& object, const wchar_t* name, uint64_t* value) {
  const Json* field = Field(object, name, L'#');
  if (!field || field->text.empty() || field->text.size() > 7) return false;
  *value = 0;
  for (wchar_t c : field->text) { if (c < L'0' || c > L'9') return false; *value = *value * 10 + c - L'0'; }
  return true;
}

struct State {
  Com<ICoreWebView2Environment> environment;
  Com<ICoreWebView2Controller> controller;
  Com<ICoreWebView2> core;
  Com<ICoreWebView2_22> filter;
  Com<ICoreWebView2_18> external;
  Com<ICoreWebView2WebResourceRequest> deferred_request;
  Com<ICoreWebView2WebResourceRequestedEventArgs> deferred_args;
  Com<ICoreWebView2Deferral> uri_deferral;
  EventRegistrationToken resource_token{}, message_token{}, external_token{};
  std::wstring origin, pattern, denied_uri, worker_identity;
  std::array<unsigned, kCaseCount> intercepted{};
  std::array<std::shared_ptr<StreamStats>, kCaseCount> streams{};
  std::array<bool, kCaseCount> rejected{}, type_error{}, request_identity{}, timing_identity{};
  unsigned observations = 0;
  unsigned external_launches = 0, uri_completions = 0, abort_returns = 0;
  bool environment_done = false, controller_done = false, closed = false, failed = false;
  bool filtering = false, resources = false, messages = false, external_guard = false, in_request = false;
  bool page = false, document = false, worker = false, complete = false;

  void Failure(const wchar_t* reason) {
    failed = true; wprintf(L"FAIL  worker-response: %ls\n", reason); fflush(stdout);
  }
  void Message(const Json& value) {
    if (Text(value, L"event", L"page-ready")) {
      if (page || !Field(value, L"secure", L't')) Failure(L"page is duplicate or not a secure context");
      page = true;
    } else if (Text(value, L"event", L"document")) {
      uint64_t status = 0;
      if (!page || document || !Number(value, L"status", &status) || status != 200 ||
          !Text(value, L"body", L"origin:document")) Failure(L"document network control failed");
      document = true;
    } else if (Text(value, L"event", L"worker-ready")) {
      const Json* script = Field(value, L"script", L'"'); const Json* scope = Field(value, L"scope", L'"');
      if (!document || worker || !script || script->text != origin + L"/sw.js" || !scope ||
          scope->text != origin + L"/" || !Text(value, L"state", L"activated")) Failure(L"actual worker identity/activation failed");
      worker = true;
    } else if (Text(value, L"event", L"case")) {
      if (!worker || observations >= kCaseCount || !Text(value, L"name", kCases[observations])) {
        Failure(L"unexpected, duplicate or unordered worker observation"); return;
      }
      const unsigned i = observations++;
      uint64_t status = 0, bytes = 0, timing_count = 0;
      const Json* error = Field(value, L"error", L'"'); const Json* text = Field(value, L"text", L'"');
      const Json* error_name = Field(value, L"errorName", L'"');
      const Json* identity = Field(value, L"workerIdentity", L'"');
      const std::wstring expected_url = origin + L"/network/" + kCases[i];
      if (!Number(value, L"status", &status) || status > 599 || !Number(value, L"bytes", &bytes) || bytes > 262144 ||
          !Number(value, L"timingCount", &timing_count) || timing_count > 16 ||
          !error || !error_name || !text || !Field(value, L"originHeader", L'"') ||
          !Field(value, L"requestUrl", L'"') || !Field(value, L"finalRequestUrl", L'"') ||
          !Field(value, L"responseUrl", L'"') || !Field(value, L"timingName", L'"') ||
          !Field(value, L"timingType", L'"') || !Field(value, L"timedOut", L'f') || !Field(value, L"aborted", L'f') ||
          !identity || identity->text.empty() || identity->text.size() > 64 ||
          !Text(value, L"workerScript", (origin + L"/sw.js").c_str())) {
        Failure(L"malformed or timed-out worker observation"); return;
      }
      if (i == 0) worker_identity = identity->text;
      if (identity->text != worker_identity) { Failure(L"worker restarted between diagnostic requests"); return; }
      request_identity[i] = Text(value, L"requestUrl", expected_url.c_str()) &&
                            Text(value, L"finalRequestUrl", expected_url.c_str());
      timing_identity[i] = timing_count == 1 && Text(value, L"timingName", expected_url.c_str()) &&
                           Text(value, L"timingType", L"fetch");
      if (i < 3 || i == kFollowup) {
        const wchar_t* expected = i == 0 ? L"origin:pass" : i == 1 ? L"supplied" : i == 2 ? L"denied" : L"origin:followup";
        if (!Text(value, L"fetch", L"resolved") || !Text(value, L"body", L"complete") ||
            status != (i == 2 ? 403u : 200u) || !Text(value, L"text", expected) ||
            bytes != wcslen(expected) || !error->text.empty() || !error_name->text.empty() ||
            !request_identity[i] || !timing_identity[i] || !Text(value, L"responseUrl", expected_url.c_str()) ||
            !Text(value, L"originHeader", (i == 0 || i == kFollowup) ? L"origin" : L"intercepted"))
          Failure(L"pass-through, supplied-response, HTTP403 or worker follow-up control failed");
      } else if (Text(value, L"fetch", L"rejected")) {
        if (status || bytes || !Text(value, L"body", L"none") || error->text.empty() ||
            error_name->text.empty() || !Text(value, L"responseUrl", L"") ||
            !Text(value, L"originHeader", L"") || !text->text.empty()) Failure(L"inconsistent pre-response rejection");
        rejected[i] = true;
        type_error[i] = Text(value, L"errorName", L"TypeError");
      } else if (!Text(value, L"fetch", L"resolved") ||
                 (i < kUriDeferred && (status != 200 || !Text(value, L"originHeader", L"intercepted"))) ||
                 (!Text(value, L"body", L"complete") && !Text(value, L"body", L"rejected")) ||
                 (Text(value, L"body", L"rejected") && (error->text.empty() || error_name->text.empty())) ||
                 (Text(value, L"body", L"complete") && (!error->text.empty() || !error_name->text.empty()))) {
        Failure(L"inconsistent response/body error observation");
      }
    } else if (Text(value, L"event", L"complete")) {
      if (complete || observations != kCaseCount || !Field(value, L"unregistered", L't')) Failure(L"incomplete qualification or worker unregister failed");
      complete = true;
    } else Failure(L"page/worker reported a failure or unknown event");
  }

  HRESULT Request(ICoreWebView2WebResourceRequestedEventArgs* args) {
    Com<ICoreWebView2WebResourceRequestedEventArgs2> source;
    Com<ICoreWebView2WebResourceRequest> request; CoString uri, method;
    COREWEBVIEW2_WEB_RESOURCE_REQUEST_SOURCE_KINDS kind{};
    if (!args || FAILED(args->QueryInterface(IID_ICoreWebView2WebResourceRequestedEventArgs2,
                                            reinterpret_cast<void**>(source.Put()))) ||
        FAILED(source->get_RequestedSourceKind(&kind)) ||
        kind != COREWEBVIEW2_WEB_RESOURCE_REQUEST_SOURCE_KINDS_SERVICE_WORKER ||
        FAILED(args->get_Request(request.Put())) || !request || FAILED(request->get_Uri(uri.Put())) ||
        FAILED(request->get_Method(method.Put())) || method.str() != L"GET") {
      Failure(L"request source is not a verified SERVICE_WORKER"); return S_OK;
    }
    unsigned i = 0;
    for (; i < kCaseCount; ++i) if (uri.str() == origin + L"/network/" + kCases[i]) break;
    if (i == kCaseCount || ++intercepted[i] != 1) { Failure(L"unexpected or repeated intercepted request"); return S_OK; }
    wprintf(L"INFO  worker-response: request case=%ls source=SERVICE_WORKER\n", kCases[i]);
    if (i == kUriDeferred || i == kInvokeAbort) {
      Com<ICoreWebView2WebResourceResponse> response;
      HRESULT hr = args->get_Response(response.Put()); Hr(L"denial initial response", hr);
      if (FAILED(hr) || response) { Failure(L"denial case already has a response"); return S_OK; }
      if (i == kInvokeAbort) {
        ++abort_returns;
        Hr(L"invoke-abort synchronous return; response unset; no deferral", E_ABORT);
        return E_ABORT;
      }
      if (uri_deferral) { Failure(L"duplicate deferred URI request"); return S_OK; }
      hr = args->GetDeferral(uri_deferral.Put()); Hr(L"uri-deferred GetDeferral", hr);
      if (FAILED(hr) || !uri_deferral) { Failure(L"URI request deferral failed"); return S_OK; }
      deferred_request = request;
      deferred_args = Com<ICoreWebView2WebResourceRequestedEventArgs>(args);
      // The outer STA pump completes this after DispatchMessage and Invoke
      // have returned. No nested pump, worker debugger, or fabricated response.
      return S_OK;
    }
    Com<ICoreWebView2Deferral> deferral;
    HRESULT hr = args->GetDeferral(deferral.Put()); Hr(L"GetDeferral", hr);
    if (FAILED(hr) || !deferral) { Failure(L"request deferral failed"); return S_OK; }
    if (i != 0 && i != kFollowup) {
      Com<IStream> content;
      const std::string body = i == 1 ? "supplied" : "denied";
      auto bytes = std::make_shared<const std::vector<uint8_t>>(i < 3
          ? std::vector<uint8_t>(body.begin(), body.end()) : std::vector<uint8_t>(kBodySize, 'A'));
      if (i < 3) hr = OwnedStream::Create(bytes, content.Put());
      else {
        streams[i] = std::make_shared<StreamStats>();
        hr = FaultStream::Create(bytes, i == 5 ? kMidFault : 0,
                                i == 4 ? E_ABORT : STG_E_READFAULT, streams[i], content.Put());
      }
      Hr(L"response stream creation", hr);
      Com<ICoreWebView2WebResourceResponse> response;
      const std::wstring headers = L"Content-Type: text/plain\r\nCache-Control: no-store\r\n"
          L"X-Probe-Origin: intercepted\r\nContent-Length: " + std::to_wstring(bytes->size()) + L"\r\n";
      if (SUCCEEDED(hr)) {
        hr = environment->CreateWebResourceResponse(content.get(), i == 2 ? 403 : 200,
                                                     i == 2 ? L"Forbidden" : L"OK", headers.c_str(), response.Put());
        Hr(L"CreateWebResourceResponse", hr);
      }
      if (SUCCEEDED(hr) && response) { hr = args->put_Response(response.get()); Hr(L"put_Response", hr); }
      if (FAILED(hr) || !response) Failure(L"response injection failed; no error-semantics conclusion is valid");
    }
    hr = deferral->Complete(); Hr(L"deferral.Complete", hr);
    if (FAILED(hr)) Failure(L"request deferral completion failed");
    return S_OK;
  }

  void CompleteDeferredUri() {
    if (!uri_deferral || closed || failed) return;
    if (in_request) { Failure(L"URI mutation attempted before Invoke returned"); return; }
    CoString original, rewritten;
    Com<ICoreWebView2WebResourceResponse> response;
    if (!deferred_request || !deferred_args ||
        FAILED(deferred_request->get_Uri(original.Put())) ||
        original.str() != origin + L"/network/uri-deferred" ||
        FAILED(deferred_args->get_Response(response.Put())) || response) {
      Failure(L"deferred request identity or unset response changed"); return;
    }
    HRESULT hr = deferred_request->put_Uri(denied_uri.c_str());
    Hr(L"uri-deferred put_Uri after Invoke returned", hr);
    if (FAILED(hr) || FAILED(deferred_request->get_Uri(rewritten.Put())) || rewritten.str() != denied_uri) {
      // Do not intentionally release the original request on a failed mutation.
      // Retain the deferral until controller cleanup; this is a harness failure,
      // never evidence that a network-denial primitive exists.
      Failure(L"non-network URI mutation was not accepted exactly"); return;
    }
    wprintf(L"INFO  worker-response: uri-deferred original=%ls rewritten=%ls response=unset after-invoke=yes\n",
            original.str().c_str(), rewritten.str().c_str());
    hr = uri_deferral->Complete(); Hr(L"uri-deferred Complete", hr);
    if (FAILED(hr)) Failure(L"deferred URI completion failed");
    else ++uri_completions;
    uri_deferral.Reset(); deferred_args.Reset(); deferred_request.Reset();
  }

  void Close() {
    closed = true;
    if (filtering) {
      HRESULT hr = filter->RemoveWebResourceRequestedFilterWithRequestSourceKinds(pattern.c_str(),
          COREWEBVIEW2_WEB_RESOURCE_CONTEXT_ALL, COREWEBVIEW2_WEB_RESOURCE_REQUEST_SOURCE_KINDS_SERVICE_WORKER);
      Hr(L"Remove SERVICE_WORKER filter", hr); if (FAILED(hr)) Failure(L"filter removal failed");
    }
    if (resources) { HRESULT hr = core->remove_WebResourceRequested(resource_token); Hr(L"remove request callback", hr); if (FAILED(hr)) Failure(L"request callback removal failed"); }
    if (messages) { HRESULT hr = core->remove_WebMessageReceived(message_token); Hr(L"remove message callback", hr); if (FAILED(hr)) Failure(L"message callback removal failed"); }
    if (controller) { HRESULT hr = controller->Close(); Hr(L"controller.Close", hr); if (FAILED(hr)) Failure(L"controller close failed"); }
    // Keep the external-scheme guard active through controller closure, including
    // failure cleanup of an outstanding request. A launch attempt invalidates
    // qualification even if the guard successfully cancels it.
    if (external_guard) { HRESULT hr = external->remove_LaunchingExternalUriScheme(external_token); Hr(L"remove external-scheme guard", hr); if (FAILED(hr) && hr != HRESULT_FROM_WIN32(ERROR_INVALID_STATE)) Failure(L"external-scheme guard removal failed"); }
    uri_deferral.Reset(); deferred_args.Reset(); deferred_request.Reset();
    external.Reset(); filter.Reset(); core.Reset(); controller.Reset(); environment.Reset();
  }
};

// 0: all controls/observations valid (NOT a supported-worker capability claim).
// 1: harness/timeout/cleanup failure. 2: required SDK runtime interface absent.
inline int Run(HWND parent) {
  const ULONGLONG deadline = GetTickCount64() + 120000, work_deadline = deadline - 6000;
  auto state = std::make_shared<State>(); LoopbackServer server;
  const std::string sdk = GESHER_WEBVIEW2_PROBE_SDK_VERSION, loader = LoaderKind();
  wprintf(L"INFO  worker-response: sdk=%ls loader=%ls; real worker; diagnostic only\n",
          std::wstring(sdk.begin(), sdk.end()).c_str(), std::wstring(loader.begin(), loader.end()).c_str());
  if (!CheckLoopbackRequestAccounting(work_deadline)) {
    state->Failure(L"independent HTTP byte accounting controls failed"); return 1;
  }
  wprintf(L"PASS  worker-response: independent HTTP byte accounting controls verified\n");
  wchar_t temp[MAX_PATH]{}, identity[40]{}; GUID guid{};
  const DWORD length = GetTempPathW(MAX_PATH, temp);
  if (!parent || !length || length >= MAX_PATH || FAILED(CoCreateGuid(&guid)) ||
      !StringFromGUID2(guid, identity, 40) || !server.Start(deadline)) {
    state->Failure(L"owner, fresh profile identity or loopback server setup failed"); return 1;
  }
  const std::wstring profile = std::wstring(temp) + L"gesher-worker-response-" + identity;
  if (!CreateDirectoryW(profile.c_str(), nullptr)) { state->Failure(L"fresh profile creation failed"); return 1; }
  std::wstring scheme = L"gesher-denial-";
  for (const wchar_t* c = identity; *c; ++c) {
    if (*c >= L'0' && *c <= L'9') scheme += *c;
    else if (*c >= L'A' && *c <= L'F') scheme += *c - L'A' + L'a';
    else if (*c >= L'a' && *c <= L'f') scheme += *c;
  }
  if (scheme.size() != 46) { state->Failure(L"fresh denial scheme has no complete GUID"); return 1; }
  for (REGSAM view : {KEY_WOW64_32KEY, KEY_WOW64_64KEY}) {
    HKEY registered = nullptr;
    const LSTATUS registration = RegOpenKeyExW(HKEY_CLASSES_ROOT, scheme.c_str(), 0, KEY_READ | view, &registered);
    if (registered) RegCloseKey(registered);
    if (registration != ERROR_FILE_NOT_FOUND && registration != ERROR_PATH_NOT_FOUND) {
      state->Failure(L"fresh denial scheme is registered or registration could not be checked"); return 1;
    }
  }
  // No authority/host or network scheme, no custom-scheme registration, and no
  // changes to the OS registry. The unique scheme cannot identify a server.
  state->denied_uri = scheme + L":blocked";
  state->origin = server.Origin(); state->pattern = state->origin + L"/network/*";
  wprintf(L"INFO  worker-response: profile=%ls origin=%ls\n", profile.c_str(), state->origin.c_str());
  const wchar_t* unavailable = nullptr;
  do {
    HRESULT hr = CreateEnvironment(nullptr, profile.c_str(), nullptr,
        GESHER_CALLBACK(ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler,
          [state](HRESULT result, ICoreWebView2Environment* environment) -> HRESULT {
            Hr(L"CreateEnvironment completion", result);
            if (!state->closed) {
              if (SUCCEEDED(result) && environment) state->environment = Com<ICoreWebView2Environment>(environment);
              state->environment_done = true;
            }
            return S_OK;
          }).get());
    Hr(L"CreateEnvironment immediate", hr);
    if (FAILED(hr) || !PumpUntil(work_deadline, [&] { return state->environment_done; }) || !state->environment) {
      state->Failure(L"environment setup failed or timed out"); break;
    }
    CoString runtime, actual_profile; Com<ICoreWebView2Environment7> env7;
    hr = state->environment->get_BrowserVersionString(runtime.Put()); Hr(L"runtime version", hr);
    wprintf(L"INFO  worker-response: runtime=%ls\n", runtime.str().c_str());
    if (FAILED(hr) || !state->environment.As(IID_ICoreWebView2Environment7, &env7) ||
        FAILED(env7->get_UserDataFolder(actual_profile.Put())) ||
        CompareStringOrdinal(profile.c_str(), -1, actual_profile.str().c_str(), -1, TRUE) != CSTR_EQUAL) {
      state->Failure(L"fresh profile ownership could not be verified"); break;
    }
    hr = state->environment->CreateCoreWebView2Controller(parent,
        GESHER_CALLBACK(ICoreWebView2CreateCoreWebView2ControllerCompletedHandler,
          [state](HRESULT result, ICoreWebView2Controller* controller) -> HRESULT {
            Hr(L"CreateController completion", result);
            if (state->closed) { if (controller) Hr(L"late controller.Close", controller->Close()); return S_OK; }
            if (SUCCEEDED(result) && controller) state->controller = Com<ICoreWebView2Controller>(controller);
            state->controller_done = true; return S_OK;
          }).get());
    Hr(L"CreateController immediate", hr);
    if (FAILED(hr) || !PumpUntil(work_deadline, [&] { return state->controller_done; }) || !state->controller ||
        FAILED(state->controller->put_IsVisible(FALSE)) || FAILED(state->controller->get_CoreWebView2(state->core.Put())) || !state->core) {
      state->Failure(L"controller setup failed or timed out"); break;
    }
    hr = state->core->QueryInterface(IID_ICoreWebView2_22, reinterpret_cast<void**>(state->filter.Put()));
    Hr(L"ICoreWebView2_22 QueryInterface", hr);
    if (hr == E_NOINTERFACE) { unavailable = L"SERVICE_WORKER filter interface absent"; break; }
    if (FAILED(hr) || !state->filter) { state->Failure(L"request-source interface query failed"); break; }
    hr = state->core->QueryInterface(IID_ICoreWebView2_18, reinterpret_cast<void**>(state->external.Put()));
    Hr(L"ICoreWebView2_18 QueryInterface", hr);
    if (hr == E_NOINTERFACE) { unavailable = L"external-scheme guard interface absent"; break; }
    if (FAILED(hr) || !state->external) { state->Failure(L"external-scheme guard interface query failed"); break; }
    std::weak_ptr<State> weak = state;
    hr = state->external->add_LaunchingExternalUriScheme(
        GESHER_CALLBACK(ICoreWebView2LaunchingExternalUriSchemeEventHandler,
          [weak](ICoreWebView2*, ICoreWebView2LaunchingExternalUriSchemeEventArgs* args) -> HRESULT {
            // Always cancel, including cleanup or a late callback after State was
            // released. This event must never be used as the denial mechanism.
            const HRESULT hr = args ? args->put_Cancel(TRUE) : E_POINTER;
            Hr(L"external URI launch guard put_Cancel", hr);
            if (auto state = weak.lock()) {
              ++state->external_launches;
              state->Failure(FAILED(hr) ? L"external URI launch guard failed" :
                                        L"external URI launch attempted; URI denial is not qualified");
            }
            return hr;
          }).get(), &state->external_token);
    Hr(L"add external-scheme guard", hr);
    if (FAILED(hr)) { state->Failure(L"external-scheme guard subscription failed"); break; }
    state->external_guard = true;
    hr = state->core->add_WebResourceRequested(
        GESHER_CALLBACK(ICoreWebView2WebResourceRequestedEventHandler,
          [weak](ICoreWebView2*, ICoreWebView2WebResourceRequestedEventArgs* args) -> HRESULT {
            auto state = weak.lock();
            if (!state || state->closed || state->failed) return S_OK;
            state->in_request = true;
            const HRESULT hr = state->Request(args);
            state->in_request = false;
            return hr;
          }).get(), &state->resource_token);
    Hr(L"add request callback", hr);
    if (FAILED(hr)) { state->Failure(L"request subscription failed"); break; }
    state->resources = true;
    hr = state->filter->AddWebResourceRequestedFilterWithRequestSourceKinds(state->pattern.c_str(),
        COREWEBVIEW2_WEB_RESOURCE_CONTEXT_ALL, COREWEBVIEW2_WEB_RESOURCE_REQUEST_SOURCE_KINDS_SERVICE_WORKER);
    Hr(L"Add SERVICE_WORKER filter", hr);
    if (FAILED(hr)) { state->Failure(L"SERVICE_WORKER filter admission failed"); break; }
    state->filtering = true;
    hr = state->core->add_WebMessageReceived(
        GESHER_CALLBACK(ICoreWebView2WebMessageReceivedEventHandler,
          [weak](ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT {
            auto state = weak.lock(); if (!state || state->closed || state->failed) return S_OK;
            CoString source; LPWSTR raw = nullptr;
            HRESULT hr = args ? args->get_Source(source.Put()) : E_POINTER;
            if (SUCCEEDED(hr)) hr = args->get_WebMessageAsJson(&raw);
            const size_t size = browser_session_probe::JsonLength(raw);
            wprintf(L"INFO  worker-response: message hr=0x%08lx json=%.*ls\n", static_cast<unsigned long>(hr),
                    static_cast<int>(std::min<size_t>(size, 4096)), raw ? raw : L""); fflush(stdout);
            Json value;
            if (FAILED(hr) || source.str() != state->origin + L"/index.html" || !raw || size > 4096 ||
                !JsonReader(std::wstring_view(raw, size)).Read(&value)) state->Failure(L"invalid page message source/JSON");
            else state->Message(value);
            CoTaskMemFree(raw); return S_OK;
          }).get(), &state->message_token);
    Hr(L"add message callback", hr);
    if (FAILED(hr)) { state->Failure(L"message subscription failed"); break; }
    state->messages = true;
    hr = state->core->Navigate((state->origin + L"/index.html").c_str()); Hr(L"Navigate", hr);
    if (FAILED(hr) || !PumpUntil(work_deadline, [&] {
          state->CompleteDeferredUri();
          return state->failed || state->complete;
        }))
      state->Failure(L"actual worker qualification did not complete within its deadline");
  } while (false);
  state->Close();
  PumpUntil(std::min(deadline - 5000, GetTickCount64() + 250), [] { return false; });
  if (!server.Stop() || !server.Healthy()) state->Failure(L"loopback origin failed or did not stop");
  unsigned known = server.Count("/index.html") + server.Count("/sw.js") + server.Count("/favicon.ico") +
                   server.Count("/network/document");
  for (unsigned i = 0; i < kCaseCount; ++i) {
    const std::wstring name(kCases[i]);
    known += server.Count("/network/" + std::string(name.begin(), name.end()));
  }
  const unsigned unexpected = server.TotalCount() - known;
  wprintf(L"INFO  worker-response: unknown-origin-requests=%u\n", unexpected);
  if (unexpected) state->Failure(L"independent origin received an unexpected alternate request");
  if (!unavailable && !state->failed) {
    if (!state->complete || server.Count("/index.html") != 1 || server.Count("/sw.js") == 0 ||
        server.Count("/network/document") != 1) state->Failure(L"independent page/script/document counters failed");
    for (unsigned i = 0; i < kCaseCount; ++i) {
      const std::wstring name(kCases[i]); const unsigned count = server.Count("/network/" + std::string(name.begin(), name.end()));
      wprintf(L"INFO  worker-response: case=%ls intercepted=%u origin-network=%u\n", kCases[i], state->intercepted[i], count);
      const bool denial = i == kUriDeferred || i == kInvokeAbort;
      if (state->intercepted[i] != 1 || (denial ? count > 1 : count != (i == 0 || i == kFollowup ? 1u : 0u)))
        state->Failure(L"independent origin counter detects an unexpected/missing request");
      if (i >= 3 && i < kUriDeferred) {
        const auto& stats = state->streams[i];
        if (!stats || !stats->faults || (i == 5 ? stats->bytes == 0 : stats->bytes != 0)) {
          state->Failure(L"failing stream was not exercised at the intended read boundary"); continue;
        }
        wprintf(L"INFO  worker-response: case=%ls read-hr=0x%08lx reads=%u faults=%u supplied-prefix=%llu live-streams=%u pre-response-rejection=%ls\n",
                kCases[i], static_cast<unsigned long>(i == 4 ? E_ABORT : STG_E_READFAULT), stats->reads.load(),
                stats->faults.load(), static_cast<unsigned long long>(stats->bytes.load()), stats->live.load(),
                state->rejected[i] ? L"yes" : L"no");
      }
      if (denial) {
        const bool observed = state->rejected[i] && state->type_error[i] && count == 0 &&
                              state->request_identity[i] && state->timing_identity[i] && !state->external_launches;
        wprintf(L"INFO  worker-response: denial case=%ls initial-typeerror=%ls request-identity=%ls timing-identity=%ls origin-network=%u outcome=%ls\n",
                kCases[i], state->rejected[i] && state->type_error[i] ? L"yes" : L"no",
                state->request_identity[i] ? L"yes" : L"no", state->timing_identity[i] ? L"yes" : L"no", count,
                observed ? L"network-rejection-observed" : L"not-qualified");
      }
    }
    if (state->uri_completions != 1 || state->abort_returns != 1 || state->external_launches)
      state->Failure(L"new denial primitive or external-scheme guard was not exercised as intended");
    wprintf(L"INFO  worker-response: deferred-uri-completions=%u invoke-abort-returns=%u external-launches=%u followup-same-worker=%ls\n",
            state->uri_completions, state->abort_returns, state->external_launches,
            state->complete && state->intercepted[kFollowup] == 1 ? L"yes" : L"no");
  }
  if (unavailable && !state->failed)
    wprintf(L"INFO  worker-response: unavailable: %ls; cases not run; no production worker capability enabled\n", unavailable);
  else wprintf(L"%ls  worker-response: %ls; no production worker capability enabled\n",
               state->failed ? L"FAIL" : L"PASS", state->failed ? L"harness failed; no qualification conclusion" :
               L"controls and independent network counters verified; error outcomes recorded");
  fflush(stdout);
  if (state->failed || unavailable) return state->failed ? 1 : 2;
  return worker_denial_probe::Run(parent, deadline);
}
}  // namespace worker_response_probe
