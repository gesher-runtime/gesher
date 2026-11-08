// Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0
//
// Opt-in helper regression, without creating a WebView, profile or network connection:
//   cmake ... -DGESHER_WEBVIEW_BRIDGE_TESTS=ON
//   cmake --build <build-dir> --target webview_bridge_test
//   <build-dir>/webview_bridge_test.exe
//
// Include the implementation to test its actual private parsing/cancellation helpers.
// COM request bodies use real Windows memory streams; completion counters stand in
// for already-denied request deferrals. This does not exercise live navigation
// events, Host::Destroy, HTTP interception or the WebView2 renderer. The separate
// bridgecookies APK covers real synchronous calls, cookies and origin non-leakage.
#include "../../compat/webview/webview_host.cc"

#include <assert.h>

#ifdef NDEBUG
#error This test requires active assertions; compile with -UNDEBUG.
#endif

using namespace gesher::webview;

namespace {

class BodyRequest final : public ICoreWebView2WebResourceRequest {
 public:
  explicit BodyRequest(const std::vector<uint8_t>& bytes)
      : content_(SHCreateMemStream(bytes.data(), static_cast<UINT>(bytes.size())), false) {}

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void** out) override {
    *out = nullptr;
    return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return 2; }
  ULONG STDMETHODCALLTYPE Release() override { return 1; }
  HRESULT STDMETHODCALLTYPE get_Uri(LPWSTR*) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE put_Uri(LPCWSTR) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE get_Method(LPWSTR*) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE put_Method(LPCWSTR) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE get_Content(IStream** out) override {
    *out = content_.get();
    if (*out != nullptr) (*out)->AddRef();
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE put_Content(IStream*) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE get_Headers(ICoreWebView2HttpRequestHeaders**) override {
    return E_NOTIMPL;
  }

 private:
  Com<IStream> content_;
};

class Completion final : public ICoreWebView2Deferral {
 public:
  explicit Completion(int* count) : count_(count) {}
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void** out) override {
    *out = nullptr;
    return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
  ULONG STDMETHODCALLTYPE Release() override {
    ULONG remaining = --refs_;
    if (remaining == 0) delete this;
    return remaining;
  }
  HRESULT STDMETHODCALLTYPE Complete() override {
    ++*count_;
    return S_OK;
  }

 private:
  ULONG refs_ = 1;
  int* count_;
};

void PathAndBody() {
  assert(BridgePath(L"https://example.invalid/.gesher-js-bridge/stale?q=1")
         == L"/.gesher-js-bridge/stale");
  assert(BridgePath(L"https://example.invalid/ordinary?q=/.gesher-js-bridge/stale")
         == L"/ordinary");
  assert(BridgePath(L"data:text/plain,test").empty());

  std::wstring out;
  BodyRequest valid({'{', '"', 'v', '"', ':', '"', 0xe2, 0x98, 0x83, '"', '}'});
  assert(BridgeBody(&valid, &out) && out == L"{\"v\":\"\u2603\"}");
  BodyRequest invalid({0xc0, 0xaf});
  assert(!BridgeBody(&invalid, &out));
  BodyRequest nul({'a', 0, 'b'});
  assert(!BridgeBody(&nul, &out));
  BodyRequest empty({});
  assert(!BridgeBody(&empty, &out));
  BodyRequest limit(std::vector<uint8_t>(kBridgeBodyLimit, 'x'));
  assert(BridgeBody(&limit, &out));
  BodyRequest oversized(std::vector<uint8_t>(kBridgeBodyLimit + 1, 'x'));
  assert(!BridgeBody(&oversized, &out));
}

void CancellationAndTimeoutHelpers() {
  Host::Impl impl;
  int canceled = 0, ordinary = 0, expired = 0;
  Host::Impl::Request first;
  first.bridge = true;
  first.deferral = Com<ICoreWebView2Deferral>(new Completion(&canceled), false);
  Host::Impl::Request other;
  other.deferral = Com<ICoreWebView2Deferral>(new Completion(&ordinary), false);
  impl.requests.emplace(1, std::move(first));
  impl.requests.emplace(2, std::move(other));

  CancelBridgeRequests(&impl);
  assert(canceled == 1 && ordinary == 0 && impl.requests.size() == 1
         && impl.bridge_generation == 1);
  CancelBridgeRequests(&impl);
  assert(canceled == 1); // Duplicate cleanup cannot complete a removed deferral again.

  Host::Impl::Request late;
  late.bridge = true;
  late.deadline = 0;
  late.deferral = Com<ICoreWebView2Deferral>(new Completion(&expired), false);
  impl.requests.emplace(3, std::move(late));
  g_live.insert(&impl);
  BridgeTimeouts(nullptr, 0, 0, 0);
  g_live.erase(&impl);
  assert(expired == 1 && ordinary == 0 && impl.requests.size() == 1);
}

std::wstring Prepare(Bridge* bridge, HRESULT* outcome) {
  LPOLESTR name = const_cast<LPOLESTR>(L"prepareCall");
  DISPID id = DISPID_UNKNOWN;
  assert(SUCCEEDED(bridge->GetIDsOfNames(IID_NULL, &name, 1, 0, &id)));
  VARIANT arguments[3], result;
  for (auto& argument : arguments) { VariantInit(&argument); V_VT(&argument) = VT_BSTR; }
  V_BSTR(&arguments[0]) = SysAllocString(L"[\"fixture-native-private-argument\"]");
  V_BSTR(&arguments[1]) = SysAllocString(L"check");
  V_BSTR(&arguments[2]) = SysAllocString(L"Fixture");
  VariantInit(&result);
  DISPPARAMS params{arguments, nullptr, 3, 0};
  *outcome = bridge->Invoke(id, IID_NULL, 0, DISPATCH_METHOD, &params, &result, nullptr, nullptr);
  std::wstring nonce = SUCCEEDED(*outcome) && V_VT(&result) == VT_BSTR
      ? V_BSTR(&result) : L"";
  VariantClear(&result);
  for (auto& argument : arguments) VariantClear(&argument);
  return nonce;
}

void PreparedArgumentsStayNative() {
  auto impl = std::make_shared<Host::Impl>();
  impl->ready = true;
  impl->bridge_guarded = true;
  impl->bridge_endpoint = L"/.gesher-js-bridge/native-fixture";
  Com<Bridge> bridge(new Bridge(impl), false);
  HRESULT hr;
  std::wstring first = Prepare(bridge.get(), &hr);
  assert(SUCCEEDED(hr) && first.size() == 64);
  for (wchar_t c : first) assert((c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'f'));
  assert(first.find(L"fixture-native-private-argument") == std::wstring::npos);
  assert(impl->prepared_calls.at(first).payload.find(L"fixture-native-private-argument") != std::wstring::npos);
  assert(impl->prepared_calls.at(first).generation == impl->bridge_generation);
  assert(impl->prepared_calls.at(first).deadline > GetTickCount64());
  std::wstring second = Prepare(bridge.get(), &hr);
  assert(SUCCEEDED(hr) && first != second);

  // The actual public IDispatch cancellation removes private arguments on CSP/XHR failure.
  LPOLESTR finish = const_cast<LPOLESTR>(L"finishCall");
  DISPID id = DISPID_UNKNOWN;
  assert(SUCCEEDED(bridge->GetIDsOfNames(IID_NULL, &finish, 1, 0, &id)));
  VARIANT argument; VariantInit(&argument); V_VT(&argument) = VT_BSTR;
  V_BSTR(&argument) = SysAllocString(first.c_str());
  DISPPARAMS params{&argument, nullptr, 1, 0};
  assert(SUCCEEDED(bridge->Invoke(id, IID_NULL, 0, DISPATCH_METHOD, &params, nullptr, nullptr, nullptr)));
  VariantClear(&argument);
  assert(impl->prepared_calls.count(first) == 0 && impl->prepared_calls.count(second) == 1);

  impl->prepared_calls.at(second).deadline = 0;
  assert(!Prepare(bridge.get(), &hr).empty() && SUCCEEDED(hr));
  assert(impl->prepared_calls.count(second) == 0);
  while (impl->prepared_calls.size() < 16) assert(!Prepare(bridge.get(), &hr).empty() && SUCCEEDED(hr));
  assert(Prepare(bridge.get(), &hr).empty() && FAILED(hr));
  CancelBridgeRequests(impl.get());
  assert(impl->prepared_calls.empty() && impl->bridge_generation == 1);
  impl->closing = true;
  assert(Prepare(bridge.get(), &hr).empty() && FAILED(hr));
}

}  // namespace

int main() {
  PathAndBody();
  CancellationAndTimeoutHelpers();
  PreparedArgumentsStayNative();
  puts("webview_bridge_test: strict UTF-8/body limit, native-only prepared arguments, bounded tickets and cancellation/timeout helpers passed");
}
