// Gesher: the WebView2 host behind android.webkit.WebView (webview_host.h,
// docs/webview.md).

#include "webview_host.h"

#include <shobjidl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <condition_variable>
#include <deque>
#include <limits>
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
std::map<UINT_PTR, std::function<void()>> g_timers;  // WebView STA only
UINT_PTR g_next_timer = 1;

void ReportGlobal(int32_t type, int64_t id, int32_t arg, const std::wstring& a);

LRESULT CALLBACK TaskProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
  if (msg == kRunTask) {
    auto* fn = reinterpret_cast<std::function<void()>*>(lparam);
    (*fn)();
    delete fn;
    return 0;
  }
  if (msg == WM_TIMER) {
    auto it = g_timers.find(static_cast<UINT_PTR>(wparam));
    if (it != g_timers.end()) {
      auto fn = std::move(it->second);
      g_timers.erase(it);
      KillTimer(hwnd, static_cast<UINT_PTR>(wparam));
      fn();
      return 0;
    }
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
std::wstring g_proxy_args;  // SetProxyArguments
bool g_env_started = false;  // under g_config_mutex: WithEnvironment began creating it
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
    if (!g_proxy_args.empty()) args = args.empty() ? g_proxy_args : args + L" " + g_proxy_args;
    g_env_started = true;
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
  std::atomic<HWND> published_container{nullptr};  // Container(), from any thread
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
  // The container's place (parent client coordinates) and the holes cut out
  // of its window region (SetBounds).
  RECT clip = {0, 0, 0, 0};
  std::vector<Host::Shape> shapes;
  bool has_region = false;

  // An API navigation (Navigate, reload, history) is on its way: its
  // NavigationStarting is not the page's, so the Listener is not asked.
  bool api_navigation = false;
  std::wstring main_document;  // the URL of the main frame's navigation
  std::set<UINT64> cancelled;  // navigations the Listener took
  // Basic authentication of the main document: WebView2 ends the navigation
  // (401) while it waits for the answer, and starts it again with the
  // credentials; to the app it is one page load. The 401's completion is held
  // until the app answers (reported if it cancels), and the retry's start is
  // not reported (CompleteBasicAuth).
  std::wstring auth_pending;  // the URL waiting for the app's answer
  std::wstring auth_retry;    // the URL about to be loaded again
  struct Finish {
    bool held = false;
    int http = 0;
    int32_t status = -1;
    std::wstring url;
  } auth_finish;

  std::wstring document_script_id;
  uint64_t document_script_generation = 0;
  unsigned document_script_inflight = 0;
  bool document_script_failed = false;
  UINT_PTR document_script_timer = 0;
  ULONGLONG document_script_deadline = 0;
  std::vector<std::function<void(Impl*)>> document_waiting;

  struct Frame {
    Com<ICoreWebView2Frame> frame;
    Com<ICoreWebView2Frame2> messages;
    int64_t document = 0;
    int64_t parent = 0;
  };
  std::map<int64_t, Frame> frames;  // WebView STA only; callbacks capture weak Impl + key.
  int64_t next_frame = 1;
  int64_t next_document = 1;
  int64_t main_document_token = 0;
  std::mutex document_mutex;
  std::set<int64_t> current_documents;  // read by Java without waiting on WebView STA.
  bool documents_closed = false;

  int64_t NewDocument(int64_t old) {
    int64_t token = 0;
    {
      std::lock_guard<std::mutex> guard(document_mutex);
      current_documents.erase(old);
      if (!documents_closed && next_document == std::numeric_limits<int64_t>::max()) {
        documents_closed = true;
        current_documents.clear();
      }
      if (!documents_closed) {
        token = next_document++;
        current_documents.insert(token);
      }
    }
    if (old > 0) Report(kDocumentRetired, old);
    return token;
  }
  void RetireDocument(int64_t token) {
    {
      std::lock_guard<std::mutex> guard(document_mutex);
      current_documents.erase(token);
    }
    if (token > 0) Report(kDocumentRetired, token);
  }

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

  // Requests WebView2 waits for (a deferral): permissions, new windows,
  // authentication, certificates. args: the event's arguments.
  struct Deferred {
    Com<IUnknown> args;
    Com<ICoreWebView2Deferral> deferral;
  };
  std::map<int64_t, Deferred> deferred;
  int64_t next_deferred = 1;
  int64_t Defer(IUnknown* args, ICoreWebView2Deferral* deferral) {
    int64_t id = next_deferred++;
    deferred.emplace(id, Deferred{Com<IUnknown>(args), Com<ICoreWebView2Deferral>(deferral)});
    return id;
  }
  // Takes the request back (false when it is gone).
  bool Undefer(int64_t id, Deferred* out) {
    auto it = deferred.find(id);
    if (it == deferred.end()) return false;
    *out = std::move(it->second);
    deferred.erase(it);
    return true;
  }

  Com<ICoreWebView2Find> find;

  std::vector<Com<ICoreWebView2DevToolsProtocolEventReceiver>> receivers;
  std::set<std::wstring> listened;  // ListenDevTools' events
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

// A shape's region, moved by (dx, dy) (parent client to container coordinates).
HRGN ShapeRegion(const Host::Shape& s, int dx, int dy) {
  RECT r = {s.rect.left + dx, s.rect.top + dy, s.rect.right + dx, s.rect.bottom + dy};
  switch (s.kind) {
    case Host::Shape::kHoleRoundRect:
    case Host::Shape::kClipRoundRect:
      // CreateRoundRectRgn's right and bottom are exclusive by one more pixel.
      return CreateRoundRectRgn(r.left, r.top, r.right + 1, r.bottom + 1, 2 * s.radius,
                                2 * s.radius);
    case Host::Shape::kHoleOval:
    case Host::Shape::kClipOval:
      return CreateEllipticRgn(r.left, r.top, r.right + 1, r.bottom + 1);
    case Host::Shape::kHolePolygon:
    case Host::Shape::kClipPolygon: {
      if (s.points.size() < 3) return CreateRectRgn(r.left, r.top, r.right, r.bottom);
      std::vector<POINT> pts = s.points;
      for (POINT& p : pts) {
        p.x += dx;
        p.y += dy;
      }
      return CreatePolygonRgn(pts.data(), static_cast<int>(pts.size()), WINDING);
    }
    default:
      return CreateRectRgn(r.left, r.top, r.right, r.bottom);
  }
}

// The container's window region: its client area (or the WebView's own
// outline) less the holes (none of either: no region).
void ApplyRegion(Impl* impl) {
  if (impl->container == nullptr) return;
  int w = impl->clip.right - impl->clip.left;
  int h = impl->clip.bottom - impl->clip.top;
  int dx = -impl->clip.left, dy = -impl->clip.top;
  HRGN region = nullptr;
  for (const Host::Shape& s : impl->shapes) {
    if (s.kind < Host::Shape::kClipRoundRect) continue;
    if (region == nullptr) region = CreateRectRgn(0, 0, w, h);
    HRGN keep = ShapeRegion(s, dx, dy);
    CombineRgn(region, region, keep, RGN_AND);
    DeleteObject(keep);
  }
  for (const Host::Shape& s : impl->shapes) {
    if (s.kind >= Host::Shape::kClipRoundRect) continue;
    RECT r = {s.rect.left + dx, s.rect.top + dy, s.rect.right + dx, s.rect.bottom + dy};
    if (r.right <= 0 || r.bottom <= 0 || r.left >= w || r.top >= h) continue;
    if (region == nullptr) region = CreateRectRgn(0, 0, w, h);
    HRGN cut = ShapeRegion(s, dx, dy);
    CombineRgn(region, region, cut, RGN_DIFF);
    DeleteObject(cut);
  }
  if (region == nullptr && !impl->has_region) return;
  // The system owns `region` from here on.
  SetWindowRgn(impl->container, region, TRUE);
  impl->has_region = region != nullptr;
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
  // WebSettings.setSafeBrowsingEnabled: Microsoft Defender SmartScreen.
  Com<ICoreWebView2Settings8> s8;
  if (s.As(IID_ICoreWebView2Settings8, &s8)) {
    s8->put_IsReputationCheckingRequired(v.reputation_checking ? TRUE : FALSE);
  }
}

// A JSON string literal.
std::wstring JsonString(const std::wstring& s) {
  std::wstring out = L"\"";
  for (wchar_t ch : s) {
    switch (ch) {
      case L'"': out += L"\\\""; break;
      case L'\\': out += L"\\\\"; break;
      case L'\n': out += L"\\n"; break;
      case L'\r': out += L"\\r"; break;
      case L'\t': out += L"\\t"; break;
      default:
        if (ch < 0x20) {
          wchar_t buf[8];
          swprintf(buf, 8, L"\\u%04x", static_cast<unsigned>(ch));
          out += buf;
        } else {
          out += ch;
        }
    }
  }
  return out + L"\"";
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

void ClearDocumentTimer(Impl* impl) {
  if (impl->document_script_timer != 0) {
    KillTimer(g_task_window, impl->document_script_timer);
    g_timers.erase(impl->document_script_timer);
    impl->document_script_timer = 0;
  }
  impl->document_script_deadline = 0;
}

bool CheckDocumentDeadline(Impl* impl) {
  if (impl->document_script_deadline == 0 || GetTickCount64() < impl->document_script_deadline)
    return true;
  if (!impl->document_script_failed) {
    impl->document_script_failed = true;
    impl->document_waiting.clear();
    impl->Report(kFailed, 0, E_FAIL, L"document script registration exceeded three seconds");
  }
  return false;
}

void DrainDocumentNavigations(Impl* impl) {
  CheckDocumentDeadline(impl);
  if (impl->document_script_inflight != 0) return;
  ClearDocumentTimer(impl);
  auto waiting = std::move(impl->document_waiting);
  impl->document_waiting.clear();
  if (impl->document_script_failed || impl->destroyed) return;
  for (auto& navigation : waiting) navigation(impl);
}

void ArmDocumentTimer(const std::shared_ptr<Impl>& impl) {
  if (impl->document_script_timer != 0) return;
  if (impl->document_script_deadline == 0) impl->document_script_deadline = GetTickCount64() + 3000;
  if (g_next_timer == 0 || g_next_timer == std::numeric_limits<UINT_PTR>::max()) {
    impl->document_script_failed = true;
    impl->document_waiting.clear();
    impl->Report(kFailed, 0, E_FAIL, L"document script timer exhausted");
    return;
  }
  UINT_PTR id = g_next_timer++;
  if (SetTimer(g_task_window, id, 3000, nullptr) == 0) {
    impl->document_script_failed = true;
    impl->document_waiting.clear();
    impl->Report(kFailed, 0, E_FAIL, L"document script timer creation failed");
    return;
  }
  impl->document_script_timer = id;
  std::weak_ptr<Impl> weak = impl;
  g_timers.emplace(id, [weak, id] {
    auto host = weak.lock();
    if (!host || host->destroyed || host->document_script_timer != id) return;
    host->document_script_timer = 0;
    host->document_script_failed = true;
    host->document_waiting.clear();
    host->Report(kFailed, 0, E_FAIL, L"document script registration exceeded three seconds");
  });
}

void RunDocumentNavigation(Impl* impl, std::function<void(Impl*)> navigation) {
  CheckDocumentDeadline(impl);
  if (impl->document_script_failed) {
    impl->Report(kFailed, 0, E_FAIL, L"navigation rejected after document script failure");
  } else if (impl->document_script_inflight != 0) {
    impl->document_waiting.push_back(std::move(navigation));
  } else {
    navigation(impl);
  }
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
// chrome.webview.hostObjects.sync.gesherBridge.callJava(interface, method, argsJson)
// (not "call": the proxy is a function, whose .call is Function.prototype.call).
class Bridge final : public IDispatch {
 public:
  explicit Bridge(std::weak_ptr<Impl> impl, bool callable = false)
      : impl_(std::move(impl)), callable_(callable) {}

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
      if (names[i] != nullptr && _wcsicmp(names[i], L"callJava") == 0) {
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
    UINT argc = params != nullptr ? params->cArgs : 0;
    if (getenv("GESHER_WEBVIEW_DEBUG") != nullptr) {
      fprintf(stderr, "gesher-webview: bridge Invoke(dispid %ld, flags %u, %u args)%s\n",
              static_cast<long>(id), static_cast<unsigned>(flags), argc,
              callable_ ? " on the method" : "");
      for (UINT i = 0; i < argc; ++i) {
        VARIANT& v = params->rgvarg[argc - 1 - i];
        std::wstring text = Arg(v);
        fprintf(stderr, "gesher-webview:   arg %u: vt %u '%ls'\n", i,
                static_cast<unsigned>(V_VT(&v)), text.c_str());
      }
      fflush(stderr);
    }
    // Without type information WebView2's proxy may read "callJava" as a property
    // and then calls what it got: hand out a callable object for it.
    if (id == kCall && !callable_ && (flags & DISPATCH_PROPERTYGET) != 0 && argc == 0) {
      if (result != nullptr) {
        VariantInit(result);
        V_VT(result) = VT_DISPATCH;
        V_DISPATCH(result) = new Bridge(impl_, /*callable=*/true);
      }
      return S_OK;
    }
    bool call = (flags & DISPATCH_METHOD) != 0 &&
                (id == kCall || (callable_ && id == DISPID_VALUE));
    if (!call) return DISP_E_MEMBERNOTFOUND;
    if (argc != 3) return DISP_E_BADPARAMCOUNT;
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
  bool callable_;  // the object "callJava" reads as
};

// The STA owns frame references; handlers hold only a weak host and a native key.
void RetireFrameTree(Impl* impl, int64_t parent) {
  std::vector<int64_t> children;
  for (const auto& pair : impl->frames) {
    if (pair.second.parent == parent) children.push_back(pair.first);
  }
  for (int64_t key : children) {
    RetireFrameTree(impl, key);
    auto it = impl->frames.find(key);
    if (it != impl->frames.end()) {
      impl->RetireDocument(it->second.document);
      Impl::Frame retired = std::move(it->second);
      impl->frames.erase(it);
      (void)retired;
    }
  }
}

void RegisterFrame(const std::shared_ptr<Impl>& impl, ICoreWebView2Frame* raw, int64_t parent) {
  if (!raw || impl->destroyed) return;
  if (parent != 0 && impl->frames.count(parent) == 0) return;
  if (impl->next_frame == std::numeric_limits<int64_t>::max()) return;
  Com<ICoreWebView2Frame> frame(raw);
  Com<ICoreWebView2Frame2> messages;
  if (!frame.As(IID_ICoreWebView2Frame2, &messages)) return;
  const int64_t key = impl->next_frame++;
  Impl::Frame entry;
  entry.frame = frame;
  entry.messages = messages;
  entry.parent = parent;
  entry.document = impl->NewDocument(0);
  impl->frames.emplace(key, std::move(entry));
  std::weak_ptr<Impl> weak = impl;
  EventRegistrationToken token;
  HRESULT hr = frame->add_Destroyed(
      GESHER_CALLBACK(ICoreWebView2FrameDestroyedEventHandler,
                      [weak, key](ICoreWebView2Frame*, IUnknown*) -> HRESULT {
                        auto host = weak.lock();
                        if (!host || host->destroyed) return S_OK;
                        auto it = host->frames.find(key);
                        if (it != host->frames.end()) {
                          RetireFrameTree(host.get(), key);
                          it = host->frames.find(key);
                          if (it != host->frames.end()) {
                            host->RetireDocument(it->second.document);
                            Impl::Frame retired = std::move(it->second);
                            host->frames.erase(it);
                            (void)retired;
                          }
                        }
                        return S_OK;
                      }).get(), &token);
  if (SUCCEEDED(hr)) hr = messages->add_NavigationStarting(
      GESHER_CALLBACK(ICoreWebView2FrameNavigationStartingEventHandler,
                      [weak, key](ICoreWebView2Frame*, ICoreWebView2NavigationStartingEventArgs*) -> HRESULT {
                        auto host = weak.lock();
                        if (!host || host->destroyed) return S_OK;
                        auto it = host->frames.find(key);
                        if (it != host->frames.end()) {
                          RetireFrameTree(host.get(), key);
                          it = host->frames.find(key);
                          if (it != host->frames.end()) it->second.document = host->NewDocument(it->second.document);
                        }
                        return S_OK;
                      }).get(), &token);
  if (SUCCEEDED(hr)) hr = messages->add_WebMessageReceived(
      GESHER_CALLBACK(ICoreWebView2FrameWebMessageReceivedEventHandler,
                      [weak, key](ICoreWebView2Frame*, ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT {
                        auto host = weak.lock();
                        if (!host || host->destroyed) return S_OK;
                        auto it = host->frames.find(key);
                        if (it == host->frames.end()) return S_OK;
                        CoString json, source;
                        if (FAILED(args->get_WebMessageAsJson(json.Put())) || FAILED(args->get_Source(source.Put()))) return S_OK;
                        host->Report(kWebMessage, it->second.document, 1, json.str(), source.str());
                        return S_OK;
                      }).get(), &token);
  if (FAILED(hr)) {
    auto it = impl->frames.find(key);
    if (it != impl->frames.end()) {
      impl->RetireDocument(it->second.document);
      Impl::Frame retired = std::move(it->second);
      impl->frames.erase(it);
      (void)retired;
    }
    return;
  }
  Com<ICoreWebView2Frame7> nested;
  if (frame.As(IID_ICoreWebView2Frame7, &nested)) {
    nested->add_FrameCreated(
        GESHER_CALLBACK(ICoreWebView2FrameChildFrameCreatedEventHandler,
                        [weak, key](ICoreWebView2Frame*, ICoreWebView2FrameCreatedEventArgs* args) -> HRESULT {
                          auto host = weak.lock();
                          if (!host || host->destroyed) return S_OK;
                          Com<ICoreWebView2Frame> child;
                          if (SUCCEEDED(args->get_Frame(child.Put())) && child) RegisterFrame(host, child.get(), key);
                          return S_OK;
                        }).get(), &token);
  }
}

// Registers the controller's and the WebView's handlers. The WebView thread.
void AddHandlers(const std::shared_ptr<Impl>& shared) {
  Impl* impl = shared.get();
  std::weak_ptr<Impl> weak = shared;
  EventRegistrationToken token;
  ICoreWebView2* wv = impl->webview.get();
  impl->main_document_token = impl->NewDocument(0);
  Com<ICoreWebView2_4> frame_source;
  if (impl->webview.As(IID_ICoreWebView2_4, &frame_source)) {
    frame_source->add_FrameCreated(
        GESHER_CALLBACK(ICoreWebView2FrameCreatedEventHandler,
                        [weak](ICoreWebView2*, ICoreWebView2FrameCreatedEventArgs* args) -> HRESULT {
                          auto host = weak.lock();
                          if (!host || host->destroyed) return S_OK;
                          Com<ICoreWebView2Frame> frame;
                          if (SUCCEEDED(args->get_Frame(frame.Put())) && frame) RegisterFrame(host, frame.get(), 0);
                          return S_OK;
                        }).get(), &token);
  }

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
                        if (!impl->auth_retry.empty() && url == impl->auth_retry && !redirect) {
                          impl->auth_retry.clear();
                          RetireFrameTree(impl.get(), 0);
                          impl->main_document_token = impl->NewDocument(impl->main_document_token);
                          impl->main_document = url;
                          return S_OK;
                        }
                        impl->auth_retry.clear();
                        if (!api && impl->listener != nullptr &&
                            impl->listener->OnNavigationStarting(url, user != FALSE,
                                                                 redirect != FALSE)) {
                          args->put_Cancel(TRUE);
                          // Its NavigationCompleted (OPERATION_CANCELED) is not the
                          // app's: no onPageFinished for a navigation it took.
                          UINT64 nav = 0;
                          args->get_NavigationId(&nav);
                          impl->cancelled.insert(nav);
                          return S_OK;
                        }
                        RetireFrameTree(impl.get(), 0);
                        impl->main_document_token = impl->NewDocument(impl->main_document_token);
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
                        UINT64 nav = 0;
                        args->get_NavigationId(&nav);
                        if (impl->cancelled.erase(nav) > 0) return S_OK;
                        BOOL ok = FALSE;
                        args->get_IsSuccess(&ok);
                        COREWEBVIEW2_WEB_ERROR_STATUS status = COREWEBVIEW2_WEB_ERROR_STATUS_UNKNOWN;
                        args->get_WebErrorStatus(&status);
                        bool auth_wait =
                            !impl->auth_pending.empty() && !ok &&
                            status == COREWEBVIEW2_WEB_ERROR_STATUS_VALID_AUTHENTICATION_CREDENTIALS_REQUIRED;
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
                        if (auth_wait) {
                          impl->auth_finish = {true, http, static_cast<int32_t>(status), url};
                          return S_OK;
                        }
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
                        // No browser window of WebView2's own: the app decides
                        // (WebChromeClient.onCreateWindow; CompleteNewWindow).
                        args->put_Handled(TRUE);
                        if (impl->destroyed) return S_OK;
                        CoString uri;
                        args->get_Uri(uri.Put());
                        BOOL user = FALSE;
                        args->get_IsUserInitiated(&user);
                        bool dialog = false;
                        Com<ICoreWebView2WindowFeatures> features;
                        if (SUCCEEDED(args->get_WindowFeatures(features.Put())) && features) {
                          BOOL size = FALSE, position = FALSE;
                          features->get_HasSize(&size);
                          features->get_HasPosition(&position);
                          dialog = size || position;
                        }
                        Com<ICoreWebView2Deferral> deferral;
                        if (FAILED(args->GetDeferral(deferral.Put()))) return S_OK;
                        int64_t id = impl->Defer(args, deferral.get());
                        impl->Report(kNewWindow, id, user ? 1 : 0, uri.str(),
                                     dialog ? L"dialog" : L"");
                        return S_OK;
                      })
          .get(),
      &token);

  wv->add_WindowCloseRequested(
      GESHER_CALLBACK(ICoreWebView2WindowCloseRequestedEventHandler,
                      [weak](ICoreWebView2*, IUnknown*) -> HRESULT {
                        if (auto impl = weak.lock()) impl->Report(kCloseWindow);
                        return S_OK;
                      })
          .get(),
      &token);

  // Permissions go to the app (WebChromeClient.onPermissionRequest,
  // onGeolocationPermissionsShowPrompt): CompletePermission.
  wv->add_PermissionRequested(
      GESHER_CALLBACK(ICoreWebView2PermissionRequestedEventHandler,
                      [weak](ICoreWebView2*, ICoreWebView2PermissionRequestedEventArgs* args)
                          -> HRESULT {
                        auto impl = weak.lock();
                        Com<ICoreWebView2Deferral> deferral;
                        if (!impl || impl->destroyed || FAILED(args->GetDeferral(deferral.Put()))) {
                          args->put_State(COREWEBVIEW2_PERMISSION_STATE_DENY);
                          return S_OK;
                        }
                        CoString uri;
                        args->get_Uri(uri.Put());
                        COREWEBVIEW2_PERMISSION_KIND kind = COREWEBVIEW2_PERMISSION_KIND_UNKNOWN_PERMISSION;
                        args->get_PermissionKind(&kind);
                        BOOL user = FALSE;
                        args->get_IsUserInitiated(&user);
                        int64_t id = impl->Defer(args, deferral.get());
                        impl->Report(kPermission, id, static_cast<int32_t>(kind), uri.str(),
                                     user ? L"1" : L"0");
                        return S_OK;
                      })
          .get(),
      &token);

  // chrome.webview.postMessage from the page (WebMessagePort's page side).
  wv->add_WebMessageReceived(
      GESHER_CALLBACK(ICoreWebView2WebMessageReceivedEventHandler,
                      [weak](ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs* args)
                          -> HRESULT {
                        auto impl = weak.lock();
                        if (!impl || impl->destroyed) return S_OK;
                        CoString json, source;
                        args->get_WebMessageAsJson(json.Put());
                        args->get_Source(source.Put());
                        impl->Report(kWebMessage, impl->main_document_token, 0, json.str(), source.str());
                        return S_OK;
                      })
          .get(),
      &token);

  Com<ICoreWebView2_5> wv5;
  if (impl->webview.As(IID_ICoreWebView2_5, &wv5)) {
    wv5->add_ClientCertificateRequested(
        GESHER_CALLBACK(
            ICoreWebView2ClientCertificateRequestedEventHandler,
            [weak](ICoreWebView2*, ICoreWebView2ClientCertificateRequestedEventArgs* args)
                -> HRESULT {
              auto impl = weak.lock();
              Com<ICoreWebView2Deferral> deferral;
              if (!impl || impl->destroyed || FAILED(args->GetDeferral(deferral.Put()))) {
                args->put_Cancel(TRUE);
                return S_OK;
              }
              CoString host;
              args->get_Host(host.Put());
              INT port = 0;
              args->get_Port(&port);
              std::wstring certs = L"[";
              Com<ICoreWebView2ClientCertificateCollection> list;
              UINT32 count = 0;
              if (SUCCEEDED(args->get_MutuallyTrustedCertificates(list.Put())) && list) {
                list->get_Count(&count);
              }
              for (UINT32 i = 0; i < count; ++i) {
                Com<ICoreWebView2ClientCertificate> c;
                CoString subject, issuer, name, pem;
                if (SUCCEEDED(list->GetValueAtIndex(i, c.Put())) && c) {
                  c->get_Subject(subject.Put());
                  c->get_Issuer(issuer.Put());
                  c->get_DisplayName(name.Put());
                  c->ToPemEncoding(pem.Put());
                }
                if (i > 0) certs += L",";
                certs += L"{\"subject\":" + JsonString(subject.str()) +
                         L",\"issuer\":" + JsonString(issuer.str()) +
                         L",\"name\":" + JsonString(name.str()) +
                         L",\"pem\":" + JsonString(pem.str()) + L"}";
              }
              certs += L"]";
              std::wstring cas = L"[";
              Com<ICoreWebView2StringCollection> authorities;
              UINT32 n = 0;
              if (SUCCEEDED(args->get_AllowedCertificateAuthorities(authorities.Put())) &&
                  authorities) {
                authorities->get_Count(&n);
              }
              for (UINT32 i = 0; i < n; ++i) {
                CoString ca;
                authorities->GetValueAtIndex(i, ca.Put());
                if (i > 0) cas += L",";
                cas += JsonString(ca.str());
              }
              cas += L"]";
              int64_t id = impl->Defer(args, deferral.get());
              impl->Report(kClientCert, id, port, host.str(), certs, cas);
              return S_OK;
            })
            .get(),
        &token);
  }

  Com<ICoreWebView2_10> wv10;
  if (impl->webview.As(IID_ICoreWebView2_10, &wv10)) {
    wv10->add_BasicAuthenticationRequested(
        GESHER_CALLBACK(
            ICoreWebView2BasicAuthenticationRequestedEventHandler,
            [weak](ICoreWebView2*, ICoreWebView2BasicAuthenticationRequestedEventArgs* args)
                -> HRESULT {
              auto impl = weak.lock();
              Com<ICoreWebView2Deferral> deferral;
              if (!impl || impl->destroyed || FAILED(args->GetDeferral(deferral.Put()))) {
                args->put_Cancel(TRUE);
                return S_OK;
              }
              CoString uri, challenge;
              args->get_Uri(uri.Put());
              args->get_Challenge(challenge.Put());
              if (uri.str() == impl->main_document) impl->auth_pending = uri.str();
              int64_t id = impl->Defer(args, deferral.get());
              impl->Report(kBasicAuth, id, 0, uri.str(), challenge.str());
              return S_OK;
            })
            .get(),
        &token);
  }

  Com<ICoreWebView2_14> wv14;
  if (impl->webview.As(IID_ICoreWebView2_14, &wv14)) {
    wv14->add_ServerCertificateErrorDetected(
        GESHER_CALLBACK(
            ICoreWebView2ServerCertificateErrorDetectedEventHandler,
            [weak](ICoreWebView2*, ICoreWebView2ServerCertificateErrorDetectedEventArgs* args)
                -> HRESULT {
              auto impl = weak.lock();
              Com<ICoreWebView2Deferral> deferral;
              if (!impl || impl->destroyed || FAILED(args->GetDeferral(deferral.Put()))) {
                return S_OK;
              }
              CoString uri, pem;
              args->get_RequestUri(uri.Put());
              COREWEBVIEW2_WEB_ERROR_STATUS status = COREWEBVIEW2_WEB_ERROR_STATUS_UNKNOWN;
              args->get_ErrorStatus(&status);
              Com<ICoreWebView2Certificate> cert;
              if (SUCCEEDED(args->get_ServerCertificate(cert.Put())) && cert) {
                cert->ToPemEncoding(pem.Put());
              }
              int64_t id = impl->Defer(args, deferral.get());
              impl->Report(kServerCertError, id, static_cast<int32_t>(status), uri.str(),
                           pem.str());
              return S_OK;
            })
            .get(),
        &token);
  }

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

// Controller creations waiting for the one in progress (the WebView thread).
std::deque<std::function<void()>> g_controller_queue;
bool g_controller_busy = false;

void NextController() {
  if (g_controller_busy || g_controller_queue.empty()) return;
  std::function<void()> next = std::move(g_controller_queue.front());
  g_controller_queue.pop_front();
  g_controller_busy = true;
  next();
}

void ControllerDone() {
  g_controller_busy = false;
  NextController();
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
  impl->published_container.store(impl->container);
  SetWindowLongPtrW(impl->container, GWLP_USERDATA, static_cast<LONG_PTR>(impl->options.background));
  if (parent != nullptr) EnsureClipChildren(parent);
  WithEnvironment([impl](ICoreWebView2Environment* env, HRESULT hr) {
    if (impl->destroyed) return;
    if (env == nullptr) {
      Fail(impl.get(), hr, L"CreateCoreWebView2EnvironmentWithOptions");
      return;
    }
    impl->env = Com<ICoreWebView2Environment>(env);
    // One controller at a time: a second CreateCoreWebView2Controller while
    // one is pending fails with ERROR_INVALID_STATE (0x8007139f; ci run
    // 40129082164, an Activity with two WebViews).
    g_controller_queue.push_back([impl] {
      if (impl->destroyed || !impl->env) {
        ControllerDone();
        return;
      }
      HRESULT created = impl->env->CreateCoreWebView2Controller(
          impl->container,
          GESHER_CALLBACK(ICoreWebView2CreateCoreWebView2ControllerCompletedHandler,
                          [impl](HRESULT result, ICoreWebView2Controller* controller) -> HRESULT {
                            OnController(impl, result, controller);
                            ControllerDone();
                            return S_OK;
                          })
              .get());
      if (FAILED(created)) {
        Fail(impl.get(), created, L"CreateCoreWebView2Controller");
        ControllerDone();
      }
    });
    NextController();
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
  // Java's paths are drive-less (ADR 0010: /a/b is \a\b on the current
  // drive); WebView2 wants a full Windows path.
  std::wstring path = user_data_folder;
  for (wchar_t& ch : path) {
    if (ch == L'/') ch = L'\\';
  }
  if (!path.empty()) {
    wchar_t full[MAX_PATH * 2];
    DWORD n = GetFullPathNameW(path.c_str(), MAX_PATH * 2, full, nullptr);
    if (n > 0 && n < MAX_PATH * 2) path.assign(full, n);
  }
  g_user_data = path;
  g_browser_args = browser_args;
}

HWND Host::Container() const { return impl_->published_container.load(); }

bool Host::SetProxyArguments(const std::wstring& args) {
  std::lock_guard<std::mutex> lock(g_config_mutex);
  g_proxy_args = args;
  return !g_env_started;
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
  {
    std::lock_guard<std::mutex> guard(impl->document_mutex);
    impl->documents_closed = true;
    impl->current_documents.clear();
  }
  PostToWebViewThread([impl] {
    if (impl->destroyed) return;
    ClearDocumentTimer(impl.get());
    impl->document_waiting.clear();
    // Pending requests and dialogs go on unanswered.
    for (auto& r : impl->requests) r.second.deferral->Complete();
    impl->requests.clear();
    for (auto& d : impl->dialogs) d.second.deferral->Complete();
    impl->dialogs.clear();
    for (auto& d : impl->deferred) {
      Com<ICoreWebView2PermissionRequestedEventArgs> permission;
      Com<ICoreWebView2BasicAuthenticationRequestedEventArgs> auth;
      Com<ICoreWebView2ClientCertificateRequestedEventArgs> cert;
      if (d.second.args.As(IID_ICoreWebView2PermissionRequestedEventArgs, &permission)) {
        permission->put_State(COREWEBVIEW2_PERMISSION_STATE_DENY);
      } else if (d.second.args.As(IID_ICoreWebView2BasicAuthenticationRequestedEventArgs, &auth)) {
        auth->put_Cancel(TRUE);
      } else if (d.second.args.As(IID_ICoreWebView2ClientCertificateRequestedEventArgs, &cert)) {
        cert->put_Cancel(TRUE);
      }
      d.second.deferral->Complete();
    }
    impl->deferred.clear();
    impl->find.Reset();
    // Reentrant frame-destroy callbacks must not mutate the map being released.
    impl->destroyed = true;
    impl->frames.clear();
    g_live.erase(impl.get());
    if (impl->controller) impl->controller->Close();
    impl->receivers.clear();
    impl->controller.Reset();
    impl->webview.Reset();
    impl->published_container.store(nullptr);
    if (impl->container != nullptr) DestroyWindow(impl->container);
    impl->container = nullptr;
    impl->pending.clear();
    Listener* listener = impl->listener;
    if (listener != nullptr) listener->OnDestroyed();
  });
  delete this;
}

void Host::Navigate(const std::wstring& url, const std::wstring& headers,
                    const std::wstring& method, std::vector<uint8_t> body) {
  Do(impl_, [url, headers, method, body = std::move(body)](Impl* impl) {
    RunDocumentNavigation(impl, [url, headers, method, body](Impl* impl) {
    impl->api_navigation = true;
    HRESULT hr = E_FAIL;
    Com<ICoreWebView2_2> wv2;
    Com<ICoreWebView2Environment2> env2;
    bool get = method.empty() || method == L"GET";
    if ((!headers.empty() || !get) && impl->webview.As(IID_ICoreWebView2_2, &wv2) &&
        impl->env.As(IID_ICoreWebView2Environment2, &env2)) {
      Com<IStream> stream;
      if (!get) {
        stream = Com<IStream>(SHCreateMemStream(body.empty() ? nullptr : body.data(),
                                                static_cast<UINT>(body.size())),
                              /*add_ref=*/false);
      }
      Com<ICoreWebView2WebResourceRequest> request;
      hr = env2->CreateWebResourceRequest(url.c_str(), get ? L"GET" : method.c_str(),
                                          stream.get(), headers.c_str(), request.Put());
      if (SUCCEEDED(hr)) hr = wv2->NavigateWithWebResourceRequest(request.get());
    } else {
      hr = impl->webview->Navigate(url.c_str());
    }
    if (FAILED(hr)) {
      impl->api_navigation = false;
      impl->Report(kFailed, 0, static_cast<int32_t>(hr), L"Navigate " + url + L": " + Hex(hr));
    }
    });
  });
}

void Host::NavigateToString(const std::wstring& html) {
  Do(impl_, [html](Impl* impl) {
    RunDocumentNavigation(impl, [html](Impl* impl) {
    impl->api_navigation = true;
    HRESULT hr = impl->webview->NavigateToString(html.c_str());
    if (FAILED(hr)) {
      impl->api_navigation = false;
      impl->Report(kFailed, 0, static_cast<int32_t>(hr), L"NavigateToString: " + Hex(hr));
    }
    });
  });
}

void Host::Run(Command command) {
  Do(impl_, [command](Impl* impl) {
    if (command == kStop) {
      impl->document_waiting.clear();
      impl->webview->Stop();
      return;
    }
    if (command == kReload || command == kGoBack || command == kGoForward) {
      RunDocumentNavigation(impl, [command](Impl* current) {
        current->api_navigation = true;
        if (command == kReload) current->webview->Reload();
        else if (command == kGoBack) current->webview->GoBack();
        else current->webview->GoForward();
      });
      return;
    }
    switch (command) {
      case kReload:
      case kStop:
      case kGoBack:
      case kGoForward:
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
                     int clip_width, int clip_height, bool visible,
                     const std::vector<Shape>& shapes) {
  std::shared_ptr<Impl> shared = impl_;
  PostToWebViewThread([shared, parent, x, y, width, height, clip_x, clip_y, clip_width,
                       clip_height, visible, shapes] {
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
    impl->clip = {clip_x, clip_y, clip_x + clip_width, clip_y + clip_height};
    impl->shapes = shapes;
    ApplyRegion(impl);
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
    if (impl->document_script_inflight != 0 &&
        (!CheckDocumentDeadline(impl) || impl->document_script_failed)) return;
    impl->document_script_failed = false;
    if (!impl->document_script_id.empty()) {
      HRESULT removed = impl->webview->RemoveScriptToExecuteOnDocumentCreated(impl->document_script_id.c_str());
      if (FAILED(removed)) {
        impl->document_script_failed = true;
        impl->document_waiting.clear();
        impl->Report(kFailed, 0, static_cast<int32_t>(removed), L"document script removal failed");
        return;
      }
      impl->document_script_id.clear();
    }
    uint64_t generation = ++impl->document_script_generation;
    if (script.empty()) { DrainDocumentNavigations(impl); return; }
    ++impl->document_script_inflight;
    ArmDocumentTimer(impl->shared_from_this());
    if (impl->document_script_failed) {
      --impl->document_script_inflight;
      if (impl->document_script_inflight == 0) ClearDocumentTimer(impl);
      return;
    }
    HRESULT started = impl->webview->AddScriptToExecuteOnDocumentCreated(
        script.c_str(),
        GESHER_CALLBACK(ICoreWebView2AddScriptToExecuteOnDocumentCreatedCompletedHandler,
                        [weak, generation](HRESULT error, LPCWSTR id) -> HRESULT {
                          auto impl = weak.lock();
                          if (!impl || impl->destroyed || !impl->webview) return S_OK;
                          CheckDocumentDeadline(impl.get());
                          if (SUCCEEDED(error) && id != nullptr) {
                            if (generation != impl->document_script_generation) {
                              HRESULT removed = impl->webview->RemoveScriptToExecuteOnDocumentCreated(id);
                              if (FAILED(removed)) impl->document_script_failed = true;
                            } else {
                              impl->document_script_id = id;
                            }
                          } else if (generation == impl->document_script_generation) {
                            impl->document_script_failed = true;
                          }
                          --impl->document_script_inflight;
                          if (impl->document_script_failed) impl->Report(kFailed, 0, E_FAIL, L"document script registration or removal failed");
                          DrainDocumentNavigations(impl.get());
                          return S_OK;
                        })
            .get());
    if (FAILED(started)) {
      --impl->document_script_inflight;
      impl->document_script_failed = true;
      impl->Report(kFailed, 0, static_cast<int32_t>(started), L"document script registration failed");
      DrainDocumentNavigations(impl);
    }
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

void Host::PostWebMessage(const std::wstring& json) {
  Do(impl_, [json](Impl* impl) {
    HRESULT hr = impl->webview->PostWebMessageAsJson(json.c_str());
    if (FAILED(hr)) {
      impl->Report(kFailed, 0, static_cast<int32_t>(hr), L"PostWebMessageAsJson: " + Hex(hr));
    }
  });
}

bool Host::IsDocumentCurrent(int64_t document) const {
  std::shared_ptr<Impl> impl = impl_;
  std::lock_guard<std::mutex> guard(impl->document_mutex);
  return document > 0 && impl->current_documents.count(document) != 0;
}

void Host::PostFrameMessage(int64_t document, const std::wstring& json) {
  Do(impl_, [document, json](Impl* impl) {
    {
      std::lock_guard<std::mutex> guard(impl->document_mutex);
      if (document <= 0 || impl->current_documents.count(document) == 0) return;
    }
    if (document == impl->main_document_token) {
      impl->webview->PostWebMessageAsJson(json.c_str());
      return;
    }
    for (auto& pair : impl->frames) {
      if (pair.second.document == document) {
        pair.second.messages->PostWebMessageAsJson(json.c_str());
        return;
      }
    }
  });
}

void Host::CallDevTools(const std::wstring& method, const std::wstring& params_json,
                        int64_t callback_id) {
  std::weak_ptr<Impl> weak = impl_;
  Do(impl_, [method, params_json, callback_id, weak](Impl* impl) {
    HRESULT hr = impl->webview->CallDevToolsProtocolMethod(
        method.c_str(), params_json.empty() ? L"{}" : params_json.c_str(),
        GESHER_CALLBACK(ICoreWebView2CallDevToolsProtocolMethodCompletedHandler,
                        [weak, callback_id](HRESULT error, LPCWSTR json) -> HRESULT {
                          if (auto impl = weak.lock()) {
                            impl->Report(kDevToolsResult, callback_id,
                                         static_cast<int32_t>(error),
                                         json != nullptr ? json : L"null");
                          }
                          return S_OK;
                        })
            .get());
    if (FAILED(hr)) impl->Report(kDevToolsResult, callback_id, static_cast<int32_t>(hr), L"null");
  });
}

void Host::ListenDevTools(const std::wstring& event) {
  std::weak_ptr<Impl> weak = impl_;
  Do(impl_, [event, weak](Impl* impl) {
    if (!impl->listened.insert(event).second) return;
    Com<ICoreWebView2DevToolsProtocolEventReceiver> receiver;
    if (FAILED(impl->webview->GetDevToolsProtocolEventReceiver(event.c_str(), receiver.Put())) ||
        !receiver) {
      return;
    }
    EventRegistrationToken token;
    receiver->add_DevToolsProtocolEventReceived(
        GESHER_CALLBACK(ICoreWebView2DevToolsProtocolEventReceivedEventHandler,
                        [weak, event](ICoreWebView2*,
                                      ICoreWebView2DevToolsProtocolEventReceivedEventArgs* args)
                            -> HRESULT {
                          auto impl = weak.lock();
                          if (!impl) return S_OK;
                          CoString json;
                          args->get_ParameterObjectAsJson(json.Put());
                          impl->Report(kDevToolsEvent, 0, 0, event, json.str());
                          return S_OK;
                        })
            .get(),
        &token);
    impl->receivers.push_back(std::move(receiver));
  });
}

void Host::CompletePermission(int64_t id, int32_t state, bool remember) {
  std::shared_ptr<Impl> shared = impl_;
  PostToWebViewThread([shared, id, state, remember] {
    Impl::Deferred d;
    if (!shared->Undefer(id, &d)) return;
    Com<ICoreWebView2PermissionRequestedEventArgs> args;
    if (d.args.As(IID_ICoreWebView2PermissionRequestedEventArgs, &args)) {
      Com<ICoreWebView2PermissionRequestedEventArgs3> args3;
      if (args.As(IID_ICoreWebView2PermissionRequestedEventArgs3, &args3)) {
        args3->put_SavesInProfile(remember ? TRUE : FALSE);
      }
      args->put_State(static_cast<COREWEBVIEW2_PERMISSION_STATE>(state));
    }
    d.deferral->Complete();
  });
}

void Host::CompleteNewWindow(int64_t id, Host* window) {
  std::shared_ptr<Impl> opener = impl_;
  auto finish = [opener, id](ICoreWebView2* wv) {
    Impl::Deferred d;
    if (!opener->Undefer(id, &d)) return;
    Com<ICoreWebView2NewWindowRequestedEventArgs> args;
    if (wv != nullptr && d.args.As(IID_ICoreWebView2NewWindowRequestedEventArgs, &args)) {
      HRESULT hr = args->put_NewWindow(wv);
      if (FAILED(hr)) opener->Report(kFailed, 0, static_cast<int32_t>(hr), L"put_NewWindow: " + Hex(hr));
    }
    d.deferral->Complete();
  };
  if (window == nullptr || !window->impl_) {
    PostToWebViewThread([finish] { finish(nullptr); });
    return;
  }
  std::shared_ptr<Impl> target = window->impl_;
  PostToWebViewThread([target, finish] {
    if (target->destroyed || target->failed) {
      finish(nullptr);
    } else if (target->ready) {
      finish(target->webview.get());
    } else {
      // Once the new WebView's controller exists (and its settings and
      // document scripts, posted before, are applied).
      target->pending.push_back([finish](Impl* t) { finish(t->webview.get()); });
    }
  });
}

void Host::CompleteBasicAuth(int64_t id, bool proceed, const std::wstring& user,
                             const std::wstring& password) {
  std::shared_ptr<Impl> shared = impl_;
  PostToWebViewThread([shared, id, proceed, user, password] {
    Impl::Deferred d;
    if (!shared->Undefer(id, &d)) return;
    Com<ICoreWebView2BasicAuthenticationRequestedEventArgs> args;
    bool main = false;
    if (d.args.As(IID_ICoreWebView2BasicAuthenticationRequestedEventArgs, &args)) {
      CoString uri;
      args->get_Uri(uri.Put());
      main = !shared->auth_pending.empty() && uri.str() == shared->auth_pending;
      Com<ICoreWebView2BasicAuthenticationResponse> response;
      if (proceed && SUCCEEDED(args->get_Response(response.Put())) && response) {
        response->put_UserName(user.c_str());
        response->put_Password(password.c_str());
        if (main) shared->auth_retry = uri.str();
      } else {
        args->put_Cancel(TRUE);
      }
    }
    if (main) {
      shared->auth_pending.clear();
      Impl::Finish held = shared->auth_finish;
      shared->auth_finish = {};
      // Cancelled: the 401 page is what loaded.
      if (!proceed && held.held) shared->Report(kPageFinished, held.http, held.status, held.url);
    }
    d.deferral->Complete();
  });
}

void Host::CompleteClientCert(int64_t id, int32_t choice) {
  std::shared_ptr<Impl> shared = impl_;
  PostToWebViewThread([shared, id, choice] {
    Impl::Deferred d;
    if (!shared->Undefer(id, &d)) return;
    Com<ICoreWebView2ClientCertificateRequestedEventArgs> args;
    if (d.args.As(IID_ICoreWebView2ClientCertificateRequestedEventArgs, &args)) {
      if (choice == -2) {
        args->put_Cancel(TRUE);
      } else {
        Com<ICoreWebView2ClientCertificateCollection> list;
        Com<ICoreWebView2ClientCertificate> cert;
        if (choice >= 0 && SUCCEEDED(args->get_MutuallyTrustedCertificates(list.Put())) && list &&
            SUCCEEDED(list->GetValueAtIndex(static_cast<UINT32>(choice), cert.Put())) && cert) {
          args->put_SelectedCertificate(cert.get());
        }
        args->put_Handled(TRUE);
      }
    }
    d.deferral->Complete();
  });
}

void Host::CompleteServerCertError(int64_t id, bool allow) {
  std::shared_ptr<Impl> shared = impl_;
  PostToWebViewThread([shared, id, allow] {
    Impl::Deferred d;
    if (!shared->Undefer(id, &d)) return;
    Com<ICoreWebView2ServerCertificateErrorDetectedEventArgs> args;
    if (d.args.As(IID_ICoreWebView2ServerCertificateErrorDetectedEventArgs, &args)) {
      args->put_Action(allow ? COREWEBVIEW2_SERVER_CERTIFICATE_ERROR_ACTION_ALWAYS_ALLOW
                             : COREWEBVIEW2_SERVER_CERTIFICATE_ERROR_ACTION_CANCEL);
    }
    d.deferral->Complete();
  });
}

void Host::Find(int32_t op, const std::wstring& term, bool case_sensitive, bool whole_word) {
  std::weak_ptr<Impl> weak = impl_;
  Do(impl_, [op, term, case_sensitive, whole_word, weak](Impl* impl) {
    auto report = [weak](const wchar_t* what) {
      auto impl = weak.lock();
      if (!impl || !impl->find) return;
      INT32 active = -1, count = 0;
      impl->find->get_ActiveMatchIndex(&active);
      impl->find->get_MatchCount(&count);
      impl->Report(kFind, active, count, what);
    };
    if (!impl->find) {
      Com<ICoreWebView2_28> wv28;
      if (!impl->webview.As(IID_ICoreWebView2_28, &wv28) ||
          FAILED(wv28->get_Find(impl->find.Put())) || !impl->find) {
        impl->find.Reset();
        impl->Report(kFind, -1, 0, L"unsupported");
        return;
      }
      EventRegistrationToken token;
      impl->find->add_MatchCountChanged(
          GESHER_CALLBACK(ICoreWebView2FindMatchCountChangedEventHandler,
                          [report](ICoreWebView2Find*, IUnknown*) -> HRESULT {
                            report(L"count");
                            return S_OK;
                          })
              .get(),
          &token);
      impl->find->add_ActiveMatchIndexChanged(
          GESHER_CALLBACK(ICoreWebView2FindActiveMatchIndexChangedEventHandler,
                          [report](ICoreWebView2Find*, IUnknown*) -> HRESULT {
                            report(L"active");
                            return S_OK;
                          })
              .get(),
          &token);
    }
    switch (op) {
      case kFindStart: {
        Com<ICoreWebView2Environment15> env15;
        Com<ICoreWebView2FindOptions> options;
        if (!impl->env.As(IID_ICoreWebView2Environment15, &env15) ||
            FAILED(env15->CreateFindOptions(options.Put())) || !options) {
          impl->Report(kFind, -1, 0, L"unsupported");
          return;
        }
        options->put_FindTerm(term.c_str());
        options->put_IsCaseSensitive(case_sensitive ? TRUE : FALSE);
        options->put_ShouldMatchWord(whole_word ? TRUE : FALSE);
        options->put_ShouldHighlightAllMatches(TRUE);
        options->put_SuppressDefaultFindDialog(TRUE);
        HRESULT hr = impl->find->Start(
            options.get(), GESHER_CALLBACK(ICoreWebView2FindStartCompletedHandler,
                                           [report](HRESULT) -> HRESULT {
                                             report(L"done");
                                             return S_OK;
                                           })
                               .get());
        if (FAILED(hr)) impl->Report(kFind, -1, 0, L"unsupported");
        break;
      }
      case kFindNext:
        impl->find->FindNext();
        break;
      case kFindPrevious:
        impl->find->FindPrevious();
        break;
      case kFindStop:
        impl->find->Stop();
        break;
    }
  });
}

void Host::ShowFileDialog(int64_t callback_id, bool multiple, const std::wstring& title,
                          const std::wstring& filters) {
  std::shared_ptr<Impl> shared = impl_;
  PostToWebViewThread([shared, callback_id, multiple, title, filters] {
    Impl* impl = shared.get();
    if (impl->destroyed) return;
    HWND owner = impl->container != nullptr ? GetAncestor(impl->container, GA_ROOT) : nullptr;
    if (owner == g_parking) owner = nullptr;
    Com<IFileOpenDialog> dialog;
    HRESULT hr = CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_IFileOpenDialog, reinterpret_cast<void**>(dialog.Put()));
    std::wstring files;
    if (SUCCEEDED(hr)) {
      FILEOPENDIALOGOPTIONS options = 0;
      dialog->GetOptions(&options);
      options |= FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST | FOS_PATHMUSTEXIST;
      if (multiple) options |= FOS_ALLOWMULTISELECT;
      dialog->SetOptions(options);
      if (!title.empty()) dialog->SetTitle(title.c_str());
      // "Name|*.a;*.b" lines.
      std::vector<std::wstring> names, specs;
      size_t start = 0;
      while (start < filters.size()) {
        size_t end = filters.find(L'\n', start);
        if (end == std::wstring::npos) end = filters.size();
        std::wstring line = filters.substr(start, end - start);
        size_t bar = line.find(L'|');
        if (bar != std::wstring::npos) {
          names.push_back(line.substr(0, bar));
          specs.push_back(line.substr(bar + 1));
        }
        start = end + 1;
      }
      std::vector<COMDLG_FILTERSPEC> spec;
      for (size_t i = 0; i < names.size(); ++i) spec.push_back({names[i].c_str(), specs[i].c_str()});
      if (!spec.empty()) dialog->SetFileTypes(static_cast<UINT>(spec.size()), spec.data());
      hr = dialog->Show(owner);
      Com<IShellItemArray> items;
      if (SUCCEEDED(hr)) hr = dialog->GetResults(items.Put());
      DWORD count = 0;
      if (SUCCEEDED(hr) && items) items->GetCount(&count);
      for (DWORD i = 0; i < count; ++i) {
        Com<IShellItem> item;
        LPWSTR path = nullptr;
        if (SUCCEEDED(items->GetItemAt(i, item.Put())) && item &&
            SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path != nullptr) {
          if (!files.empty()) files += L"\n";
          files += path;
          CoTaskMemFree(path);
        }
      }
    }
    if (!impl->destroyed) impl->Report(kFileDialog, callback_id, static_cast<int32_t>(hr), files);
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
