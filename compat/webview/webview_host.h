// Gesher: android.webkit.WebView on Microsoft Edge WebView2 (docs/webview.md,
// ADR 0041), the native half: a WebView2 controller per WebView, hosted in a
// child window of the app's Win32 window.
//
// Threads. WebView2 is single-threaded (an STA with a message loop), so every
// WebView of the process lives on one thread of its own, "the WebView
// thread", started with the first one. Every Host method may be called from
// any thread: it posts the work to the WebView thread, in order, and returns.
// What WebView2 reports comes back on the WebView thread through the Host's
// Listener (the JNI side hands it on to the app's Looper).
//
// Windows. Each Host has a container window (a WS_CHILD window of class
// "GesherWebView") that WebView2 draws into. Its parent is the Android
// window's Win32 window (gesher::Window, compat/hwui/window) while the
// WebView is attached to it, or a hidden "parking" window of the WebView
// thread otherwise. The container covers the part of the WebView that is
// visible (its clip rectangle); the controller's bounds are the whole view,
// relative to the container, so what is scrolled out of view is cut off.
#pragma once

#include <stdint.h>

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "com_util.h"

namespace gesher::webview {

// Events a Host reports (Listener::OnEvent); the Java side
// (com.gesher.app.webview.WebView2Host) has the same numbers.
enum Event : int32_t {
  kReady = 1,          // arg: 0; a: the runtime's version
  kFailed = 2,         // arg: HRESULT; a: what failed
  kPageStarted = 3,    // id: navigation id; arg: 1 if the app started it; a: url
  kPageFinished = 4,   // id: HTTP status (0 unknown); arg: -1 success, else
                       // COREWEBVIEW2_WEB_ERROR_STATUS; a: url
  kSourceChanged = 5,  // a: url
  kHistory = 6,        // arg: 1 can go back | 2 can go forward
  kTitle = 7,          // a: title
  kContentLoading = 8, // arg: 1 for an error page
  kDomContentLoaded = 9,
  kScriptResult = 10,  // id: callback id; arg: HRESULT; a: the result as JSON
  kDialog = 11,        // id: dialog id; arg: kind (1 alert, 2 confirm, 3 prompt,
                       // 4 beforeunload); a: url; b: message; c: default text
  kConsole = 12,       // a: DevTools event name; b: its parameters (JSON)
  kDownload = 13,      // id: total bytes (-1 unknown); a: url; b: MIME type;
                       // c: Content-Disposition
  kFocus = 14,         // arg: 1 gained, 0 lost, 2 tab past the end, 3 before the start
  kProcessFailed = 15, // arg: COREWEBVIEW2_PROCESS_FAILED_KIND
  kNewWindow = 16,     // arg: 1 if user initiated; a: url
  kCapture = 17,       // id: callback id; arg: HRESULT; bytes: PNG
  kParked = 18,        // id: request id (the container is parked)
};

// Global results (Listener-less operations: cookies, browsing data).
enum GlobalEvent : int32_t {
  kGlobalCookies = 1,  // id: callback id; arg: HRESULT; a: "name=value; ..."
  kGlobalDone = 2,     // id: callback id; arg: HRESULT
};

class Listener {
 public:
  virtual ~Listener() = default;
  // On the WebView thread.
  virtual void OnEvent(int32_t type, int64_t id, int32_t arg, const std::wstring& a,
                       const std::wstring& b, const std::wstring& c,
                       const std::vector<uint8_t>* bytes) = 0;
  // A main-frame navigation the page (or the user) started is about to begin:
  // true cancels it (WebViewClient.shouldOverrideUrlLoading). On the WebView
  // thread, synchronously.
  virtual bool OnNavigationStarting(const std::wstring& url, bool user_initiated,
                                    bool redirect) = 0;
  // A request the Listener asked to see (SetSettings' intercept_all, file
  // URLs, documents): answer with Host::RespondToRequest(id, ...), from any
  // thread, later. On the WebView thread.
  virtual void OnResourceRequest(int64_t id, const std::wstring& url, const std::wstring& method,
                                 const std::wstring& headers, int32_t context,
                                 bool main_frame) = 0;
  // window.<name>.<method>(...) from the page: the result as the bridge
  // script expects it ({"v": value} or {"e": message}). Synchronous, on the
  // WebView thread (Android calls @JavascriptInterface methods off the UI
  // thread too).
  virtual std::wstring OnBridgeCall(const std::wstring& object, const std::wstring& method,
                                    const std::wstring& args_json) = 0;
  // The Host is gone (after Destroy): the Listener may be deleted.
  virtual void OnDestroyed() = 0;
};

// Where file:///... URLs are served from (docs/webview.md, "URLs"):
// file:///android_asset/x is https://gesher-file.example/android_asset/x.
constexpr wchar_t kFileOrigin[] = L"https://gesher-file.example";

struct HostOptions {
  // The Win32 window to show the WebView in, or null: parked (hidden).
  HWND parent = nullptr;
  // Device pixels per CSS pixel (Android's density / 160).
  double scale = 1.0;
  // Background before the page draws (0xAARRGGBB).
  uint32_t background = 0xffffffff;
};

class Host {
 public:
  // The process's WebView2 environment: the user data folder (the app's
  // app_webview directory) and extra browser arguments. The first call wins;
  // later ones are ignored.
  static void Configure(const std::wstring& user_data_folder, const std::wstring& browser_args);
  // The runtime's version, or an empty string and *hr when none is installed.
  static std::wstring RuntimeVersion(HRESULT* hr);

