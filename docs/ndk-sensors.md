# NDK sensors over the Windows sensor hub

This implements the static sensor inventory and ordinary event queues in H01 of
[issue #56](https://github.com/gesher-runtime/gesher/issues/56). It does not close
that roadmap row. Android comparison and Windows physical-device evidence are
separate qualification requirements.

Previously `libandroid.cc` always returned an empty NDK sensor list and dummy
event queues even when the shared Windows sensor hub exposed a real device to
`SystemSensorManager`. Native applications could therefore neither discover nor
read that sensor. `sensors_ndk.cc` now exposes the same process-lived inventory
and samples through the existing `compat/devices/sensors.cc` backend. Ordinary
use never enables a synthetic source. The pre-existing `GESHER_SENSOR_TEST`
switch is used explicitly by the Windows regression fixture only.

## Observable contracts

| Area | Implemented behavior |
| --- | --- |
| Inventory | Stable sensor/list pointers, real hub names, vendor, type, resolution and reporting mode; absent default sensors return null. A sensorless machine remains sensorless. |
| Event ABI | The 104-byte LP64 `ASensorEvent` layout used by x86_64 and arm64, boot-time nanosecond timestamps from the hub, zero reserved fields, byte-sized vector accuracy, and intact rotation-vector quaternion W. |
| Registration | A 200 ms default; explicit periods and maximum batching latency validation; physical minimum periods; registration maximum-period clamp; errors from the actual hub propagated. Setting a rate never enables a disabled sensor. |
| Queue | A bounded 256-event stream, oldest samples dropped when full, negative error on empty read, level-triggered eventfd readiness retained through partial drains, and a retained Looper until destruction. |
| Callbacks | Guest System V entry through the same loaded-code TLS boundary as `ALooper_addFd`; callback data preserved; returning zero removes the watch; destroying one's own queue is safe. |
| Lifecycle | Inactive authenticated apps may register, but receive no samples until active. Visible activities and foreground services use the existing UID activity model. An observed policy generation change clears queued data/readiness; unchanged on-change sensors produce a fresh reading after reactivation. |
| Rate permission | For capped sensor types, target SDK 31+ without `HIGH_SAMPLING_RATE_SENSORS` gets a 5 ms minimum; a debuggable app's faster request fails with permission denied. Sensor metadata reports the effective permission minimum. Application-supplied manager package strings never choose the caller's identity. |
| Unsupported features | No dynamic devices, wake-up behavior, hardware FIFO, or direct channels are invented. Additional-info selection is accepted without generating unsupported events. |

The current hub supports only the accelerometer, magnetic field, gyroscope,
light, and rotation-vector sources that Windows reports. It has no hardware
FIFO, so streaming immediately satisfies a maximum batching latency without
claiming batching capacity. It does not implement trigger sensors or direct
channels; those remain separate roadmap work. There is no implemented device
microphone-privacy control to connect in this round. The access snapshot reserves
and enforces its rate cap when supplied, but this is not a claim that such a
control is available.

## Policy and ownership

`SensorAccessService` authenticates the calling Binder UID/PID against the
existing installed-app/process admission record. It derives target SDK,
debuggability and the declared high-rate permission from that record. The private
transport returns only the caller's policy. It does not expose an unrestricted
`isUidActive` API or trust a package name passed to an NDK manager function.

A foreground launch becomes eligible after its launch/resume transaction is
successfully enqueued, without waiting for the application's `onResume` to
return. The server requires the current admitted process and target code, a
visible top activity selected for resume, and no native/chooser cover or pending
pause handoff. A per-attempt dispatch result prevents a failed retry from
inheriting a previous launch's eligibility. The server publishes the recomputed
state immediately after dispatch, so a subscription opened during startup can
receive the subsequent activation. Cancellation, hidden tasks and retired owners
remain ineligible; a stopped acknowledgement publishes revocation. A successful
re-resume also publishes without waiting for its acknowledgement, while failed
enqueue restores the preceding state. Existing visible paused activities retain
their foreground classification.

The Java client subscribes once per authenticated process generation and receives
ordered policy notifications. The server enforces that single policy subscription;
individual NDK event queues keep their own registration and lifetime.
Connection loss revokes access; stale connection/generation messages cannot
revive it. Native policy flags and generation share one atomic word. The sampler
does not call JNI or Binder per event, and reads/drains do not perform IPC.
Initial connection establishment occurs outside the queue and sampler locks.
On service loss a queue signals readiness so a blocked reader can observe
`-EPIPE`. This is readiness for an error, not a sensor sample or a claim that the
eventfd itself has a socket hangup. Recovery clears that signal and old samples.

The registry retains live queues while APIs use them. Destruction removes the
public handle, stops the shared sampler's callbacks, and removes the Looper
watch. Looper callback adapters retain the queue only while dispatching, which
allows self-destruction without a cycle. API mutations serialize separately from
the event buffer; hub operations are never called while its buffer mutex is
held. The sampler's new delivery epoch defaults to one for the existing Java
sink, preserving its ordinary behavior.

## Android contract sources

The implementation was checked against these primary sources:

- [Android 16 NDK sensor entry points](https://android.googlesource.com/platform/frameworks/base/+/android-16.0.0_r4/native/android/sensor.cpp): argument validation, metadata sentinels, queue API and additional-info selection.
- [Android 16 sensor header](https://android.googlesource.com/platform/frameworks/native/+/android-16.0.0_r4/include/android/sensor.h): ABI, reporting modes and capability definitions.
- [Android 16 SensorEventQueue](https://android.googlesource.com/platform/frameworks/native/+/android-16.0.0_r4/libs/sensor/SensorEventQueue.cpp): default registration and read semantics.
- [Android 16 SensorService](https://android.googlesource.com/platform/frameworks/native/+/android-16.0.0_r4/services/sensorservice/SensorService.cpp): `getSensorList`, `enable`, `setEventRate`, `hasSensorAccessLocked`, `UidPolicy`, and high-rate/microphone-policy checks.
- [Android 16 ActivityTaskSupervisor](https://android.googlesource.com/platform/frameworks/base/+/android-16.0.0_r4/services/core/java/com/android/server/wm/ActivityTaskSupervisor.java): `realStartActivityLocked` updates the selected activity and process state after launch dispatch without awaiting a client resume acknowledgement.
- [Android 16 ResumeActivityItem](https://android.googlesource.com/platform/frameworks/base/+/android-16.0.0_r4/core/java/android/app/servertransaction/ResumeActivityItem.java): the client sends `activityResumed` from `postExecute`, after `handleResumeActivity` returns.
- [SensorEventConnection at 3f678a9](https://android.googlesource.com/platform/frameworks/native/+/3f678a903819c7aae6a757bda4076d6f43f04927/services/sensorservice/SensorEventConnection.cpp): permission adjustment precedes the service's hardware period clamp for both registration and rate changes.
- [Public NDK sensor reference](https://developer.android.com/ndk/reference/group/sensor): supported API and application-visible contracts.

The code uses Gesher's own queue and Windows integration; these references do not
imply that the complete Android sensor service or HAL has been ported.

## Validation and limits

`tools/test_ndk_sensor_contracts.py --sanitize` links the actual production bridge
and exported entry points. Only the sensor supplier, policy transport and
Android Looper boundary are controlled. Real Linux eventfds and polling exercise
partial-drain readiness, queue overflow, concurrent producer/destruction,
callback return/self-destruction, event packing, backend errors, policy epochs,
rate decisions and sensorless behavior. `--source-ref 8b316aebf09ab46d5407e955bf86ed748a1e739f`
runs the same inventory assertions against the original production sensor
bodies: the two discovery assertions fail, establishing the previous gap.

`tools/test_sensor_flush_contracts.py --sanitize` exercises the actual shared
sampler, including the new on-change reactivation rule. Existing Java-style
sinks retain their default epoch. `tools/test_sensor_access.py` exercises the
actual server policy and native atomics, with optional full server/client host
checks described in [the sensor policy test notes](../tests/sensors/README.md).
The immutable framework CI recipe already invokes the checked-out
`tools/test_appops.py` with the app classes it builds. That driver preserves its
original checks, then runs the full sensor policy suite with the same five
inputs; any child failure fails the existing framework gate. This does not rely
on editing the pinned reusable workflow. The native2 job also runs `tools/test_sensor_access_jni.py`
against a real host JVM and the authenticated NDK's JNI header, checking native
thread attachment ownership, pending exceptions and a missing runtime class.

`tests/apps/ndksensors` builds an APK for each guest ABI using the NDK's real
headers. `tests/windows/tracks/44-native2.ps1` runs both in standalone and device
mode with the explicit test supplier. It checks first use on a native pthread,
the real event layout and clock, identifier readiness, callback TLS, return-zero
removal and self-destruction through the production ART/guest boundary. The APKs
are produced by the existing `native2` CI artifact workflow. A compiled APK is
not evidence that these Windows executions passed; their exact-head CI results
must be reviewed separately.

The guest APK is non-debuggable and checks that a release app's fast sampling
request succeeds before restoring the queue-test period. Its explicit test
accelerometer has a 10 ms hardware minimum, so this fixture cannot independently
establish the 5 ms permission cap. The production host bridge tests separately
cover that cap, effective metadata, and permission denial for debuggable apps.
The actual server/controller host suite also verifies that the authenticated
package's debug flag reaches its sensor policy alongside the rate restriction.

The guest intentionally waits for its native worker inside `onResume`; this
retains the device-mode regression for activation before the resume
acknowledgement. The host controller suite also executes the real launch method
with a recording client that never acknowledges resume. It checks existing
subscription activation, failed enqueue/retry, cancellation during dispatch,
visibility and ownership exclusions, and stop/re-resume publication. Those host
checks exercise server ordering; remote Binder delivery still requires the
unchanged Windows guest tests on both ABIs.

Local Linux host tests, the Windows object cross-build against the repository's
patched AOSP Looper headers, and NDK compilation establish bounded pieces of the
implementation. They do not establish WinRT hardware behavior, actual Android
device parity, or translated guest execution. Physical device discovery, rate
accuracy, orientation/unit calibration, suspend/resume and privacy-control
integration remain qualification work for H01/H02.

At the initial audit, open PR #107 modified `libandroid.cc` symbol-provider
registration and PR #190 added performance-hint exports; neither changed the
sensor implementation. This bridge is kept in a separate compilation unit to
keep that integration surface small.
