# Bounded worker denial experiments

The first phase of `webview2_probe.exe --worker-response` tests two hypotheses.
It changes no production worker API, setting, feature flag or activation gate.
The original pass-through, supplied HTTP 200, supplied HTTP 403 and three
faulting-stream cases remain. A final ordinary worker fetch must succeed in the
same worker instance after both new cases.

A second, isolated scope matrix now exercises five remaining denial risks.
Its source and local validation are described below. The actual execution
receipt in this document covers the first phase at its stated commit only;
it does not contain outcomes for the later scope matrix.

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

The server version in this first-phase receipt began its path counters only
after complete HTTP headers. Its zero counters therefore do not independently
exclude a request line or partial-header prefix that arrived before the socket
closed. The later byte-accounting repair described below closes that measurement
gap in source; only a new Windows run can establish the stronger observation.
The earlier recorded `TypeError`, JavaScript identity and resource-timing values
remain the actual outcomes of that older run.

## Scope matrix extension: measured at 7a41155f

The [exact-head results](webview-worker-denial-7a41155f.md) and
[machine-readable receipt](webview-worker-denial-7a41155f.json) now record both
actual Windows runtimes. POST, final redirect target and concurrent denial
produced initial `TypeError` with zero requests at their denied targets. The
POST-only CORS case leaked its deliberately allowed OPTIONS request. Closing
the owner before completing its deferral leaked the full original POST. Both
negative results remain `not-qualified`; no production capability is enabled.

The same command continues into a fresh profile with two ephemeral loopback
origins and two controllers, within its existing 120-second native budget.
The original nine observations and their supplied-response controls still run
first. No production source, activation flag or Android worker API is changed.

| Risk | Actual worker fixture and independent control |
| --- | --- |
| POST/upload escape | A permitted and a denied same-origin POST each construct a complete 32 KiB body containing the case name and worker identity. The server verifies every byte of the permitted body. A complete method and request target count immediately, even if the remaining request line, headers or body never complete. Nonempty bytes that cannot be attributed invalidate the measurement. |
| Cross-origin/preflight escape | A second loopback origin accepts a permitted JSON/custom-header POST after a real OPTIONS preflight. A separate denied POST uses the same request shape and a distinct URL. OPTIONS and non-OPTIONS requests are counted separately; any escaped preflight disqualifies network blocking even if the worker fetch rejects. |
| Redirect routing | Both GETs intentionally reach an initial URL that redirects to the second origin. The permitted chain must return the real final response. The denied case mutates only an observed final-target callback. Logs record the initial network request and whether that target callback occurred; zero `target-network` means only the final target was blocked. It never means the initial redirect request was blocked or that Android redirect callback parity was established. |
| Request identity and completion order | Two concurrent POSTs share exactly one URL but carry distinct case tags and full bodies. Native deferrals complete in reverse admission order, permitting one and denying the other. A subsequent permitted POST to that same URL verifies continued operation. The two permitted bodies must independently match their expected content. |
| Pending terminal denial during owner closure | An observer controller in the same profile reuses the existing worker. Native code obtains its POST deferral, applies and reads back the terminal nonnetwork URI, clears the upload and only then removes the owner's filters and closes that controller. The observer remains alive to report the actual fetch result and a further real fetch. A later test callback attempts to allow the old request; the proposed terminal-decision gate must reject it before any API can restore the original URI. |

Only the owner registers `SERVICE_WORKER` filters. Both controllers install
external-launch cancellation guards before navigation. The observer does not
register a second worker or a second resource filter. All eleven matrix
observations must retain one actual worker identity. Native logs contain each
observed source-kind callback, original request method and URL, terminal URI
readback, deferral completion, close result, and the observer's exact messages.
The original and final JavaScript `Request.url` and `Request.method` are checked
without replacing any observed value. Resource-timing entries retain their
actual names; the concurrent pair must yield two entries for their shared URL.

Each denied case reports `network-rejection-observed` only for a real initial
`TypeError`, preserved request/timing identities, no deadline abort, zero HTTP
requests at its denial target and zero preflight requests. Redirect qualification
additionally requires an observed target callback. Valid negative outcomes are
kept as `not-qualified`. Missing controls, malformed observations, uncertain
ownership, failed mutation or cleanup, unknown origin requests, and any external
launch attempt fail the harness instead. Total accepted TCP connections are
reported for both servers; those aggregate counts are not per-case attribution
or system-wide DNS/packet telemetry.

