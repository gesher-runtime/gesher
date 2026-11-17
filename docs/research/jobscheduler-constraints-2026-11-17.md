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
- [JobSchedulerService](https://android.googlesource.com/platform/frameworks/base/+/android-16.0.0_r4/apex/jobscheduler/service/java/com/android/server/job/JobSchedulerService.java):
  a network constraint requires `ACCESS_NETWORK_STATE` for target SDK 34 onward.
  Missing `RECEIVE_BOOT_COMPLETED` for a persisted job produces
  `IllegalArgumentException`. User-initiated job permission is independent of
  optional compatibility settings.
- [BatteryController](https://android.googlesource.com/platform/frameworks/base/+/android-16.0.0_r4/apex/jobscheduler/service/java/com/android/server/job/controllers/BatteryController.java):
  ordinary charging jobs require stable power (charging and battery not low).
  Top-app jobs at normal priority or higher retain the connected-power exemption
  during that execution. AC power does not erase a battery-not-low requirement.
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

## Validation and limits

`JobSchedulerConstraintContractTest` enters production `Run.started`, receives the
actual constructed JobParameters and acknowledges the actual callback. It covers
unavailable observations, actual netId handover, cellular versus metered Wi-Fi,
custom requests, periodic/one-shot deadlines, pending reasons, power/storage/idle
stop reasons, acknowledgement ordering, callback failures, binding/start/stop timeout
outcomes, retry/backoff and permission admission.

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
