# StorageStats: share overlapping scans and bound disk concurrency

This implements a bounded part of JAVA-10 in [#178](https://github.com/gesher-runtime/gesher/issues/178).
It does not complete the epic's Windows workload or soak acceptance.

## Problem and behavior

Each StorageStats request previously walked its package or external trees again,
even while identical requests were reading those same trees. A burst could occupy
many Binder threads with duplicate disk traversal. The walk also silently returned
partial sums on entry/depth limits and read failures.

The service now shares only **overlapping** scans, after independently checking
each caller's volume, user and authorization. A key includes the query kind and
installed package object, UID and storage paths. A new install generation or
different query cannot reuse another generation's result. Before publication and
after a joined reader resumes, package identity and removal state are rechecked;
permissions are checked again for each returning caller.

Two requesting threads can perform filesystem I/O at a time. Up to 16 distinct
in-flight keys, including running scans, are retained. Further distinct requests
wait before admission; admitted pending scans start in order. Identical requests
join the existing flight even when the distinct-key budget is full. Work executes
outside the coordinator monitor. No executor, permanent worker, polling timer or
completed-result cache is added. Binder callers still wait synchronously, as the
API already requires; this change does not claim to eliminate Binder occupancy.

Completion removes the key and wakes its readers, including on failure. An
interrupted joiner leaves the owner and other readers intact. An interrupted
queued owner completes its shared flight with an error; an active owner's walk
checks interruption between filesystem visits. A blocked filesystem operation
cannot be forcibly interrupted and remains accounted for until it returns.
Each successful caller receives a separate StorageStats/ExternalStorageStats
object, so in-process callers cannot mutate a peer's shared result.

No filesystem snapshot is promised: concurrent file modifications can affect an
ordinary walk. New requests after completion always rescan, including after
external writes or deletes. No application data, permission policy, quotas or
retention policy is changed.

## Complete results and Android contract

[StorageStatsManager](https://developer.android.com/reference/android/app/usage/StorageStatsManager)
provides separate own-UID/package and privileged user/external queries. The
existing Gesher authorization policy remains unchanged; results never substitute
for an authorization decision. Checked I/O errors use ParcelableException for
the framework's Binder unwrapping, as other StorageStats failures do.

The existing 200,000-entry query budget and 64-level traversal depth remain.
Reaching a limit or failing to read part of a tree now fails the whole query
instead of presenting an underestimate as a successful complete measurement.
An absent optional root is still empty. Traversal does not follow symbolic links;
APK size reads also use no-follow attributes. Package replacement/removal during
a queued or active query produces an I/O error, allowing a fresh request.

This fixes an existing correctness problem alongside coalescing: sharing an
incomplete size would otherwise spread that incorrect result to more callers.

## Measured work reduction

Baseline: `8b316aebf09ab46d5407e955bf86ed748a1e739f`.
The benchmark calls the production external-stats method on actual host trees.
Only Binder identity/package metadata are stubbed. A counting wrapper delegates
Files.walkFileTree to the real filesystem and the unchanged production visitor.
A latch keeps requests overlapping before the first traversal; it does not
replace the scan or its answer. All 32 callers must be blocked before release.

| Files in one tree | Concurrent queries | Baseline walks / visits | Candidate walks / visits |
| ---: | ---: | ---: | ---: |
| 1 | 32 | 32 / 64 | 1 / 2 |
| 100 | 32 | 32 / 3,232 | 1 / 101 |
| 10,000 | 32 | 32 / 320,032 | 1 / 10,001 |

Every result has identical total/image bytes. This is **96.875% less traversal
work for this overlapping-query fixture**, not a latency, CPU or application
speedup percentage. Non-overlapping requests intentionally receive no cache
benefit. The two-scan concurrency budget may trade distinct-query latency for
lower disk contention; representative Windows measurements remain necessary.

Source hashes, environment and observed counts are recorded in
[`storage-stats-scans-2026-11-22.json`](storage-stats-scans-2026-11-22.json).

```sh
python3 tools/test_storage_stats_scans.py --java-home "$JAVA_HOME"
python3 tools/test_storage_stats_scans.py --java-home "$JAVA_HOME" \
  --source-ref 8b316aebf09ab46d5407e955bf86ed748a1e739f --benchmark --report-only
```

Removing `--report-only` from the baseline command is an intentionally failing
negative control: the first workload detects 32 walks where the new gate requires
one. No threshold based on noisy wall time is used.

## Validation and remaining qualification

The production-source host fixture covers CE/DE/cache/external totals, independent
mutable outputs, 16 coalesced package callers, freshness, replacement generations,
authorization before I/O and after waiting, invalid UID/user/volume, read failures
and later retry, symlinks, entry/depth limits, interruption, 40 distinct requests,
the two-active/16-key limits and retirement.
All 181 checks pass. Coverage explicitly includes interruption of an admitted
owner waiting behind both active scans, failure delivery to its joined reader,
retirement without running that queued work, and subsequent progress.

CI runs it on Ubuntu and Windows 2022/2025. The existing Windows/ART
SettingsIntentsTest now also verifies fresh results after a real file write and
delete; its existing checks preserve package/UID/external accounting. Those
Windows/ART runs were not executed in the local Linux session. Host faults and
gates are explicit test controls, never production fallbacks.

Remaining JAVA-10 qualification includes physical-disk CPU/I/O/latency under mixed
workloads, actual Binder wait time, broad rename/uninstall stress and long runs.
There is no persistent size index or cross-query partial-subtree reuse.
