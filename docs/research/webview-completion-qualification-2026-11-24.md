# WebView completion: runtime qualification and overlap audit

This continuation belongs to [PR391](https://github.com/gesher-runtime/gesher/pull/391),
starting at `a36320ccd9e0bd8b983c9912ea95dc77d83536c9`. It investigates the
remaining worker and full-profile WebStorage contracts. It does not enable a
production capability merely because a diagnostic runs successfully.

The current [issue56](https://github.com/gesher-runtime/gesher/issues/56) labels
**W07 as WebStorage** and **W08 as AndroidX/renderer lifecycle**. Earlier handoff
shorthand for worker/storage work must not be read as completion of the entire
renderer roadmap. Render-process control/client APIs remain a separate gap.

## Update — 25 November 2026

The actual Windows experiment has completed: the full persisted/live/selective-
deletion/payload storage sequence passed on WebView2 131 and 157 at diagnostic
head `908d99eb480b6d40480235e13c678fb791e7e636`. The
[runtime report](webview-runtime-results-2026-11-25.md) and its linked JSON retain
the exact source, build artifact, job links and observations. The worker stream
faults remained successful HTTP responses with empty or truncated bodies; their
valid diagnostic result is not a qualified network-error implementation.

PR381 and PR399 have since merged into main. The final PR399 head removed its
earlier worker changes and reused the graphics/usage implementation, adding an
independent suspend regression. PR388 is externally closed. The continuation
incorporates main `a0291539acd9fef036a305c23a0cd33dd297048a`, keeps its landed
production and complete source-qualification policy, and imports the corrected
quota probe without the temporary diagnostic workflow trigger.

The [new open-PR inventory](webview-open-prs-2026-11-25.json) records all 86 open
pull refs. Each actual fetched head was compared against its listed base, with
no fetch/diff failures. PR493 moved between listing and fetch; both identities
are recorded and the fetched head was inspected. Only PR414 intersects the
nine remaining production paths: its independent `Host::Configure` change
handles Windows profile paths. No current open PR supplies the missing complete
WebView storage/worker backend. The older table below is a dated record, not an
assessment of PR399's final merged code.

Full `getOrigins`, arbitrary full-key effective quotas, partition-aware
production deletion and complete worker semantics still require production
implementation and acceptance. The successful registry experiment removes an
uncertainty about actual discovery/deletion; it does not enable unsupported
public features.

## Historical open-PR audit — 24 November 2026

The public open-PR collection returned 100 entries and an empty second page on
2026-11-24. All 100 pull refs were fetched; their actual heads matched the listed
SHAs. Three-dot file diffs against each listed base were inspected for WebView
overlap, with no fetch/diff errors. The compact accompanying
[audit inventory](webview-open-prs-2026-11-24.json) records every examined head and
base, relevant paths, and content digests of the complete original inventories.

Besides the existing continuation PR, these PRs change relevant paths:

| PR and inspected head | Actual overlap | Decision |
| --- | --- | --- |
| [399](https://github.com/gesher-runtime/gesher/pull/399), `5bffcb1e0c57056afdeb706066bb541ccf6b0c01` | Java service-worker/settings helpers and feature strings; storage origins changes an empty collection to a new empty map. No native worker event wiring. | Do not duplicate it or treat its Java helpers as a completed backend. Fabricated HTTP errors, collapsed cache status and permissive response headers do not establish Android behavior. |
| [447](https://github.com/gesher-runtime/gesher/pull/447), `bbe54232ded0330fb35c467248917c4141c6c9c4` | Provider `readAll` allocation improvement, still buffering the whole response. | Leave this allocation work with its PR; it does not establish progressive streaming or network-error semantics. |
| [414](https://github.com/gesher-runtime/gesher/pull/414), `1a9543a5a9cc8b2f68779a3679693e2350ae4e6a` | Host user-data path conversion and longer Windows path buffer. | Leave path work with its PR; it does not supply a worker or storage backend. |

The source audit also found no relevant production WebView change between the
continuation's merge base and then-current main
`e074b67b2f83ce9128c11713171329e9fbbf6739`. No other PR was modified, closed or
merged as part of this inspection.

## Real worker-response diagnostic

`webview2_probe.exe --worker-response` creates a fresh owned profile and serves
an actual service worker from an ephemeral loopback HTTP origin. Native
interception is restricted by `ICoreWebView2_22` to the `SERVICE_WORKER` source
kind, which is verified again for every event. The independent server counts
requests, so a swallowed or escaped interception cannot be hidden by a page
callback.

The controls are a normal document request, worker pass-through, a supplied 200
response, and a supplied 403 response. The latter must resolve `fetch`, making
it an explicit control against reporting an HTTP error as a network failure.
The three stream faults are `STG_E_READFAULT` before the first byte, `E_ABORT`
before the first byte, and `STG_E_READFAULT` after 64 KiB.

The worker records initial-fetch rejection separately from failure while
consuming an already-resolved response. Its own deadline-triggered abort
invalidates the observation. Native counters record reads, supplied bytes,
faults and the corresponding HRESULT. All backing data is allocated before
response completion: **this experiment does not demonstrate progressive
streaming**. The documented WebView2 response contract requires content to be
available before completing the event deferral.

- Exit0 means valid controls, independent network counters and recorded
  outcomes. A body-read failure can be a valid diagnostic result while still
  being insufficient to implement Android request cancellation.
- Exit2 is accepted only with the exact marker for an absent source-filter
  interface. It is reported as unavailable, without support claims.
- Exit1, timeout, missing evidence or contradictory markers fail the gate.

The probe closes its controller and callback subscriptions. It reports retained
stream counts but does not establish browser-process exit or complete stream
destruction. Those lifecycle properties are not part of its success claim.

## Live storage-registry diagnostic

`webview2_probe.exe --quota-registry` tests the built-in
`edge://quota-internals/` route in a private controller. Its availability inside
WebView2 is a runtime question; Chromium source alone does not establish it.
The page's own browser proxy supplies bucket/storage-key inventory after the
global-usage bootstrap barrier. The diagnostic reads actual data, rather than
reconstructing an origin registry from visited URLs or copying a live database.

The experiment writes and verifies real 256-KiB IndexedDB payloads for three
distinct HTTPS schemeful sites. It records actual serialized storage keys and
checks both the environment user-data directory and actual profile path.

1. Seed A first-party and C under A, close the browser, and wait for the retained
   browser-process handle to signal exit.
2. Reopen the same profile and require both positive-usage keys **before any
   fixture origin is revisited**.
3. Add B first-party, C under B and A under B. Require five distinct measured
   storage keys in the live inventory.
4. Delete the observed A-first-party and C-under-A keys. Require those entries
   absent or zero and the other three positive before reopening a database.
5. Check deleted databases are absent and all three preserved payloads are
   intact.

This partition test follows Android's trusted-origin deletion semantics:
first-party keys match the origin, while partitioned keys match their top-level
schemeful site. Deleting A therefore preserves A under B as well as C under B.
A generic implementation must preserve that distinction.

- Exit0 requires the complete persisted/live/deletion/payload evidence.
- Exit3 identifies an unavailable route, schema or required partition behavior;
  the PowerShell gate prints this limitation explicitly.
- Exit1, timeouts, malformed results and inconsistent data fail the gate.

The fixture's 64-entry inventory cap is a diagnostic bound, not an acceptable
production limit. The registry offers per-bucket usage, not per-storage-key
quota. A full `getOrigins` implementation still needs faithful key-to-origin
mapping, quota values, profile/lifecycle ownership and callback/error handling.
CDP `Storage.getUsageAndQuota` constructs a first-party key, so summing all
partitioned usages and attaching that first-party quota would not establish
Android parity.

## Validation and interpretation

Both diagnostics have separate bounded processes in Windows track37, preserving
the existing visual/bridge test and its deadlines. The unsharded runner loads
the original `RunTimed` definition before these diagnostics when needed; it does not run
another track or copy a second timeout implementation.

Local qualification checks include strict x64/ARM64 static/DLL compilation of
the worker implementation, the existing probe project's combined x64 builds,
27 exact quota inventory/parser checks under ASan/UBSan, embedded JavaScript
syntax checks and nine exact worker-script observations through explicitly
identified host fetch adapters. Deliberate mutations must reject omitted
origins, incomplete deletion and conflation of body failure with initial-fetch
rejection. None of these local checks is a substitute for actual Windows
WebView2 execution. Runtime results and exact CI identities belong in PR391's
continuation record once observed.

## Alternatives inspected if native qualification is insufficient

CEF156.0.14 at commit `4fa4ab5` supplies genuine resource cancellation and async
response bodies, including worker requests without a page frame. Stock public
APIs cannot express all required Android cache flags while preserving unrelated
credentials/load flags. CEF also drops the underlying worker factory kind, but
Android146 itself sends null-frame worker factories through its worker client;
that metadata difference alone is not a proven Android mismatch. Its precise
contract needs reconciliation before requiring an attribution extension. A
complete CEF route would need a shared page/worker/profile engine, a cache-policy
API extension, and the existing sandbox bootstrap, plus the full Gesher WebView host surface.
A worker-only engine beside WebView2 would split browser state and is not a
faithful replacement.

A profile extension is not proven to expose `chrome.debugger` in WebView2.
Debugging a service worker also changes ordinary timeout/idle-termination
behavior. Empty browser-target enumeration from the previous probe establishes
only that browser-target route's result; it does not prove that a live worker
target can never be enumerated or attached through COM. That narrower route
needs its own capability and lifecycle evidence.

Running Android's actual WebView provider APK through Gesher is another
legitimate route. The preserved AOSP factory path is currently bypassed by
bootstrap registration of the WebView2 provider. A real provider requires
consistent installed package/library/signature metadata, package dependency
invalidation, platform drawing glue, distinct external renderer-service
instances and actual native-binary qualification. Shared RELRO optimization
failure alone is not necessarily fatal to ordinary provider loading.
Existing ownership must be reused: [PR107](https://github.com/gesher-runtime/gesher/pull/107)
for native classloader namespaces, [PR477](https://github.com/gesher-runtime/gesher/pull/477)
for directed guest signals, and [PR478](https://github.com/gesher-runtime/gesher/pull/478)
for ARM64 native fork. The latter explicitly does not establish Java-after-fork
or WebView zygote behavior.

## Primary implementation references

- [WebView2 response content and IStream contract](https://learn.microsoft.com/en-us/microsoft-edge/webview2/reference/win32/icorewebview2webresourceresponse)
- [ISequentialStream::Read error/partial-read contract](https://learn.microsoft.com/en-us/windows/win32/api/objidl/nf-objidl-isequentialstream-read)
- [Chromium146 Android quota bridge](https://chromium.googlesource.com/chromium/src/+/refs/tags/146.0.7647.2/android_webview/browser/aw_quota_manager_bridge.cc)
- [Chromium146 storage-key origin matcher](https://chromium.googlesource.com/chromium/src/+/refs/tags/146.0.7647.2/third_party/blink/common/storage_key/storage_key.cc)
- [Chromium146 quota internals interface](https://chromium.googlesource.com/chromium/src/+/refs/tags/146.0.7647.2/storage/browser/quota/quota_internals.mojom)
- [Chromium156 Android cache request flags](https://chromium.googlesource.com/chromium/src/+/refs/tags/156.0.8078.48/android_webview/browser/network_service/net_helpers.cc)
- [Chromium146 Android worker request client selection](https://chromium.googlesource.com/chromium/src/+/refs/tags/146.0.7647.2/android_webview/browser/network_service/aw_proxying_url_loader_factory.cc)
- [Pinned CEF request flag translation](https://github.com/chromiumembedded/cef/blob/4fa4ab5/libcef/common/request_impl.cc)
- [Pinned CEF worker factory transport](https://github.com/chromiumembedded/cef/blob/4fa4ab5/libcef/browser/chrome/chrome_content_browser_client_cef.cc)
- [CEF sandbox setup](https://chromiumembedded.github.io/cef/sandbox_setup)
- [Chromium146 service-worker debug lifecycle](https://chromium.googlesource.com/chromium/src/+/refs/tags/146.0.7647.2/content/browser/service_worker/service_worker_version.cc)
- [Android17 WebView provider factory](https://android.googlesource.com/platform/frameworks/base/+/refs/tags/android-17.0.0_r1/core/java/android/webkit/WebViewFactory.java)
- [Android17 WebView platform drawing bridge](https://android.googlesource.com/platform/frameworks/base/+/refs/tags/android-17.0.0_r1/native/webview/plat_support/draw_functor.cpp)
