# Service-worker interception and WebStorage measurements

## Delivered behavior and remaining scope

This change replaces the service-worker client/settings no-op with a native
WebView2 request path and replaces constant zero quota/usage answers with actual
Chromium storage measurements. It does **not** close all of R2 W07/W08.

| Operation | Implementation and limit |
| --- | --- |
| Framework and AndroidX worker client | One framework singleton and AndroidX boundary adapters. Native service-worker requests carry their actual URL, method and headers to the application on a background thread. A supplied response carries its status, reason, headers and complete body; a null response resumes the original request. |
| Worker lifetime | One hidden controller owns the `SERVICE_WORKER` filter for the app profile. Public WebView destruction does not remove it. Initialization verifies `ICoreWebView2_22`, filter installation and the existing request guard/timer before reporting support. |
| Worker settings | Default cache mode is `LOAD_DEFAULT`. Unsupported cache modes throw; no successful cache-policy change is invented. File/content settings feed the actual Java loaders when those URLs reach interception. `INTERNET` permission is checked; enabling network without it throws. |
| Worker capability reporting | Only `SERVICE_WORKER_BASIC_USAGE` and `SERVICE_WORKER_SHOULD_INTERCEPT_REQUEST` are added, after native initialization succeeds. Cache, block-network, file and content flags are withheld. |
| Storage usage/quota | `Storage.getUsageAndQuota` on a real controller, including the hidden controller when no public WebView exists. Native URLMON admission validates/canonicalizes HTTP(S) tuple origins. Java returns the requested numeric field once on the caller's Looper; malformed data, failed initialization, rejected admission and backend errors yield Android's zero query-failure value. |
| Storage deletion | Existing origin admission and `Storage.clearDataForStorageKey` deletion are preserved; origin deletion still targets `file_systems,indexeddb`. Existing delete-all behavior is unchanged. |
| Full origin enumeration | Still unsupported. `getOrigins` retains the previous empty result, which must not be interpreted as an audited empty profile. No visited-origin or cookie list is substituted for Chromium's storage-key registry. |

### Why cache mode support is withheld

Android's cache-only and cache-else-network modes require Chromium cache load
flags. WebView2's stable request API exposes HTTP request/response data, not those
flags. Chromium's `GetLoadFlagsForExtraHeaders` recognizes bypass/revalidation
headers but does not turn `only-if-cached` into `LOAD_ONLY_FROM_CACHE` or supply
`LOAD_SKIP_CACHE_VALIDATION`. Adding those headers cannot establish the Android
contract. CDP's target-level cache disable switch cannot implement the full set
of modes either. The implementation therefore rejects unsupported setters and
withholds `SERVICE_WORKER_CACHE_MODE`.

### Network failure is still an explicit limitation

WebView2's stable `WebResourceRequested` args do not expose request cancellation.
The native boundary installs an HTTP 403 before calling Java and retains it on
queue rejection, callback/stream failure, timeout or missing INTERNET permission.
This prevents a failed callback from releasing the original network request,
but its observable HTTP response differs from Android's network-error completion.
Consequently `SERVICE_WORKER_BLOCK_NETWORK_LOADS` is withheld and an explicit
attempt to enable blocking in an INTERNET-enabled process throws
`UnsupportedOperationException` without changing the setting. Disabling it
without INTERNET still throws `SecurityException`.

A faithful engine-level cancellation path, including redirect/error behavior,
is a remaining W07 requirement. Returning HTTP 403 is not counted as that work's
completion. Raw `content:` interception also requires scheme registration and
Windows runtime evidence; the Java resolver path alone does not establish it.
File/content feature flags remain withheld. The bounded synthetic-response
bridge rejects bodies larger than 64 MiB instead of truncating them; unrestricted
streaming needs a separate design and runtime validation.

#### Independent check of a CDP alternative

An independent review inspected both the pinned SDK 1.0.4258.31 and stable
1.0.4370.26. Neither request-event interface has cancellation or a CDP request ID.
The newer service-worker management interface adds lifecycle/messaging, not a
before-start request interception guarantee. Changing an HRESULT, inventing a
status code, failing a response stream or rewriting the request URL is not an
established equivalent of an initial network failure.

CDP `Fetch.failRequest` is a real network-failure primitive, including
`AccessDenied`, on a request owned by `Fetch.requestPaused`. WebView2's
`ICoreWebView2_11::CallDevToolsProtocolMethodForSession` can address an attached
worker session. It cannot safely retrofit cancellation onto an unrelated
`WebResourceRequested` deferral: URL/method/order matching is ambiguous for
concurrent identical requests and redirects.

The Chromium 146.0.7647.2 source reveals the remaining attachment problem:

