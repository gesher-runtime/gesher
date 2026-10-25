# 0050. WebView: the rest of android.webkit on WebView2

Status: accepted (webview2 track, Phase 5). Builds on [ADR 0041](0041-webview-webview2.md).
The coordinator may renumber.

## Context

ADR 0041 put `android.webkit.WebView` on Microsoft Edge WebView2: a
controller per WebView in a child window of the app's Win32 window, a
WebView thread, file URLs served by Gesher, JavaScript interfaces over a host
object. It left out message ports, most of `WebChromeClient` (file choosers,
permissions, new windows), authentication and certificate callbacks, POST
loads, the back/forward list, find in page, web archives, `capturePicture`,
real text zoom, accessibility, and cut-outs other than rectangles. WebView2
has an event for most of these, and the DevTools protocol
(`CallDevToolsProtocolMethod`) covers several that WebView2's own API lacks.

## Decision

- **Generic host primitives, logic in Java.** `Host` (compat/webview) gains
  `PostWebMessage`/`kWebMessage` (chrome.webview messages), `CallDevTools` and
  `ListenDevTools` (any protocol method and event), and one deferral table for
  the requests WebView2 lets the host answer later (permissions, new windows,
  Basic authentication, client certificates, server certificate errors), each
  completed by its own `Complete*` call. Destroying a host denies or cancels
  what is still open. `GesherWebViewProvider` maps them onto
  `WebViewClient`/`WebChromeClient`, so the native side stays small.
- **WebMessagePort over chrome.webview messages.** A port sent to the page
  becomes a `MessageChannel` made by a script in every document
  (`PageScripts.PORTS`): its `port1` is in the page's message event, its
  `port2` relays both ways (`PostWebMessageAsJson` in, `chrome.webview.postMessage`
  out) under a random id. Ports die with the page's next navigation. Shared
  buffers were not needed: Android's messages are strings.
- **File choosers through the DevTools protocol.** `Page.setInterceptFileChooserDialog`
  turns every `<input type=file>` opening into `Page.fileChooserOpened`;
  `onShowFileChooser` gets `FileChooserParams` from the input
  (`DOM.resolveNode`, `Runtime.callFunctionOn`), and the chosen files go in
  with `DOM.setFileInputFiles` (content: Uris copied to the cache first). An
  app that does not handle it gets a Windows file dialog (`IFileOpenDialog`,
  owned by its window) — Gesher's equivalent of a device's document picker.
- **Permissions**: `PermissionRequested` with a deferral; geolocation to
  `onGeolocationPermissionsShowPrompt` (retained answers in
  `GeolocationPermissions`), camera, microphone and MIDI SysEx to
  `onPermissionRequest`, autoplay by `mediaPlaybackRequiresUserGesture`, the
  rest denied (Android's WebView has none of them).
- **New windows**: `NewWindowRequested` with a deferral; with
  `supportMultipleWindows` the app's `onCreateWindow` gets a `WebViewTransport`
  and the WebView it puts there becomes the window (`put_NewWindow`, so
  `window.opener` works); `WindowCloseRequested` is `onCloseWindow`.
- **Authentication and certificates**: `BasicAuthenticationRequested` is
  `onReceivedHttpAuthRequest`; WebView2 ends the navigation with a 401 before
  the app answers and starts it again with the credentials, so the host holds
  the first completion back and hides the second start: the app sees one page
  load. `ServerCertificateErrorDetected` is `onReceivedSslError`
  (`ALWAYS_ALLOW` on proceed). `ClientCertificateRequested` is
  `onReceivedClientCertRequest`; WebView2 takes certificates from Windows'
  store only, so `proceed(key, chain)` selects the store's certificate equal
  to `chain[0]` and otherwise goes on without one.
- **History, archives, find, pictures**: `copyBackForwardList` is
  `Page.getNavigationHistory` read after each history change (multi-step
  `goBackOrForward` is `Page.navigateToHistoryEntry`); `saveWebArchive` is
  `Page.captureSnapshot` (MHTML); find uses `ICoreWebView2Find` when the
  runtime has it (1-based index mapped to Android's 0-based ordinal) and a
  script otherwise; `capturePicture` waits (3 s at most) for `CapturePreview`
  and draws it into a `Picture`; synchronous waits are answered on the
  WebView thread, never through the UI thread that waits.
- **Text zoom is text-only**: WebView2 has page zoom only; a document script
  multiplies every element's computed font size and re-applies after DOM
  changes, so the layout's other lengths stay (Android's text zoom).
- **Shaped cut-outs**: the window region is built from shapes —
  `ViewOutlineProvider` outlines of the views over the WebView (round rects,
  ovals, paths flattened to polygons) are cut out, and the WebView's own and
  its ancestors' outlines with `clipToOutline` are kept. Real mouse input
  (SendInput) at a point of the circle reaches the app's view; at the corner
  of its bounding box, the page.
- **Safe Browsing** maps to SmartScreen (`IsReputationCheckingRequired`);
  WebView2 has no callback for a blocked page, so `onSafeBrowsingHit` is never
  called. **Accessibility** of web content is WebView2's own UI Automation
  tree under its window (Narrator and UIA clients read it directly); Gesher
  does not mirror it into Android's `AccessibilityNodeProvider`.
- **androidx.webkit (WebViewCompat)** is not reachable: it loads
  `org.chromium.support_lib_glue` from the WebView provider's class loader and
  speaks Chromium's boundary interfaces; Gesher does not implement them, so
  `WebViewFeature.isFeatureSupported` is false and apps use the framework APIs
  above.

## Consequences

- Tests: `webapp.apk`'s `FeaturesActivity` with a small HTTP/HTTPS server in
  the test's process (tests/windows/webview), real clicks and UI automation of
  the file dialog (tests/windows/webview/Features.ps1).
- Limits: shapes ignore view scale and rotation; translucent views over a
  WebView still show the app's surface; ports carry strings, not
  `ArrayBuffer`s; `proceed()` with a key outside Windows' store sends no
  certificate; the text zoom script cannot scale text in cross-origin
  iframes or closed shadow roots.
