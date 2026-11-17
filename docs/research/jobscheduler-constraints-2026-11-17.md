# JobScheduler constraint and callback contracts

This review targets Android `android-16.0.0_r4` and the F29 constraint-monitoring
change. Windows supplies observations; the scheduler retains Android job admission,
start/stop ordering, deadline rules and callback outcomes.

## Android sources

- [JobStatus](https://android.googlesource.com/platform/frameworks/base/+/android-16.0.0_r4/apex/jobscheduler/service/java/com/android/server/job/controllers/JobStatus.java):
  `isReady`, `setDeadlineConstraintSatisfied` and `constraintToStopReason` distinguish
  an explicit one-shot deadline from a periodic window. The former can override
  explicit constraints; the latter cannot. Idle loss has its own public stop reason.
  [TimeController](https://android.googlesource.com/platform/frameworks/base/+/android-16.0.0_r4/apex/jobscheduler/service/java/com/android/server/job/controllers/TimeController.java)
  continues evaluating an unexpired deadline until it is satisfied, including when
  the deadline passes after execution was selected.
- [JobServiceContext](https://android.googlesource.com/platform/frameworks/base/+/android-16.0.0_r4/apex/jobscheduler/service/java/com/android/server/job/JobServiceContext.java):
  binding, starting, executing and stopping are distinct states. Cancellation during
  start waits for the start acknowledgement. Binding timeout retries; missing start
  acknowledgement ends that attempt; missing stop acknowledgement retries. The
  source uses 18 seconds for binding and 8 seconds for app acknowledgements. Network
  updates wait until execution and remote failure retires the run for retry.
  `isOverrideDeadlineExpired` is captured when execution is selected, including for
  an expired periodic window; that parameter flag is not constraint immunity.
  Binding/start response timeouts set the internal ANR reason, while a stop response
  timeout retains the reason that initiated the stop. `jobFinished` replaces both
  stop reasons with undefined/successful-finish, even if a stop was already pending.
  A null binding is an explicit no-retry outcome. An exception sending `startJob`
  leaves the start acknowledgement timer active; a later confirmed disconnect
  retries, whereas an unacknowledged start times out without retrying.
- [JobSchedulerService](https://android.googlesource.com/platform/frameworks/base/+/android-16.0.0_r4/apex/jobscheduler/service/java/com/android/server/job/JobSchedulerService.java):
  a network constraint requires `ACCESS_NETWORK_STATE` for target SDK 34 onward.
  Missing `RECEIVE_BOOT_COMPLETED` for a persisted job produces
  `IllegalArgumentException`. User-initiated job permission is independent of
  optional compatibility settings.
  `getRescheduleJobForFailureLocked` separates application failures from system
  stops. `getRescheduleJobForPeriodic` clears the failure counters and retains the
  original periodic cadence across retry attempts and missed windows.
- [BatteryController](https://android.googlesource.com/platform/frameworks/base/+/android-16.0.0_r4/apex/jobscheduler/service/java/com/android/server/job/controllers/BatteryController.java):
  ordinary charging jobs require stable power (charging and battery not low).
  Top-app jobs at normal priority or higher retain the connected-power exemption
  during that execution. AC power does not erase a battery-not-low requirement.
  The priority comparison uses `JobStatus.getEffectivePriority`, including decay
  after application failures; system stops do not lower a job's priority.
- [DeviceIdlenessTracker](https://android.googlesource.com/platform/frameworks/base/+/android-16.0.0_r4/apex/jobscheduler/service/java/com/android/server/job/controllers/idle/DeviceIdlenessTracker.java)
  and [framework config](https://android.googlesource.com/platform/frameworks/base/+/android-16.0.0_r4/core/res/res/values/config.xml):
  inactivity is considered after an observed noninteractive state. The default
  inactivity thresholds on this release are both 1,860,000 ms. Keyboard inactivity
  while the display remains interactive is insufficient.
- [StorageManager](https://android.googlesource.com/platform/frameworks/base/+/android-16.0.0_r4/core/java/android/os/storage/StorageManager.java):
  the default low-storage threshold is 5% of the data volume, capped at 500 MiB.

## Windows observations and scheduling

Network matching uses `NetworkRequest.canBeSatisfiedBy` against the same interface
snapshot and capabilities used by ConnectivityManager. It passes the selected
interface's actual netId to the application. A metered Wi-Fi network is not cellular;
custom transport and roaming requirements remain effective. No observed satisfying
network means `null`, even if a deadline allows the job to run. The scheduler
subscribes through an explicit in-process ConnectivityService dependency, constructed
before the scheduler, without inheriting an app's Binder identity for registration.

Unknown power information does not mean charging or a full/absent battery. An
explicit no-battery observation on AC remains supported. Storage constraints query
the actual data volume's total and usable bytes using Android's default threshold;
unknown volume capacity remains unsatisfied.

The native host watches
[`GUID_SESSION_DISPLAY_STATUS`](https://learn.microsoft.com/en-us/windows/win32/power/power-setting-guids)
through `RegisterPowerSettingNotification` on its existing event window. Only an
observed off display starts the inactivity interval. On, dimmed and unavailable
states are not idle. `WindowsHost.sessionDisplayState()` separately preserves the
observed OFF/ON/DIMMED value and reports UNKNOWN until a notification is available.
The result is bounded by the actual last-input interval and
uses Windows elapsed time, including suspend. A display change triggers reevaluation;
periodic polling also evaluates elapsed inactivity and storage. This does not
claim support for Android dreaming or a wireless charging dock on Windows.

Each pass samples the host once rather than enumerating native interfaces for every
job. Running attempts serialize outbound start, stop and network calls with binding
cleanup, without holding the scheduler monitor over those calls. Each attempt owns
at most one timeout in a shared daemon executor; completed/canceled timers are
removed. Repeated constraint notifications cannot start a second stop thread or
send duplicate stop callbacks. Cancellation during binding retains retryable work,
unless the job was explicitly removed or its owner changed. Delayed acknowledgements
cannot restart a finished attempt. Retry preserves pending content trigger metadata.

## Completion, retry history and unfinished work

Every run records its public and internal completion reasons, including during
binding before its `JobParameters` exist. The retry decision follows the pinned
Android source and its default system-stop-to-failure ratio of three:

| Completion requesting retry | History change | Backoff contribution |
| --- | --- | --- |
| Application `jobFinished(true)`, execution timeout, internal ANR, or public user stop | One application failure | One attempt immediately |
| Constraint loss, unexpected disconnect, or other system stop | One system stop | One attempt for every three system stops |
| Abandoned execution timeout, when enabled by the framework flag and app compatibility metadata | One abandoned failure and one application failure | One attempt; after the fourth abandoned failure the policy is exponential |

The attempt count is `failures + systemStops / 3`, using integer division. Zero
attempts means no earliest runtime at all. Positive counts apply the requested
linear/exponential policy, Android's minimum initial delay and maximum backoff
cap. Counters and delay multiplication saturate without wrapping. A new `schedule`
starts fresh history; `enqueue` onto the same job retains it. A successful periodic
transition clears application, abandoned and system-stop histories. Failed periodic
attempts have no latest-runtime deadline; a separate original window anchors the
next normal periodic schedule and its flex/buffer calculation.

For ordinary jobs, zero/one application failure retains the requested priority,
two/three cap it at default, four/five cap it at low, and six or more use minimum.
Requested expedited jobs retain at least high priority after failures, and
user-initiated jobs retain their priority. The charging exemption uses this
effective value for a current or previously top-started attempt.

`handleAbandonedJob` marks an executing attempt and leaves its lifecycle intact.
When its execution limit arrives, the scheduler chooses the ordinary or abandoned
timeout reason using the Android flag and compatibility table. It does not treat
the cleanup notification itself as an application request to finish.

Pending or delivered work is transferred only when the job is actually retained
for retry/periodicity. Work remaining in a nonperiodic job does not override
`jobFinished(false)`, `onStartJob(false)`, `onStopJob(false)`, a null binding or a
missing start acknowledgement. Those outcomes remove the scheduled job and release
the queue references. Completed callbacks cannot resurrect it. Requested retries
preserve work IDs and increment delivery counts when that work is dequeued again.

The priority and retry/periodic calculations are adapted from the linked AOSP
`android-16.0.0_r4` sources, copyright 2014 The Android Open Source Project,
under Apache License 2.0; the source file retains that attribution.

## Validation and limits

`JobSchedulerConstraintContractTest` enters production `Run.started`, receives the
actual constructed JobParameters and acknowledges the actual callback. It covers
unavailable observations, actual netId handover, cellular versus metered Wi-Fi,
custom requests, periodic/one-shot deadlines, pending reasons, power/storage/idle
stop reasons, acknowledgement ordering, callback failures, binding/start/stop timeout
outcomes, retry/backoff and permission admission. Additional regressions cover the
first six system stops, mixed application/system history, linear/exponential caps,
periodic retry reset and cadence, effective-priority charging, expedited and
user-initiated priority exceptions, abandoned callbacks/timeouts, and pending plus
delivered work under retry and no-retry completions. Independent probes on
`182bf771c85566af9ca0b1a7ff71aba7eea54ead` reproduced three errors: constraint stops
charged as application failures, charging exemption based on raw priority, and a
missing start acknowledgement retaining pending work. The repaired suite asserts
the pinned Android outcomes instead of treating those old outcomes as expected.

The missed-window calculation uses explicit elapsed-time inputs passed to the
production `reschedulePeriodic` helper, while the surrounding retry and successful
completion tests still exercise the actual callbacks and counter reset. It checks
both window bounds and the next original boundary for missed windows, a valid zero
boundary, custom-flex spacing, and invalid negative/missing boundary recovery.
This avoids requiring the machine to have been running for 37.5 minutes. The old
fixture subtracted that duration from host uptime; on a fresh runner it supplied a
negative original boundary that AOSP correctly replaces with completion time.
[CI run 44158610661, framework job 134341791550](https://github.com/gesher-runtime/gesher/actions/runs/44158610661/job/134341791550)
failed at that assertion on `7f1f089677cacf15f58f2b0d3bac2278dfb7dc77`. An isolated
host-clock reproduction starting at 60 seconds reproduced the same failure and
600,000–900,000 ms recovery window; starting at 30,000,000 ms passed the unchanged
fixture. These clock values are test inputs, not claims about Windows uptime.

The existing scheduler host runner compiles this suite and the work/alarm regression
suite against current production classes. Its host JNI adapter supplies only the
declared identity, allocation, clock and Parcel-write boundaries. Package/process
and permission-definition metadata are explicit fixtures. Scheduler selection and
timer boundaries are driven deterministically; normal production does not receive
those fixture observations. Existing notification and binding fixtures explicitly
construct the executing phase they test.

Framework CI runs the host suites, and Windows track 94 runs the constraint suite
on ART. Host success does not verify Windows display hardware notifications,
suspend timing, cross-process Binder transport or a fresh native Windows build.
Those remain Windows CI/device validation responsibilities; prior-commit CI is
not evidence for the repaired head.
