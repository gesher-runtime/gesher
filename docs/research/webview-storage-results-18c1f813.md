# Storage and lifecycle results at 18c1f813

Both actual production storage executions completed the deletion, preserved-data,
callback and shutdown sequence without the earlier native access violation.
Runtime 131.0.2903.86 ran in attempt 1; runtime 157.0.4341.46 reached storage in
the single retry of its Windows job. **Both complete storage gates still failed**
at the final sentinel check. Neither execution produced a `QUALIFIED` result.

The [paired JSON receipt](webview-storage-results-18c1f813.json) retains exact
source/artifact identities and anchored observations. The unresolved sentinel
finding is a review-only item under the requested scope. This report changes no
network policy, isolation, bypass, acceptance gate or capability. The continuation
remains draft; no merge or auto-merge is requested.

## Exact run and unchanged acceptance gate

| Item | Identity or outcome |
| --- | --- |
| Run | [45618078011](https://github.com/gesher-runtime/gesher/actions/runs/45618078011), actor `palmoni5`, final conclusion `failure` |
| Source | [`18c1f8135ac82f4dbc8ba3ea36f8fb5dec93f9b9`](https://github.com/gesher-runtime/gesher/commit/18c1f8135ac82f4dbc8ba3ea36f8fb5dec93f9b9) |
| Source tree | `6555b82331f00d603c87e9e00df555ad8f11319b` |
| Build | [139218593155](https://github.com/gesher-runtime/gesher/actions/runs/45618078011/job/139218593155), successful |
| Windows 2022, attempt 1 | [139219879436](https://github.com/gesher-runtime/gesher/actions/runs/45618078011/job/139219879436), runtime 131, storage ran and failed its final gate |
| Windows 2025, attempt 1 | [139219879492](https://github.com/gesher-runtime/gesher/actions/runs/45618078011/job/139219879492), runtime 157, storage skipped after startup failure |
| Windows 2025, attempt 2 | [139225908726](https://github.com/gesher-runtime/gesher/actions/runs/45618078011/job/139225908726), runtime 157, storage ran and failed its final gate |
| Storage execution | SDK `1.0.4258.31`, static `WebView2LoaderStatic.lib` |
| Created / final update | `2026-11-25T12:53:15Z` / `2026-11-25T13:08:00Z` |

The source tree matches local commit `d9c5585888731484f2bdcd0567a1f66bd1b32807`.
Compared with the preceding 6ff0bd78 candidate, the only production file change
is `origin_storage_delete.h`, blob `2ec43f18d9dcb6e07b32c97196ccdffe62f0a0a6`.
The storage fixture, its Windows wrapper, the shared server and workflow are
unchanged. Their exact blob identities are recorded in the JSON.

The reviewed change `2857fb1bc03eb4906868de0827f6a4e7f56fc231` marks the operation
terminal in its SDK callback, retains the controllers and admitted work lease,
then performs checked cleanup, completion reporting and queue advancement on the
existing owner STA task queue. It preserves shutdown draining and the existing
timer as an asynchronous fallback if posting fails. The
[preceding matched-image evidence](webview-storage-crash-6ff0bd78.md) explains why
that ordering was tested. The present executions support this scoped stability
change; they do not establish every possible callback or timing interleaving.

All three Windows jobs downloaded artifact `webview`, ID `15022464196`, with the
same verified ZIP digest. The single retry selected Windows 2025 only, preserving
the source, artifact, deadlines, ordering and gates. The latest-attempt job list
also contains inherited completed build/Windows 2022 entries; those are not
counted as fresh execution evidence here.

## What both production executions passed

The two logs reach the same storage and lifecycle observations:

- Five persisted keys are discovered after browser exit and before an ordinary
  storage-origin revisit. The expanded fixture observes fourteen real full keys,
  including the browser's nested A → B → A key ending in `/^31`.
- Two queued A deletions complete, followed by the sentinel-origin request and
  separate HTTPS and HTTP deletions. All five callbacks are successful, ordered,
  exactly once and measured on the actual owner STA.
- The state progresses from fourteen keys to nine after A, remains nine after
  the sentinel request, then becomes seven after HTTPS and five after HTTP.
  Nine databases are read back as absent. Each of the five survivors retains
  its complete 262,144-byte payload.
- All fourteen localStorage sentinels and six tested first-party cookies remain.
  A's 8,192-byte OPFS file is absent and B's complete file remains readable.
- The last admitted HTTP deletion drains through `ShutdownIdle`; the owner
  thread joins, and a later deletion admission is rejected. Browser process
  exits are also observed during fixture setup and final cleanup.

The ordinary-profile concurrent control reports zero failures and actual
completed fetches during each operation. These observations are distinct from
the later failed sentinel condition:

| Operation interval | Runtime 131 completed fetches | Runtime 157 completed fetches |
| --- | ---: | ---: |
| A deletions | 37 | 33 |
| Sentinel-origin deletion | 27 | 19 |
| HTTPS deletion | 24 | 20 |
| HTTP deletion and shutdown drain | 16 | 15 |

The independent native exception control passes on both runners. Its deliberate
`0xE0470001` record remains in the logs; no actual storage native exception is
recorded. The storage subprocess instead returns ordinary exit code `1` after
the later gate failure. This is one actual storage execution per runtime, not
general proof that a native fault can never recur.

## The complete gate remains failed

In each first captured storage output, line 2176 reports that the production
resolver or ordinary observer failed the combined resource-isolation condition.
Line 2190 records these cumulative counters after cleanup and server joins:

| Runtime | Connections | Recorded requests | Received bytes | Unattributed bytes | Healthy |
| --- | ---: | ---: | ---: | ---: | --- |
| 131.0.2903.86 | 5 | 2 | 3,214 | 1,797 | false |
| 157.0.4341.46 | 6 | 1 | 2,454 | 1,709 | false |

Both wrappers report `exit=1 qualified=0 missing=3 final-callback-checks=2`
at line 2196. The missing later proofs are zero resolver-origin connections,
the after-operation positive network control, and the final ordinary-network
stop/count agreement. The two final callback summaries are expected observations
before and after the content checks, not duplicate callback deliveries.

The retained records do **not** identify which component or phase caused the
sentinel failure. The actual connection baseline is taken after seeding and
browser restarts at fixture line 580, but is not printed. The earlier printed
positive-control counts of two connections on 131 and three on 157 are not that
baseline. The comparison at lines 643–645 follows both production operations
and ordinary observer content navigations. The shared server keeps a cumulative
unhealthy state for nonempty unclassified input, without logging the input or
first-failure phase. Neither a production-only delta nor a traffic protocol can
be inferred from these numbers. No expectations have been relaxed and no new
network implementation or diagnostic experiment was performed for this finding.

## Preserved startup failure and evidence limits

Windows 2025 attempt 1 failed the preceding DLL browser-session controller
deadline. Its delayed completion returned `S_OK`, and the late controller was
closed successfully; the subsequent static probe created its controller. The
already-failed prerequisite caused the live storage step and native exception
control to be skipped. This failure remains in the receipt and is not counted
as a storage attempt or crash. Buffered log replay does not establish where
the shared startup deadline was spent.

The complete storage gate remains unqualified on both tested runtimes. The
separate [full-key quota contract](webstorage-full-key-contract-2026-11-25.md)
also remains outside this result. This receipt qualifies no `getOrigins` or
worker capability and requests no merge. Its positive data/lifecycle findings
and unresolved final gate are retained together for review.

## Evidence identities

Each saved log was checked byte-for-byte against its cached connector result.
The uploaded, downloaded and retained artifact ZIP digests agree, and all six
extracted members match the ZIP. Anchors are one-based physical log lines.
Failure output echoed later in each log is the same execution and is not counted
again.

| Evidence | SHA-256 |
| --- | --- |
| Build log | `41a84bccb8eafda7f6b6ce8613986076634db425ea16f90acdd9e250c50f6fe8` |
| Windows 2022 attempt 1 log | `fe64a0b6d7cc6145fcc7a11bd039349ad00d2db9dd629ceafe3e1f06485f07d6` |
| Windows 2025 attempt 1 log, storage skipped | `a7057ae214d3f0edf5653957fcd907e51eb49fd8c730cac769f0cb58b46bfbe6` |
| Windows 2025 attempt 2 log | `eacd8fbf9e5a4e7320b5eec0d253b2e0be2db62009b37b406e36d518000d7980` |
| Probe ZIP, 2,861,155 bytes | `45d80e711bb77156dd739437d3328eabc0bdedccd0b2c9ccfd98a54559c110fc` |
| Exact static probe | `2783f5533b590ad17abc6483e3512b2ba15a071463e5229caef4734baadcab55` |
| Exact DLL-loader probe | `eac862484e8e51800847ec8ec6a039d3e1735fa330245593aa6571ed107e5be9` |
