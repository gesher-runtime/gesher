# Bounded selection for activity usage event queries

Issue [#56](https://github.com/gesher-runtime/gesher/issues/56), F31/T12 query
selection leaf; [reserved scope](https://github.com/gesher-runtime/gesher/issues/56#issuecomment-7082880912).

## Behavior and ownership

The existing service copied all 12,000 retained activity events before applying
the requested time, package and event-type filters. A short recent query or an
empty query therefore allocated roughly 97 KB for data it would not return.
It also constructed unrelated interval metadata for every event query.

The bounded history now uses a lazily grown circular array. Its logical indexes
remain ordered across physical wraparound. Two lower-bound searches select the
inclusive beginning and exclusive end, preserving all equal-timestamp events in
their original order. Package and state filters run before copying the selected
snapshot. A full unfiltered result reserves its actual range size; the service
can then reserve the exact number of public result objects. Query selection
cost is O(log n + k), where k is the number of retained events in the requested
time window. A full-window package query still examines that window; there is
no claim that an absent package lookup is O(1).

Event capacity remains 12,000. An empty history starts with a zero-length array;
growth is geometric and stops at that existing cap. Appending/evicting an event
is amortized constant-time. Purge compacts surviving logical positions in order
and nulls every vacated reference. Returned snapshots own their list storage and
contain immutable event records, so later append, purge or RTC rebasing cannot
modify them. Persisted data remains version 3; no migration or format change is
introduced.

The existing monitor still owns event mutation, clock reconciliation and snapshot
construction. The same clock-generation path persists an observed RTC correction
before a public query returns; ordinary queries do not flush, accrue live time,
roll an interval or prune stored history. The public retention filter moves into
snapshot selection without changing its effective boundary. The separate full
history snapshot used by checkpointing and existing controls remains unfiltered.
The ordered-time invariant is already enforced by the checkpoint decoder and
the elapsed-to-wall event clock; rebasing moves retained event times together.

DesktopUsageStatsService still evaluates its actual AppOps/package identity and
permission policy before querying. Self queries still authenticate the requested
package and recheck its generation after the snapshot. STOPPED and TERMINATED
still map to the same public stop type; internal checkpoint records remain
excluded from public queries. No caller, event, interval or capability is
invented or substituted.

Contract references are the pinned Android 16 r4
[UsageStatsService](https://android.googlesource.com/platform/frameworks/base/+/45034f0663f960d9ee5fb0a101a4732b71f6e2f4/services/usage/java/com/android/server/usage/UsageStatsService.java),
[UserUsageStatsService](https://android.googlesource.com/platform/frameworks/base/+/45034f0663f960d9ee5fb0a101a4732b71f6e2f4/services/usage/java/com/android/server/usage/UserUsageStatsService.java),
and [UsageEvents](https://android.googlesource.com/platform/frameworks/base/+/45034f0663f960d9ee5fb0a101a4732b71f6e2f4/core/java/android/app/usage/UsageEvents.java).
The established [activity usage contract report](activity-usage-contracts.md)
describes the unchanged lifecycle, permissions and persistence boundaries.

## Actual public-service allocation measurements

The shared fixture seeds 24,000 explicit lifecycle transitions through the actual
DesktopUsageStatsService, retaining its normal 12,000-entry suffix. It uses 32
activity instances, two packages, and groups of four equal timestamps. It then
calls the public IUsageStatsManager methods and iterates actual framework
UsageEvents objects, verifying every returned time, instance, package, class and
state against an independent input model. The same fixture and complete paired
production source files run unchanged at historical baseline
`6759e71cde265d0c76fef8725d8b1ceecd246279`.

Host ThreadMXBean counters measure all allocation and CPU on the calling thread,
including identical result iteration and assertions. They do not measure RSS,
the background writer or a complete application's resource use. Warm-up precedes
each workload; execution order is candidate, baseline, baseline, candidate.

| Workload | Queries | Events/query | Baseline heap bytes, two runs | Candidate heap bytes, two runs | Reduction |
| --- | ---: | ---: | ---: | ---: | ---: |
| Recent global window | 1,000 | 20 | 100,659,624 / 100,639,208 | 4,109,352 / 4,109,416 | >95.9% |
| Empty future window | 1,000 | 0 | 96,976,160 / 96,963,952 | 480,160 / 480,160 | >99.5% |
| Package + type window | 1,000 | 64 | 106,372,000 / 106,394,656 | 9,655,320 / 9,656,160 | >90.9% |
| Absent package, full window | 256 | 0 | 24,834,208 / 24,834,208 | 229,536 / 213,152 | >99.0% |
| Full global control | 8 | 12,000 | 13,658,464 / 13,658,464 | 12,303,584 / 12,304,224 | >9.9% |

All corresponding output checksums match. The unchanged historical source fails
the candidate's allocation ceiling on the first recent-window workload, with
`Usage query allocation still scales with the unselected event suffix`.

CPU and elapsed time are recorded, including costs that did not improve:

| Workload | Baseline CPU ms | Candidate CPU ms | Baseline elapsed ms | Candidate elapsed ms |
| --- | ---: | ---: | ---: | ---: |
| Recent global window | 91.31 / 100.10 | 11.70 / 7.65 | 101.56 / 105.37 | 11.83 / 7.62 |
| Empty future window | 62.06 / 66.48 | 1.70 / 0.92 | 64.24 / 68.33 | 1.67 / 0.91 |
| Package + type window | 44.76 / 33.17 | 45.37 / 15.49 | 48.70 / 34.45 | 45.32 / 15.46 |
| Absent package, full window | 29.12 / 22.76 | 16.75 / 18.66 | 29.06 / 22.72 | 16.72 / 18.60 |
| Full global control | 8.34 / 7.60 | 9.97 / 13.15 | 8.31 / 7.58 | 13.27 / 16.84 |

The full-range control allocates less but is slower in this sequence. Filtered
CPU timing also varies across runs. Circular indexing and filtering have their
own costs; this evidence does not establish a universal speedup or stable
throughput ratio. No timing ratio is a correctness gate.

The [structured receipt](usage-query-selection-2026-11-22.json) contains every
raw measurement, execution order, baseline/candidate source hashes and log
hashes. Production hashes:

- ActivityUsageHistory.java: `a618aee391add30922350eee773b6e7b750ef38a272fa409458861fa8718f8ff`
- DesktopUsageStatsService.java: `8a009693912c06d0297106216d897f10793e26e18e43233e51e79680506df29f`

## Validation and boundaries

The 67 existing history contracts pass. The independent input-model fixture
covers lazy capacity, multiple ring wraps, equal timestamps, all endpoint shapes,
640 deterministic mixed queries, package/type masks, purge and vacated references,
forward/backward RTC corrections, persisted recovery, ordinary-query retention
without pruning, real flush pruning, and concurrent append/snapshot isolation.
Its number of checks varies with the concurrent schedule; both coordinator and
independent reviewer replays pass. This fixture executes the complete production
history with actual filesystem persistence and controlled wall/elapsed clocks.

The complete existing activity/controller/lifecycle/public-usage helper passes
901,542 checks including the new resource workloads. The independent reviewer
also replayed the focused public-query suite (901,153 checks) and existing history
suite, reviewed both production files and verified the final CI wiring.
Full application and services fixtures compile against real framework/libcore
classes and D8.

The pinned framework recipe already invokes the candidate checkout's
`tools/test_activity_finish.py` default `all` group. That entry point now runs the
standalone model with checked failure propagation, then the shared public-service
fixture. No workflow pin or controller deadline changes are needed. An explicit
historical-source measurement uses the unchanged old history/service pair through
the same public API; the new internal-API model is not compiled into that baseline.

The existing Windows ActivityFinishContractTest invokes the shared public fixture
with generated IUsageStatsManager AIDL/Parcel transport. Host execution uses real
framework classes and Java policy, with declared native identity/initialization
and constructed package/process boundaries. The optional host allocation counters
are reflective and are not dependencies of the ART path. Windows/ART execution
and complete published-head fork CI remain pending.

This leaf does not implement Usage Access UI, usage-limit observers, screen or
configuration telemetry, Doze/standby administration or complete F31/T12 closure.

## Reproduction

```sh
python3 tools/test_activity_finish.py --java-home /path/to/jdk \
  --framework-classes /path/to/framework-classes --core-classes /path/to/coremod \
  --app-classes /path/to/gesher-app-classes.jar --apksig /path/to/apksig.jar \
  --group usage-query
# Same public fixture with the unchanged historical pair and baseline reporting:
python3 tools/test_activity_finish.py --java-home /path/to/jdk \
  --framework-classes /path/to/framework-classes --core-classes /path/to/coremod \
  --app-classes /path/to/gesher-app-classes.jar --apksig /path/to/apksig.jar \
  --group usage-query --usage-source-ref 6759e71cde265d0c76fef8725d8b1ceecd246279 \
  --usage-baseline
```

Omit `--usage-baseline` from the historical command to observe the expected
candidate-budget failure. Omit `--group usage-query` from the candidate command
to run the complete existing lifecycle regression alongside the new workloads.
