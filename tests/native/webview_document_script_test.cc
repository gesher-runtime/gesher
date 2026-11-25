// Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0
//
// Opt-in regression test for Host::Impl's SetDocumentScript vs Navigate /
// NavigateToString / Reload ordering, without creating a WebView2 runtime:
//   cmake ... -DGESHER_WEBVIEW_BRIDGE_TESTS=ON
//   cmake --build <build-dir> --target webview_document_script_test
//   <build-dir>/webview_document_script_test.exe
//
// Includes compat/webview/webview_host.cc directly to exercise the actual
// production DoAfterDocumentScript, ApplyDocumentScript, DrainDocumentScriptWaiters,
// and Fail helpers against an in-process fake ICoreWebView2.
#include "../../compat/webview/webview_host.cc"

#include <assert.h>

#include <deque>
#include <map>
#include <string>
#include <vector>

#ifdef NDEBUG
#error This test requires active assertions; compile with -UNDEBUG.
#endif

using namespace gesher::webview;

namespace {

class FakeWebView2 final : public ICoreWebView2 {
 public:
  struct PendingAddScript {
    std::wstring script;
    Com<ICoreWebView2AddScriptToExecuteOnDocumentCreatedCompletedHandler> handler;
  };

  struct NavigationRecord {
    std::wstring kind;
    std::wstring target;
    std::vector<std::wstring> active_scripts;
  };

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

