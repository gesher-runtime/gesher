# Inert service-worker preparation and WebStorage measurements

## Runtime update — 25 November 2026

[Actual run 45583346318](https://github.com/gesher-runtime/gesher/actions/runs/45583346318)
established a bounded deferred-denial primitive at source
`6c104766a73d0778747111898744326f22e00ade` on WebView2 131.0.2903.86 and
157.0.4341.46. Changing a retained worker request to a fresh unregistered
nonnetwork URI after `Invoke` returned produced an initial `fetch` `TypeError`,
zero complete-header original-origin requests, unchanged JavaScript request/resource-timing
identity, no timeout/abort or external launch, and a successful later request
from the same worker. Synchronous handler `E_ABORT` did not cancel; all three
response-stream failures also remained successful HTTP responses.

The expanded [exact report and receipt](webview-worker-denial-7a41155f.md) at
`7a41155fc31285b803a9bf4d422c89622f9ad4d0`,
[run 45593812298](https://github.com/gesher-runtime/gesher/actions/runs/45593812298),
adds independent accounting of partial HTTP requests and received bytes. Both
runtimes observed genuine initial rejection with original request/timing identity
and zero denied-target requests for a full same-origin POST, an observed final
redirect target and concurrent requests. The redirect's initial request was
intentionally allowed; Android redirect parity remains unqualified.

Two measured negative controls remain: denying only the CORS POST allowed one
OPTIONS request, and closing the owner before completing its deferral allowed
the original complete 32 KiB POST to reach the server. Those outcomes recurred
at `107d7fa57c3daf66511d475ceeb09ec907c969cb` in attempt 2 of
[run 45597266059](https://github.com/gesher-runtime/gesher/actions/runs/45597266059).

The separate alternatives now have actual Windows evidence at
`33fc2e50af97c95023e350ac4c8ba1e2d6d5e877` in
[run 45602648001](https://github.com/gesher-runtime/gesher/actions/runs/45602648001).
On both runtimes, denying the actual worker OPTIONS request produced initial
`TypeError`, no later POST callback and zero OPTIONS/POST requests at the
independently observed origin. A fresh-profile experiment completed the deferral
while its owner was live, then immediately removed both filters and closed the
owner on the same STA turn, without pumping or waiting for an acknowledgement.
It likewise produced initial `TypeError`, zero original POST requests and a
successful follow-up from the same worker. Both preserved the original request
and timing identities. The [exact report and receipt](webview-worker-denial-33fc2e50.md)
retain the full twelve-case matrix, separate three-case ordering experiment,
original negative controls, actual message sequence and socket observations.

These results supersede the older absence of runtime evidence below. They do
not supply cache modes, unrestricted streaming or a complete production worker
implementation; the public gates remain unavailable.

The separately qualified quota registry now backs a candidate production
partition-aware origin deletion operation with owned ordinary/InPrivate
controllers. Its [full-key contract record](webstorage-full-key-contract-2026-11-25.md)
states the implementation and actual-Host acceptance requirement. At `107d7fa5`
and `33fc2e50`, both Windows runtimes seeded and discovered all fourteen actual
keys, then the native process crashed during production deletion. The later
fault stacks locate an asynchronous `EmbeddedBrowserWebView.dll` dispatch;
the actual probe executable frame returns from `DispatchMessageW`. They do not
yet establish the responsible lifetime defect. Successful completion callbacks
before the crash do not establish deletion/preservation acceptance.
Full origin enumeration still needs effective quotas for arbitrary full storage
keys.

The continuation is limited to compatibility, stability and performance. Any
security concerns already raised by a separate review remain unresolved review
notes; this work neither implements security fixes nor asserts their clearance.
PR391 remains a draft, with no merge or automatic-merge request.

## Delivered behavior and remaining scope

This change replaces constant zero quota/usage answers with actual Chromium
storage measurements. The prepared service-worker request bridge is unavailable:
its retained HTTP-response failure path does not preserve Android's network
error, and the newly measured denial primitive is not a complete worker backend.
All worker feature flags are withheld, both provider controller getters throw
before initialization, and native activation rejects admission before scheduling
work or installing a filter. The retained dispatch code is inert preparation,
exercised only by isolated component fixtures. R2 W07 remains blocked and W08
remains incomplete.

| Operation | Implementation and limit |
| --- | --- |
| Framework and AndroidX worker client | `GesherServiceWorkers.get()` throws `UnsupportedOperationException` before configuring WebView or publishing a controller. Both provider entry points use this getter. No client is exposed as working. |
| Worker activation | `Host::EnableServiceWorkers` returns false without posting work, creating a controller, registering the wildcard `SERVICE_WORKER` filter or calling back. The private initializer used only by that filter was removed. |
| Prepared worker internals | The retained request/settings/response helpers have isolated component coverage. They do not establish a supported worker capability, faithful failures, complete streaming or working file/content routing. |
| Worker capability reporting | All `SERVICE_WORKER_*` flags are withheld. `SupportLib.Factory.getSupportedFeatures()` returns a copy of the supported-feature list and performs no worker initialization. |
| Storage usage/quota | `Storage.getUsageAndQuota` on a real controller, including the hidden controller when no public WebView exists. Native URLMON admission validates/canonicalizes HTTP(S) tuple origins. Java returns the requested numeric field once on the caller's Looper; malformed data, failed initialization, rejected admission and backend errors yield Android's zero query-failure value. |
| Storage deletion | Existing origin admission delegates to an owned asynchronous operation that discovers actual full keys, derives sites using the runtime, and calls `Storage.clearDataForStorageKey` for the selected original keys. The mask remains `file_systems,indexeddb`; delete-all is unchanged. Actual-Host acceptance is separate from the historical raw-CDP experiment. |
| Full origin enumeration | Still unsupported. `getOrigins` retains the previous empty result, which must not be interpreted as an audited empty profile. No visited-origin or cookie list is substituted for Chromium's storage-key registry. |

### Why both provider and native activation are unavailable

Installing a WebView2 filter proves that an interception event can be received;
it does not prove that Android's response, permission and error contracts can be
preserved. Previously, querying any AndroidX capability initialized that filter
and exposed ordinary worker requests to the HTTP-error substitution described
below. Capability discovery now has no such side effect.

Removing provider feature strings alone is insufficient. AndroidX's
`ServiceWorkerControllerImpl` prefers the framework controller when framework
support exists, and `ApiFeature.isSupported()` combines framework and provider
support with OR. On Gesher's framework level, an application can therefore reach
the framework getter regardless of the provider list. Both controller getters
now fail explicitly through the common Java gate, and the native entry also
rejects activation. This does not make every AndroidX framework-derived feature
query return false or establish API 37 worker compatibility. There is no runtime
opt-in that exposes the incomplete bridge.

`WithCoreWebView`, cookie operations, actual storage measurements/deletion and
the reserved JavaScript-bridge request guard retain their independent behavior.
Creating their hidden controller does not enable worker interception.

### Why cache mode support is withheld

Android's cache-only and cache-else-network modes require Chromium cache load
flags. WebView2's stable request API exposes HTTP request/response data, not those
flags. Chromium's `GetLoadFlagsForExtraHeaders` recognizes bypass/revalidation
headers but does not turn `only-if-cached` into `LOAD_ONLY_FROM_CACHE` or supply
`LOAD_SKIP_CACHE_VALIDATION`. Adding those headers cannot establish the Android
contract. CDP's target-level cache disable switch cannot implement the full set
of modes either. The retained settings helper rejects unsupported setters;
neither it nor `SERVICE_WORKER_CACHE_MODE` is exposed as available.

### The prepared failure path still needs replacement

WebView2's stable `WebResourceRequested` args do not expose request cancellation.
The retained native dispatch helper installs an HTTP 403 before calling Java
and retains it on queue rejection, callback/stream failure, timeout or missing
INTERNET permission. That prevents an original request from escaping the helper,
but its observable HTTP response differs from Android's network-error completion.
This mismatch affects the interception contract itself, not just the separate
block-network setting. The entire activation path and all worker flags, including
basic usage and request interception, therefore remain unavailable.

A faithful engine-level cancellation path, including redirect/error behavior,
is a remaining W07 requirement. Returning HTTP 403 is not counted as that work's
completion. Raw `content:` interception also requires scheme registration and
Windows runtime evidence; the retained Java resolver helper does not establish
it. The prepared synthetic-response bridge rejects bodies larger than 64 MiB
instead of truncating them; unrestricted streaming needs a separate design and
runtime validation before worker support can be claimed.

#### Response streaming and cancellation boundary

The reviewed SDK `1.0.4258.31` documents an additional precondition in
`ICoreWebView2WebResourceResponse::get_Content`: all response bytes must already
be available when the request deferral completes. The same rule appears in
[Microsoft's response-content reference](https://learn.microsoft.com/en-us/microsoft-edge/webview2/reference/win32/icorewebview2webresourceresponse?view=webview2-1.0.4258.31#get_content).
A ring buffer that receives future bytes after `Complete` does not satisfy that
documented contract. Making the COM stream agile or reading on a background
thread does not remove the precondition.

The current inert transport also has explicit implementation limits:

- [Java dispatch](../../app/java/com/gesher/app/webview/GesherServiceWorkers.java)
  drains the application's stream on a shared two-thread request pool, rejects
  more than 64 MiB and submits one final `byte[]` after EOF.
- [JNI](../../compat/webview/webview_jni.cc) copies that final array into a native
  vector. There is no begin/chunk/credit/EOF/error/cancel protocol.
- [Native request ownership](../../compat/webview/webview_host.cc) ends when the
  first response consumes its pending request ID and completes the deferral.
  Timeout and cancellation remove native pending requests without notifying
  the Java producer; there is no continuing body-producer registry or lease.
- [OwnedStream](../../compat/webview/owned_stream.h) exposes immutable, fully
  materialized bytes with accurate size and independent clone cursors. Its
  thread-safe ownership does not establish future-byte production or backpressure.

A complete owned file spool could bound memory and remove the finite 64 MiB
limit, but it would still wait for EOF. It would not establish progressive
delivery, nor make a slow or nonterminating application stream complete. The
actual first-read/mid-body fault results above also show that merely returning
an `IStream::Read` error did not produce the required worker failure on the two
tested runtimes. The pre-response URI-denial experiment supplies no evidence
about a failure after response delivery has begun.

The Java try-with-resources ordering has a separate cancellation limit. If a
read throws, `close()` executes before the outer catch submits failure; a blocked
close can therefore delay failure indefinitely. After a successful read, the
native response is submitted before close, but a blocked close still occupies
one of the two request threads. A future transport could wake native consumers,
reject late chunks and schedule one close outside native locks and the STA.
It cannot assume that arbitrary application `read()` or `close()` methods will
terminate; attempted native cancellation and observed Java close completion
must be recorded separately.

Before a supported progressive route could be exposed, real ART/Java/JNI/native/
WebView2 acceptance would need the following independent observations:

| Case | Required observation |
| --- | --- |
| Progress before EOF | The producer waits for a real consumer acknowledgement of a prefix before emitting the tail; the worker must consume that prefix before EOF. |
| Large and unknown-length bodies | Exact bytes beyond 64 MiB, preserved metadata and measured Java/native buffer high-water marks independent of total length. |
| Read failure | Actual initial-fetch/body-read errors before the first byte and after a consumed prefix, compared with Android; no successful truncated body. |
| Blocked application close | Native cancellation and unrelated requests complete while close is blocked; release the fixture latch and verify exactly-once eventual close. |
| Concurrency and late activity | Same-URL bodies remain independent; cancellation, duplicate EOF, late chunks and stale request generations cannot revive another body. |
| Owner and browser shutdown | Actual consumer abort, blocked-read wakeup and ownership cleanup without joining arbitrary application code from the STA. |

No such new transport or engine capability is claimed by the current source.

#### Independent check of a CDP alternative

An independent review inspected both the pinned SDK 1.0.4258.31 and stable
1.0.4370.26. Neither request-event interface has cancellation or a CDP request ID.
The newer service-worker management interface adds lifecycle/messaging, not a
before-start request interception guarantee. Changing an HRESULT, inventing a
status code, or failing a response stream was not an established equivalent of
an initial network failure. The later deferred URI experiment above supplies
actual evidence for its stated GET boundary. The expanded POST, CORS, redirect,
concurrency and teardown results above further delimit that primitive; they do
not establish a complete worker contract.

CDP `Fetch.failRequest` is a real network-failure primitive, including
`AccessDenied`, on a request owned by `Fetch.requestPaused`. WebView2's
`ICoreWebView2_11::CallDevToolsProtocolMethodForSession` can address an attached
worker session. It cannot safely retrofit cancellation onto an unrelated
`WebResourceRequested` deferral: URL/method/order matching is ambiguous for
concurrent identical requests and redirects.

The Chromium 146.0.7647.2 source distinguishes worker and browser session routes:

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
- [BrowserDevToolsAgentHost](https://chromium.googlesource.com/chromium/src/+/refs/tags/146.0.7647.2/content/browser/devtools/browser_devtools_agent_host.cc)
  installs Fetch for trusted, non-discovery browser sessions.
  [Loader-factory instrumentation](https://chromium.googlesource.com/chromium/src/+/refs/tags/146.0.7647.2/content/browser/devtools/devtools_instrumentation.cc)
  applies it to worker requests and main scripts without a worker debugger
  attachment. Its patterns have no profile/worker predicate, and the browser
  handler's enable callback does not refresh existing loader factories.
- `Target.attachToTarget` can attach an existing browser ID from a trusted root,
  but `Target.getTargets` uses
  [GetOrCreateAll](https://chromium.googlesource.com/chromium/src/+/refs/tags/146.0.7647.2/content/browser/devtools/devtools_agent_host_impl.cc),
  which does not enumerate browser hosts even with an explicit browser filter.
  Discovery notifications might expose an existing browser host; documented
  WebView2 COM does not supply a browser-session creation guarantee.

Per-worker discovery followed by attachment leaves first-request races. A browser
session avoids that attachment requirement but still needs supported acquisition,
request/profile isolation and coverage of existing factories. A faithful CDP
replacement needs independent profile-scoped ownership before worker
execution, a table keyed by session and Fetch request ID, and terminal
continue/fulfill/fail operations. Its Windows acceptance probe must cover cold
and persisted workers, background restarts, identical concurrent URLs, redirects,
callback/stream failures, INTERNET denial, closure of the last public WebView,
teardown acknowledgements and unchanged page requests. That architecture and
runtime evidence are not provided by the current COM request-filter design.

The existing [Windows probe](../../probes/webview/webview2_probe.cc) now includes a
[session-access diagnostic](../../probes/webview/browser_session_probe.h) with a
fresh GUID profile, actual user-data-folder verification and a 15-second total
budget including cleanup. It records SDK/runtime/loader, HRESULTs and JSON,
enumerates/discovers browser targets, attaches only structurally observed browser
IDs, and calls `Target.getBrowserContexts` through the flattened COM session.
Explicit unsupported/access denial or no observed browser is an unavailable
finding; malformed results, timeouts, setup and cleanup failures fail the probe.
It adds no Fetch interception, worker activation, external navigation, debugger
port or production API. Existing Windows CI runs it in the probe executable;
cross-compilation alone supplies no session-access evidence. A successful session
result would establish neither Fetch coverage nor W07 completion; all worker
capability gates remain closed.

Actual execution on head `e909052a`, CI run
[45230894563](https://github.com/gesher-runtime/gesher/actions/runs/45230894563),
passed on Windows 2022 with WebView2 `131.0.2903.86` and Windows 2025 with
`157.0.4341.46`, using SDK `1.0.4258.31` and the static loader. Setup, subscription,
discovery and cleanup succeeded, but `Target.getTargets` returned
`{"targetInfos":[]}` and discovery observed no browser target. Consequently the
diagnostic did not attempt attachment or session browser-scope proof. Its
precise finding is unavailable through the tested routes, with no Fetch or
worker capability established. This does not exclude every possible route.

### WebStorage scope

Current Chromium Android WebStorage uses the quota manager's complete storage-key
registry for `getOrigins`, and its usage/quota callbacks use the quota manager's
actual measurements. The CDP storage API supplies per-origin measurements but
not that registry. First-party HTTP(S) queries and deletion do not establish
opaque-origin or partitioned third-party behavior. Complete enumeration and
partition-aware origin deletion remain W08 requirements; neither is marked done.

Direct access to the application's profile database was also investigated. The
repository has a Win32 SQLite backend, and the SDK exposes the actual profile
path. However, inspected Chromium 143/146 quota databases default to exclusive
locking and retain browser-owned transactions. Bucket deletion can remain
uncommitted until a scheduled 30-second commit, while insertion of a new bucket
commits immediately. Android's bridge enumerates the complete storage keys and
obtains their measurements through the live quota manager after its bootstrap
ordering. An external SQLite connection or committed snapshot therefore does
not establish the same live result; database rows alone also do not provide
current per-key usage. This is a source-based finding, not an assertion that
the tested proprietary Edge runtimes use an identical schema or locking policy.
No live-profile database workaround was introduced.

Sources: the pinned Chromium 146
[quota database](https://chromium.googlesource.com/chromium/src/+/refs/tags/146.0.7647.2/storage/browser/quota/quota_database.cc),
[database defaults](https://chromium.googlesource.com/chromium/src/+/refs/tags/146.0.7647.2/sql/database.h),
[Android quota bridge](https://chromium.googlesource.com/chromium/src/+/refs/tags/146.0.7647.2/android_webview/browser/aw_quota_manager_bridge.cc),
and [SQLite transaction isolation](https://www.sqlite.org/isolation.html).

## Prepared admission, callbacks and cleanup

These internal paths have component coverage but cannot be activated by the
provider. They use two daemon callback workers, a queue of 64 and at most 128
native outstanding deferrals. Native deadlines expire after 30 seconds using
the existing STA timer. A timeout retains denial, completes the deferral and
releases its global-work lease. Duplicate and late completions are ignored.
Callback execution never blocks the STA. Admitted responses can complete while
shutdown is draining; new requests cannot enter during closure. Fatal
service-controller closure scrubs outstanding original request destinations and
upload bodies before completing them. These resource controls do not repair the
failure-contract mismatch.

There is no successful worker initialization path. The Java startup/wait code
was removed, and the native entry fails before global-work admission. Existing
native STA initialization for other WebView operations is unchanged.

## Validation

These checks exercise the unavailable provider boundaries, retained Java/native
components and live storage-query methods with explicit framework/COM/task
recording seams. Constructing an isolated worker fixture does not activate a
worker in ordinary application use. The checks are not Windows browser
integration evidence.

The final unavailable-boundary checks pass: 206 feature-query assertions and
310 fresh public-controller/storage assertions in Java, plus 101 native
activation/storage assertions with zero filters installed. Both Java mutations
(a feature-query lookup and native activation before throwing) are rejected.
Restoring the exact published `fed039fc` native activation/helper bodies is also
rejected, with three filter installations recorded. Separate prepared-component
fixtures retain 63 Java and 314 native checks; the missing-denial control remains
effective. The Java suite also passes with actual Android JSON classes, and the
existing storage-forwarding suite retains all 16 checks. Native runs use ASan
and UBSan; LeakSanitizer is disabled only for this traced local environment.
Independent review approved the final tests at local `a44350ef` (published as
`d5e4ae9f`); no production switch was introduced to activate the prepared bridge.

- `tools/test_service_worker_contracts.py`: provider availability boundaries,
  prepared settings/adapters/converters, response ownership and query callbacks;
  supports `--core-classes` to run against actual Android `org.json` classes.
- `tools/test_service_worker_native_contracts.py`: actual native request admission,
  response completion, expiration and controller-failure cleanup. Includes an
  executable negative control removing the initial denial; the fixture must fail.
- `tools/test_webstorage_origin_contracts.py`: retains all existing deletion
  assertions. `tools/test_webstorage_native_contracts.py` additionally extracts
  the actual quota query and checks the method, origin, result, failure and
  once-only callback semantics using the real Windows URLMON origin parser.
- Full `webview_host.cc` syntax checks pass for Windows x64 and ARM64 with the
  repository's pinned WebView2 SDK 1.0.4258.31, global CMake definitions including
  `NOGDI`, and `-Wall -Wextra -Werror` plus the target's existing
  `-Wno-unused-parameter`. The now-unused worker-only initializer was removed.
- Full `webview_jni.cc` builtin/shared syntax checks pass for Windows x64 using
  a temporary AOSP JNI header with both repository `libnativehelper` JNI patches
  applied. ARM64 rejects that existing patch's unconditional `sysv_abi` attribute
  under `-Werror`; the JNI source is unchanged from the published pre-gate base.
  Earlier ARM64 JNI checks used the original AOSP header and did not establish
  Gesher's patched JNI ABI. No warning suppression or ABI patch change is part
  of the worker gate. The canonical ARM64 toolchain/target supplies no ABI
  override for this attribute; `tools/ci/build-arm64-windows.sh` currently builds
  native probes and the ELF/bionic/POSIX subset, not WebView/JNI targets.
- Existing SupportLib boundary and owned-stream contract suites remain applicable.

The existing CI entry points also run the new suites: WebStorage Java forwarding
invokes the worker/AndroidX/query contracts; owned-stream testing invokes the
prepared native worker admission/completion checks with the same compiler and
sanitizer settings and the missing-denial negative control. Historical-source
and measurement-only runs remain isolated. No workflow changes are required.

The complete unchanged `app/build.sh` recipe compiled the integrated app against
actual framework/core classes, generated AIDL delegates, passed BootLinkCheck
(292 references, zero unresolved) and completed D8. This is stronger compile
evidence than the earlier selected-WebView compilation with unrelated seams.
The locally available class artifacts are not a fresh framework build. The AOSP
Java sources needed to regenerate the full SOURCE-retention compatibility table
are absent; the generated local app package is for validation, not distribution.

No worker performance improvement or functional availability is claimed. The
old constant-zero storage paths did not perform equivalent work to real quota
queries, so an ABBA comparison would also be misleading. The component suites
verify bounded concurrency and resource ownership. A future worker backend
needs Windows/ART execution with a real WebView2 runtime for source-filter
routing, browser response consumption during shutdown, exact redirect/error
behavior, thread affinity and offline worker execution. The public per-origin
storage usage/quota and first-party deletion-isolation case subsequently passed
on both Windows runtimes in that exact run: two real 256 KiB IndexedDB payloads,
positive actual measurements, A converging to zero usage and its database
absent after deletion, B's entire payload retained, and ten once-only callbacks
on the invoking UI thread. See the [runtime evidence](../handoff/round2_validation.md#public-webstorage-runtime-acceptance).
These results do not cover the complete storage-key registry or partition-aware
deletion.

## Primary contract references

- [Android ServiceWorkerClient](https://developer.android.com/reference/android/webkit/ServiceWorkerClient):
  background-thread callback and null/response behavior.
- [Chromium service-worker settings](https://chromium.googlesource.com/chromium/src/+/refs/tags/146.0.7647.2/android_webview/java/src/org/chromium/android_webview/AwServiceWorkerSettings.java):
  settings defaults and INTERNET permission behavior.
- [AndroidX converter boundary](https://chromium.googlesource.com/chromium/src/+/HEAD/android_webview/support_library/boundary_interfaces/src/org/chromium/support_lib_boundary/WebkitToCompatConverterBoundaryInterface.java):
  framework-to-boundary settings conversion in both directions.
- [AndroidX controller](https://android.googlesource.com/platform/frameworks/support/+/androidx-main/webkit/webkit/src/main/java/androidx/webkit/internal/ServiceWorkerControllerImpl.java)
  and [feature support](https://android.googlesource.com/platform/frameworks/support/+/androidx-main/webkit/webkit/src/main/java/androidx/webkit/internal/ApiFeature.java):
  framework-first routing and framework/provider capability combination.
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
