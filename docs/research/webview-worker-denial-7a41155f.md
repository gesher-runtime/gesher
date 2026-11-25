# Worker denial scope results at 7a41155f

The actual Windows experiment at commit
[`7a41155fc31285b803a9bf4d422c89622f9ad4d0`](https://github.com/gesher-runtime/gesher/commit/7a41155fc31285b803a9bf4d422c89622f9ad4d0)
(tree `ec678bb390c04b340becd99b17b2574ab699161e`) completed its worker
diagnostic controls on both runners in
[run 45593812298](https://github.com/gesher-runtime/gesher/actions/runs/45593812298).
This receipt records that tested source, before any later ordering experiment.
The earlier [6c104766 receipt](webview-worker-denial-2026-11-25.json) is retained.

[Build job 139134534716](https://github.com/gesher-runtime/gesher/actions/runs/45593812298/job/139134534716)
produced artifact `15012481782`. The upload step recorded ZIP SHA-256
`b1fd161972f999b157032807674057b1d65bb1bbc49b63070c7a0f5ef2cf1233`.

| Runner and job | Actual runtime | SDK / loader | Scope experiment |
| --- | --- | --- | --- |
| [Windows 2022, 139136048419](https://github.com/gesher-runtime/gesher/actions/runs/45593812298/job/139136048419) | 131.0.2903.86 | 1.0.4258.31 / static | Valid; three observed denials, two negative outcomes |
| [Windows 2025, 139136048551](https://github.com/gesher-runtime/gesher/actions/runs/45593812298/job/139136048551) | 157.0.4341.46 | 1.0.4258.31 / static | Valid; three observed denials, two negative outcomes |

The jobs also contain an independently failing storage-deletion fixture. A
successful worker diagnostic does not make those overall jobs successful.
No production worker capability was enabled.

## Observed outcomes on both runtimes

| Case | Initial worker fetch | Requests at denial target | OPTIONS at other origin | Recorded outcome |
| --- | --- | ---: | ---: | --- |
| Same-origin POST | `TypeError` rejection | 0 | 0 | `network-rejection-observed` |
| CORS POST-only denial | `TypeError` rejection | 0 POST | 1 | `not-qualified` |
| Redirect final target | `TypeError` rejection | 0 | 0 | `network-rejection-observed` for that target |
| Concurrent POST denial | `TypeError` rejection | 0 attributable to denied request | 0 | `network-rejection-observed` |
| Close owner before `Complete` | Resolved HTTP 200; actual origin body | **1 complete 32 KiB POST** | 0 | `not-qualified` |

Every matrix observation retained its original JavaScript request URL and
method, its expected resource-timing URL, and one worker identity per run.
None timed out or used the deadline abort. Permitted POST, CORS, redirect,
concurrent, sequential and observer follow-up controls returned their real
origin responses; native server checks verified the complete permitted uploads.
The redirect experiment deliberately reached its initial URL once. Its result
does not imply zero network for the whole chain or Android redirect parity.

### CORS: an observed preflight callback was deliberately allowed

Log lines 1791–1799 in each job record `OPTIONS` then `POST`, with
`source=SERVICE_WORKER`, for both CORS routes. The tested handler returns
unchanged for those OPTIONS requests. Independent origin records at lines
1917–1921 confirm one preflight for each route, the permitted POST and no denied
POST. This demonstrates a leak from POST-only denial; it does **not** demonstrate
that WebView2 cannot intercept the preflight. A separate OPTIONS-denial
experiment is justified by this actual event stream, but remains unmeasured in
this receipt. Policy must already deny network at preflight admission to use
such a path; a decision made after permitting preflight cannot undo it.

### Teardown: successful mutation and HRESULTs did not prevent the upload

The exact native order, lines 1865–1889 in each job, was:

1. Observe the Service Worker POST and retain its deferral.
2. Set the fresh, unregistered nonnetwork URI and verify the readback.
3. Clear the upload with `put_Content(nullptr)`.
4. Remove both filters and the request/message callbacks; close the owner.
5. Call `Complete` after closure; reject a later attempt to allow that request.

All mutation, filter-removal, owner-close and completion calls returned `S_OK`.
Removing the external-launch guard after closure returned `0x8007139f`. The
independent server nevertheless received the **original URL and full 32 KiB
body**; the observer received HTTP 200 and `origin:teardown-deny`. The observer's
next real fetch succeeded with the same worker identity. There were zero late
URI restores and zero external launches. Local terminal-state bookkeeping and
successful HRESULTs therefore do not establish safe shutdown ordering.

Microsoft documents [GetDeferral](https://learn.microsoft.com/en-us/microsoft-edge/webview2/reference/win32/icorewebview2webresourcerequestedeventargs?view=webview2-1.0.4258.31#getdeferral)
as deferring the event, and [Complete](https://learn.microsoft.com/en-us/microsoft-edge/webview2/reference/win32/icorewebview2deferral?view=webview2-1.0.4258.31#complete)
as completing it once. Testing `Complete` while the filter owner is alive,
followed immediately by removal/closure, is a supported call-order experiment.
Those documents do not promise that `S_OK` synchronously acknowledges a browser
network decision. The new ordering must be measured, without treating a fixed
delay as acknowledgement. The failed original order remains a negative control.

## Independent accounting and limits

Both jobs passed the actual socket accounting controls, including partial
request lines/headers/body, unclassifiable input at EOF/deadline/Stop, empty
speculative connections and 80 consecutive real responses on one repeated
route. Each scope run recorded 11 known origin requests, zero unknown requests
and zero unattributed HTTP bytes. Aggregate accepted connections were A=11/B=4.
Received bytes were A=137353/B=34897 on runtime 131 and A=137654/B=35069 on
runtime 157. All received bytes were accounted for after shutdown.

These counters measure the two owned loopback servers. Aggregate TCP counts
are not per-case attribution or system-wide network telemetry. Native log
order establishes operation order; GitHub's output-flush timestamps do not
measure time between operations. Full Android permission/error behavior,
production ownership, cache modes and incremental streaming remain open.

The [machine-readable receipt](webview-worker-denial-7a41155f.json) preserves
both log SHA-256 hashes, source callbacks, teardown call order, origin records,
22 exact worker observations and ten outcome rows, with original log lines.
