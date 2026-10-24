# WebView on WebView2 (roadmap phase 5)

Goal: `android.webkit.WebView` works in Gesher's apps, backed by Microsoft
Edge WebView2 (the Chromium runtime Windows 10/11 ship), with no WebView APK.
Work happens on branch `wip/webview`; the decision record is
[ADR 0041](decisions/0041-webview-webview2.md).

## How it fits

```
 app's View tree (hwui draws it into the app's Win32 window, gesher::Window)
   └─ android.webkit.WebView ── mProvider ──► GesherWebViewProvider (gesher-app.jar)
                                                │  WebView2Host (JNI, posts to ...)
                                                ▼
 the WebView thread (one per process, an STA with a message loop)
   Host (compat/webview/webview_host.cc) ── ICoreWebView2Controller
     container window "GesherWebView" = WS_CHILD of the app's Win32 window,
     at the view's visible rectangle; WebView2 draws into it (DirectComposition)
```

- **The provider.** `com.gesher.app.webview.GesherWebView.install()` (called
  from `Main.bootstrap`, so in gesher-app's single process, the system server
  and every app process) sets `WebViewFactory.sProviderInstance` to
  `GesherWebViewFactory`. `WebViewFactory.getProvider()` returns it before it
  would look for a WebView package, so `new WebView(context)`,
  `CookieManager.getInstance()`, `WebStorage.getInstance()`,
  `WebSettings.getDefaultUserAgent()` and `WebView.setWebContentsDebuggingEnabled`
  work with no WebView APK and no `webviewupdate` service.
- **One WebView = one WebView2 controller** (`GesherWebViewProvider`, which is
  the WebViewProvider, its ViewDelegate and ScrollDelegate). It is created
  with the WebView, parked in a hidden window of the WebView thread until the
  view is attached; `loadUrl` and friends work before that.
- **Placement.** On attach the provider registers pre-draw, scroll-changed and
  global-layout listeners. Each time the view's place may have changed it
  computes the HWND of the window it is in (`WindowSession.hwndOf(view.getWindowToken())`:
  the session keys its windows by the IWindow binder, which is the view's
  window token), `getLocationInWindow` and `getGlobalVisibleRect`, and when
  anything differs sends them to the host: the container window gets the
  visible rectangle (so a WebView scrolled partly out of a ScrollView is cut
  off) and the controller's bounds are the whole view relative to it. A view
  that is not shown, in an invisible window or fully clipped hides the
  container. Detaching parks the container (waiting up to 1 s, before the
  window can go). Window coordinates are the Win32 client area's (one device
  pixel each, as hwui draws them). The app window gets `WS_CLIPCHILDREN`, so
  gesher::Window's `WM_PAINT` of hwui's frame does not cover WebView2.
  Moves of the top-level window reach WebView2
  (`NotifyParentWindowPositionChanged`) through a `SetWinEventHook`
  (`EVENT_OBJECT_LOCATIONCHANGE`, out of context) on the WebView thread, so
  `<select>` popups and the like open in the right place. No code of the
  window track changes: the container is a child window of another thread
  (Windows attaches the two threads' input).
- **Views over the WebView.** The container is a child window, so it covers
  whatever the app draws over the WebView in its own surface (Notes' floating
  edit button stayed hidden under a blank WebView2, e2e run 40135241966).
  Each placement also collects the views drawn above the WebView's branch:
  in every ancestor, the siblings after it (or with a higher Z), descended to
  views with a background or without children, visible and overlapping the
  visible rectangle. Their rectangles are cut out of the container's window
  region (`SetWindowRgn`), so the app's pixels show and the app's window gets
  the mouse there. Rectangles, not shapes: a round button's corners show the
  app's surface behind them, not the page. The log says
  `gesher-webview: N view(s) over the WebView cut out of its window` when the
  count changes.
- **Input and focus.** Mouse, touch, pen and keys over the WebView go to
  WebView2's own window; the app's window and its InputChannel never see
  them. WebView2 getting focus (a click) makes the WebView the focused view
  (`requestFocus`); the WebView getting Android focus moves keyboard focus
  into WebView2 (`MoveFocus(PROGRAMMATIC)`); losing it gives the keyboard
  back to the app's window. Tab past either end of the page moves Android's
  focus to the next or previous view.
- **Density.** The controller ignores the monitor's scale
  (`ShouldDetectMonitorScaleChanges` off) and rasterizes at Android's density
  (`RasterizationScale` = density / 160), so a CSS pixel is a dp, as in
  Android's WebView.

Controllers are created one at a time: a second
`CreateCoreWebView2Controller` while one is pending fails with
`ERROR_INVALID_STATE` (0x8007139f), so creations queue on the WebView thread.

## Threads

WebView2 objects belong to the WebView thread. Every `Host` call posts a task
there (a message-only window) and returns; the order of calls is kept. Events
come back on the WebView thread; `GesherWebViewProvider` hands them to the
WebView's Looper (`Handler`), so WebViewClient and WebChromeClient run on the
app's UI thread as on Android. Exceptions, each bounded so a stuck side never
hangs the other for long:

- `shouldOverrideUrlLoading`: WebView2's `NavigationStarting` must be answered
  synchronously, so the WebView thread waits for the UI thread's answer (up
  to 5 s, then the navigation goes ahead). It is asked only when the client
  overrides either `shouldOverrideUrlLoading` and only for navigations the
  page or the user started (not `loadUrl`, `reload`, back/forward).
