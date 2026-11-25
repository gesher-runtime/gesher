# WebView runtime results — 25 November 2026

## Outcome and scope

The storage-registry experiment **passed its complete persisted/live/deletion/payload sequence on both tested Windows runtimes**. The successful workflow did not merely accept an unavailable capability.

The worker-response experiment remained a valid diagnostic but **did not provide the required network-error behavior**: its injected stream errors became successfully consumed HTTP 200 responses with empty or truncated bodies. No production worker capability follows from that result.

This record covers immutable diagnostic head
[`908d99eb480b6d40480235e13c678fb791e7e636`](https://github.com/gesher-runtime/gesher/commit/908d99eb480b6d40480235e13c678fb791e7e636).
It is evidence for the tested mechanisms, not a declaration that the complete Android WebStorage or service-worker APIs are implemented. The accompanying
[machine-readable evidence](webview-runtime-qualification-908d99eb.json)
contains the actual inventory snapshots, worker observations, counters, profile identities and process-exit records.

## Build and execution identity

[Workflow run 45498551083](https://github.com/gesher-runtime/gesher/actions/runs/45498551083),
attempt 1, was a push run on `palmoni5/webview-runtime-qualification` and completed successfully on 24 November 2026 at 22:22:28 UTC.

| Item | Verified identity |
| --- | --- |
| Source commit | `908d99eb480b6d40480235e13c678fb791e7e636` |
| Source tree | `cf7cad35dc850e9130d269710355b07999f64cc9` |
| Parent commit | `81d5fc3ff0651b23b9aa8a2fa043b9992cf5b7c0` |
| Build job | [138815400328](https://github.com/gesher-runtime/gesher/actions/runs/45498551083/job/138815400328) |
| WebView2 SDK | `1.0.4258.31` |
| Artifact | `webview`, ID `14970632267`, 2,571,476 bytes |
| Artifact SHA-256 | `deac166af322f374041c059c5b537c30a4ad087239ff9a0d7f4b4f1fd1771dbf` |
| New qualification modes | Static loader, `WebView2LoaderStatic.lib` |

The build log records checkout of the exact source commit, and GitHub's artifact metadata identifies the same run and head. Both static and DLL native targets were built. The new quota and worker qualification modes executed through the static loader; the earlier default/COM checks also remained in the workflow. This does not claim separate DLL execution of the new modes.

| Windows runner | Actual WebView2 runtime | Runtime job | Qualification step |
| --- | --- | --- | --- |
| Windows 2022 | `131.0.2903.86` | [138818825817](https://github.com/gesher-runtime/gesher/actions/runs/45498551083/job/138818825817) | Passed |
| Windows 2025 | `157.0.4341.46` | [138818825742](https://github.com/gesher-runtime/gesher/actions/runs/45498551083/job/138818825742) | Passed |

## Storage: the complete experiment passed

The preceding `81d5fc3f` run had reached the live quota registry and discovered persisted keys, but measured zero usage. The successor adds the browser-owned IndexedDB discovery barrier before reading the quota registry. Both actual runtimes completed that barrier with one reported storage partition.

The [probe source](https://github.com/gesher-runtime/gesher/blob/908d99eb480b6d40480235e13c678fb791e7e636/probes/webview/quota_registry_probe.h) retains all existing identity, positive-usage, deletion, payload and deadline checks:

1. Write 262,144-byte IndexedDB payloads in A first-party and C embedded under A. Obtain their actual serialized storage keys from browser frame contexts.
2. Close the writer controller and observe its retained browser-process handle signal exit.
3. Reopen the same verified user-data directory and profile path. Await actual IndexedDB disk discovery and obtain both positive-usage keys from the live quota registry **before revisiting any fixture origin**.
4. Write B first-party, C under B and A under B. Observe five complete, distinct storage keys with positive usage.
5. Delete the observed A-first-party and C-under-A keys. Observe zero usage for those keys and positive usage for the other three before opening a database again.
6. Confirm that the two deleted databases are absent. Read and compare every byte of all three preserved 262,144-byte payloads.
7. Close the reader controller and observe its browser-process exit.

Both logs contain the final qualified marker with the same counts: persisted 2, live 5, deleted 2 and preserved 3. Every earlier phase also passed. Neither runtime used the unavailable exit path.

The measured usage values below are the browser's actual quota counters. The 262,144-byte value is the logical payload size checked by the fixture; it is not substituted for the browser's storage-usage measurement.

| Runtime | Persisted global usage, 2 keys | Live global usage, 5 keys | Post-delete global usage, 3 positive keys |
| --- | ---: | ---: | ---: |
| 131.0.2903.86 | 30,572 bytes | 76,392 bytes | 45,820 bytes |
| 157.0.4341.46 | 30,576 bytes | 76,396 bytes | 45,820 bytes |

The post-delete registry still listed the two retired default buckets with usage zero. That is an accepted state under the unchanged gate; the subsequent database absence checks independently confirmed deletion. The other three entries and their complete contents survived.

This establishes the tested private-profile registry route, cold-start initialization, real serialized-key discovery, selective deletion and IndexedDB content preservation. It does **not** yet establish a production `getOrigins` bridge, arbitrary per-storage-key quota projection, all storage-client payloads, or application callback and profile-lifecycle ownership. The fixture's 64-entry inventory bound is a diagnostic limit, not a production limit. Android's default partitioning configuration and the observed Windows partitioned state also remain distinct contracts to reconcile.

## Worker responses: valid measurements, unsuitable failure mechanism

All results below were the same on both runtimes. A real service worker made each request; source-kind filtering was checked at native interception, and the independent loopback server counted requests reaching the origin. The document request remained outside the worker-only filter.

| Case | Initial fetch / body read | HTTP status | Body bytes | Origin requests |
| --- | --- | ---: | ---: | ---: |
| Worker pass-through | Resolved / complete | 200 | 11 | 1 |
| Supplied native response | Resolved / complete | 200 | 8 | 0 |
| HTTP-error control | Resolved / complete | 403 | 6 | 0 |
| First-read `STG_E_READFAULT` | Resolved / complete | 200 | 0 | 0 |
| First-read `E_ABORT` | Resolved / complete | 200 | 0 | 0 |
| `STG_E_READFAULT` after 64 KiB | Resolved / complete | 200 | 65,536 | 0 |

The first-read cases each recorded one stream read and one injected fault. The mid-body case recorded three reads, one injected fault and a supplied 65,536-byte prefix. All three recorded `pre-response-rejection=no`, an empty JavaScript error, no deadline abort and no escaped origin request.

Thus neither initial-fetch rejection nor a later body-read rejection implemented cancellation in these experiments. The HTTP 403 control also demonstrates that an HTTP error response is not a network error. The workflow passed because the diagnostic correctly measured and reported those outcomes; its success is not a worker-compatibility claim.

Observed retained-stream counts were zero and controller/subscription cleanup succeeded. The worker diagnostic does not establish complete browser-process retirement. Its response data was allocated before response completion, so the experiment also makes no progressive-streaming or worker-cache-mode claim.

## Existing PR ownership and historical CI

The repository state was checked on 25 November 2026. These are observed external states; this audit did not close or merge a PR.

| PR | Observed state | Relevant identity |
| --- | --- | --- |
| [381](https://github.com/gesher-runtime/gesher/pull/381) | Merged on 24 November at 23:33:23 UTC | Head `0a4bd5896b7f2dcbf6c535d9de5faa11b4bad17d`; merge `dcc29a49143c4bd33e0e60a81b25e0e1f2cfc12a` |
| [399](https://github.com/gesher-runtime/gesher/pull/399) | Merged on 25 November at 00:21:36 UTC | Head `20e4d091d239181e0bb0b3669c3c3fbddc44a78c`; merge `669d40a1218bdf3a38a2eb1ccc96ff3297f4c0d0` |
| [388](https://github.com/gesher-runtime/gesher/pull/388) | Closed, not merged | Head `895170db79c4d2d36baac84a20ea2aa2cca2a848` |

Consequently the landed R1 and graphics/usage implementations belong to current main. The remaining continuation should reuse those landed changes and keep new WebView work scoped to the existing PR391.

R1 head `895170db` had a successful historical [full CI run 45496569034](https://github.com/gesher-runtime/gesher/actions/runs/45496569034): attempt 1, 81 jobs, 75 successful and 6 skipped. Its source-identity job recorded test source `a6204272bda7269c7a9f636a54e9f208d1126a30`.

The older PR391 head `4fe77c03` had a failed [full CI run 45490428616](https://github.com/gesher-runtime/gesher/actions/runs/45490428616): 67 jobs, 53 successful, 5 failed and 9 skipped. The two root failures were the already diagnosed exact-source qualification checks in `artifact-preflight` and `compile-selected-minimum`. Both Windows aggregate jobs reported `SHARD_RESULT=skipped`; the final required gate propagated those failed and skipped dependencies. Those aggregate failures do not identify an additional Windows runtime defect. The run's tested merge source was `7485682204724b73d8c3fb4b9d423d64c1346daf`.

These historical results do not replace CI for a new integration commit.