The server retains received-byte counts for every request, including documents
and scripts. EOF, per-client deadline and Stop cleanup drain pending nonblocking
input and classify any remaining prefix. An incomplete request target with
nonempty input makes the server unhealthy; an attributable prefix contributes
to that target's request count. Empty speculative TCP connections remain
informational. After shutdown the sum of all recorded and unattributed bytes
must equal the total received count. The Windows diagnostic begins with bounded
real-socket controls for partial request lines/headers/body, complete requests,
malformed headers, empty connections and unclassifiable EOF/deadline/Stop input.
They also verify 80 consecutive real HTTP 200 responses on the ordinary
storage-deletion control route. Repeated requests have no per-request record
ceiling; the existing process deadline, simultaneous-client, unique-route and
input-size bounds remain.

The late-allow guard is a test of the proposed terminal-decision design, not a
claim that a production Java callback or Android permission path was exercised.
An undecided deferral at owner closure is not covered or treated as safely
denied. Production integration must preserve this decision ordering and obtain
its own callback/permission/lifecycle evidence. Cached loads, Android cache
modes, incremental streams and stream-read errors remain separate open work.

Local validation of this extension:

- Both static and DLL probe targets compile with cached llvm-mingw/Clang
  23.1.3, SDK 1.0.4258.31 and the project's `-Wall -Werror` flags.
- The original exact scripts retain 159 passing adapter checks. The new
  `worker_denial_script_test.js` executes the exact embedded scripts for 451
  checks covering complete uploads, CORS request shape, reversed same-URL
  concurrency, response and body errors, deadlines, observed timing, observer
  reuse and its real-follow-up protocol. These are isolated protocol checks.
- `worker_response_gate_test.ps1` feeds the exact Windows log gate 28 isolated
  fixtures, including escaped preflights, incomplete headers/uploads, missing redirect
  callbacks, contradictory counters, lost identities and attempted late
  restoration. Fixture logs are suppressed so they cannot be mistaken for real
  native observations in CI. The receipt above records the subsequent actual
  execution at the exact tested commit.

The new socket accounting controls passed in both actual Windows jobs recorded
above. They were compiled, but not executed, in this Linux workspace. The
Windows sparse checkout includes both the gate and its fixture script.

## Follow-up variants: awaiting actual Windows execution

The exact `7a41155f` results justify two separate experiments. They preserve
the measured POST-only CORS and Close-before-Complete negative controls.
Neither variant enables a production flag or changes Android API behavior.

`cors-preflight-deny` adds a distinct cross-origin JSON/custom-header POST to
the first matrix. The handler observes the actual `SERVICE_WORKER` OPTIONS
callback, checks `Access-Control-Request-Method: POST` and the originating
worker origin, takes its deferral, and applies the same unregistered nonnetwork
URI after the callback returns. This models a policy already denying network
before preflight admission. It does not turn OPTIONS into an application POST
callback or claim that a later Java decision can retract an earlier preflight.
Any ensuing POST is observed and left unchanged so the independent server can
expose a failed preflight denial. Qualification requires the actual OPTIONS
callback, no subsequent POST callback, initial `TypeError`, unchanged original
JavaScript POST identity and timing, and zero OPTIONS or POST at the origin.
Missing interception, escaped traffic or another genuine fetch result is a
recorded negative; malformed headers, timeouts and uncertain evidence fail
the harness.

`teardown-complete-deny` runs in a second fresh, verified profile with its own
worker, two controllers and loopback origins. A full permitted POST first
proves ordinary operation. The observer then asks that same worker for a new
32 KiB POST. The owner retains its native deferral, applies and reads back the
terminal URI, clears the upload, and calls `Complete` **while the owner and
both source filters are still alive**. In that same STA turn it immediately
removes the filters/callbacks and closes the owner. No delay or explicit pump
occurs between completion and closure. A successful HRESULT records call
acceptance; it is not treated as synchronous acknowledgement of a network
decision. The separate observer reports the actual fetch and then another
real GET from the same worker. Late allow is rejected before restoring any URI.

This second scenario logs as `worker-ordering`; its three observations and
independent byte accounting cannot substitute for the first matrix's twelve
observations. Both run within the existing 120-second native deadline and
140-second process watchdog. All existing response controls run first. The
PowerShell gate checks actual native call order, distinct profile paths,
follow-up/control evidence and independent origin counts. Valid negative
outcomes are retained even when the new ordering accepts every API call.

Local validation compiles both static/DLL targets with the cached SDK and
`-Wall -Werror`, executes 577 exact-script adapter checks plus the original
159, and checks 44 positive/negative Windows-log fixtures. These local checks
validate the observation protocol only. The two new outcomes require a later
exact-head Windows receipt; no result is inferred from the previous run.