- `@JavascriptInterface` methods run on the WebView thread, synchronously with
  the page's call (Android runs them on its JavaBridge thread, not the UI
  thread, too).
- Requests (`WebResourceRequested`) are deferred and answered from a worker
  pool (`shouldInterceptRequest` runs there, off the UI thread, as on
  Android).
- `CookieManager.getCookie` waits for WebView2 (up to 5 s); it returns null
  when called on the WebView thread itself.
- Detaching waits up to 1 s for the container to be parked.

## URLs

WebView2 cannot serve an APK's assets and does not let the host intercept
`file:` requests, so `file:///X` is loaded as
`https://gesher-file.example/X` (`FileUrls`, `kFileOrigin`), and every request
there is answered by Gesher: `file:///android_asset/...` from the APK's
assets, `file:///android_res/<type>/<name>` from its resources, other paths
from the disk when `WebSettings.getAllowFileAccess()` (Android's default for
current target SDKs is no). The app sees the `file:///` URLs everywhere
(`getUrl`, `onPageStarted/Finished`, `WebResourceRequest`, console sources).
Pages see the https origin: same-origin requests between assets work without
`setAllowFileAccessFromFileURLs`; `location.protocol` is `https:`.

- `loadUrl(url, headers)`: `Navigate`, or `NavigateWithWebResourceRequest`
  with the extra headers; `javascript:` URLs run as scripts.
- `loadData`: a `data:` URL (the data percent-decoded, then base64), as
  Android loads it.
