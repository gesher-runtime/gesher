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
  visible rectangle. Their shapes are cut out of the container's window
  region (`SetWindowRgn`), so the app's pixels show and the app's window gets
  the mouse there: each view's outline (`ViewOutlineProvider`: a round button
  is an oval, a card a round rect, a path a polygon from `Path.approximate`),
  else its visible rectangle. The WebView's own outline, and its ancestors',
  when they `clipToOutline` is kept as the region's base (rounded corners).
  Scale and rotation of those views are not applied. The log says
  `gesher-webview: N view(s) over the WebView cut out of its window: oval
  300,300-360,360; ...` when they change (ADR 0050).
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
- `capturePicture` and `findAll` wait up to 3 s; their results are taken on
  the WebView thread (not posted to the UI thread, which may be the waiting
  one).
- The Windows file dialog runs modally on the WebView thread (its modal loop
  keeps WebView2's events going).

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
- `postUrl` posts the data (form-encoded, as Android) with
  `NavigateWithWebResourceRequest`; other schemes load without it.

## The API surface

| API | On WebView2 |
|---|---|
| `loadUrl`, `loadData`, `loadDataWithBaseURL`, `reload`, `stopLoading`, `goBack`, `goForward`, `canGoBack/Forward` (from `HistoryChanged`), `getUrl`, `getTitle`, `getProgress` | as above |
| `WebSettings` | JavaScript (`IsScriptEnabled`), user agent (`Settings2.UserAgent`; the default is Android WebView's format with WebView2's Chromium version), zoom (`supportZoom` and `builtInZoomControls`: zoom keys and pinch), text zoom (text only: a document script multiplies every element's font size, `PageScripts.textZoom`), Safe Browsing (SmartScreen: `Settings8.IsReputationCheckingRequired`; no `onSafeBrowsingHit`), DOM storage (off: a document script removes `localStorage`/`sessionStorage`), `blockNetworkLoads`/`blockNetworkImage`/`loadsImagesAutomatically` (requests answered 403), `allowFileAccess`; the rest is kept and reported |
| `WebViewClient` | `shouldOverrideUrlLoading` (both forms), `onPageStarted` (`NavigationStarting`), `onPageFinished` (`NavigationCompleted`), `onReceivedError` (WebView2's web error status mapped to `ERROR_*`), `onReceivedHttpError` (HTTP status >= 400), `shouldInterceptRequest` (both forms, `WebResourceRequested` with a deferral), `doUpdateVisitedHistory`, `onRenderProcessGone`, `onReceivedHttpAuthRequest` (`BasicAuthenticationRequested`; the 401 completion and the retry's start are hidden: one page load), `onReceivedSslError` (`ServerCertificateErrorDetected`, `proceed` is `ALWAYS_ALLOW`), `onReceivedClientCertRequest` (`ClientCertificateRequested`; `proceed` selects the Windows store's certificate equal to `chain[0]`, `ignore` goes on without one) |
| `WebChromeClient` | `onProgressChanged` (10 at start, 30 on commit, 70 at DOMContentLoaded, 100), `onReceivedTitle`, `onConsoleMessage` (DevTools protocol `Runtime.consoleAPICalled` and `Runtime.exceptionThrown`), `onJsAlert/Confirm/Prompt/BeforeUnload` (`ScriptDialogOpening` with a deferral; WebView2's own dialogs are off; without a client answer the app's dialog is shown by the framework's `JsDialogHelper`, an AlertDialog in the WebView's Activity), `onShowFileChooser`, `onPermissionRequest`, `onGeolocationPermissionsShowPrompt`, `onCreateWindow`/`onCloseWindow` (see below) |
| `evaluateJavascript` | `ExecuteScript`; the result is JSON, as Android's |
| `addJavascriptInterface` | see below |
| `CookieManager` | WebView2's cookie manager (`setCookie` parsed with `HttpCookie`, `getCookie`, `removeAllCookies`, `removeSessionCookies`, `hasCookies`); before any WebView exists a hidden service controller provides it |
| `WebStorage.deleteAllData`, `clearCache` | `Profile2.ClearBrowsingData` (DOM storage kinds; disk cache) |
| `DownloadListener` | `DownloadStarting`: WebView2's download is cancelled and the listener gets the URL, user agent, Content-Disposition, MIME type and length (Android's WebView never downloads itself) |
| `window.open`, `window.close` | `NewWindowRequested` with a deferral: with `supportMultipleWindows` (and a user gesture or `javaScriptCanOpenWindowsAutomatically`) `onCreateWindow` gets a `WebViewTransport` and the WebView put there becomes the window (`put_NewWindow`: `window.opener` works); otherwise the page opens in the same WebView, as Android's. `WindowCloseRequested` is `onCloseWindow` |
| Geolocation | navigator.geolocation in main frames from LocationManager (`GeolocationShim`, ADR 0058): the WebChromeClient decides per origin, positions are Windows' location service's or the app's test providers |
| Permissions | `PermissionRequested` with a deferral: iframes' geolocation to `onGeolocationPermissionsShowPrompt` (retained answers in `GeolocationPermissions`), camera, microphone, MIDI SysEx to `onPermissionRequest` (its default denies), autoplay by `mediaPlaybackRequiresUserGesture`, the rest denied |
| File chooser | DevTools `Page.setInterceptFileChooserDialog`: `<input type=file>` calls `onShowFileChooser` (`FileChooserParams` from the input); the Uris go in with `DOM.setFileInputFiles` (content: copied to the cache). Not handled by the app: a Windows file dialog (`IFileOpenDialog`, owned by the app's window, filters from `accept`) |
| `createWebMessageChannel`, `postWebMessage` | `GesherWebMessagePort`: a port sent to the page is a `MessageChannel` of a document script (`PageScripts.PORTS`) relaying over `PostWebMessageAsJson` and `chrome.webview.postMessage`; strings only; ports end with the page |
| `copyBackForwardList`, `goBackOrForward(n)`, `clearHistory` | DevTools `Page.getNavigationHistory` (read after each history change), `Page.navigateToHistoryEntry`, `Page.resetNavigationHistory` |
| `findAllAsync`, `findNext`, `clearMatches`, `findAll` | `ICoreWebView2Find` when the runtime has it (its 1-based index is Android's 0-based ordinal), else a script (`PageScripts.find`, CSS Custom Highlights); `findAll` (synchronous) uses the script |
| `saveWebArchive` | DevTools `Page.captureSnapshot` (MHTML) |
| `capturePicture` | `CapturePreview` (waited for up to 3 s) drawn into a `Picture` of the view's size: what is shown, not the whole page |
| Accessibility | the WebView's AccessibilityNodeProvider: WebView2's UI Automation tree as virtual views (click, focus, set text, scroll); UI Automation clients reading the app's window find WebView2's own tree under the WebView's element (ADR 0058) |
| androidx.webkit (`WebViewCompat`) | Gesher's support library glue (`SupportLib`, ADR 0058): WebMessageListener, document-start scripts, ProxyController, multi-process query, the framework-backed features |

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

### webview2 track (ADR 0050)

- webapp.apk's `FeaturesActivity` (tests/windows/webview/Features.ps1, a
  local HTTP/HTTPS test server in the test's process), ci run 40198256072 on
  windows-2025 (runtime 155): `PASS  gesher-app: webapp.apk's
  FeaturesActivity on WebView2: WebMessagePorts both ways, find in page,
  capturePicture, geolocation and camera/microphone permissions (granted and
  denied), window.open into the WebView onCreateWindow gave and window.close,
  onShowFileChooser and the Windows file dialog (clicked with real mouse
  input (SendInput)), text zoom (text only), Basic authentication, postUrl
  with a body, a certificate error and a client certificate request, the
  back/forward list, an MHTML web archive, a round button cut out of the
  WebView2 window by its outline and clicks (real mouse input (SendInput))
  inside and outside its circle`, with `INFO  features read back: button
  center #0000ff, its corner #00ff00, inside its right edge #0000ff, beside
  it #00ff00`, `gesher-webview: 1 view(s) over the WebView cut out of its
  window: oval 300,300-360,360` and `INFO  features: UI Automation of the
  WebView2 window: found: ControlType.Pane 'Gesher features'`. Final run
  after merging phase4/integration 8bfb83f: ci run 40216380541 at 55ab46a,
  green on both runners (the same FeaturesActivity PASS line on windows-2025
  and windows-2022, the webapp PASS lines in both modes, e2e Notes PASS).
- Runtime 131 (windows-2022): a real click on the app's view in a cut-out
  waits in the app window's thread until something wakes it (the thread's
  input queue is attached to the WebView2 windows', and the wake goes
  elsewhere); the test posts WM_NULL after 20 s and says so (`INFO  features:
  no click on the button after 20 s ...`), then the click arrives. Runtime 155
  delivers it at once. Next step: the window thread could wake itself on
  input (outside this track's files: compat/hwui/window).
- Observed: on windows-2025 the OS denies geolocation (the page gets error 1
  after the app granted it); on windows-2022 it times out (error 3). Runtime
  131 keeps `window.opener` in the new window but the opener gets no
  `postMessage` from it and `window.close` raises no `WindowCloseRequested`
  (the test reports it as INFO there; runtime 155 does both). WebView2 counts
  `evaluateJavascript` as a user gesture (`onCreateWindow`'s
  `isUserGesture` is true for a script-opened window).
- The Find API exists on both runtimes' paths: `ICoreWebView2Find` where
  `ICoreWebView2_28` is there, `PageScripts.find` otherwise (`find: match 0
  of 4`, `match 1 of 4` on both).

### web3 track (ADR 0058)

- androidx.webkit: Gesher's support library glue (`SupportLib`, `org.chromium.support_lib_glue.SupportLibReflectionUtil`):
  WebMessageListener (main frames, strings), document-start scripts, ProxyController (applies
  to the process's WebView2 environment when set before it starts), multi-process query and
  the framework-backed features. Test: web3app.apk (androidx.webkit 1.18.0 from Maven) in
  54-web3.ps1.
- navigator.geolocation from LocationManager (`GeolocationShim`), the WebView's
  AccessibilityNodeProvider from WebView2's UI Automation tree (`WebViewAccessibility`), and
  WebView2's tree under the WebView's element for UI Automation clients (docs/services.md).
- Status and CI runs: see "Status and next steps (web3)" below.

### In progress

- Nothing: verified at 55ab46a (ci run 40216380541).

### Not done / limits

- Shapes ignore the views' scale and rotation; partly transparent views over
  a WebView show the app's surface; a view drawn over the WebView by a
  parent's `onDrawForeground`/`dispatchDraw` (not a child view) is not
  found.
- `WebMessagePort`s carry strings only (no `ArrayBuffer` payloads, no ports
  from the page to the app inside a port's message); `WebViewCompat`
  (androidx.webkit boundary interfaces) is not implemented.
- Client certificates: only certificates in Windows' store can be sent;
  `proceed()` with any other key goes on without one. Tested: the request
  callback and `ignore()`.
- `onSafeBrowsingHit` is never called (SmartScreen has no callback).
- Accessibility: web content is WebView2's UI Automation tree only, not in
  Android's `AccessibilityNodeProvider`.
- `capturePicture` is what is shown, not the whole page; text zoom does not
  reach cross-origin iframes or closed shadow roots; multi-step
  `goBackOrForward` may ask `shouldOverrideUrlLoading`.
- Typical apps open their chooser with `FileChooserParams.createIntent()`
  (ACTION_GET_CONTENT): Gesher has no document picker Activity for it yet;
  apps that return false get the Windows dialog.

### Status and next steps (web3)

- Continuation (2026-10-26): read ci 40285810159's windows-2025 log (job
  121453338756), ending with `3 test(s) failed`. Compat/proxy checks pass.
  Geolocation returns both Jerusalem and Tel Aviv, but GeoActivity stringifies
  the watch array before evaluateJavascript serializes it again; the test
  expects an array. Evaluate the array itself so its JSON has the expected
  shape, retaining both location assertions. The two accessibility checks
  remain real failures: the page provider has zero nodes, and the external
  UI Automation client times out. Do not mark accessibility verified.
- Removed the UIA provider cache lock across its Java callback (a Java
  accessibility event can reenter UIA). Added a snapshot timeout phase to
  stderr to locate any remaining UIA stall in the next run. This is a
  reentrancy fix; the CI result must establish whether it resolves the
  observed accessibility failures.

- Branch wip/web3 (worktree C:\work\gesher-webview).
- ci 40261113581: web3app's build.sh was not executable (fixed).
- ci 40262184075 (b44730c): everything built (glue, UIA bridge, proxy natives); NetActivity
  showed Windows' proxy as getDefaultProxy/Proxy.getDefaultHost, the sticky CONNECTIVITY_ACTION
  and a PendingIntent request working (cleartext was refused: the test app now permits it).
  Failures: the system's own UIA clients read every window, which turned accessibility on in
  every app, and UIA's threads exited while attached to the VM (ART aborts: webapp, ime,
  popups failed); Windows' WPAD default became a PAC proxy for every request. Fixed: the
  UIA bridge serves windows only while the screen reader flag is set (the test sets it, as
  Narrator), threads attach per call and detach; WPAD is not followed.
- ci 40285810159 (at 77d659a, the fixes above): in progress when the track was
  stopped (2026-10-26); its result is unknown. Nothing of this track is CI-verified yet; the
  last verified SHA of the branch is its base (9e449b0, phase4/integration).

**Done, not verified**: androidx.webkit glue (`SupportLib`, `org.chromium.support_lib_glue`),
WebMessageListener, document-start scripts, ProxyController (browser arguments; only before
the environment starts), `GeolocationShim`, `WebViewAccessibility` (AccessibilityNodeProvider
from WebView2's UIA tree, `compat/accessibility/uia_client.cc`), the `accessibility` service
and UIA bridge (`app/java/com/gesher/app/accessibility`, `compat/accessibility/uia_bridge.cc`),
Windows' proxy (`com.gesher.net.WindowsProxy`, `compat/net/windows_proxy.cc`),
CONNECTIVITY_ACTION and PendingIntent requests, 56-net's local HTTPS NetApp check, tests
(tests/windows/tracks/54-web3.ps1, tests/apps/web3app, tests/windows/web3/Web3Servers.ps1),
ADR 0058, docs (webview.md, network.md, services.md).
**Partly seen working in ci 40262184075**: NetActivity on the device: getDefaultProxy and
Proxy.getDefaultHost from Windows' proxy server and from a PAC URL, the sticky
CONNECTIVITY_ACTION, a PendingIntent request; the PAC script fetched by Windows and its
DIRECT URL not proxied (`PASS  web3: the PAC script was fetched by Windows, ...`).
**Not started**: the Wi-Fi/VPN/captive-portal/per-network-DNS leftovers of network.md;
ArrayBuffer web messages; restarting the WebView2 environment for a late ProxyController.

**Next steps**
1. Read ci 40285810159's test logs (`gh api repos/gesher-runtime/gesher/actions/jobs/<id>/logs`;
   artifact downloads are blocked by NetFree locally). Check that 37-webview, 33/35 ime,
   popups and glapp are green again (they failed in ci 40262184075 only because of the
   UIA/attach bug, now fixed).
2. In ci 40262184075 CompatActivity and GeoActivity hung with no page loaded (the main thread
   idle, no onPageFinished); the new run logs gesher-webview lines, onPageStarted/
   onReceivedError and the proxy's log to find out why (suspects: the old accessibility bug;
   Chromium refusing `--proxy-server` for the navigation to `http://web3-proxied.test`).
3. Fix, rerun, merge origin/phase4/integration before the final run, tick the roadmap boxes
   only for what CI verifies.

**Pitfalls**: a thread attached to ART must detach before it exits (UIA's thread pool);
never enable accessibility for every UIA client (the system has some on every window);
Windows' WPAD default must not become a PAC proxy; the runner's proxy settings are changed
only around the NetActivity runs and restored (`GesherWeb3Proxy.Save/Restore`), with a
forwarding proxy and a GitHub bypass list; `gh run download` is blocked by NetFree here; local
checks: JDK 21 + gfx-local framework classes for javac, llvm-mingw for -fsyntax-only.
