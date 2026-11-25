# One-shot application usage observers

## Reference and scope

This implements `registerAppUsageObserver` and `unregisterAppUsageObserver` in the
managed desktop profile, plus the `reportUsageStart` / `reportPastUsageStart` /
`reportUsageStop` entity inputs those observers accept. It follows
[`UsageStatsService`](https://android.googlesource.com/platform/frameworks/base/+/android-16.0.0_r4/services/usage/java/com/android/server/usage/UsageStatsService.java),
[`AppTimeLimitController`](https://android.googlesource.com/platform/frameworks/base/+/android-16.0.0_r4/services/usage/java/com/android/server/usage/AppTimeLimitController.java),
and the API descriptions in
[`UsageStatsManager`](https://android.googlesource.com/platform/frameworks/base/+/android-16.0.0_r4/core/java/android/app/usage/UsageStatsManager.java).
No AOSP implementation code is copied. Usage session observers and persistent
usage-limit observers remain explicitly unsupported.

The clock contract was verified against the exact Android 16 r4 source:
`AppTimeLimitController.java` Git blob
`1b34b817def139c47734e1799dd52ee0646b40d6`, SHA-256
`06fae6bdf1af1da03dc01bf6d273e40c4174f746505e06114eca0b01522af80c`.
Its `getElapsedRealtime()` returns `SystemClock.elapsedRealtime()` and is used
for registration, usage start/stop and timeout accounting. An older AOSP
revision used uptime; that older contract does not govern this runtime.

The older handoff describes these budgets as foreground time. Android 16's actual
`reportEvent` path starts observation on `ACTIVITY_RESUMED` **or a first
`ACTIVITY_PAUSED`**, and stops it at `ACTIVITY_STOPPED` / destruction. Pausing a
still-visible activity does not stop its time budget. `UsageLimitObservers`
follows that behavior independently of the historical foreground-time statistic.

## Observable behavior

- Registrations require `OBSERVE_APP_USAGE` (trusted system callers retain their
  existing authority), authenticated package ownership and the current installed
  package generation. The runtime has no active profile/device owner that could
  claim Android's additional owner exemption.
  Identity and permission checks enter ATM before taking the PM monitor; under
  PM, admission revalidates the captured installations without calling ATM.
  Shared-UID calls retain both the attached process's package and the named peer.
- Null or empty entity arrays fail with `IllegalArgumentException`; a null
  callback fails with `NullPointerException`; limits below 60,000 ms fail with
  `IllegalArgumentException`. There are at most 1,000 registrations per caller
  UID. `(UID, observerId)` replacement is allowed at that quota and resets the
  budget. Invalid replacements leave the previous registration intact.
- Time is the union of observed active packages / reported tokens. Concurrent
  windows of one package, overlapping packages and duplicate entity names never
  multiply elapsed time. The first visible event captures the task-root package,
  consistent with `getUsageSource()`. History continues to describe the actual
  activity package. Newly registered observers do not inherit past active time.
- A feature report belongs to its authenticated caller's `package/token`
  namespace. Duplicate starts and unmatched stops fail. Backdated starts count
  past usage without double-counting an earlier completed interval; stopping or
  destroying the activity also terminates its reports. All accounting uses the
  monotonic elapsed-realtime clock, independently of wall-clock corrections.
  Suspend and hibernate time count while a usage interval remains open, matching
  Android 16's AppTimeLimitController. The timer does not wake the suspended
  host; the next lifecycle accounting or timer callback observes the elapsed
  budget. Stopped intervals do not count.
- The timer sends a real `PendingIntent` target with `EXTRA_OBSERVER_ID`,
  `EXTRA_TIME_LIMIT`, and `EXTRA_TIME_USED` in milliseconds. One-shot registration
  removal precedes callback delivery. Sends use the runtime's existing
  `IIntentSender` server path with cleared Binder identity; there is no synthetic
  success or app-specific branch. Remote cancellation/failure cannot prevent
  other due callbacks. Callbacks never run while holding the observer monitor.
  When several callbacks become due together, each remains revocable by
  unregister, replacement or package retirement until its own delivery begins.
- Package retirement removes its registrations and live inputs. An installation
  generation check additionally suppresses an already claimed callback when the
  registering package has been replaced or is being removed.
- The standalone process does not supply authoritative managed lifecycle events.
  Registration and usage reporting there explicitly throw
  `UnsupportedOperationException`; authenticated unregistration remains
  idempotent. A registration cannot silently succeed against absent inputs.

## Scheduling and validation

The engine indexes groups by observed entity and orders live deadlines in a
`TreeSet`. It maintains **one pending timer**, irrespective of observer count.
Unchanged lifecycle events do not rearm it; inactive groups have no scheduled
work. A cancelled task racing its replacement is rejected by a generation check.
Callback code may reenter registration or fail without corrupting another group.

Run:

```sh
python3 tools/test_usage_observer_contracts.py --java-home "$JAVA_HOME"
```

The test compiles the entire production helper, service and history. Android
type/permission/Binder/intent-sender boundaries are explicit host fixtures, not
alternative observer implementations. It covers deadlines, overlap, multiple
windows, paused-first visibility, registration while active, replacement,
unregister races, UID isolation, minimum/quota errors, token reports, source
attribution, package retirement, permission errors, callback extras, suspend, failure,
reentrancy and concurrent registration with the real scheduled executor.
The service fixture also models CallerIdentity's actual ATM monitor boundary:
six concurrent lifecycle/admission cases reject the PM-to-ATM inversion, and
retirement between authentication and admission cannot acquire a replacement's
authority. `--source-ref` and `--case engine|service` run these same regressions
against earlier production sources as negative controls.

A deterministic workload with 1,000 live observers and 10,000 duplicate lifecycle
events schedules one task and delivers all overdue callbacks in one deadline
wakeup; inactive observers generate zero wakeups. These are workload counters,
not Windows CPU measurements. An ABBA speedup against the former unconditional
`SecurityException` stub would compare different operations and is not claimed.
The existing `test_activity_finish.py` entry point invokes this contract suite,
so the existing framework workflow runs it without workflow edits. Real Android
framework/AIDL compilation, Binder transport, PendingIntent flag handling and
Windows lifecycle integration remain separate framework/Windows validation.

## Integrated framework validation

On the repaired PR 399 source, including the latest-main merge at `16e474e2`,
the unchanged `app/build.sh` compiled the complete app with actual downloaded
framework classes and the Android core module, generated all AIDL delegates,
passed BootLinkCheck (296 references, zero
unresolved) and completed D8. With its generated `gesher-app-classes.jar`, the
unchanged actual-framework `test_activity_finish.py --group all` passed 901,542
checks; the query model passed 12,655 checks. This entry point also reran the
1,117-check observer engine, complete service and task-root hook suites. A
separate suspend regression models 30,000 ms of awake usage and 86,400,000 ms
suspended while the activity remains visible. At the next STOPPED event and
callback opportunity it reports 86,430,000 ms exactly once. It fails against
the initial review's mistaken uptime change (`b3ee32c5`) and passes with the
restored elapsed clock required by the pinned Android 16 source.

These runs execute real framework/service Java with controlled native identity
and app acknowledgements. They do not establish ART/Parcel transport, native
resource loading or Windows window rendering. The local framework class inputs
are prebuilt; AOSP source inputs for the complete SOURCE-retention compatibility
change table are unavailable, so the locally generated app package is a
validation artifact rather than a distributable runtime build. The
[PR review record](../audits/pr399-review.md) identifies the reviewed base,
repair provenance, other checks and remaining runtime limits.
