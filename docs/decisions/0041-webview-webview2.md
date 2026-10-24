# 0041. WebView: android.webkit.WebView on Microsoft Edge WebView2

Status: accepted (webview track, Phase 5). The coordinator may renumber.

## Context

Apps create `android.webkit.WebView`; on a device `WebViewFactory` loads the
WebView APK's Chromium (`WebViewChromiumFactoryProvider`) through the
`webviewupdate` service and a zygote of its own. Gesher has no WebView APK, and
porting Chromium's Android WebView (its GPU process, its compositor hooks into
hwui's functors, its own networking stack) is far out of reach. Windows 10 and
11 carry a Chromium runtime meant for embedding: Microsoft Edge WebView2, an
out-of-process browser driven by COM interfaces (`ICoreWebView2*`) from a
window of the host. The question is how a View in Gesher's hwui-drawn Win32
window becomes a WebView2, and how the loader and SDK fit the llvm-mingw
build.

## Decision

- **Gesher's own WebViewFactoryProvider.** `GesherWebView.install()` (from
  `Main.bootstrap`) sets `WebViewFactory.sProviderInstance`;
  `GesherWebViewProvider` implements `WebViewProvider` with its
  `ViewDelegate`/`ScrollDelegate`. Framework code stays unpatched.
- **A WebView2 controller per WebView, in a child window of the app's Win32
  window**, kept at the view's visible rectangle by ViewTreeObserver
  callbacks (pre-draw, scroll, layout), hidden with the view, parked in a
  hidden window when detached. The child window is the WebView's surface:
  hwui draws nothing there and the parent gets `WS_CLIPCHILDREN`. The HWND
  comes from the window session (`WindowSession.hwndOf(windowToken)`), so the
  window track's code gains one lookup and nothing else.
- **One WebView thread per process** (an STA with its own message loop) owns
  every WebView2 object; Java posts to it, events come back and are handed to
  the WebView's Looper. Synchronous answers WebView2 needs (navigation
  decisions) wait for the UI thread with a time limit; request interception
  and dialogs use WebView2's deferrals.
- **file:/// URLs as https://gesher-file.example/**, answered by Gesher from
  the APK's assets, resources or the disk (WebView2 cannot intercept file:
  requests); the app sees file:/// URLs.
- **JavaScript interfaces over a host object** (an `IDispatch` without type
  information) and a document script defining `window.<name>`; calls are
  synchronous, as Android's.
- **The SDK from NuGet, pinned by version and SHA-256, the static loader
  linked into dalvikvm** with a small shim of MSVC runtime pieces
  (`msvc_shims.cc`); the DLL loader remains a build option. No new file ships.
- **The Evergreen runtime is a prerequisite**, present on Windows 11, current
  Windows 10 and the CI images; the installer offers to install it when it is
  missing, and without it WebViews stay empty instead of crashing apps.

Rejected:
- **Porting Chromium's Android WebView**: its size, its GPU/compositor
  integration (hwui functors, SurfaceControl) and its network stack.
- **WebView2's composition hosting** (`ICoreWebView2CompositionController`,
  drawing into a DirectComposition visual hwui would have to compose): it
  needs input forwarding of every mouse/touch/key event and a DComp tree in
  the app's window; windowed hosting gets native input, IME and accessibility
  for free and composes over hwui's frame.
- **The DLL loader by default**: one more file to ship and to keep next to
  dalvikvm; the static library links with a few dozen lines of shims.
- **A synchronous-XHR bridge** for `@JavascriptInterface` (a request the host
  intercepts): blocked by pages' CSP and deprecated; the host object needs
  none of that.

## Consequences

- Web content gets Chromium as current as the installed Edge, its own
  networking (Windows' certificate store and proxy settings), its own GPU
  process and its own input and IME; the net track's Java TLS is not
  involved.
- Input over a WebView never reaches the app's InputChannel: apps that
  intercept touches above a WebView (e.g. `onInterceptTouchEvent` in a parent)
  do not see them.
- A WebView draws over every View below and above it in z-order within its
  visible rectangle (child windows are above their parent's content): Views
  drawn on top of a WebView are hidden by it.
- Pages see an https origin for local files; `location.protocol` differs from
  Android's.
- See docs/webview.md for the API mapping and what is not done.
