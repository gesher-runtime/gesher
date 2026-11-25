# Bounded worker denial experiments

This diagnostic adds two hypotheses to `webview2_probe.exe --worker-response`.
It changes no production worker API, setting, feature flag or activation gate.
The original pass-through, supplied HTTP 200, supplied HTTP 403 and three
faulting-stream cases remain. A final ordinary worker fetch must succeed in the
same worker instance after both new cases.

## New cases

- **`uri-deferred`:** verify a real `SERVICE_WORKER` GET and an unset response;
  take a deferral and return `S_OK` from `Invoke`. Only after the outer STA
  message dispatcher returns, change the retained request URI to a fresh
  `gesher-denial-<GUID>:blocked` URI, verify the exact readback, and complete the
  deferral once. This URI has no network scheme, authority or host. The scheme
  is checked absent in both OS registry views and is never registered with
  Windows or WebView2. No replacement response is created for this case.
- **`invoke-abort`:** verify the same request/source conditions and return
  `E_ABORT` directly from `Invoke`, without a response or deferral. This only
  tests a synchronous primitive. It cannot report a later Java callback or
  stream failure after the production bridge has returned `S_OK`.

An external-URI launch handler is installed before navigation and remains active
through controller closure. It cancels every launch attempt. Any such attempt
invalidates the diagnostic, even when cancellation succeeds; the guard is never
counted as the requested network-denial mechanism.

## Evidence and result meanings

The worker reports initial-fetch and body-read outcomes separately, including
the actual exception name, deadline/abort state, original and final JavaScript
`Request.url`, response URL, worker identity, and observed resource-timing name
and initiator type. Missing or rewritten timing entries are reported unchanged.
The independent loopback server counts original paths and all other requests.

Each new case records `network-rejection-observed` only when the initial fetch
rejects with `TypeError`, no deadline/abort was used, the original request and
timing identities were retained, no external launch was attempted, and the
origin received zero requests for that path. A fulfilled response, an origin
request, or changed/missing timing identity is `not-qualified`. Either outcome
can be a valid experiment; the diagnostic exit code reports whether its controls,
observations and cleanup were valid. Malformed messages, wrong sources, failed
mutation/readback, timeouts, launch attempts and missing follow-up controls fail
the harness and supply no qualification conclusion.

These are fresh-profile, same-origin, bodyless GET observations. Even an observed
rejection does not establish POST/upload behavior, cross-origin preflights,
redirects, cached responses, all profile/worker lifecycles, callback permissions,
streaming, Android cache modes, or compatibility on other runtime versions.
No result from this diagnostic automatically enables a capability. Server
counters establish whether the fixture origin received a request; they are not
system-wide packet or DNS telemetry.

## Contract basis and validation

Microsoft documents [mutable request URI](https://learn.microsoft.com/en-us/microsoft-edge/webview2/reference/win32/icorewebview2webresourcerequest?view=webview2-1.0.4258.31),
the [event-handler HRESULT signature](https://learn.microsoft.com/en-us/microsoft-edge/webview2/reference/win32/icorewebview2webresourcerequestedeventhandler?view=webview2-1.0.4258.31),
and [once-only deferral completion](https://learn.microsoft.com/en-us/microsoft-edge/webview2/reference/win32/icorewebview2deferral?view=webview2-1.0.4258.31).
None of those references promises these operations implement Android network
denial. The test follows the documented [STA/reentrancy rules](https://learn.microsoft.com/en-us/microsoft-edge/webview2/concepts/threading-model)
and [external-launch cancellation contract](https://learn.microsoft.com/en-us/microsoft-edge/webview2/reference/win32/icorewebview2launchingexternalurischemeeventargs?view=webview2-1.0.4258.31).

`node tests/native/worker_response_script_test.js` executes the exact embedded
worker and page scripts using isolated adapters. It checks initial versus body
errors, HTTP 403 resolution, deadline aborts, unchanged reporting of missing or
rewritten timing entries, and ordered dispatch including the final control.
These checks establish the observation protocol only. Actual Windows outcomes
must be attached to the tested commit and runtime versions before interpreting
either primitive.
