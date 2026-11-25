# Actual worker results at 33fc2e50 — 25 November 2026

Both tested WebView2 runtimes completed the 12-case worker matrix and the separate
3-case controller-ordering scenario. The new preflight decision and
Complete-before-Close cases produced an initial `TypeError` and no request to
their original target. The two earlier negative controls remain negative:
deciding only on the CORS POST lets its OPTIONS request reach the origin, and
closing the owner before completing its deferral lets the complete original
upload reach the origin. These are standalone diagnostic results; production
worker support remains disabled. The separate production storage test crashed,
so this workflow is **failed** and storage deletion is **not qualified**.

The [paired JSON receipt](webview-worker-denial-33fc2e50.json) preserves every
actual case message, the other scenario messages, native event order,
independent origin records, exact log anchors and artifact identities. Evidence
comes from [run 45602648001, attempt 1](https://github.com/gesher-runtime/gesher/actions/runs/45602648001)
and its two completed Windows jobs, rather than the earlier script adapters.

## Executed source and runtime identity

| Item | Observed identity |
| --- | --- |
| Source commit | [`33fc2e50af97c95023e350ac4c8ba1e2d6d5e877`](https://github.com/gesher-runtime/gesher/commit/33fc2e50af97c95023e350ac4c8ba1e2d6d5e877) |
| Source tree | `3c5ac7bb2bc86631601340a8680c917a19b65dc0` |
| Actor / attempt | `palmoni5` / `1` |
| Run created / updated | `2026-11-25T11:14:54Z` / `2026-11-25T11:17:22Z` |
| Windows 2022 | [Job 139166035087](https://github.com/gesher-runtime/gesher/actions/runs/45602648001/job/139166035087), runtime `131.0.2903.86` |
| Windows 2025 | [Job 139166035174](https://github.com/gesher-runtime/gesher/actions/runs/45602648001/job/139166035174), runtime `157.0.4341.46` |
| Runtime diagnostic SDK / loader | `1.0.4258.31` / `static (WebView2LoaderStatic.lib)` |
| Build job | [139165250170](https://github.com/gesher-runtime/gesher/actions/runs/45602648001/job/139165250170) |
| Artifact | [`webview`, ID 15016452291](https://github.com/gesher-runtime/gesher/actions/runs/45602648001/artifacts/15016452291), ZIP `2,856,600` bytes |

The build log records the source at line 112, the uploaded ZIP digest at line
321 and the finalized artifact ID and size at line 324. Both Windows jobs
downloaded that same digest. Runtime evidence below is from the static-loader
probe; compiling both loader variants does not constitute a second runtime
execution of these scenarios.

## Outcomes on both runtimes

The following seven denial outcomes were the same on runtime 131 and runtime
157. Request counts come from independent loopback origin records, checked
against each actual worker message. A scenario's final PASS means its controls
and observations are valid; it does not convert a `not-qualified` outcome into
success.

| Case | Initial fetch outcome | Original target requests | Original OPTIONS requests | Recorded outcome |
| --- | --- | ---: | ---: | --- |
| `post-deny` | Rejected with `TypeError` | 0 | 0 | `network-rejection-observed` |
| `cors-deny` — original POST-only decision | Rejected with `TypeError` | 0 | **1** | **`not-qualified`** |
| `redirect-deny` | Rejected with `TypeError` | 0 at the final target | 0 | `network-rejection-observed` for the final target |
| `parallel-deny` | Rejected with `TypeError` | 0 denied requests | 0 | `network-rejection-observed` |
| `cors-preflight-deny` — new OPTIONS decision | Rejected with `TypeError` | 0 | 0 | `network-rejection-observed` |
| `teardown-deny` — original Close-first order | **Resolved HTTP 200**, complete 20-byte response | **1 complete 32,768-byte POST** | 0 | **`not-qualified`** |
| `teardown-complete-deny` — new Complete-first order | Rejected with `TypeError` | 0 | 0 | `network-rejection-observed` |

The initial redirect GET intentionally reached its origin once before the final
target was denied. The parallel case retained the permitted requests on its
shared route; the denied request count is checked together with those positive
controls, not inferred from an empty shared route.

The main scenario emitted actual case messages in this order:

`post-pass`, `post-deny`, `cors-pass`, `cors-deny`, `redirect-pass`,
`redirect-deny`, `parallel-allow`, `parallel-deny`, `sequential-pass`,
`cors-preflight-deny`, `teardown-deny`, `teardown-followup`.

The separate scenario emitted `post-pass`, `teardown-complete-deny`, and
`teardown-complete-followup`. Across the two runtimes this gives **30 actual
case messages**, plus readiness, start and completion messages retained in the
JSON. Each scenario has a fresh profile and one stable worker identity. The
recorded requests retain their original URL and method, and reported resource
timing names retain the original request identity. No case reports a timeout
or abort. Both workers report successful unregistration on both runtimes.

## What the new preflight case establishes

The native event records identify an actual `SERVICE_WORKER` OPTIONS request
whose requested method is POST and whose Origin matches the observed worker
origin. The diagnostic already has its denial decision when it receives this
OPTIONS event. It returns from the event handler, applies the terminal request
change through the held deferral, and records one intercepted OPTIONS and no
subsequent POST callback. The worker reports the original 32,768-byte POST as an
initial `TypeError`; the independent second origin records neither OPTIONS nor
POST for this route.

This gives a working observation for a decision made **before preflight
admission**. It does not show that a later application decision on POST can
retract a preflight that already ran. The original `cors-deny` case makes that
limit concrete: its POST also rejects, but one OPTIONS request still reaches
the origin. Neither case establishes Android callback parity by itself.

| Evidence | Runtime 131 log line | Runtime 157 log line |
| --- | ---: | ---: |
| Actual OPTIONS requested method and Origin | 2358 | 2252 |
| New preflight case's worker message | 2368 | 2262 |
| Independent original CORS OPTIONS record | 2432 | 2326 |
| Original CORS outcome remains `not-qualified` | 2466 | 2360 |
| New preflight outcome | 2472 | 2366 |
| One OPTIONS / zero POST callback accounting | 2478 | 2372 |

## What the new controller-ordering case establishes

A separate fresh profile prevents the earlier Close-first experiment from
altering this scenario's initial state. An allowed POST first transfers all
32,768 bytes to the origin. The next request is denied while the owning
controller is still alive. The actual native calls occur in the following
order, each returning `S_OK`:

| Native action | Runtime 131 log line | Runtime 157 log line |
| --- | ---: | ---: |
| Change the request URI to the terminal route | 2516 | 2410 |
| Clear request content | 2520 | 2414 |
| Complete the held deferral | 2522 | 2416 |
| Remove the first resource filter | 2526 | 2420 |
| Remove the second resource filter | 2528 | 2422 |
| Close the owner controller | 2534 | 2428 |

The [executed diagnostic source](https://github.com/gesher-runtime/gesher/blob/33fc2e50af97c95023e350ac4c8ba1e2d6d5e877/probes/webview/worker_denial_probe.h)
performs these actions in one drain turn, without an inserted delay or message
pump between them. The logged HRESULTs establish that the native calls were
accepted; they do not independently prove synchronous browser completion.

The surviving observer receives the denied request's initial `TypeError` and
then a real request from the **same worker** that returns HTTP 200 with the
complete 33-byte text `origin:teardown-complete-followup`. The origin records
only the first allowed POST and this follow-up GET. The denied route has no
request. The recorded late attempt to restore permission is rejected, and no
original-request restoration is recorded. Observer closure occurs after these
messages.

For runtime 131, the denial and follow-up messages are at lines 2540 and 2542,
origin records at 2552 and 2554, and the outcome at 2566. Runtime 157 records
them at 2432, 2436, 2446, 2448 and 2460. The earlier Close-first control remains
in the main matrix and still transfers the full original upload: lines
2402/2424/2474 on runtime 131 and 2296/2318/2368 on runtime 157. Completion before
owner closure has therefore been observed to work in this fixture; an
undecided deferral at owner closure remains unqualified.

## Independent request accounting and harness validity

Both jobs pass the actual socket controls for partial request accounting before
the worker scenarios. The server records complete requests and attributable
partial requests. Nonempty input that cannot be attributed invalidates the
measurement. Empty speculative connections are counted separately. The fixture
also exercises more than 64 sequential requests on a repeated route, avoiding
the earlier shared-server row-cap regression.

| Runtime / scenario | Accepted connections A / B | Received HTTP bytes A / B | Unknown requests | Unattributed HTTP bytes |
| --- | --- | --- | ---: | ---: |
| 131 / main matrix | 11 / 4 | 137,353 / 34,897 | 0 | 0 |
| 157 / main matrix | 13 / 4 | 137,654 / 35,069 | 0 | 0 |
| 131 / Complete-first | 6 / 0 | 36,591 / 0 | 0 | 0 |
| 157 / Complete-first | 6 / 0 | 36,677 / 0 | 0 | 0 |

These connection totals describe each fixture scenario as a whole; they are
not attribution of individual TCP connections to a particular request. The
original-route claims rely on the request and byte accounting, positive
controls, and actual worker results together. The JSON retains the native
observations instead of deriving success from a marker alone.

## Storage failed independently

Both jobs first establish all 14 real storage payloads, including the nested
ancestor-chain key, but the actual production Host deletion subprocess then
exits with `-1073741819` (`0xC0000005`). Neither runtime prints a production
callback summary in **this run**. There is no final deletion or preservation
proof and no successful storage qualification marker.

The fault record names `EmbeddedBrowserWebView.dll`, at RVA `0x353752` on
runtime 131 and `0x44ce24` on runtime 157. These addresses identify where the
fault was recorded; this report does not establish its cause. The log repeats
captured storage output when reporting failure, so the repeated seed lines are
not counted as additional executions.

A separate intentional native exception control exits with its expected
`-532217855` and records code `0xE0470001`, both expected parameters and actual
executable caller frames. The executed wrapper nevertheless rejects its flags
`0x00000081`, because that version requires exactly `0x00000001`. This control
failure and the real storage access violation are distinct. A later wrapper
correction or diagnostic change is outside this source pin and cannot supply
runtime evidence for it.

| Storage evidence | Runtime 131 log line | Runtime 157 log line |
| --- | ---: | ---: |
| First all-14 payload result | 1534 | 1432 |
| Actual storage process failure | 1600 | 1498 |
| Access violation / faulting module record | 2028 / 2029 | 1926 / 1927 |
| Intentional control rejected / actual flags record | 738 / 739 | 636 / 637 |

## Evidence identities and remaining compatibility work

All anchors above are one-based physical lines in the saved complete logs,
including blank lines. The receipt's anchor text omits only the timestamp.
Each saved log was checked byte-for-byte against its cached tool-result
content. The existing artifact ZIP and executable were independently hashed;
no additional CI download was needed for this report.

| Evidence | SHA-256 |
| --- | --- |
| Build log, 345 lines / 27,498 bytes | `79fa5f31ac5a65ddd4cd67fc2e54348012ce03489b6c453cf27588fe65776b84` |
| Windows 2022 log, 2,594 lines / 278,798 bytes | `45922a7c75ee8d6dd221c0d6bebc8b21df9a63461984937bf6795a2cec6ad2dd` |
| Windows 2025 log, 2,488 lines / 272,546 bytes | `e85bf368a77041bface9120341bf78d2b211bdbe2b592e2d629bd1dfcb94bc84` |
| Uploaded, downloaded and locally retained ZIP | `98a96f6e41820131ab3f9f0a4d61fea7b46fefa3530ded2afc6d0b8754f7d477` |
| Retained `webview2_probe.exe`, 4,253,696 bytes | `6a3b251bbedacfc4e1b873da23d0c000be77f41c4a63b705c85bf0481ecdc457` |

These observations narrow the implementation choices for compatibility work.
They do not qualify the real application callback, callback cancellation and
ownership, Android cache modes, cached responses, incremental upload streams,
stream-read errors, or broader worker and profile lifecycles. They also do not
change the storage result. Production capability activation and complete
Android behavior remain outside what this diagnostic establishes.
