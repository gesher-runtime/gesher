// Gesher: the WebView2 host behind android.webkit.WebView (webview_host.h,
// docs/webview.md).

#include "webview_host.h"

#include <stdio.h>
#include <string.h>

#include <algorithm>
#include <condition_variable>
#include <mutex>
#include <set>
#include <utility>

namespace gesher::webview {

namespace {

constexpr UINT kRunTask = WM_APP + 1;
constexpr wchar_t kTaskClass[] = L"GesherWebViewTasks";
constexpr wchar_t kContainerClass[] = L"GesherWebView";
constexpr wchar_t kParkingClass[] = L"GesherWebViewParking";

// --- The WebView thread ---------------------------------------------------

std::once_flag g_thread_once;
HWND g_task_window = nullptr;  // a message-only window: posted tasks
HWND g_parking = nullptr;      // a hidden top-level window: parked containers
DWORD g_thread_id = 0;

void ReportGlobal(int32_t type, int64_t id, int32_t arg, const std::wstring& a);

LRESULT CALLBACK TaskProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
  if (msg == kRunTask) {
    auto* fn = reinterpret_cast<std::function<void()>*>(lparam);
    (*fn)();
    delete fn;
    return 0;
  }
  return DefWindowProcW(hwnd, msg, wparam, lparam);
}

LRESULT CALLBACK ContainerProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
  if (msg == WM_ERASEBKGND) {
    // The page's background until WebView2 draws (HostOptions.background).
    RECT r;
    GetClientRect(hwnd, &r);
    auto color = static_cast<uint32_t>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    HBRUSH brush = CreateSolidBrush(RGB((color >> 16) & 0xff, (color >> 8) & 0xff, color & 0xff));
    FillRect(reinterpret_cast<HDC>(wparam), &r, brush);
    DeleteObject(brush);
    return 1;
  }
  return DefWindowProcW(hwnd, msg, wparam, lparam);
}

void CALLBACK OnLocationChange(HWINEVENTHOOK, DWORD, HWND hwnd, LONG object, LONG child, DWORD,
                               DWORD);

DWORD WINAPI ThreadMain(void* ready) {
  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
  HINSTANCE instance = GetModuleHandleW(nullptr);
  WNDCLASSEXW wc = {};
  wc.cbSize = sizeof(wc);
  wc.hInstance = instance;
  wc.lpfnWndProc = TaskProc;
  wc.lpszClassName = kTaskClass;
  RegisterClassExW(&wc);
  wc.lpfnWndProc = ContainerProc;
  wc.lpszClassName = kContainerClass;
  wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));  // IDC_ARROW
  RegisterClassExW(&wc);
  wc.lpfnWndProc = DefWindowProcW;
  wc.lpszClassName = kParkingClass;
  RegisterClassExW(&wc);
  g_task_window = CreateWindowExW(0, kTaskClass, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr,
                                  instance, nullptr);
  g_parking = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, kParkingClass,
                              L"Gesher WebView (parked)", WS_POPUP | WS_CLIPCHILDREN, -32000,
                              -32000, 64, 64, nullptr, nullptr, instance, nullptr);
  g_thread_id = GetCurrentThreadId();
  // The app windows' moves (for NotifyParentWindowPositionChanged): out of
  // context, so they come through this thread's message loop.
  SetWinEventHook(EVENT_OBJECT_LOCATIONCHANGE, EVENT_OBJECT_LOCATIONCHANGE, nullptr,
                  OnLocationChange, GetCurrentProcessId(), 0, WINEVENT_OUTOFCONTEXT);
  SetEvent(static_cast<HANDLE>(ready));
  MSG msg;
  while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
  }
  return 0;
}

void EnsureThread() {
  std::call_once(g_thread_once, [] {
    HANDLE ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE thread = CreateThread(nullptr, 0, ThreadMain, ready, 0, nullptr);
    WaitForSingleObject(ready, INFINITE);
    CloseHandle(ready);
    if (thread != nullptr) CloseHandle(thread);
  });
}

bool StartsWith(const std::wstring& s, const std::wstring& prefix) {
  return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
}

std::wstring Hex(HRESULT hr) {
  wchar_t buf[16];
  swprintf(buf, 16, L"0x%08lx", static_cast<unsigned long>(hr));
  return buf;
}

// --- The environment -------------------------------------------------------

std::mutex g_config_mutex;
std::wstring g_user_data;
std::wstring g_browser_args;
bool g_configured = false;

enum class EnvState { kNone, kCreating, kReady, kFailed };
EnvState g_env_state = EnvState::kNone;  // the WebView thread's
Com<ICoreWebView2Environment> g_env;
HRESULT g_env_error = S_OK;
std::vector<std::function<void(ICoreWebView2Environment*, HRESULT)>> g_env_waiting;