  // A new WebView. The listener is called on the WebView thread until
  // OnDestroyed.
  static Host* Create(Listener* listener, const HostOptions& options);

  void Destroy();  // the Host goes; OnDestroyed follows on the WebView thread

  // Navigation. headers: "Name: value\r\n..." or empty.
  void Navigate(const std::wstring& url, const std::wstring& headers);
  void NavigateToString(const std::wstring& html);
  enum Command : int32_t {
    kReload = 1,
    kStop = 2,
    kGoBack = 3,
    kGoForward = 4,
    kFocus = 5,     // MoveFocus(programmatic)
    kBlur = 6,      // keyboard focus back to the parent window
    kDevTools = 7,  // OpenDevToolsWindow
  };
  void Run(Command command);

  // Where the WebView is: its rectangle (x, y, width, height) and the part of
  // it that is visible (clip_*), both in the parent's client coordinates.
  // A new parent (or null: park it) moves the container there first.
  void SetBounds(HWND parent, int x, int y, int width, int height, int clip_x, int clip_y,
                 int clip_width, int clip_height, bool visible);
  // Parks the container; Listener gets kParked(id) once it is done.
  void Park(int64_t id);

  struct Settings {
    bool javascript = true;
    bool zoom_controls = true;
    bool pinch_zoom = true;
    bool devtools = false;
    bool intercept_all = false;  // every request to OnResourceRequest
    std::wstring user_agent;     // empty: WebView2's
  };
  void SetSettings(const Settings& settings);
  void SetZoom(double factor);
  void SetBackground(uint32_t argb);

  void ExecuteScript(const std::wstring& script, int64_t callback_id);
  // The script run in every new document before its own (the JavaScript
  // interfaces' bridge); replaces the previous one. Empty: none.
  void SetDocumentScript(const std::wstring& script);
  // Answers OnResourceRequest(id): status 0 lets the request go to the
  // network; otherwise the response (headers "Name: value\r\n...").
  void RespondToRequest(int64_t id, int32_t status, const std::wstring& reason,
                        const std::wstring& headers, std::vector<uint8_t> body);
  // Answers kDialog(id): accept (OK) or not, and a prompt's text.
  void CompleteDialog(int64_t id, bool accept, const std::wstring& text);
  // A PNG of what the WebView shows (CapturePreview): kCapture(id).
  void CapturePreview(int64_t callback_id);

  // Cookies and browsing data (any WebView's profile: they share one). The
  // result comes to the global callback.
  using GlobalCallback = void (*)(int32_t type, int64_t id, int32_t arg, const std::wstring& a);
  static void SetGlobalCallback(GlobalCallback callback);
  struct Cookie {
    std::wstring name, value, domain, path;
    double expires = -1;  // seconds since 1970; -1: a session cookie
    bool secure = false, http_only = false;
  };
  static void SetCookie(const std::wstring& url, const Cookie& cookie, int64_t callback_id);
  static void GetCookies(const std::wstring& url, int64_t callback_id);
  // which: 1 all cookies, 2 session cookies.
  static void DeleteCookies(int32_t which, int64_t callback_id);
  // COREWEBVIEW2_BROWSING_DATA_KINDS.
  static void ClearBrowsingData(int32_t kinds, int64_t callback_id);

  // For the WebView thread (webview_host.cc).
  struct Impl;

 private:
  explicit Host(std::shared_ptr<Impl> impl) : impl_(std::move(impl)) {}
  std::shared_ptr<Impl> impl_;
};

// Runs fn on the WebView thread (started on first use), in order.
void PostToWebViewThread(std::function<void()> fn);

}  // namespace gesher::webview
