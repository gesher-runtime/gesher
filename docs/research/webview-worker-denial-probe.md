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

## Actual execution: 2026-11-25

The [successful run 45583346318](https://github.com/gesher-runtime/gesher/actions/runs/45583346318)
tested commit `6c104766a73d0778747111898744326f22e00ade`, tree
`c1e2b95ede34478ac428ee82bfadff07e85a351a`. Both loader variants compiled with
llvm-mingw/Clang 23.1.3; the worker diagnostic used the static loader and SDK
`1.0.4258.31`. The build also passed the 159 isolated script checks. Build job
`139098618964` produced artifact `15007701553`; its ZIP SHA-256 was
`53c914f8e98b5f155b993d5f1388c762e230854afdcde94818322e0130d562c0`.

| Windows job | Actual WebView2 runtime | Diagnostic result |
| --- | --- | --- |
| [2022 / 139098916455](https://github.com/gesher-runtime/gesher/actions/runs/45583346318/job/139098916455) | `131.0.2903.86` | Controls and observations passed |
| [2025 / 139098916473](https://github.com/gesher-runtime/gesher/actions/runs/45583346318/job/139098916473) | `157.0.4341.46` | Controls and observations passed |

Both runtimes produced the same outcomes:

| Case | Initial fetch / body | HTTP status / body bytes | Original-path server requests |
| --- | --- | --- | --- |
| Pass-through | Resolved / complete | 200 / 11 | 1 |
| Supplied response | Resolved / complete | 200 / 8 | 0 |
| HTTP-error control | Resolved / complete | 403 / 6 | 0 |
| First-read `STG_E_READFAULT` | Resolved / complete | 200 / 0 | 0 |
| First-read `E_ABORT` | Resolved / complete | 200 / 0 | 0 |
| Mid-body `STG_E_READFAULT` | Resolved / complete | 200 / 65536 | 0 |
| Deferred URI mutation | **Rejected with `TypeError` / no body** | No HTTP response / 0 | **0** |
| Synchronous handler `E_ABORT` | **Resolved / complete** | **200 / 19** | **1** |
| Same-worker follow-up | Resolved / complete | 200 / 15 | 1 |

For deferred URI mutation, `put_Uri` and `Complete` returned `S_OK`; the exact
unregistered non-network URI was read back after the handler had returned.
The worker reported `TypeError: Failed to fetch`, `timedOut=false`,
`aborted=false`, empty response URL, and no response body. Its original and final
JavaScript request URLs and the one resource-timing entry all retained the
original HTTP URL; the timing initiator was `fetch`. No external URI launch or
unknown origin request occurred. All nine observations used the same worker
identity within each job, including the successful final fetch. Controller
closure returned `S_OK`; guard removal after closure returned the expected
closed-object `0x8007139f` in both jobs.

The [machine-readable receipt](webview-worker-denial-2026-11-25.json) preserves
every case's exact JSON observation, independent counters, relevant native
operation lines, job links and decoded-log hashes. These results establish a
deferred denial candidate for the measured same-origin GET boundary. They also
disprove synchronous handler `E_ABORT` as a denial mechanism on these runtimes:
the original request was sent and its response consumed. None of the unmeasured
contracts listed above is closed by this result, and no production worker
capability was enabled.