// ICoreWebView2EnvironmentOptions, as WebView2EnvironmentOptions.h would
// give with WRL.
class EnvironmentOptions final : public ICoreWebView2EnvironmentOptions {
 public:
  explicit EnvironmentOptions(std::wstring args) : args_(std::move(args)) {}

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override {
    if (out == nullptr) return E_POINTER;
    if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_ICoreWebView2EnvironmentOptions)) {
      *out = static_cast<ICoreWebView2EnvironmentOptions*>(this);
      AddRef();
      return S_OK;
    }
    *out = nullptr;
    return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
  ULONG STDMETHODCALLTYPE Release() override {
    ULONG n = --refs_;
    if (n == 0) delete this;
    return n;
  }
  HRESULT STDMETHODCALLTYPE get_AdditionalBrowserArguments(LPWSTR* value) override {
    return Copy(args_, value);
  }
  HRESULT STDMETHODCALLTYPE put_AdditionalBrowserArguments(LPCWSTR value) override {
    args_ = value != nullptr ? value : L"";
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE get_Language(LPWSTR* value) override { return Copy(language_, value); }
  HRESULT STDMETHODCALLTYPE put_Language(LPCWSTR value) override {
    language_ = value != nullptr ? value : L"";
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE get_TargetCompatibleBrowserVersion(LPWSTR* value) override {
    return Copy(target_, value);
  }
  HRESULT STDMETHODCALLTYPE put_TargetCompatibleBrowserVersion(LPCWSTR value) override {
    target_ = value != nullptr ? value : L"";
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE get_AllowSingleSignOnUsingOSPrimaryAccount(BOOL* allow) override {
    if (allow == nullptr) return E_POINTER;
    *allow = FALSE;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE put_AllowSingleSignOnUsingOSPrimaryAccount(BOOL) override {
    return S_OK;
  }

 private:
  ~EnvironmentOptions() = default;
  static HRESULT Copy(const std::wstring& s, LPWSTR* out) {
    if (out == nullptr) return E_POINTER;
    size_t bytes = (s.size() + 1) * sizeof(wchar_t);
    *out = static_cast<LPWSTR>(CoTaskMemAlloc(bytes));
    if (*out == nullptr) return E_OUTOFMEMORY;
    memcpy(*out, s.c_str(), bytes);
    return S_OK;
  }
  std::atomic<ULONG> refs_{1};
  std::wstring args_;
  std::wstring language_;
  // The oldest runtime Gesher's host works with: newer interfaces are asked
  // for with QueryInterface and used when the runtime has them.
  std::wstring target_ = L"86.0.616.0";
};

// Calls fn with the environment (or null and the error), creating it first.
// The WebView thread.
void WithEnvironment(std::function<void(ICoreWebView2Environment*, HRESULT)> fn) {
  switch (g_env_state) {
    case EnvState::kReady:
      fn(g_env.get(), S_OK);
      return;
    case EnvState::kFailed:
      fn(nullptr, g_env_error);
      return;
    case EnvState::kCreating:
      g_env_waiting.push_back(std::move(fn));
      return;
    case EnvState::kNone:
      break;
  }
  g_env_waiting.push_back(std::move(fn));
  g_env_state = EnvState::kCreating;
  std::wstring user_data, args;
  {
    std::lock_guard<std::mutex> lock(g_config_mutex);
    user_data = g_user_data;
    args = g_browser_args;
  }
  // Chromium stops drawing windows it thinks are covered or off the desktop
  // (tests show windows off it); an app's WebView draws whenever it is shown.
  std::wstring all = L"--disable-features=CalculateNativeWinOcclusion "
                     L"--disable-backgrounding-occluded-windows";
  if (!args.empty()) all += L" " + args;
  Com<ICoreWebView2EnvironmentOptions> options(new EnvironmentOptions(all), false);
  auto finish = [](HRESULT hr, ICoreWebView2Environment* env) {
    if (SUCCEEDED(hr) && env != nullptr) {
      g_env = Com<ICoreWebView2Environment>(env);
      g_env_state = EnvState::kReady;
    } else {
      g_env_error = FAILED(hr) ? hr : E_FAIL;
      g_env_state = EnvState::kFailed;
    }
    auto waiting = std::move(g_env_waiting);
    g_env_waiting.clear();
    for (auto& w : waiting) w(g_env.get(), g_env_error);
  };
  auto handler = GESHER_CALLBACK(
      ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler,
      [finish](HRESULT hr, ICoreWebView2Environment* env) -> HRESULT {
        finish(hr, env);
        return S_OK;
      });
  HRESULT hr = CreateEnvironment(nullptr, user_data.empty() ? nullptr : user_data.c_str(),
                                 options.get(), handler.get());
  if (FAILED(hr)) finish(hr, nullptr);
}

}  // namespace

// --- A WebView ---------------------------------------------------------------

struct Host::Impl : std::enable_shared_from_this<Host::Impl> {
  Listener* listener = nullptr;
  HostOptions options;
  HWND container = nullptr;
  Com<ICoreWebView2Environment> env;
  Com<ICoreWebView2Controller> controller;
  Com<ICoreWebView2> webview;
  bool ready = false;
  bool failed = false;
  bool destroyed = false;
  std::vector<std::function<void(Impl*)>> pending;  // until ready

  Settings settings;
  bool has_settings = false;
  // The last bounds (applied to the controller once it is ready).
  RECT bounds = {0, 0, 0, 0};
  bool visible = false;
  bool has_bounds = false;

  // An API navigation (Navigate, reload, history) is on its way: its
  // NavigationStarting is not the page's, so the Listener is not asked.
  bool api_navigation = false;
  std::wstring main_document;  // the URL of the main frame's navigation

  std::wstring document_script_id;
  uint64_t document_script_generation = 0;

  struct Request {
    Com<ICoreWebView2WebResourceRequestedEventArgs> args;
    Com<ICoreWebView2Deferral> deferral;
  };
  std::map<int64_t, Request> requests;
  int64_t next_request = 1;

  struct Dialog {
    Com<ICoreWebView2ScriptDialogOpeningEventArgs> args;
    Com<ICoreWebView2Deferral> deferral;
    COREWEBVIEW2_SCRIPT_DIALOG_KIND kind;
  };
  std::map<int64_t, Dialog> dialogs;
  int64_t next_dialog = 1;

  std::vector<Com<ICoreWebView2DevToolsProtocolEventReceiver>> receivers;
  // Run once this (service) host is ready or has failed.
  std::vector<std::function<void(ICoreWebView2*)>> service_waiting;

  void Report(int32_t type, int64_t id = 0, int32_t arg = 0, const std::wstring& a = {},
              const std::wstring& b = {}, const std::wstring& c = {},
              const std::vector<uint8_t>* bytes = nullptr) {
    if (listener != nullptr && !destroyed) listener->OnEvent(type, id, arg, a, b, c, bytes);
  }
};

namespace {

using Impl = Host::Impl;

std::set<Impl*> g_live;  // the WebView thread's: hosts with a controller
std::shared_ptr<Impl> g_service;  // a hidden host for cookies and data, when no WebView exists
Host::GlobalCallback g_global_callback = nullptr;

class NullListener final : public Listener {
 public:
  void OnEvent(int32_t, int64_t, int32_t, const std::wstring&, const std::wstring&,
               const std::wstring&, const std::vector<uint8_t>*) override {}
  bool OnNavigationStarting(const std::wstring&, bool, bool) override { return false; }
  void OnResourceRequest(int64_t, const std::wstring&, const std::wstring&, const std::wstring&,
                         int32_t, bool) override {}
  std::wstring OnBridgeCall(const std::wstring&, const std::wstring&,
                            const std::wstring&) override {
    return L"{\"e\":\"no bridge\"}";
  }
  void OnDestroyed() override {}
};
NullListener g_null_listener;

void ReportGlobal(int32_t type, int64_t id, int32_t arg, const std::wstring& a) {
  if (g_global_callback != nullptr) g_global_callback(type, id, arg, a);
}

void CALLBACK OnLocationChange(HWINEVENTHOOK, DWORD, HWND hwnd, LONG object, LONG child, DWORD,
                               DWORD) {
  if (object != OBJID_WINDOW || child != CHILDID_SELF || hwnd == nullptr) return;
  if (GetAncestor(hwnd, GA_ROOT) != hwnd) return;  // only top-level windows
  for (Impl* impl : g_live) {
    if (impl->controller && impl->container != nullptr &&
        GetAncestor(impl->container, GA_ROOT) == hwnd) {
      impl->controller->NotifyParentWindowPositionChanged();
    }
  }
}

// Paints children last: hwui's frame (gesher::Window's WM_PAINT) must not
// cover the WebView's container.
void EnsureClipChildren(HWND parent) {
  LONG_PTR style = GetWindowLongPtrW(parent, GWL_STYLE);
  if ((style & WS_CLIPCHILDREN) == 0) SetWindowLongPtrW(parent, GWL_STYLE, style | WS_CLIPCHILDREN);
}

void Reparent(Impl* impl, HWND parent) {
  if (impl->container == nullptr) return;
  if (parent != nullptr && !IsWindow(parent)) parent = nullptr;
  HWND target = parent != nullptr ? parent : g_parking;
  if (GetParent(impl->container) == target) return;
  if (parent == nullptr) ShowWindow(impl->container, SW_HIDE);
  SetParent(impl->container, target);
  if (parent != nullptr) EnsureClipChildren(parent);
  if (impl->controller) impl->controller->NotifyParentWindowPositionChanged();
}

void ApplyBounds(Impl* impl) {
  if (!impl->controller || !impl->has_bounds) return;
  if (!impl->visible) {
    impl->controller->put_IsVisible(FALSE);
    return;
  }
  impl->controller->put_Bounds(impl->bounds);
  impl->controller->put_IsVisible(TRUE);
}

void ApplySettings(Impl* impl) {
  Com<ICoreWebView2Settings> s;
  if (!impl->webview || FAILED(impl->webview->get_Settings(s.Put())) || !s) return;
  const Host::Settings& v = impl->settings;
  s->put_IsScriptEnabled(v.javascript ? TRUE : FALSE);
  s->put_IsWebMessageEnabled(TRUE);
  s->put_AreDefaultScriptDialogsEnabled(FALSE);  // the app's dialogs (kDialog)
  s->put_IsStatusBarEnabled(FALSE);
  s->put_AreDevToolsEnabled(v.devtools ? TRUE : FALSE);
  s->put_AreHostObjectsAllowed(TRUE);
  s->put_IsZoomControlEnabled(v.zoom_controls ? TRUE : FALSE);
  s->put_IsBuiltInErrorPageEnabled(TRUE);
  Com<ICoreWebView2Settings2> s2;
  if (!v.user_agent.empty() && s.As(IID_ICoreWebView2Settings2, &s2)) {
    s2->put_UserAgent(v.user_agent.c_str());
  }
  // An Android WebView has no browser keys (F5, Ctrl+F, ...).
  Com<ICoreWebView2Settings3> s3;
  if (s.As(IID_ICoreWebView2Settings3, &s3)) s3->put_AreBrowserAcceleratorKeysEnabled(FALSE);
  Com<ICoreWebView2Settings5> s5;
  if (s.As(IID_ICoreWebView2Settings5, &s5)) s5->put_IsPinchZoomEnabled(v.pinch_zoom ? TRUE : FALSE);
  Com<ICoreWebView2Settings6> s6;
  if (s.As(IID_ICoreWebView2Settings6, &s6)) s6->put_IsSwipeNavigationEnabled(FALSE);
}

void Fail(Impl* impl, HRESULT hr, const std::wstring& what) {
  impl->failed = true;
  impl->pending.clear();
  impl->Report(kFailed, 0, static_cast<int32_t>(hr), what + L": " + Hex(hr));
  auto waiting = std::move(impl->service_waiting);
  impl->service_waiting.clear();
  for (auto& w : waiting) w(nullptr);
}

// Runs fn on the WebView thread once the host is ready (dropped if it fails
// or is destroyed).
void Do(const std::shared_ptr<Impl>& impl, std::function<void(Impl*)> fn) {
  PostToWebViewThread([impl, fn = std::move(fn)]() mutable {
    if (impl->destroyed || impl->failed) return;
    if (!impl->ready) {
      impl->pending.push_back(std::move(fn));
      return;
    }
    fn(impl.get());
  });
}

std::wstring Headers(ICoreWebView2HttpRequestHeaders* headers) {
  std::wstring out;
  Com<ICoreWebView2HttpHeadersCollectionIterator> it;
  if (headers == nullptr || FAILED(headers->GetIterator(it.Put())) || !it) return out;
  BOOL has = FALSE;
  while (SUCCEEDED(it->get_HasCurrentHeader(&has)) && has) {
    CoString name, value;
    if (SUCCEEDED(it->GetCurrentHeader(name.Put(), value.Put()))) {
      out += name.str() + L": " + value.str() + L"\r\n";
    }
    BOOL next = FALSE;
    if (FAILED(it->MoveNext(&next)) || !next) break;
  }
  return out;
}

// window.<interface>.<method>(...) reaches the Listener through this host
// object: the bridge script (com.gesher.app.webview.JsBridge) calls
// chrome.webview.hostObjects.sync.gesherBridge.call(interface, method, argsJson).
class Bridge final : public IDispatch {
 public:
  explicit Bridge(std::weak_ptr<Impl> impl) : impl_(std::move(impl)) {}

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override {
    if (out == nullptr) return E_POINTER;
    if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_IDispatch)) {
      *out = static_cast<IDispatch*>(this);
      AddRef();
      return S_OK;
    }
    *out = nullptr;
    return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
  ULONG STDMETHODCALLTYPE Release() override {
    ULONG n = --refs_;
    if (n == 0) delete this;
    return n;
  }
  HRESULT STDMETHODCALLTYPE GetTypeInfoCount(UINT* count) override {
    if (count == nullptr) return E_POINTER;
    *count = 0;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE GetTypeInfo(UINT, LCID, ITypeInfo** info) override {
    if (info != nullptr) *info = nullptr;
    return E_NOTIMPL;
  }
  HRESULT STDMETHODCALLTYPE GetIDsOfNames(REFIID, LPOLESTR* names, UINT count, LCID,
                                          DISPID* ids) override {
    HRESULT hr = S_OK;
    for (UINT i = 0; i < count; ++i) {
      if (names[i] != nullptr && _wcsicmp(names[i], L"call") == 0) {
        ids[i] = kCall;
      } else {
        ids[i] = DISPID_UNKNOWN;
        hr = DISP_E_UNKNOWNNAME;
      }
    }
    return hr;
  }
  HRESULT STDMETHODCALLTYPE Invoke(DISPID id, REFIID, LCID, WORD flags, DISPPARAMS* params,
                                   VARIANT* result, EXCEPINFO*, UINT*) override {
    if (id != kCall) return DISP_E_MEMBERNOTFOUND;
    if ((flags & DISPATCH_METHOD) == 0) return DISP_E_MEMBERNOTFOUND;
    if (params == nullptr || params->cArgs != 3) return DISP_E_BADPARAMCOUNT;
    // Arguments come last to first.
    std::wstring object = Arg(params->rgvarg[2]);
    std::wstring method = Arg(params->rgvarg[1]);
    std::wstring args = Arg(params->rgvarg[0]);
    std::wstring answer = L"{\"e\":\"the WebView is gone\"}";
    if (auto impl = impl_.lock()) {
      if (!impl->destroyed && impl->listener != nullptr) {
        answer = impl->listener->OnBridgeCall(object, method, args);
      }
    }
    if (result != nullptr) {
      VariantInit(result);
      V_VT(result) = VT_BSTR;
      V_BSTR(result) = SysAllocStringLen(answer.data(), static_cast<UINT>(answer.size()));
    }
    return S_OK;
  }

 private:
  static constexpr DISPID kCall = 1;
  ~Bridge() = default;
  static std::wstring Arg(VARIANT& v) {
    if (V_VT(&v) == VT_BSTR) {
      return V_BSTR(&v) != nullptr ? std::wstring(V_BSTR(&v), SysStringLen(V_BSTR(&v)))
                                   : std::wstring();
    }
    VARIANT s;
    VariantInit(&s);
    std::wstring out;
    if (SUCCEEDED(VariantChangeType(&s, &v, 0, VT_BSTR)) && V_BSTR(&s) != nullptr) {
      out.assign(V_BSTR(&s), SysStringLen(V_BSTR(&s)));
    }
    VariantClear(&s);
    return out;
  }
  std::atomic<ULONG> refs_{1};
  std::weak_ptr<Impl> impl_;
};

// Registers the controller's and the WebView's handlers. The WebView thread.
void AddHandlers(const std::shared_ptr<Impl>& shared) {
  Impl* impl = shared.get();
  std::weak_ptr<Impl> weak = shared;
  EventRegistrationToken token;
  ICoreWebView2* wv = impl->webview.get();

  wv->add_NavigationStarting(
      GESHER_CALLBACK(ICoreWebView2NavigationStartingEventHandler,
                      [weak](ICoreWebView2*, ICoreWebView2NavigationStartingEventArgs* args)
                          -> HRESULT {
                        auto impl = weak.lock();
                        if (!impl || impl->destroyed) return S_OK;
                        CoString uri;
                        args->get_Uri(uri.Put());
                        BOOL user = FALSE, redirect = FALSE;
                        args->get_IsUserInitiated(&user);
                        args->get_IsRedirected(&redirect);
                        bool api = impl->api_navigation && !redirect;
                        if (!redirect) impl->api_navigation = false;
                        std::wstring url = uri.str();
                        if (!api && impl->listener != nullptr &&
                            impl->listener->OnNavigationStarting(url, user != FALSE,
                                                                 redirect != FALSE)) {
                          args->put_Cancel(TRUE);
                          return S_OK;
                        }
                        impl->main_document = url;
                        if (!redirect) {
                          UINT64 nav = 0;
                          args->get_NavigationId(&nav);
                          impl->Report(kPageStarted, static_cast<int64_t>(nav), api ? 1 : 0, url);
                        }
                        return S_OK;
                      })
          .get(),
      &token);

  wv->add_NavigationCompleted(
      GESHER_CALLBACK(ICoreWebView2NavigationCompletedEventHandler,
                      [weak](ICoreWebView2* sender, ICoreWebView2NavigationCompletedEventArgs* args)
                          -> HRESULT {
                        auto impl = weak.lock();
                        if (!impl || impl->destroyed) return S_OK;
                        BOOL ok = FALSE;
                        args->get_IsSuccess(&ok);
                        COREWEBVIEW2_WEB_ERROR_STATUS status = COREWEBVIEW2_WEB_ERROR_STATUS_UNKNOWN;
                        args->get_WebErrorStatus(&status);
                        int http = 0;
                        Com<ICoreWebView2NavigationCompletedEventArgs2> a2;
                        Com<ICoreWebView2NavigationCompletedEventArgs> a(args);
                        if (a.As(IID_ICoreWebView2NavigationCompletedEventArgs2, &a2)) {
                          a2->get_HttpStatusCode(&http);
                        }
                        CoString source;
                        sender->get_Source(source.Put());
                        std::wstring url = source.str();
                        if (url.empty() || url == L"about:blank") url = impl->main_document;
                        impl->Report(kPageFinished, http, ok ? -1 : static_cast<int32_t>(status), url);
                        return S_OK;
                      })
          .get(),
      &token);

  wv->add_ContentLoading(
      GESHER_CALLBACK(ICoreWebView2ContentLoadingEventHandler,
                      [weak](ICoreWebView2*, ICoreWebView2ContentLoadingEventArgs* args) -> HRESULT {
                        auto impl = weak.lock();
                        if (!impl) return S_OK;
                        BOOL error = FALSE;
                        args->get_IsErrorPage(&error);
                        impl->Report(kContentLoading, 0, error ? 1 : 0);
                        return S_OK;
                      })
          .get(),
      &token);

  wv->add_SourceChanged(
      GESHER_CALLBACK(ICoreWebView2SourceChangedEventHandler,
                      [weak](ICoreWebView2* sender, ICoreWebView2SourceChangedEventArgs*) -> HRESULT {
                        auto impl = weak.lock();
                        if (!impl) return S_OK;
                        CoString source;
                        sender->get_Source(source.Put());
                        impl->Report(kSourceChanged, 0, 0, source.str());
                        return S_OK;
                      })
          .get(),
      &token);

  wv->add_HistoryChanged(
      GESHER_CALLBACK(ICoreWebView2HistoryChangedEventHandler,
                      [weak](ICoreWebView2* sender, IUnknown*) -> HRESULT {
                        auto impl = weak.lock();
                        if (!impl) return S_OK;
                        BOOL back = FALSE, forward = FALSE;
                        sender->get_CanGoBack(&back);
                        sender->get_CanGoForward(&forward);
                        impl->Report(kHistory, 0, (back ? 1 : 0) | (forward ? 2 : 0));
                        return S_OK;
                      })
          .get(),
      &token);

  wv->add_DocumentTitleChanged(
      GESHER_CALLBACK(ICoreWebView2DocumentTitleChangedEventHandler,
                      [weak](ICoreWebView2* sender, IUnknown*) -> HRESULT {
                        auto impl = weak.lock();
                        if (!impl) return S_OK;
                        CoString title;
                        sender->get_DocumentTitle(title.Put());
                        impl->Report(kTitle, 0, 0, title.str());
                        return S_OK;
                      })
          .get(),
      &token);

  wv->add_ScriptDialogOpening(
      GESHER_CALLBACK(ICoreWebView2ScriptDialogOpeningEventHandler,
                      [weak](ICoreWebView2*, ICoreWebView2ScriptDialogOpeningEventArgs* args)
                          -> HRESULT {
                        auto impl = weak.lock();
                        if (!impl || impl->destroyed) return S_OK;
                        Impl::Dialog d;
                        d.args = Com<ICoreWebView2ScriptDialogOpeningEventArgs>(args);
                        args->GetDeferral(d.deferral.Put());
                        args->get_Kind(&d.kind);
                        CoString uri, message, text;
                        args->get_Uri(uri.Put());
                        args->get_Message(message.Put());
                        args->get_DefaultText(text.Put());
                        int64_t id = impl->next_dialog++;
                        int32_t kind = static_cast<int32_t>(d.kind) + 1;
                        impl->dialogs.emplace(id, std::move(d));
                        impl->Report(kDialog, id, kind, uri.str(), message.str(), text.str());
                        return S_OK;
                      })
          .get(),
      &token);

  // Every request is seen here; the Listener gets file URLs, documents, and
  // with intercept_all (WebViewClient.shouldInterceptRequest) the rest.
  wv->AddWebResourceRequestedFilter(L"*", COREWEBVIEW2_WEB_RESOURCE_CONTEXT_ALL);
  wv->add_WebResourceRequested(
      GESHER_CALLBACK(ICoreWebView2WebResourceRequestedEventHandler,
                      [weak](ICoreWebView2*, ICoreWebView2WebResourceRequestedEventArgs* args)
                          -> HRESULT {
                        auto impl = weak.lock();
                        if (!impl || impl->destroyed || impl->listener == nullptr) return S_OK;
                        Com<ICoreWebView2WebResourceRequest> request;
                        if (FAILED(args->get_Request(request.Put())) || !request) return S_OK;
                        CoString uri;
                        request->get_Uri(uri.Put());
                        std::wstring url = uri.str();
                        COREWEBVIEW2_WEB_RESOURCE_CONTEXT context =
                            COREWEBVIEW2_WEB_RESOURCE_CONTEXT_OTHER;
                        args->get_ResourceContext(&context);
                        bool file = StartsWith(url, std::wstring(kFileOrigin) + L"/");
                        bool document = context == COREWEBVIEW2_WEB_RESOURCE_CONTEXT_DOCUMENT;
                        if (!file && !document && !impl->settings.intercept_all) return S_OK;
                        CoString method;
                        request->get_Method(method.Put());
                        Com<ICoreWebView2HttpRequestHeaders> headers;
                        request->get_Headers(headers.Put());
                        Impl::Request r;
                        r.args = Com<ICoreWebView2WebResourceRequestedEventArgs>(args);
                        if (FAILED(args->GetDeferral(r.deferral.Put()))) return S_OK;
                        int64_t id = impl->next_request++;
                        impl->requests.emplace(id, std::move(r));
                        bool main = document && url == impl->main_document;
                        impl->listener->OnResourceRequest(id, url, method.str(),
                                                          Headers(headers.get()),
                                                          static_cast<int32_t>(context), main);
                        return S_OK;
                      })
          .get(),
      &token);

  wv->add_NewWindowRequested(
      GESHER_CALLBACK(ICoreWebView2NewWindowRequestedEventHandler,
                      [weak](ICoreWebView2*, ICoreWebView2NewWindowRequestedEventArgs* args)
                          -> HRESULT {
                        auto impl = weak.lock();
                        if (!impl) return S_OK;
                        // No browser window of WebView2's own: the app decides.
                        args->put_Handled(TRUE);
                        CoString uri;
                        args->get_Uri(uri.Put());
                        BOOL user = FALSE;
                        args->get_IsUserInitiated(&user);
                        impl->Report(kNewWindow, 0, user ? 1 : 0, uri.str());
                        return S_OK;
                      })
          .get(),
      &token);

  // Permissions are denied, as a WebView whose app does not grant them
  // (WebChromeClient.onPermissionRequest's default).
  wv->add_PermissionRequested(
      GESHER_CALLBACK(ICoreWebView2PermissionRequestedEventHandler,
                      [](ICoreWebView2*, ICoreWebView2PermissionRequestedEventArgs* args)
                          -> HRESULT {
                        args->put_State(COREWEBVIEW2_PERMISSION_STATE_DENY);
                        return S_OK;
                      })
          .get(),
      &token);

  wv->add_ProcessFailed(
      GESHER_CALLBACK(ICoreWebView2ProcessFailedEventHandler,
                      [weak](ICoreWebView2*, ICoreWebView2ProcessFailedEventArgs* args) -> HRESULT {
                        auto impl = weak.lock();
                        if (!impl) return S_OK;
                        COREWEBVIEW2_PROCESS_FAILED_KIND kind =
                            COREWEBVIEW2_PROCESS_FAILED_KIND_BROWSER_PROCESS_EXITED;
                        args->get_ProcessFailedKind(&kind);
                        impl->Report(kProcessFailed, 0, static_cast<int32_t>(kind));
                        return S_OK;
                      })
          .get(),
      &token);

  Com<ICoreWebView2_2> wv2;
  if (impl->webview.As(IID_ICoreWebView2_2, &wv2)) {
    wv2->add_DOMContentLoaded(
        GESHER_CALLBACK(ICoreWebView2DOMContentLoadedEventHandler,
                        [weak](ICoreWebView2*, ICoreWebView2DOMContentLoadedEventArgs*) -> HRESULT {
                          if (auto impl = weak.lock()) impl->Report(kDomContentLoaded);
                          return S_OK;
                        })
            .get(),
        &token);
  }

  // Downloads go to the app's DownloadListener; WebView2 downloads nothing.
  Com<ICoreWebView2_4> wv4;
  if (impl->webview.As(IID_ICoreWebView2_4, &wv4)) {
    wv4->add_DownloadStarting(
        GESHER_CALLBACK(ICoreWebView2DownloadStartingEventHandler,
                        [weak](ICoreWebView2*, ICoreWebView2DownloadStartingEventArgs* args)
                            -> HRESULT {
                          args->put_Cancel(TRUE);
                          auto impl = weak.lock();
                          if (!impl) return S_OK;
                          Com<ICoreWebView2DownloadOperation> op;
                          if (FAILED(args->get_DownloadOperation(op.Put())) || !op) return S_OK;
                          CoString uri, mime, disposition;
                          op->get_Uri(uri.Put());
                          op->get_MimeType(mime.Put());
                          op->get_ContentDisposition(disposition.Put());
                          INT64 total = -1;
                          op->get_TotalBytesToReceive(&total);
                          impl->Report(kDownload, total, 0, uri.str(), mime.str(), disposition.str());
                          return S_OK;
                        })
            .get(),
        &token);
  }

  ICoreWebView2Controller* c = impl->controller.get();
  c->add_GotFocus(GESHER_CALLBACK(ICoreWebView2FocusChangedEventHandler,
                                  [weak](ICoreWebView2Controller*, IUnknown*) -> HRESULT {
                                    if (auto impl = weak.lock()) impl->Report(kFocus, 0, 1);
                                    return S_OK;
                                  })
                      .get(),
                  &token);
  c->add_LostFocus(GESHER_CALLBACK(ICoreWebView2FocusChangedEventHandler,
                                   [weak](ICoreWebView2Controller*, IUnknown*) -> HRESULT {
                                     if (auto impl = weak.lock()) impl->Report(kFocus, 0, 0);
                                     return S_OK;
                                   })
                       .get(),
                   &token);
  c->add_MoveFocusRequested(
      GESHER_CALLBACK(ICoreWebView2MoveFocusRequestedEventHandler,
                      [weak](ICoreWebView2Controller*, ICoreWebView2MoveFocusRequestedEventArgs* args)
                          -> HRESULT {
                        COREWEBVIEW2_MOVE_FOCUS_REASON reason =
                            COREWEBVIEW2_MOVE_FOCUS_REASON_NEXT;
                        args->get_Reason(&reason);
                        args->put_Handled(TRUE);
                        if (auto impl = weak.lock()) {
                          impl->Report(kFocus, 0,
                                       reason == COREWEBVIEW2_MOVE_FOCUS_REASON_PREVIOUS ? 3 : 2);
                        }
                        return S_OK;
                      })
          .get(),
      &token);

  // The page's console (WebChromeClient.onConsoleMessage) through the
  // DevTools protocol's Runtime domain.
  wv->CallDevToolsProtocolMethod(
      L"Runtime.enable", L"{}",
      GESHER_CALLBACK(ICoreWebView2CallDevToolsProtocolMethodCompletedHandler,
                      [](HRESULT, LPCWSTR) -> HRESULT { return S_OK; })
          .get());
  for (const wchar_t* name : {L"Runtime.consoleAPICalled", L"Runtime.exceptionThrown"}) {
    Com<ICoreWebView2DevToolsProtocolEventReceiver> receiver;
    if (FAILED(wv->GetDevToolsProtocolEventReceiver(name, receiver.Put())) || !receiver) continue;
    std::wstring event = name;
    receiver->add_DevToolsProtocolEventReceived(
        GESHER_CALLBACK(ICoreWebView2DevToolsProtocolEventReceivedEventHandler,
                        [weak, event](ICoreWebView2*,
                                      ICoreWebView2DevToolsProtocolEventReceivedEventArgs* args)
                            -> HRESULT {
                          auto impl = weak.lock();
                          if (!impl) return S_OK;
                          CoString json;
                          args->get_ParameterObjectAsJson(json.Put());
                          impl->Report(kConsole, 0, 0, event, json.str());
                          return S_OK;
                        })
            .get(),
        &token);
    impl->receivers.push_back(std::move(receiver));
  }

  // The JavaScript interfaces' host object.
  VARIANT bridge;
  VariantInit(&bridge);
  V_VT(&bridge) = VT_DISPATCH;
  V_DISPATCH(&bridge) = new Bridge(weak);
  wv->AddHostObjectToScript(L"gesherBridge", &bridge);
  VariantClear(&bridge);
}

void OnController(const std::shared_ptr<Impl>& impl, HRESULT hr,
                  ICoreWebView2Controller* controller) {
  if (impl->destroyed) {
    if (controller != nullptr) controller->Close();
    return;
  }
  if (FAILED(hr) || controller == nullptr) {
    Fail(impl.get(), FAILED(hr) ? hr : E_FAIL, L"CreateCoreWebView2Controller");
    return;
  }
  impl->controller = Com<ICoreWebView2Controller>(controller);
  if (FAILED(controller->get_CoreWebView2(impl->webview.Put())) || !impl->webview) {
    Fail(impl.get(), E_FAIL, L"get_CoreWebView2");
    return;
  }
  // CSS pixels follow Android's density, not the monitor's scale.
  Com<ICoreWebView2Controller3> c3;
  if (impl->controller.As(IID_ICoreWebView2Controller3, &c3)) {
    c3->put_ShouldDetectMonitorScaleChanges(FALSE);
    c3->put_RasterizationScale(impl->options.scale);
  }
  Com<ICoreWebView2Controller2> c2;
  if (impl->controller.As(IID_ICoreWebView2Controller2, &c2)) {
    uint32_t c = impl->options.background;
    COREWEBVIEW2_COLOR color = {static_cast<BYTE>(c >> 24), static_cast<BYTE>(c >> 16),
                                static_cast<BYTE>(c >> 8), static_cast<BYTE>(c)};
    c2->put_DefaultBackgroundColor(color);
  }
  AddHandlers(impl);
  if (impl->has_settings) ApplySettings(impl.get());
  ApplyBounds(impl.get());
  impl->ready = true;
  if (impl->listener != &g_null_listener) g_live.insert(impl.get());
  CoString version;
  if (impl->env) impl->env->get_BrowserVersionString(version.Put());
  impl->Report(kReady, 0, 0, version.str());
  auto pending = std::move(impl->pending);
  impl->pending.clear();
  for (auto& fn : pending) {
    if (impl->destroyed) break;
    fn(impl.get());
  }
  auto waiting = std::move(impl->service_waiting);
  impl->service_waiting.clear();
  for (auto& w : waiting) w(impl->webview.get());
}

// The WebView thread.
void Start(const std::shared_ptr<Impl>& impl) {
  HWND parent = impl->options.parent;
  if (parent != nullptr && !IsWindow(parent)) parent = nullptr;
  impl->container = CreateWindowExW(
      WS_EX_NOPARENTNOTIFY, kContainerClass, L"", WS_CHILD | WS_CLIPCHILDREN | WS_CLIPSIBLINGS, 0,
      0, 1, 1, parent != nullptr ? parent : g_parking, nullptr, GetModuleHandleW(nullptr), nullptr);
  if (impl->container == nullptr) {
    Fail(impl.get(), HRESULT_FROM_WIN32(GetLastError()), L"CreateWindowEx");
    return;
  }
  SetWindowLongPtrW(impl->container, GWLP_USERDATA, static_cast<LONG_PTR>(impl->options.background));
  if (parent != nullptr) EnsureClipChildren(parent);
  WithEnvironment([impl](ICoreWebView2Environment* env, HRESULT hr) {
    if (impl->destroyed) return;
    if (env == nullptr) {
      Fail(impl.get(), hr, L"CreateCoreWebView2EnvironmentWithOptions");
      return;
    }
    impl->env = Com<ICoreWebView2Environment>(env);
    HRESULT created = env->CreateCoreWebView2Controller(
        impl->container,
        GESHER_CALLBACK(ICoreWebView2CreateCoreWebView2ControllerCompletedHandler,
                        [impl](HRESULT result, ICoreWebView2Controller* controller) -> HRESULT {
                          OnController(impl, result, controller);
                          return S_OK;
                        })
            .get());
    if (FAILED(created)) Fail(impl.get(), created, L"CreateCoreWebView2Controller");
  });
}

// Calls fn with a CoreWebView2 of the process (a WebView's, or the hidden
// service host's), or null when WebView2 is unavailable. The WebView thread.
void WithCoreWebView(std::function<void(ICoreWebView2*)> fn) {
  for (Impl* impl : g_live) {
    if (impl->ready && impl->webview) {
      fn(impl->webview.get());
      return;
    }
  }
  if (!g_service) {
    g_service = std::make_shared<Impl>();
    g_service->listener = &g_null_listener;
    g_service->service_waiting.push_back(std::move(fn));
    Start(g_service);
    return;
  }
  if (g_service->ready) {
    fn(g_service->webview.get());
  } else if (g_service->failed) {
    fn(nullptr);
  } else {
    g_service->service_waiting.push_back(std::move(fn));
  }
}

void WithCookieManager(int64_t callback_id,
                       std::function<void(ICoreWebView2CookieManager*)> fn) {
  WithCoreWebView([callback_id, fn = std::move(fn)](ICoreWebView2* wv) {
    Com<ICoreWebView2> holder(wv);
    Com<ICoreWebView2_2> wv2;
    Com<ICoreWebView2CookieManager> cookies;
    if (wv == nullptr || !holder.As(IID_ICoreWebView2_2, &wv2) ||
        FAILED(wv2->get_CookieManager(cookies.Put())) || !cookies) {
      ReportGlobal(kGlobalDone, callback_id, static_cast<int32_t>(E_NOINTERFACE), L"");
      return;
    }
    fn(cookies.get());
  });
}

}  // namespace

// --- Host ------------------------------------------------------------------

void PostToWebViewThread(std::function<void()> fn) {
  EnsureThread();
  auto* task = new std::function<void()>(std::move(fn));
  if (!PostMessageW(g_task_window, kRunTask, 0, reinterpret_cast<LPARAM>(task))) delete task;
}

void Host::Configure(const std::wstring& user_data_folder, const std::wstring& browser_args) {
  std::lock_guard<std::mutex> lock(g_config_mutex);
  if (g_configured) return;
  g_configured = true;
  g_user_data = user_data_folder;
  g_browser_args = browser_args;
}

std::wstring Host::RuntimeVersion(HRESULT* hr) {
  LPWSTR version = nullptr;
  HRESULT result = AvailableBrowserVersion(&version);
  if (hr != nullptr) *hr = result;
  std::wstring out;
  if (SUCCEEDED(result) && version != nullptr) out = version;
  CoTaskMemFree(version);
  return out;
}

Host* Host::Create(Listener* listener, const HostOptions& options) {
  auto impl = std::make_shared<Impl>();
  impl->listener = listener;
  impl->options = options;
  PostToWebViewThread([impl] { Start(impl); });
  return new Host(impl);
}

void Host::Destroy() {
  std::shared_ptr<Impl> impl = std::move(impl_);
  PostToWebViewThread([impl] {
    if (impl->destroyed) return;
    // Pending requests and dialogs go on unanswered.
    for (auto& r : impl->requests) r.second.deferral->Complete();
    impl->requests.clear();
    for (auto& d : impl->dialogs) d.second.deferral->Complete();
    impl->dialogs.clear();
    g_live.erase(impl.get());
    if (impl->controller) impl->controller->Close();
    impl->receivers.clear();
    impl->controller.Reset();
    impl->webview.Reset();
    if (impl->container != nullptr) DestroyWindow(impl->container);
    impl->container = nullptr;
    impl->pending.clear();
    Listener* listener = impl->listener;
    impl->destroyed = true;
    if (listener != nullptr) listener->OnDestroyed();
  });
  delete this;
}

void Host::Navigate(const std::wstring& url, const std::wstring& headers) {
  Do(impl_, [url, headers](Impl* impl) {
    impl->api_navigation = true;
    HRESULT hr = E_FAIL;
    Com<ICoreWebView2_2> wv2;
    Com<ICoreWebView2Environment2> env2;
    if (!headers.empty() && impl->webview.As(IID_ICoreWebView2_2, &wv2) &&
        impl->env.As(IID_ICoreWebView2Environment2, &env2)) {
      Com<ICoreWebView2WebResourceRequest> request;
      hr = env2->CreateWebResourceRequest(url.c_str(), L"GET", nullptr, headers.c_str(),
                                          request.Put());
      if (SUCCEEDED(hr)) hr = wv2->NavigateWithWebResourceRequest(request.get());
    } else {
      hr = impl->webview->Navigate(url.c_str());
    }
    if (FAILED(hr)) {
      impl->api_navigation = false;
      impl->Report(kFailed, 0, static_cast<int32_t>(hr), L"Navigate " + url + L": " + Hex(hr));
    }
  });
}

void Host::NavigateToString(const std::wstring& html) {
  Do(impl_, [html](Impl* impl) {
    impl->api_navigation = true;
    HRESULT hr = impl->webview->NavigateToString(html.c_str());
    if (FAILED(hr)) {
      impl->api_navigation = false;
      impl->Report(kFailed, 0, static_cast<int32_t>(hr), L"NavigateToString: " + Hex(hr));
    }
  });
}

void Host::Run(Command command) {
  Do(impl_, [command](Impl* impl) {
    switch (command) {
      case kReload:
        impl->api_navigation = true;
        impl->webview->Reload();
        break;
      case kStop:
        impl->webview->Stop();
        break;
      case kGoBack:
        impl->api_navigation = true;
        impl->webview->GoBack();
        break;
      case kGoForward:
        impl->api_navigation = true;
        impl->webview->GoForward();
        break;
      case kFocus:
        impl->controller->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
        break;
      case kBlur: {
        // Keyboard focus back to the app's window, if WebView2 has it.
        HWND focus = GetFocus();
        if (focus != nullptr && impl->container != nullptr &&
            (focus == impl->container || IsChild(impl->container, focus))) {
          HWND parent = GetParent(impl->container);
          if (parent != nullptr && parent != g_parking) SetFocus(parent);
        }
        break;
      }
      case kDevTools:
        impl->webview->OpenDevToolsWindow();
        break;
    }
  });
}

void Host::SetBounds(HWND parent, int x, int y, int width, int height, int clip_x, int clip_y,
                     int clip_width, int clip_height, bool visible) {
  std::shared_ptr<Impl> shared = impl_;
  PostToWebViewThread([shared, parent, x, y, width, height, clip_x, clip_y, clip_width,
                       clip_height, visible] {
    Impl* impl = shared.get();
    if (impl->destroyed || impl->container == nullptr) return;
    Reparent(impl, parent);
    bool shown = visible && parent != nullptr && clip_width > 0 && clip_height > 0;
    if (shown) {
      SetWindowPos(impl->container, HWND_TOP, clip_x, clip_y, clip_width, clip_height,
                   SWP_NOACTIVATE | SWP_SHOWWINDOW);
    } else {
      ShowWindow(impl->container, SW_HIDE);
    }
    impl->bounds = {x - clip_x, y - clip_y, x - clip_x + width, y - clip_y + height};
    impl->visible = shown;
    impl->has_bounds = true;
    ApplyBounds(impl);
    if (impl->controller) impl->controller->NotifyParentWindowPositionChanged();
  });
}

void Host::Park(int64_t id) {
  std::shared_ptr<Impl> shared = impl_;
  PostToWebViewThread([shared, id] {
    Impl* impl = shared.get();
    if (impl->destroyed) return;
    Reparent(impl, nullptr);
    if (impl->container != nullptr) ShowWindow(impl->container, SW_HIDE);
    impl->visible = false;
    if (impl->controller) impl->controller->put_IsVisible(FALSE);
    impl->Report(kParked, id);
  });
}

void Host::SetSettings(const Settings& settings) {
  std::shared_ptr<Impl> shared = impl_;
  PostToWebViewThread([shared, settings] {
    Impl* impl = shared.get();
    if (impl->destroyed) return;
    impl->settings = settings;
    impl->has_settings = true;
    if (impl->ready) ApplySettings(impl);
  });
}

void Host::SetZoom(double factor) {
  Do(impl_, [factor](Impl* impl) { impl->controller->put_ZoomFactor(factor); });
}

void Host::SetBackground(uint32_t argb) {
  Do(impl_, [argb](Impl* impl) {
    if (impl->container != nullptr) {
      SetWindowLongPtrW(impl->container, GWLP_USERDATA, static_cast<LONG_PTR>(argb));
    }
    Com<ICoreWebView2Controller2> c2;
    if (impl->controller.As(IID_ICoreWebView2Controller2, &c2)) {
      COREWEBVIEW2_COLOR color = {static_cast<BYTE>(argb >> 24), static_cast<BYTE>(argb >> 16),
                                  static_cast<BYTE>(argb >> 8), static_cast<BYTE>(argb)};
      // WebView2 takes opaque or fully transparent backgrounds only.
      if (color.A != 0) color.A = 255;
      c2->put_DefaultBackgroundColor(color);
    }
  });
}

void Host::ExecuteScript(const std::wstring& script, int64_t callback_id) {
  std::weak_ptr<Impl> weak = impl_;
  Do(impl_, [script, callback_id, weak](Impl* impl) {
    HRESULT hr = impl->webview->ExecuteScript(
        script.c_str(),
        GESHER_CALLBACK(ICoreWebView2ExecuteScriptCompletedHandler,
                        [weak, callback_id](HRESULT error, LPCWSTR result) -> HRESULT {
                          if (auto impl = weak.lock()) {
                            impl->Report(kScriptResult, callback_id, static_cast<int32_t>(error),
                                         result != nullptr ? result : L"null");
                          }
                          return S_OK;
                        })
            .get());
    if (FAILED(hr)) impl->Report(kScriptResult, callback_id, static_cast<int32_t>(hr), L"null");
  });
}

void Host::SetDocumentScript(const std::wstring& script) {
  std::weak_ptr<Impl> weak = impl_;
  Do(impl_, [script, weak](Impl* impl) {
    if (!impl->document_script_id.empty()) {
      impl->webview->RemoveScriptToExecuteOnDocumentCreated(impl->document_script_id.c_str());
      impl->document_script_id.clear();
    }
    uint64_t generation = ++impl->document_script_generation;
    if (script.empty()) return;
    impl->webview->AddScriptToExecuteOnDocumentCreated(
        script.c_str(),
        GESHER_CALLBACK(ICoreWebView2AddScriptToExecuteOnDocumentCreatedCompletedHandler,
                        [weak, generation](HRESULT error, LPCWSTR id) -> HRESULT {
                          auto impl = weak.lock();
                          if (!impl || FAILED(error) || id == nullptr) return S_OK;
                          if (impl->destroyed || !impl->webview) return S_OK;
                          if (generation != impl->document_script_generation) {
                            // Replaced while it was being added.
                            impl->webview->RemoveScriptToExecuteOnDocumentCreated(id);
                          } else {
                            impl->document_script_id = id;
                          }
                          return S_OK;
                        })
            .get());
  });
}

void Host::RespondToRequest(int64_t id, int32_t status, const std::wstring& reason,
                            const std::wstring& headers, std::vector<uint8_t> body) {
  std::shared_ptr<Impl> shared = impl_;
  PostToWebViewThread([shared, id, status, reason, headers, body = std::move(body)] {
    Impl* impl = shared.get();
    auto it = impl->requests.find(id);
    if (it == impl->requests.end()) return;
    Impl::Request r = std::move(it->second);
    impl->requests.erase(it);
    if (status > 0 && impl->env && !impl->destroyed) {
      Com<IStream> stream(SHCreateMemStream(body.empty() ? nullptr : body.data(),
                                            static_cast<UINT>(body.size())),
                          /*add_ref=*/false);
      Com<ICoreWebView2WebResourceResponse> response;
      if (stream && SUCCEEDED(impl->env->CreateWebResourceResponse(
                        stream.get(), status, reason.c_str(), headers.c_str(), response.Put()))) {
        r.args->put_Response(response.get());
      }
    }
    r.deferral->Complete();
  });
}

void Host::CompleteDialog(int64_t id, bool accept, const std::wstring& text) {
  std::shared_ptr<Impl> shared = impl_;
  PostToWebViewThread([shared, id, accept, text] {
    Impl* impl = shared.get();
    auto it = impl->dialogs.find(id);
    if (it == impl->dialogs.end()) return;
    Impl::Dialog d = std::move(it->second);
    impl->dialogs.erase(it);
    if (accept) {
      if (d.kind == COREWEBVIEW2_SCRIPT_DIALOG_KIND_PROMPT) d.args->put_ResultText(text.c_str());
      d.args->Accept();
    }
    d.deferral->Complete();
  });
}

void Host::CapturePreview(int64_t callback_id) {
  std::weak_ptr<Impl> weak = impl_;
  Do(impl_, [callback_id, weak](Impl* impl) {
    Com<IStream> stream(SHCreateMemStream(nullptr, 0), /*add_ref=*/false);
    if (!stream) {
      impl->Report(kCapture, callback_id, static_cast<int32_t>(E_OUTOFMEMORY));
      return;
    }
    HRESULT hr = impl->webview->CapturePreview(
        COREWEBVIEW2_CAPTURE_PREVIEW_IMAGE_FORMAT_PNG, stream.get(),
        GESHER_CALLBACK(ICoreWebView2CapturePreviewCompletedHandler,
                        [weak, callback_id, stream](HRESULT error) -> HRESULT {
                          auto impl = weak.lock();
                          if (!impl) return S_OK;
                          std::vector<uint8_t> png;
                          if (SUCCEEDED(error)) {
                            STATSTG stat = {};
                            stream->Stat(&stat, STATFLAG_NONAME);
                            png.resize(static_cast<size_t>(stat.cbSize.QuadPart));
                            LARGE_INTEGER zero = {};
                            stream->Seek(zero, STREAM_SEEK_SET, nullptr);
                            ULONG read = 0;
                            if (!png.empty()) {
                              stream->Read(png.data(), static_cast<ULONG>(png.size()), &read);
                            }
                            png.resize(read);
                          }
                          impl->Report(kCapture, callback_id, static_cast<int32_t>(error), {}, {},
                                       {}, &png);
                          return S_OK;
                        })
            .get());
    if (FAILED(hr)) impl->Report(kCapture, callback_id, static_cast<int32_t>(hr));
  });
}

void Host::SetGlobalCallback(GlobalCallback callback) { g_global_callback = callback; }

void Host::SetCookie(const std::wstring& url, const Cookie& cookie, int64_t callback_id) {
  (void)url;
  PostToWebViewThread([cookie, callback_id] {
    WithCookieManager(callback_id, [cookie, callback_id](ICoreWebView2CookieManager* cookies) {
      Com<ICoreWebView2Cookie> c;
      HRESULT hr = cookies->CreateCookie(cookie.name.c_str(), cookie.value.c_str(),
                                         cookie.domain.c_str(), cookie.path.c_str(), c.Put());
      if (SUCCEEDED(hr)) {
        if (cookie.expires >= 0) c->put_Expires(cookie.expires);
        c->put_IsSecure(cookie.secure ? TRUE : FALSE);
        c->put_IsHttpOnly(cookie.http_only ? TRUE : FALSE);
        hr = cookies->AddOrUpdateCookie(c.get());
      }
      ReportGlobal(kGlobalDone, callback_id, static_cast<int32_t>(hr), L"");
    });
  });
}

void Host::GetCookies(const std::wstring& url, int64_t callback_id) {
  PostToWebViewThread([url, callback_id] {
    WithCookieManager(callback_id, [url, callback_id](ICoreWebView2CookieManager* cookies) {
      HRESULT hr = cookies->GetCookies(
          url.c_str(),
          GESHER_CALLBACK(ICoreWebView2GetCookiesCompletedHandler,
                          [callback_id](HRESULT error, ICoreWebView2CookieList* list) -> HRESULT {
                            std::wstring out;
                            UINT32 count = 0;
                            if (SUCCEEDED(error) && list != nullptr) list->get_Count(&count);
                            for (UINT32 i = 0; i < count; ++i) {
                              Com<ICoreWebView2Cookie> c;
                              if (FAILED(list->GetValueAtIndex(i, c.Put())) || !c) continue;
                              CoString name, value;
                              c->get_Name(name.Put());
                              c->get_Value(value.Put());
                              if (!out.empty()) out += L"; ";
                              out += name.str() + L"=" + value.str();
                            }
                            ReportGlobal(kGlobalCookies, callback_id, static_cast<int32_t>(error),
                                         out);
                            return S_OK;
                          })
              .get());
      if (FAILED(hr)) ReportGlobal(kGlobalCookies, callback_id, static_cast<int32_t>(hr), L"");
    });
  });
}

void Host::DeleteCookies(int32_t which, int64_t callback_id) {
  PostToWebViewThread([which, callback_id] {
    WithCookieManager(callback_id, [which, callback_id](ICoreWebView2CookieManager* cookies) {
      if (which != 2) {
        HRESULT hr = cookies->DeleteAllCookies();
        ReportGlobal(kGlobalDone, callback_id, static_cast<int32_t>(hr), L"");
        return;
      }
      Com<ICoreWebView2CookieManager> manager(cookies);
      HRESULT hr = cookies->GetCookies(
          L"",
          GESHER_CALLBACK(ICoreWebView2GetCookiesCompletedHandler,
                          [manager, callback_id](HRESULT error,
                                                 ICoreWebView2CookieList* list) -> HRESULT {
                            UINT32 count = 0;
                            if (SUCCEEDED(error) && list != nullptr) list->get_Count(&count);
                            for (UINT32 i = 0; i < count; ++i) {
                              Com<ICoreWebView2Cookie> c;
                              BOOL session = FALSE;
                              if (SUCCEEDED(list->GetValueAtIndex(i, c.Put())) && c &&
                                  SUCCEEDED(c->get_IsSession(&session)) && session) {
                                manager->DeleteCookie(c.get());
                              }
                            }
                            ReportGlobal(kGlobalDone, callback_id, static_cast<int32_t>(error), L"");
                            return S_OK;
                          })
              .get());
      if (FAILED(hr)) ReportGlobal(kGlobalDone, callback_id, static_cast<int32_t>(hr), L"");
    });
  });
}

void Host::ClearBrowsingData(int32_t kinds, int64_t callback_id) {
  PostToWebViewThread([kinds, callback_id] {
    WithCoreWebView([kinds, callback_id](ICoreWebView2* wv) {
      Com<ICoreWebView2> holder(wv);
      Com<ICoreWebView2_13> wv13;
      Com<ICoreWebView2Profile> profile;
      Com<ICoreWebView2Profile2> profile2;
      if (wv == nullptr || !holder.As(IID_ICoreWebView2_13, &wv13) ||
          FAILED(wv13->get_Profile(profile.Put())) || !profile.As(IID_ICoreWebView2Profile2, &profile2)) {
        ReportGlobal(kGlobalDone, callback_id, static_cast<int32_t>(E_NOINTERFACE), L"");
        return;
      }
      HRESULT hr = profile2->ClearBrowsingData(
          static_cast<COREWEBVIEW2_BROWSING_DATA_KINDS>(kinds),
          GESHER_CALLBACK(ICoreWebView2ClearBrowsingDataCompletedHandler,
                          [callback_id](HRESULT error) -> HRESULT {
                            ReportGlobal(kGlobalDone, callback_id, static_cast<int32_t>(error), L"");
                            return S_OK;
                          })
              .get());
      if (FAILED(hr)) ReportGlobal(kGlobalDone, callback_id, static_cast<int32_t>(hr), L"");
    });
  });
}

}  // namespace gesher::webview