  HRESULT STDMETHODCALLTYPE get_Settings(ICoreWebView2Settings**) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE get_Source(LPWSTR*) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE Navigate(LPCWSTR uri) override {
    navigations.push_back({L"navigate", uri ? uri : L"", ActiveScripts()});
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE NavigateToString(LPCWSTR htmlContent) override {
    navigations.push_back({L"navigateToString", htmlContent ? htmlContent : L"", ActiveScripts()});
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE add_NavigationStarting(
      ICoreWebView2NavigationStartingEventHandler*, EventRegistrationToken*) override {
    return E_NOTIMPL;
  }
  HRESULT STDMETHODCALLTYPE remove_NavigationStarting(EventRegistrationToken) override {
    return E_NOTIMPL;
  }
  HRESULT STDMETHODCALLTYPE add_ContentLoading(
      ICoreWebView2ContentLoadingEventHandler*, EventRegistrationToken*) override {
    return E_NOTIMPL;
  }
  HRESULT STDMETHODCALLTYPE remove_ContentLoading(EventRegistrationToken) override {
    return E_NOTIMPL;
  }
  HRESULT STDMETHODCALLTYPE add_SourceChanged(
      ICoreWebView2SourceChangedEventHandler*, EventRegistrationToken*) override {
    return E_NOTIMPL;
  }
  HRESULT STDMETHODCALLTYPE remove_SourceChanged(EventRegistrationToken) override {
    return E_NOTIMPL;
  }
  HRESULT STDMETHODCALLTYPE add_HistoryChanged(
      ICoreWebView2HistoryChangedEventHandler*, EventRegistrationToken*) override {
    return E_NOTIMPL;
  }
  HRESULT STDMETHODCALLTYPE remove_HistoryChanged(EventRegistrationToken) override {
    return E_NOTIMPL;
  }
  HRESULT STDMETHODCALLTYPE add_NavigationCompleted(
      ICoreWebView2NavigationCompletedEventHandler*, EventRegistrationToken*) override {
    return E_NOTIMPL;
  }
  HRESULT STDMETHODCALLTYPE remove_NavigationCompleted(EventRegistrationToken) override {
    return E_NOTIMPL;
  }
  HRESULT STDMETHODCALLTYPE add_FrameNavigationStarting(
      ICoreWebView2NavigationStartingEventHandler*, EventRegistrationToken*) override {
    return E_NOTIMPL;
  }
  HRESULT STDMETHODCALLTYPE remove_FrameNavigationStarting(EventRegistrationToken) override {
    return E_NOTIMPL;
  }
  HRESULT STDMETHODCALLTYPE add_FrameNavigationCompleted(
      ICoreWebView2NavigationCompletedEventHandler*, EventRegistrationToken*) override {
    return E_NOTIMPL;
  }
  HRESULT STDMETHODCALLTYPE remove_FrameNavigationCompleted(EventRegistrationToken) override {
    return E_NOTIMPL;
  }
  HRESULT STDMETHODCALLTYPE add_ScriptDialogOpening(
      ICoreWebView2ScriptDialogOpeningEventHandler*, EventRegistrationToken*) override {
    return E_NOTIMPL;
  }
  HRESULT STDMETHODCALLTYPE remove_ScriptDialogOpening(EventRegistrationToken) override {
    return E_NOTIMPL;
  }
  HRESULT STDMETHODCALLTYPE add_PermissionRequested(
      ICoreWebView2PermissionRequestedEventHandler*, EventRegistrationToken*) override {
    return E_NOTIMPL;
  }
  HRESULT STDMETHODCALLTYPE remove_PermissionRequested(EventRegistrationToken) override {
    return E_NOTIMPL;
  }
  HRESULT STDMETHODCALLTYPE add_ProcessFailed(
      ICoreWebView2ProcessFailedEventHandler*, EventRegistrationToken*) override {
    return E_NOTIMPL;
  }
  HRESULT STDMETHODCALLTYPE remove_ProcessFailed(EventRegistrationToken) override {
    return E_NOTIMPL;
  }
  HRESULT STDMETHODCALLTYPE AddScriptToExecuteOnDocumentCreated(
      LPCWSTR javaScript,
      ICoreWebView2AddScriptToExecuteOnDocumentCreatedCompletedHandler* handler) override {
    ++add_calls;
    if (FAILED(next_add_hr)) {
      HRESULT hr = next_add_hr;
      next_add_hr = S_OK;
      return hr;
    }
    pending_adds.push_back(
        {javaScript ? javaScript : L"",
         Com<ICoreWebView2AddScriptToExecuteOnDocumentCreatedCompletedHandler>(handler)});
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE RemoveScriptToExecuteOnDocumentCreated(LPCWSTR id) override {
    ++remove_calls;
    if (id != nullptr) installed_scripts.erase(id);
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE ExecuteScript(
      LPCWSTR, ICoreWebView2ExecuteScriptCompletedHandler*) override {
    return E_NOTIMPL;
  }
  HRESULT STDMETHODCALLTYPE CapturePreview(
      COREWEBVIEW2_CAPTURE_PREVIEW_IMAGE_FORMAT, IStream*,
      ICoreWebView2CapturePreviewCompletedHandler*) override {
    return E_NOTIMPL;
  }
  HRESULT STDMETHODCALLTYPE Reload() override {
    navigations.push_back({L"reload", L"", ActiveScripts()});
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE PostWebMessageAsJson(LPCWSTR) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE PostWebMessageAsString(LPCWSTR) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE add_WebMessageReceived(
      ICoreWebView2WebMessageReceivedEventHandler*, EventRegistrationToken*) override {
    return E_NOTIMPL;
  }
  HRESULT STDMETHODCALLTYPE remove_WebMessageReceived(EventRegistrationToken) override {
    return E_NOTIMPL;
  }
  HRESULT STDMETHODCALLTYPE CallDevToolsProtocolMethod(
      LPCWSTR, LPCWSTR, ICoreWebView2CallDevToolsProtocolMethodCompletedHandler*) override {
    return E_NOTIMPL;
  }
  HRESULT STDMETHODCALLTYPE get_BrowserProcessId(UINT32*) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE get_CanGoBack(BOOL*) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE get_CanGoForward(BOOL*) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE GoBack() override {
    navigations.push_back({L"goBack", L"", ActiveScripts()});
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE GoForward() override {
    navigations.push_back({L"goForward", L"", ActiveScripts()});
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE GetDevToolsProtocolEventReceiver(
      LPCWSTR, ICoreWebView2DevToolsProtocolEventReceiver**) override {
    return E_NOTIMPL;
  }
  HRESULT STDMETHODCALLTYPE Stop() override {
    navigations.push_back({L"stop", L"", ActiveScripts()});
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE add_NewWindowRequested(
      ICoreWebView2NewWindowRequestedEventHandler*, EventRegistrationToken*) override {
    return E_NOTIMPL;
  }
  HRESULT STDMETHODCALLTYPE remove_NewWindowRequested(EventRegistrationToken) override {
    return E_NOTIMPL;
  }
  HRESULT STDMETHODCALLTYPE add_DocumentTitleChanged(
      ICoreWebView2DocumentTitleChangedEventHandler*, EventRegistrationToken*) override {
    return E_NOTIMPL;
  }
  HRESULT STDMETHODCALLTYPE remove_DocumentTitleChanged(EventRegistrationToken) override {
    return E_NOTIMPL;
  }
  HRESULT STDMETHODCALLTYPE get_DocumentTitle(LPWSTR*) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE AddHostObjectToScript(LPCWSTR, VARIANT*) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE RemoveHostObjectFromScript(LPCWSTR) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE OpenDevToolsWindow() override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE add_ContainsFullScreenElementChanged(
      ICoreWebView2ContainsFullScreenElementChangedEventHandler*,
      EventRegistrationToken*) override {
    return E_NOTIMPL;
  }
  HRESULT STDMETHODCALLTYPE remove_ContainsFullScreenElementChanged(
      EventRegistrationToken) override {
    return E_NOTIMPL;
  }
  HRESULT STDMETHODCALLTYPE get_ContainsFullScreenElement(BOOL*) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE add_WebResourceRequested(
      ICoreWebView2WebResourceRequestedEventHandler*, EventRegistrationToken*) override {
    return E_NOTIMPL;
  }
  HRESULT STDMETHODCALLTYPE remove_WebResourceRequested(EventRegistrationToken) override {
    return E_NOTIMPL;
  }
  HRESULT STDMETHODCALLTYPE AddWebResourceRequestedFilter(
      LPCWSTR, COREWEBVIEW2_WEB_RESOURCE_CONTEXT) override {
    return E_NOTIMPL;
  }
  HRESULT STDMETHODCALLTYPE RemoveWebResourceRequestedFilter(
      LPCWSTR, COREWEBVIEW2_WEB_RESOURCE_CONTEXT) override {
    return E_NOTIMPL;
  }
  HRESULT STDMETHODCALLTYPE add_WindowCloseRequested(
      ICoreWebView2WindowCloseRequestedEventHandler*, EventRegistrationToken*) override {
    return E_NOTIMPL;
  }
  HRESULT STDMETHODCALLTYPE remove_WindowCloseRequested(EventRegistrationToken) override {
    return E_NOTIMPL;
  }

  void CompleteNextAdd(HRESULT hr) {
    assert(!pending_adds.empty());
    PendingAddScript next = std::move(pending_adds.front());
    pending_adds.pop_front();
    std::wstring id;
    if (SUCCEEDED(hr)) {
      id = L"script-" + std::to_wstring(++next_id);
      installed_scripts[id] = next.script;
    }
    next.handler->Invoke(hr, SUCCEEDED(hr) ? id.c_str() : nullptr);
  }

  std::vector<std::wstring> ActiveScripts() const {
    std::vector<std::wstring> out;
    for (const auto& entry : installed_scripts) out.push_back(entry.second);
    return out;
  }

  ULONG refs_ = 1;
  int add_calls = 0;
  int remove_calls = 0;
  int next_id = 0;
  HRESULT next_add_hr = S_OK;
  std::deque<PendingAddScript> pending_adds;
  std::map<std::wstring, std::wstring> installed_scripts;
  std::vector<NavigationRecord> navigations;
};

void DispatchOnReady(const std::shared_ptr<Host::Impl>& impl, std::function<void(Host::Impl*)> fn) {
  if (impl->destroyed || impl->closing || impl->failed) return;
  if (!impl->ready) {
    impl->pending.push_back(std::move(fn));
    return;
  }
  if (impl->document_script_in_flight) {
    impl->document_script_waiters.push_back(std::move(fn));
    return;
  }
  fn(impl.get());
}

void DrainPending(const std::shared_ptr<Host::Impl>& impl) {
  impl->ready = true;
  auto pending = std::move(impl->pending);
  impl->pending.clear();
  for (auto& fn : pending) {
    if (impl->destroyed) break;
    fn(impl.get());
  }
}

void PendingStartupCoalescingAndOrdering() {
  auto impl = std::make_shared<Host::Impl>();
  auto* fake = new FakeWebView2();
  impl->webview = Com<ICoreWebView2>(fake, false);

  const std::wstring script1 = L"Object.defineProperty(window, 'localStorage', {value: null});";
  const std::wstring script2 = L"// domStorageEnabled=true";

  // Simulate !ready queue: SetDocumentScript(script1) -> SetDocumentScript(script2) -> Navigate.
  DispatchOnReady(impl, [script1](Host::Impl* i) {
    if (i->document_script_in_flight) {
      i->document_script_waiters.push_back([script1](Host::Impl* j) { ApplyDocumentScript(j, script1); });
    } else {
      ApplyDocumentScript(i, script1);
    }
  });
  DispatchOnReady(impl, [script2](Host::Impl* i) {
    if (i->document_script_in_flight) {
      i->document_script_waiters.push_back([script2](Host::Impl* j) { ApplyDocumentScript(j, script2); });
    } else {
      ApplyDocumentScript(i, script2);
    }
  });
  DispatchOnReady(impl, [](Host::Impl* i) {
    if (i->document_script_in_flight) {
      i->document_script_waiters.push_back([](Host::Impl* j) { j->webview->Navigate(L"https://app.example/index.html"); });
    } else {
      i->webview->Navigate(L"https://app.example/index.html");
    }
  });

  DrainPending(impl);

  // Only script1 is in flight; Navigate must wait for script1 and script2.
  assert(fake->add_calls == 1);
  assert(fake->pending_adds.size() == 1);
  assert(fake->pending_adds.front().script == script1);
  assert(fake->navigations.empty());
  assert(impl->document_script_in_flight);

  // Completing script1 installs script1, immediately removes it, and starts script2.
  fake->CompleteNextAdd(S_OK);
  assert(fake->remove_calls == 1);
  assert(fake->add_calls == 2);
  assert(fake->pending_adds.size() == 1);
  assert(fake->pending_adds.front().script == script2);
  assert(fake->navigations.empty());
  assert(impl->document_script_in_flight);

  // Completing script2 installs script2 and flushes Navigate with ONLY script2 active.
  fake->CompleteNextAdd(S_OK);
  assert(!impl->document_script_in_flight);
  assert(fake->navigations.size() == 1);
  assert(fake->navigations[0].kind == L"navigate");
  assert(fake->navigations[0].target == L"https://app.example/index.html");
  assert(fake->navigations[0].active_scripts == std::vector<std::wstring>{script2});
}

void InterleavedNavigationsAndEmptyClear() {
  auto impl = std::make_shared<Host::Impl>();
  auto* fake = new FakeWebView2();
  impl->webview = Com<ICoreWebView2>(fake, false);
  impl->ready = true;

  ApplyDocumentScript(impl.get(), L"s1");
  DispatchOnReady(impl, [](Host::Impl* i) { i->webview->Navigate(L"https://example.test/1"); });
  DispatchOnReady(impl, [](Host::Impl* i) { ApplyDocumentScript(i, L"s2"); });
  DispatchOnReady(impl, [](Host::Impl* i) { i->webview->NavigateToString(L"<h1>two</h1>"); });
  DispatchOnReady(impl, [](Host::Impl* i) { ApplyDocumentScript(i, L""); });
  DispatchOnReady(impl, [](Host::Impl* i) { i->webview->Reload(); });

  assert(fake->navigations.empty());

  fake->CompleteNextAdd(S_OK);
  assert(fake->navigations.size() == 1);
  assert(fake->navigations[0].kind == L"navigate");
  assert(fake->navigations[0].active_scripts == std::vector<std::wstring>{L"s1"});
  assert(fake->remove_calls == 1);
  assert(fake->pending_adds.size() == 1);
  assert(fake->pending_adds.front().script == L"s2");

  fake->CompleteNextAdd(S_OK);
  assert(fake->navigations.size() == 3);
  assert(fake->navigations[1].kind == L"navigateToString");
  assert(fake->navigations[1].active_scripts == std::vector<std::wstring>{L"s2"});
  assert(fake->navigations[2].kind == L"reload");
  assert(fake->navigations[2].active_scripts.empty());
  assert(!impl->document_script_in_flight);
}

void AddScriptFailuresDoNotDeadlock() {
  auto impl = std::make_shared<Host::Impl>();
  auto* fake = new FakeWebView2();
  impl->webview = Com<ICoreWebView2>(fake, false);
  impl->ready = true;

  // Async failure: waiting navigation still runs.
  ApplyDocumentScript(impl.get(), L"s1");
  DispatchOnReady(impl, [](Host::Impl* i) { i->webview->Navigate(L"https://example.test/after-async-fail"); });
  assert(fake->navigations.empty());
  fake->CompleteNextAdd(E_FAIL);
  assert(!impl->document_script_in_flight);
  assert(fake->navigations.size() == 1);
  assert(fake->navigations[0].target == L"https://example.test/after-async-fail");
  assert(fake->navigations[0].active_scripts.empty());

  // Sync failure: in-flight flag clears and subsequent navigation runs immediately.
  fake->next_add_hr = E_FAIL;
  ApplyDocumentScript(impl.get(), L"s2");
  assert(!impl->document_script_in_flight);
  DispatchOnReady(impl, [](Host::Impl* i) { i->webview->Navigate(L"https://example.test/after-sync-fail"); });
  assert(fake->navigations.size() == 2);
  assert(fake->navigations[1].target == L"https://example.test/after-sync-fail");
}

void ClosingOrFailedDropsWaiters() {
  auto impl = std::make_shared<Host::Impl>();
  auto* fake = new FakeWebView2();
  impl->webview = Com<ICoreWebView2>(fake, false);
  impl->ready = true;

  ApplyDocumentScript(impl.get(), L"s1");
  DispatchOnReady(impl, [](Host::Impl* i) { i->webview->Navigate(L"https://example.test/never"); });
  assert(impl->document_script_waiters.size() == 1);

  Fail(impl.get(), E_FAIL, L"test fail");
  assert(!impl->document_script_in_flight);
  assert(impl->document_script_waiters.empty());

  fake->CompleteNextAdd(S_OK);
  assert(fake->navigations.empty());
  assert(impl->document_script_id.empty());
}

}  // namespace

int main() {
  PendingStartupCoalescingAndOrdering();
  InterleavedNavigationsAndEmptyClear();
  AddScriptFailuresDoNotDeadlock();
  ClosingOrFailedDropsWaiters();
  puts("webview_document_script_test: startup ordering, interleaved navigations, failure recovery, and lifecycle cleanup passed");
}
