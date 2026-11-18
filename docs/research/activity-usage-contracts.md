# Activity usage history: Android 16 contracts

The reference is `android-16.0.0_r4`, frameworks/base commit
`45034f0663f960d9ee5fb0a101a4732b71f6e2f4` (tree
`6667cba9cf8225451f0822ad94cb21902025dfa3`). The Windows service uses its own
bounded checkpoint format and managed-activity tokens; application-visible
activity states, elapsed usage, interval selection and query permissions follow
Android. Ordinary data comes from owned lifecycle acknowledgements.

## Lifecycle and accumulation

[UsageStats.updateActivity/update](https://android.googlesource.com/platform/frameworks/base/+/45034f0663f960d9ee5fb0a101a4732b71f6e2f4/core/java/android/app/usage/UsageStats.java)
accepts PAUSED as the first visible state, and after a prior stop. That transition
starts visible accounting without manufacturing a resume, foreground time or a
launch count. Multiple visible/resumed instances of a package do not double-count
its elapsed visible/foreground time. Destruction without a preceding stop is
exposed as a stop; a destruction after a stop is suppressed.

`ActivityTaskManagerService.realStartActivity` can send a launch followed by
`PauseActivityItem` when a task is covered. Its `activityPaused` acknowledgement
arrives while the record is LAUNCHING with its actual `launchingPaused` flag. The usage hook accepts that owned path,
normal PAUSING, and a STARTED visible activity. `ActivityClientController` retains
its current-owner/retirement checks. History also accepts STOPPED-to-PAUSED for
the same still-live instance identity. Duplicate acknowledgements remain suppressed.

The 12,000-entry public event suffix is not an accumulator input. Four interval
stores contain cumulative package totals; current intervals additionally retain
live per-instance state. Evicting the first resume, every event for a completed
activity, or events for an interval does not erase those totals. Private checkpoint
accounting updates live time without inserting public resume or shutdown events.
Ordinary public queries return the already recorded accumulators. They do not
report `FLUSH_TO_DISK`, accrue live time, roll intervals, or prune history merely
because the wall clock has advanced. Real lifecycle events and the periodic
flush observe the daily deadline. This follows `queryStats/queryUsageStats` and
[`UsageStatsService.flushToDisk`](https://android.googlesource.com/platform/frameworks/base/+/45034f0663f960d9ee5fb0a101a4732b71f6e2f4/services/usage/java/com/android/server/usage/UsageStatsService.java).
Persisted snapshots contain totals through the checkpoint, with no recoverable
activity tokens. A restarted broker cannot count its offline period as activity.

## Interval origins, rollover and BEST

[UserUsageStatsService](https://android.googlesource.com/platform/frameworks/base/+/45034f0663f960d9ee5fb0a101a4732b71f6e2f4/services/usage/java/com/android/server/usage/UserUsageStatsService.java)
creates an interval at the actual service load time, reuses a non-expired stored
origin, and rolls its daily interval when an observed timestamp reaches the daily
deadline. [UnixCalendar](https://android.googlesource.com/platform/frameworks/base/+/45034f0663f960d9ee5fb0a101a4732b71f6e2f4/services/usage/java/com/android/server/usage/UnixCalendar.java)
uses fixed 24-hour / 7-day / 30-day / 365-day durations. Neither Unix-epoch flooring
nor local civil-calendar alignment implements the stored-origin contract.

At rollover, Android closes active durations at the old daily expiry minus one
millisecond, reloads current intervals, and continues real still-active instances
at the new daily origin. No synthetic continuation is added to public events.
[UsageStatsProtoV2](https://android.googlesource.com/platform/frameworks/base/+/45034f0663f960d9ee5fb0a101a4732b71f6e2f4/services/usage/java/com/android/server/usage/UsageStatsProtoV2.java)
persists cumulative durations and app-launch counts, but omits live activity maps
and the deprecated activity launch counter. Therefore even a retained weekly or
monthly interval must close its live state before continuation after daily
rollover. The in-memory implementation observes that boundary without requiring
Android's protocol-buffer storage design.

[UsageStatsDatabase.findBestFitBucket](https://android.googlesource.com/platform/frameworks/base/+/45034f0663f960d9ee5fb0a101a4732b71f6e2f4/services/usage/java/com/android/server/usage/UsageStatsDatabase.java)
selects the closest stored origin at or before the query beginning. It traverses
larger interval types first and retains that choice on an equal-distance tie.
The requested range's length is not its selection rule. If no stored interval
qualifies, the service falls back to DAILY. The Gesher index marks an origin
available only after an atomic checkpoint succeeds. A real rollover publishes
its stored origins; a subsequent BEST query only selects from that index.

The database source also supplies the separate pruning windows (10 days, 4 weeks,
6 months, 2 years) and record-count bounds (100, 50, 12, 10). Public event retention
remains separately limited to 30 days and 12,000 transitions. Query snapshots copy
the event suffix or the selected overlapping interval data, not every retained
package aggregate. Archived intervals release all live instance maps.

## Wall clocks and persistence

`UserUsageStatsService.checkAndGetTimeLocked/onTimeChanged` compares real wall
time with an elapsed-time prediction and treats differences greater than 2,000 ms
as clock changes. `UsageStatsDatabase.onTimeChanged` moves interval origins;
stored relative timestamps move with them. Gesher similarly shifts event times,
origins, last-use times and its time cursor together, while leaving elapsed totals
unchanged. A backward correction cannot leave the previous monotonic clamp ahead
of all newly recorded events.

The actual Android clock-change path calls `persistActiveStats`, rebases its
database, then calls `loadActiveStats`. That persist step does not report a flush
event. Reloading V2 drops live activity maps and the non-persisted launch counter,
so Gesher also clears them. It preserves previously recorded durations without
inventing a resumed continuation across that reload. A later actual PAUSED
acknowledgement may start visible accounting again.

An observed correction is published before a public query returns its resulting
history. Encoding and fsync use an immutable image outside the history monitor.
The writer rejects a pre-correction image before atomic replacement if its clock
generation changed during I/O. A bounded retry leaves the prior valid checkpoint
in place on repeated clock changes or I/O failure. Ordinary transitions perform
no disk I/O; a transition that observes a clock change or actual daily rollover
publishes that storage boundary. Writer serialization also prevents an earlier save from
resurrecting data after a successful synchronous package purge.

Version 3 independently stores the event suffix, interval origins and cumulative
totals, and the activity-ID high-water mark. Decode publishes no partial state.
Both read and write enforce a 64 MiB checkpoint budget; exceeding it reports an
I/O failure and keeps the last valid file. This limit does not fabricate or erase
in-memory success results. The background writer can be closed by fixtures so a
retired fixture cannot later recreate a deleted checkpoint. The published
contributor version 2 uses a different interval layout; its origins, aggregate
fields, event suffix and ID counter are decoded and migrated directly. The
unpublished intermediate review candidate's different version-2 layout was never
the public format. Version 3 keeps the public layouts unambiguous.

Version 1 had no aggregate prefix or real interval origins. Migration preserves
its surviving events and known legacy origins; it cannot reconstruct discarded
observations. All newly created intervals use actual load/rollover times. A clock
correction while the broker was absent has no trustworthy boot/elapsed anchor.
Recovery filters future public events and future-origin intervals, retains valid
past-origin totals, and never extends a former live session through downtime.
It does not claim to infer the unobserved correction.
An offline backward correction may leave an older completed last-use timestamp
above a later in-memory package end. The persisted package end uses the containing
interval's end, as Android V2 restores it. New activity in another package, or a
PAUSED-only return, therefore cannot create a checkpoint rejected by its own
reader or erase already recorded durations.

## Validation and limits

The standalone production-history fixture is:

```text
javac -d <classes> app/java/com/gesher/server/ActivityUsageHistory.java tests/activity-usage/ActivityUsageHistoryTest.java
java -cp <classes> com.gesher.server.ActivityUsageHistoryTest
```

It covers overlap, duplicate suppression, initial and reentered PAUSED visibility,
real-origin rollover, retained weekly continuation, bounded event eviction,
completed/live aggregate recovery after eviction, saved-origin BEST selection,
forward/backward RTC corrections with real checkpoint recovery, uninstall purge,
legacy and published-version-2 migration, ordinary reads without flush/rollover,
new activity following an offline backward correction, and transactional rejection
of a truncated checkpoint. The version-2 fixture was emitted by the unchanged
published writer, rather than constructed by the new reader.

The owned ATM/controller acknowledgement regression is included in
`ActivityFinishFixture` and `ActivityFinishContractTest`. Its focused host command
uses `tools/test_activity_finish.py --group usage` with the normal framework,
core, app, apksig and JDK arguments. That harness executes actual Java controller,
ATM, usage-service, AppOps and permission-policy code with controlled native initialization/identity
and application acknowledgements. The Android fixture is included in the existing
Windows contract track for subsequent ART/Binder execution.

Independent local source controls use the complete pinned `UsageStats` accumulator
and exact pinned clock/onTimeChanged/persist/load/query/rollover/deadline/BEST method bodies. Their storage adapter
models V2 persisted fields and empty live maps for finite activity-only cases;
it is not execution of Android's protocol-buffer database, screen/configuration
telemetry or a booted Android device. Separate fixtures execute real candidate
persistence and controlled wall/elapsed clocks without changing the host clock.
All composed app sources are type-checked against cached Android16 framework/core
and historical generated Stub/CompatChangeTable inputs. These checks establish
neither a fresh platform build nor Windows/ART execution, Binder parcel transport,
real device comparison or native Windows filesystem behavior.

Global queries use the actual `OP_GET_USAGE_STATS` operation. Android's
[`BinderService.hasQueryPermission`](https://android.googlesource.com/platform/frameworks/base/+/45034f0663f960d9ee5fb0a101a4732b71f6e2f4/services/usage/java/com/android/server/usage/UsageStatsService.java)
accepts `MODE_ALLOWED`, falls back to the authenticated calling permission only
for `MODE_DEFAULT`, and denies other non-allowed modes. The
[`AppOpsManager.noteOp`](https://android.googlesource.com/platform/frameworks/base/+/45034f0663f960d9ee5fb0a101a4732b71f6e2f4/core/java/android/app/AppOpsManager.java)
errored mode raises `SecurityException`. Gesher uses its real package/UID AppOps
ledger, rechecks package identity after policy evaluation, and retains its
trusted control-plane exemption. App self queries authenticate their claimed package.
This repair does not implement Usage Access UI, Doze/standby administration,
usage-limit observers, foreground-service usage or configuration/screen telemetry.
