# 0058. androidx.webkit, accessibility over UI Automation, geolocation, Windows' proxy

Status: accepted (web3 track, Phase 5). Builds on [ADR 0041](0041-webview-webview2.md),
[0050](0050-webview2-completeness.md) and [0037](0037-net-networking.md). The coordinator
may renumber.

## Context

ADR 0050 left androidx.webkit unreachable (no support library glue), web content out of
Android's accessibility tree, geolocation to WebView2 (which asks Windows' location
service: denied or timed out on the runners), and ADR 0037 left Windows' proxy settings,
CONNECTIVITY_ACTION and PendingIntent network requests out. Gesher had no
AccessibilityManager service at all: AccessibilityManager had no service, so nothing was
ever enabled and nothing read an app's views.

## Decision

- **androidx.webkit's glue is Gesher's.** androidx.webkit loads
  `org.chromium.support_lib_glue.SupportLibReflectionUtil` from the WebView provider's
  class loader (gesher-app's) and speaks Chromium's boundary interfaces, every object an
  `InvocationHandler`. `com.gesher.app.webview.SupportLib` answers a boundary method with
  its delegate's public method of the same name and parameter types (the boundary passes
  only framework and java types besides InvocationHandlers), and calls the app's handlers
  with a Method of the app's copy of the interface. No Chromium code or boundary-interface
  copies are needed. Advertised features: the framework-backed ones, WEB_MESSAGE_LISTENER,
  DOCUMENT_START_SCRIPT, PROXY_OVERRIDE, MULTI_PROCESS(_QUERY), GET_VARIATIONS_HEADER.
  - **WebMessageListener**: the JavaScript object is part of the document script (main
    frames whose origin a rule allows), posting over chrome.webview; replies come as
    message events. One JavaScriptReplyProxy per page origin (getOrCreatePeer).
  - **Document-start scripts** are blocks of the same document script, guarded by the
    origin rules, in the order added; `ScriptHandler.remove` drops one from later
    documents.
  - **ProxyController**: WebView2 takes a proxy only when its browser process starts
    (`--proxy-server`, `--proxy-bypass-list`; no runtime API), so an override applies when
    set before the process's WebView2 environment exists (the first WebView or
    CookieManager use); later ones apply to the next environment, as the log says.
    Reverse bypass is not supported (Chromium's switch has none).
- **navigator.geolocation from LocationManager.** A document script replaces
  getCurrentPosition, watchPosition and clearWatch in main frames; requests come over
  chrome.webview, the app's WebChromeClient decides per origin
  (onGeolocationPermissionsShowPrompt, retained answers), the app must request a location
  permission, and positions are LocationManager's (Windows' service or test providers).
  Android's WebView decides the same way; WebView2's own path stays for iframes.
- **Accessibility is real, per process.** `com.gesher.app.accessibility.AccessibilityService`
  is IAccessibilityManager, served in every app process (ServiceManager's cache, as the
  camera service): on Windows the consumer of an app's accessibility is UI Automation,
  which reads each process's windows in that process. Its clients (the process's
  AccessibilityManager) are enabled while Windows' screen reader flag is set
  (SPI_GETSCREENREADER, Narrator sets it) or with `$GESHER_ACCESSIBILITY=1`, and the UIA
  bridge serves the windows only then: the system's own UIA clients read windows all the
  time, and enabling accessibility for any of them changed every app (ci 40262184075).
  It takes ViewRootImpl's
  interaction connections and the AccessibilityEvents, which become UIA events (focus,
  announcements as UIA notifications, invoke, value and structure changes).
- **The reverse bridge** (compat/accessibility/uia_bridge.cc, libgesher_a11y.so):
  gesher::Window windows are subclassed (a WinEvent hook finds each new one; no window-code
  change) and answer WM_GETOBJECT(UiaRootObjectId) with a fragment root whose elements are
  the window's AccessibilityNodeInfos, read with the ViewRootImpl's direct accessibility
  connection (`getDirectAccessibilityConnectionId`: the AccessibilityInteractionController,
  as an accessibility service reads it). Class names map to UIA control types; clickable
  nodes have Invoke, checkable ones Toggle, editable ones Value; actions are
  `performAccessibilityAction`. A WebView's element hosts its WebView2 window
  (`get_HostRawElementProvider` = that window's host provider), so UIA shows WebView2's
  own tree there rather than a copy.
- **Web content in Android's tree**: a WebView's AccessibilityNodeProvider shows the
  elements of WebView2's UIA tree (read as a UIA client on a thread of its own,
  compat/accessibility/uia_client.cc) as virtual views; clicks, focus, text and scrolling
  act on them (Invoke/Toggle/SelectionItem/LegacyIAccessible, SetFocus, Value, ScrollItem).
- **Windows' proxy**: the user's Internet Options (WinHTTP's view: a proxy server and
  bypass list, a PAC URL; automatic detection, on by default and mostly finding nothing, is not followed) are `ConnectivityManager.getDefaultProxy()`;
  every process applies them with `Proxy.setHttpProxyConfiguration` (as ActivityThread
  does) and follows Windows' changes (RegNotifyChangeKeyValue). A PAC script makes it a PAC
  proxy: PacProxySelector's `com.android.net.IProxyService` is served in every
  process by WinHttpGetProxyForUrl, so Windows evaluates the script.
- **CONNECTIVITY_ACTION** is a sticky broadcast of the connectivity service, sent when
  the default network changes; **PendingIntent requests and listens** are kept by the
  app's ConnectivityManager and sent (EXTRA_NETWORK, EXTRA_NETWORK_REQUEST) when a matching
  network is available.

## Consequences

- androidx.webkit apps work with the features above; others report unsupported, as with
  an old WebView APK. WebMessageListener and document-start scripts reach main frames only
  (chrome.webview); messages are strings (no WEB_MESSAGE_ARRAY_BUFFER).
- Narrator and UIA tools read and operate Android views; nodes' scale/rotation are not
  applied to bounds, and the tree is read on demand (no cache invalidation beyond 200 ms).
- PendingIntent requests live in the app process (they end with it), unlike Android's.
- Tests: tests/windows/tracks/54-web3.ps1 with web3app.apk (androidx.webkit 1.18.0 from
  Maven, pinned by SHA-256), the test's proxy and PAC servers, and a UI Automation client
  as Narrator; 56-net's NetApp HTTPS check uses a local server with a test root in
  Windows' ROOT store instead of example.com.
