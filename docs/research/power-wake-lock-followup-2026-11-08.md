# Wake-lock callbacks and lifetime — 2026-11-08

This follow-up starts from PR #8 commit
`79567dadaacefd15983e9230bfd80290d6cd52f9` and uses the pinned
`android-16.0.0_r4` framework and power-service sources.

## Confirmed missing public behavior

`PowerManager.WakeLock.setStateListener()` supplies an `IWakeLockCallback` during
acquisition and calls `updateWakeLockCallback()` when its listener changes while
held. Gesher discarded the callback in both acquisition overloads and inherited
the empty update method. Its existing Windows power requests and held-lock map
therefore operated without the listener behavior promised by the public API.

The implementation now retains the callback with the actual held lock and queues
notifications on an ordered daemon executor. Submission follows the serialized
lock lifecycle; application callbacks execute on a separate thread without
holding the service monitor. A direct-executor listener can reenter the service.
Callback delivery failure does not release the independent lock token.

| Operation | Implemented ordinary-lock transition |
| --- | --- |
| First acquisition | Apply the existing Windows power request, then queue `true`. |
| Identical reacquisition | Retain one held record and death recipient; do not duplicate `true`. |
| Attach or replace a listener while held | Queue `true` to the new listener. Do not send `false` to the removed listener. |
| Clear the listener | Stop future listener notifications without releasing the lock. |
| Binder-equivalent callback update | Leave the existing listener unchanged without another event. |
| Actual property change with the same callback | Queue `false`, then `true`, following the pinned Notifier. |
| Release or owner-token death | Remove the held record, update the Windows request and queue `false` to the current listener. |

Two pinned-source details are intentional: identical acquisition does not compare
its callback argument (listener-only changes use the explicit update method),
and Notifier's branch with two nonempty, monitored WorkSources changes accounting
without callback transitions. The implementation preserves these callback rules;
it does not claim to add Android battery attribution/accounting.

Null tokens, invalid acquisition parameters, a dead acquisition token and an
update of a non-held token are rejected with `IllegalArgumentException`. Releasing
an unknown non-null token remains harmless. Changing a held token's package owner
is rejected. A callback's `RemoteException` or local runtime failure is contained
on the dispatch thread and does not discard held state or subsequent deliveries.

References:

- [PowerManager: WakeLock acquisition and setStateListener](https://android.googlesource.com/platform/frameworks/base/+/android-16.0.0_r4/core/java/android/os/PowerManager.java)
- [PowerManagerService: acquisition, callback updates, death and callback identity](https://android.googlesource.com/platform/frameworks/base/+/android-16.0.0_r4/services/core/java/com/android/server/power/PowerManagerService.java)
- [Notifier: changing listeners and asynchronous transition delivery](https://android.googlesource.com/platform/frameworks/base/+/android-16.0.0_r4/services/core/java/com/android/server/power/Notifier.java)

## Real ownership race repaired

Previously, acquisition published its record before registering token death and
then released the service monitor. A concurrent release could retire that record
before registration finished; an older recipient or failed registration could
subsequently remove a newer acquisition by token alone.

New records register their death recipient before publication under the same
lifecycle monitor. Existing records are updated without unlinking/relinking.
Death marks the specific record and removes it only if it is still current.
Death occurring during registration cannot publish a subsequently held lock.
The tests exercise failed registration, synchronous death during registration,
late delivery to an old record, actual token death and listener-token separation.

## Windows backend retry behavior

The existing native backend remains
[KeepAwake](../../compat/devsvc/devsvc_win32.cc), called through
`WindowsHost.keepAwake`. It creates/sets/clears real Windows power requests.

The previous Java code recorded a requested state as applied before checking
the native result, suppressing a later identical retry after failure. Java now
records only successful applications and tracks a pending failed attempt. A
subsequent state application, including an identical reacquisition, retries it.
Release also calls the backend while an attempt remains pending, even if the
last successful state was empty: native failure can retain a partially applied
request handle that still needs cleanup. This cleanup test guards the repair;
it is not presented as an additional failure of the original pre-cached state.

The listener reports the service's enabled state after attempting the backend;
`true` is not proof that every Windows power-policy request was accepted. No
autonomous retry timer, complete power policy, process-freezing policy, full
WorkSource accounting or additional wake-lock hardware capabilities are added.

## Executed evidence and runtime boundary

The focused host harness recompiles the actual production `PowerManagerService`
against verified framework/core/app compilation inputs. Its final run passes
**98 assertions**. Coverage includes FIFO transitions, replacement/removal,
Binder identity, duplicate acquisition, property changes, WorkSource behavior,
async entry points, validation, registration/death ownership, listener failures,
direct reentrancy and failed-native-request retry/cleanup.

The fixture's Binder token controls only death registration/lifetime. The
production held-lock map, callback dispatcher and `WindowsHost` Java boundary
remain real. A separate JNI test adapter observes native request arguments and
injects a failure; it cannot keep a Windows computer awake. Generic framework
initialization adapters are shared with the notification host diagnostic.

```sh
python3 tools/test_power_wake_lock_callbacks.py \
  --java-home /path/to/jdk21 \
  --framework-classes /path/to/framework-classes.zip \
  --core-classes /path/to/coremod \
  --app-classes /path/to/gesher-app-classes.jar
```

Adding `--source-ref 79567dadaacefd15983e9230bfd80290d6cd52f9` compiles the old
service and fails at its first missing acquisition callback, before attempting
any new private-field inspection. The negative control is a behavioral failure.

The separately registered Windows/ART `PowerWakeLockCallbackTest` also exercises
the generated `IPowerManager` Binder/Parcel route and the real public
`PowerManager.WakeLock.setStateListener()` before acquisition and while held,
including replacement, removal and non-reference-counted repeated acquisition.
It uses actual `android_runtime` and Windows power natives. Local host results
do not execute that public-wrapper/Binder path or prove Windows power-policy
acceptance; the full runtime fixture remains a CI gate.