- `loadDataWithBaseURL`: with an http(s) or file base, the base URL is
  navigated to and its main document answered with the data (so relative
  URLs and the origin are the base's); without one, `NavigateToString`.
- `postUrl` loads the URL with GET (no body).

## The API surface

| API | On WebView2 |
|---|---|
| `loadUrl`, `loadData`, `loadDataWithBaseURL`, `reload`, `stopLoading`, `goBack`, `goForward`, `canGoBack/Forward` (from `HistoryChanged`), `getUrl`, `getTitle`, `getProgress` | as above |
| `WebSettings` | JavaScript (`IsScriptEnabled`), user agent (`Settings2.UserAgent`; the default is Android WebView's format with WebView2's Chromium version), zoom (`supportZoom` and `builtInZoomControls`: zoom keys and pinch), text zoom (approximated by the controller's zoom factor), DOM storage (off: a document script removes `localStorage`/`sessionStorage`), `blockNetworkLoads`/`blockNetworkImage`/`loadsImagesAutomatically` (requests answered 403), `allowFileAccess`; the rest is kept and reported |
| `WebViewClient` | `shouldOverrideUrlLoading` (both forms), `onPageStarted` (`NavigationStarting`), `onPageFinished` (`NavigationCompleted`), `onReceivedError` (WebView2's web error status mapped to `ERROR_*`), `onReceivedHttpError` (HTTP status >= 400), `shouldInterceptRequest` (both forms, `WebResourceRequested` with a deferral), `doUpdateVisitedHistory`, `onRenderProcessGone` |
| `WebChromeClient` | `onProgressChanged` (10 at start, 30 on commit, 70 at DOMContentLoaded, 100), `onReceivedTitle`, `onConsoleMessage` (DevTools protocol `Runtime.consoleAPICalled` and `Runtime.exceptionThrown`), `onJsAlert/Confirm/Prompt/BeforeUnload` (`ScriptDialogOpening` with a deferral; WebView2's own dialogs are off; without a client answer the app's dialog is shown by the framework's `JsDialogHelper`, an AlertDialog in the WebView's Activity) |
| `evaluateJavascript` | `ExecuteScript`; the result is JSON, as Android's |
| `addJavascriptInterface` | see below |
| `CookieManager` | WebView2's cookie manager (`setCookie` parsed with `HttpCookie`, `getCookie`, `removeAllCookies`, `removeSessionCookies`, `hasCookies`); before any WebView exists a hidden service controller provides it |
| `WebStorage.deleteAllData`, `clearCache` | `Profile2.ClearBrowsingData` (DOM storage kinds; disk cache) |
| `DownloadListener` | `DownloadStarting`: WebView2's download is cancelled and the listener gets the URL, user agent, Content-Disposition, MIME type and length (Android's WebView never downloads itself) |
| `window.open` | handled (`NewWindowRequested`): the page opens in the same WebView, as Android's does without multiple-window support |
| Permissions | denied (WebChromeClient.onPermissionRequest's default) |
| File chooser | WebView2's own Windows file dialog; `onShowFileChooser` is not called |

### JavaScript interfaces

A host object `gesherBridge` (an `IDispatch` without type information,
`Bridge` in webview_host.cc) is added to every page. A document script
(`JsBridge.script`, `AddScriptToExecuteOnDocumentCreated`, replaced whenever
interfaces change, so they apply from the next page as on Android) defines
`window.<name>` with one function per callable method; each calls
`chrome.webview.hostObjects.sync.gesherBridge.callJava(name, method, argsJson)`
synchronously and gets `{"v": value}` or `{"e": message}` back (an error is
thrown). WebView2's sync proxy reads `callJava` as a property and then calls
what it got, so the property get answers an object whose default member makes
the call. (The method is not called `call`: the proxy is a function, whose
`.call` is `Function.prototype.call`.) Callable methods are public ones
annotated `@JavascriptInterface` (all public methods for target SDK 16 and
lower). Arguments convert from JSON to the method's parameter types
(String, the primitive types and their boxes); results back to JSON (other
objects become null).

## The loader

The WebView2 SDK is the NuGet package `Microsoft.Web.WebView2` 1.0.4258.31,
pinned by SHA-256 in `cmake/webview2_sdk.cmake` and downloaded at configure
time (`WebView2.h` and the loader libraries). `WebView2.h` builds with
llvm-mingw as it is, with two compat details: an `EventToken.h` that includes
mingw-w64's `eventtoken.h` (the cross build's file system is case-sensitive),
and its MIDL pragmas silenced; its IIDs are `selectany` definitions in the
header. COM callbacks are a small template (`com_util.h`, no WRL).

dalvikvm links the SDK's **WebView2LoaderStatic.lib** (MSVC-built COFF
objects that use Chromium's libc++ with MSVC's name mangling). The few pieces
of Microsoft's C++ runtime it needs and llvm-mingw lacks are in
`msvc_shims.cc`: `operator new/delete` variants and `std::nothrow` under
MSVC's decorated names, libc++'s `__libcpp_verbose_abort`, MSVC's thread-safe
static initialization (`_Init_thread_header/footer` and the native TLS
`_Init_thread_epoch`), Control Flow Guard's `__guard_dispatch_icall_fptr` and
weak `/GS` cookie functions. Everything else comes from UCRT and the import
libraries. `GESHER_WEBVIEW2_LOADER=dll` builds the other way, loading
`WebView2Loader.dll` from dalvikvm's directory at run time; the probe builds
both (`webview2_probe.exe`, static; `webview2_probe_dll.exe`).

## The runtime and the user data folder

The Evergreen WebView2 Runtime is part of Windows 11 and of current Windows
10; GitHub's windows-2022 and windows-2025 images have it (131.x and 155.x
when this was written). `tests/windows/webview/Runtime.ps1` prints its
version from EdgeUpdate's registration and installs it with Microsoft's
bootstrapper when it is missing. The installer offers a task that does the
same (`Check: not WebView2Installed`, `DownloadTemporaryFile` of the
bootstrapper, `/silent /install`, per user without elevation). Without a
runtime, WebViews stay empty and gesher-app logs why; apps do not crash.

The user data folder is the app's `app_webview` directory (as Android's
WebView), under the package's data directory, so each app has its own
cookies, cache and storage. Chromium's occlusion tracking is off
(`--disable-features=CalculateNativeWinOcclusion`), so a WebView keeps
drawing in a window that is covered or off the desktop;
`$GESHER_WEBVIEW_ARGS` adds browser arguments.