- [FrameAutoAttacher](https://chromium.googlesource.com/chromium/src/+/refs/tags/146.0.7647.2/content/browser/devtools/frame_auto_attacher.cc)
  matches workers to the page's frame origins. The hidden blank controller does
  not auto-attach every worker in the profile.
- [TargetHandler](https://chromium.googlesource.com/chromium/src/+/refs/tags/146.0.7647.2/content/browser/devtools/protocol/target_handler.cc)
  restricts `autoAttachRelated` and `attachToBrowserTarget` to browser targets.
  The documented WebView2 default session is a top-page session; a broader
  WebView2-specific capability would need actual runtime proof.
- [ServiceWorkerVersion](https://chromium.googlesource.com/chromium/src/+/refs/tags/146.0.7647.2/content/browser/service_worker/service_worker_version.cc)
  changes normal worker request timeouts while a debugger is attached.
- [DevToolsURLLoaderInterceptor](https://chromium.googlesource.com/chromium/src/+/refs/tags/146.0.7647.2/content/browser/devtools/devtools_url_loader_interceptor.cc)
  resumes an outstanding intercepted request on detachment. Detach is not
  denial; pending failures must be acknowledged before removing interception.

Discovery followed by attachment leaves first-request races. A faithful CDP
replacement therefore needs independent profile-scoped ownership before worker
execution, a table keyed by session and Fetch request ID, and terminal
continue/fulfill/fail operations. Its Windows acceptance probe must cover cold
and persisted workers, background restarts, identical concurrent URLs, redirects,
callback/stream failures, INTERNET denial, closure of the last public WebView,
teardown acknowledgements and unchanged page requests. That architecture and
runtime evidence are not provided by the current COM request-filter design.

### WebStorage scope

Current Chromium Android WebStorage uses the quota manager's complete storage-key
registry for `getOrigins`, and its usage/quota callbacks use the quota manager's
actual measurements. The CDP storage API supplies per-origin measurements but
not that registry. First-party HTTP(S) queries and deletion do not establish
opaque-origin or partitioned third-party behavior. Complete enumeration and
partition-aware origin deletion remain W08 requirements; neither is marked done.

## Admission, callbacks and cleanup

Requests use two daemon callback workers, a queue of 64 and at most 128 native
outstanding deferrals. Native deadlines expire after 30 seconds using the existing
STA timer. A timeout retains denial, completes the deferral and releases its
global-work lease. Duplicate and late completions are ignored. Callback execution
never blocks the STA. Admitted responses can complete while shutdown is draining;
new requests cannot enter during closure. Fatal service-controller closure scrubs
outstanding original request destinations/upload bodies before completing them.

Java initialization waits at most five seconds after native submission. Native
STA startup also has a five-second bound; calls from the STA itself cannot wait
for worker initialization. Startup failure does not advertise worker support.
The controller remains scoped to the process/profile, independent of public views.

## Validation

These checks exercise production Java methods and native admission/completion
functions with explicit framework/COM/task recording seams. They are not Windows
browser integration evidence.

- `tools/test_service_worker_contracts.py`: actual controller/settings, AndroidX
  adapters/converters, response ownership and query parsing/callback logic;
  supports `--core-classes` to run against actual Android `org.json` classes.
- `tools/test_service_worker_native_contracts.py`: actual native request admission,
  response completion, expiration and controller-failure cleanup. Includes an
  executable negative control removing the initial denial; the fixture must fail.
- `tools/test_webstorage_origin_contracts.py`: retains all existing deletion
  assertions. `tools/test_webstorage_native_contracts.py` additionally extracts
  the actual quota query and checks the method, origin, result, failure and
  once-only callback semantics using the real Windows URLMON origin parser.
- Full `webview_host.cc` and `webview_jni.cc` compile checks use the repository's
  pinned WebView2 SDK 1.0.4258.31 and AOSP JNI header for Windows x64 and ARM64.
- Existing SupportLib boundary and owned-stream contract suites remain applicable.

The existing CI entry points also run the new suites: WebStorage Java forwarding
invokes the worker/AndroidX/query contracts; owned-stream testing invokes native
worker admission/completion checks with the same compiler/sanitizer settings and
the missing-denial negative control. Historical-source and measurement-only runs
remain isolated. No workflow changes are required.

The complete unchanged `app/build.sh` recipe compiled the integrated app against
actual framework/core classes, generated AIDL delegates, passed BootLinkCheck
(292 references, zero unresolved) and completed D8. This is stronger compile
evidence than the earlier selected-WebView compilation with unrelated seams.
The locally available class artifacts are not a fresh framework build. The AOSP
Java sources needed to regenerate the full SOURCE-retention compatibility table
are absent; the generated local app package is for validation, not distribution.

No performance improvement is claimed: the old no-op and constant-zero paths did
not perform equivalent work, so an ABBA timing comparison would be misleading.
The new suites verify bounded concurrency and resource ownership. Windows/ART
execution with a real WebView2 runtime is still required for source-filter
routing, browser response consumption during shutdown, exact redirect behavior,
thread affinity, offline worker execution and actual storage usage/deletion.

## Primary contract references

- [Android ServiceWorkerClient](https://developer.android.com/reference/android/webkit/ServiceWorkerClient):
  background-thread callback and null/response behavior.
- [Chromium service-worker settings](https://chromium.googlesource.com/chromium/src/+/refs/tags/146.0.7647.2/android_webview/java/src/org/chromium/android_webview/AwServiceWorkerSettings.java):
  settings defaults and INTERNET permission behavior.
- [AndroidX converter boundary](https://chromium.googlesource.com/chromium/src/+/HEAD/android_webview/support_library/boundary_interfaces/src/org/chromium/support_lib_boundary/WebkitToCompatConverterBoundaryInterface.java):
  framework-to-boundary settings conversion in both directions.
- [WebView2 source filters](https://learn.microsoft.com/en-us/microsoft-edge/webview2/reference/win32/icorewebview2_22):
  one service-worker filter owner per profile and runtime interface.
- [WebView2 request handling](https://learn.microsoft.com/en-us/microsoft-edge/webview2/how-to/webresourcerequested):
  request modification, synthetic responses and deferrals.
- [Chromium cache header mapping](https://chromium.googlesource.com/chromium/src/+/HEAD/net/http/http_cache_util.cc):
  supported header-to-load-flag transformations.
- [Chromium Android quota bridge](https://chromium.googlesource.com/chromium/src/+/refs/heads/main/android_webview/browser/aw_quota_manager_bridge.cc):
  quota-manager enumeration/measurement and deletion scopes.
- [CDP Storage](https://chromedevtools.github.io/devtools-protocol/tot/Storage/#method-getUsageAndQuota):
  actual origin usage/quota query.