## Tests

- **The probe** (`probes/webview/webview2_probe.cc`, built by
  `.github/workflows/webview.yml` in minutes, without AOSP): the runtime's
  version through the loader; HTML whose script calls the bridge host object
  synchronously, read back with `ExecuteScript`; `CapturePreview` decoded with
  WIC; `PrintWindow(PW_RENDERFULLCONTENT)` of the window showing the WebView
  at its bounds over the window's own red; a page served from
  `https://gesher-file.example/` by a deferred `WebResourceRequested`.
- **webapp.apk** (`tests/apps/webapp`, built by webview.yml from the
  framework job's artifacts; `tests/windows/tracks/37-webview.ps1`), in a
  single process and on the Gesher device: see the script's header and
  "Status". A blue 40x40 view over the asset WebView (right edge, 220 px
  down) must read back blue: its place is cut out of the WebView2 window.
- **e2e**: org.billthefarmer.notes (tests/e2e/apps.json) edits a note through
  its floating button over the WebView and renders it.
- `GESHER_WEBVIEW_DEBUG=1` logs each placement and, after each page load, a
  summary of `CapturePreview`'s picture (size, center color).

## Status and next steps

### Done and CI-verified

- The probe on windows-2022 (runtime 131.0.2903.86) and windows-2025
  (155.0.4283.62), both loader variants (webview run 40128694232):
  `PASS  webview2: the loader finds the WebView2 Runtime ...`,
  `PASS  webview2: HTML with a script calling the bridge host object
  synchronously, ExecuteScript returns "probe|{\"v\":5}"`,
  `PASS  webview2: CapturePreview 400x300, green (0x00ff00)`,
  `PASS  webview2: PrintWindow(PW_RENDERFULLCONTENT): 0x00ff00 in the
  WebView's bounds, 0xff0000 above them`,
  `PASS  webview2: a page served by WebResourceRequested (deferred), blue`.
- webapp.apk in gesher-app on both runners, single process and the Gesher
  device (ci run 40135241966, tip 93a384e): `PASS  gesher-app: webapp.apk's
  WebViews on WebView2 (child windows of its Win32 window at the views'
  bounds, moved and hidden with them): an asset page whose script calls a
  @JavascriptInterface method, console, alert through WebChromeClient,
  shouldInterceptRequest and shouldOverrideUrlLoading, evaluateJavascript
  returns values, onPageFinished; a remote https page; cookies; the window
  read back with PW_RENDERFULLCONTENT (single process)` and the same
  `(webapp.apk's app process)`; package green.
- Views over a WebView cut out of its WebView2 window, on both runners in
  both modes (ci run 40138367211, b434e2d): `INFO  webapp.apk read back
  (400x800, PW_RENDERFULLCONTENT): header red 60000 of 60000, asset WebView
  green 116800 of 118400, green above it 0, blue view over it 1280 pixels`,
  `gesher-webview: 1 view(s) over the WebView cut out of its window, the
  first 360,220-400,260`, and the webapp PASS lines (now "..., a view over
  one cut out of it)").
- e2e (same run): `PASS  e2e: org.billthefarmer.notes 1.41 (Notes) installs,
  draws its first frame (not blank), reacts to input, closes and its process
  ends`: the edit button (344,744) over the WebView, keys, accept; the log has
  the cut-out and `first page loaded: file:///`. Its SHA-256 is pinned.

### In progress

- Nothing. Integrated with phase4/integration (exec merged) at 9c610d6: ci run
  40139785905 green (build, package, test on windows-2022 and windows-2025,
  e2e).

### Not done / limits

- `WebMessagePort`/`createWebMessageChannel` (throws), `postWebMessage` is a
  `message` event only; `findAll`; `saveWebArchive`; `capturePicture` (empty);
  `onShowFileChooser`, `onPermissionRequest`, geolocation prompts,
  `onCreateWindow`; HTTP auth and client certificates callbacks; Safe
  Browsing; `postUrl` bodies; text zoom is page zoom; `WebBackForwardList` is
  null; accessibility of web content through Android's tree.
- Cut-outs are rectangles from views' bounds: rounded or partly transparent
  views over a WebView show the app's surface in their corners; a view
  drawn over the WebView by a parent's `onDrawForeground`/`dispatchDraw`
  (not a child view) is not found.
