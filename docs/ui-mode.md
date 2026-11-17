# Android night mode and Windows theme preferences

`UiModeManagerService` implements Android's night resource configuration on the
normal, single-user Gesher device. The policy authority is Android 16 r4,
frameworks/base commit `45034f0663f960d9ee5fb0a101a4732b71f6e2f4`:

- [UiModeManagerService](https://android.googlesource.com/platform/frameworks/base/+/45034f0663f960d9ee5fb0a101a4732b71f6e2f4/services/core/java/com/android/server/UiModeManagerService.java): global mode, custom schedule, permission checks, manual overrides, inactive-device deferral, attention overlay, and callback meanings.
- [PackageConfigurationUpdaterImpl](https://android.googlesource.com/platform/frameworks/base/+/45034f0663f960d9ee5fb0a101a4732b71f6e2f4/services/core/java/com/android/server/wm/PackageConfigurationUpdaterImpl.java): per-package configuration chosen from the authenticated calling process, including activities hosted in a shared process.
- [TwilightService](https://android.googlesource.com/platform/frameworks/base/+/45034f0663f960d9ee5fb0a101a4732b71f6e2f4/services/core/java/com/android/server/twilight/TwilightService.java) and [TimeUtils](https://android.googlesource.com/platform/frameworks/base/+/45034f0663f960d9ee5fb0a101a4732b71f6e2f4/core/java/android/util/TimeUtils.java): astronomical AUTO and local-clock custom intervals.
- [IUiModeManagerCallback](https://android.googlesource.com/platform/frameworks/base/+/45034f0663f960d9ee5fb0a101a4732b71f6e2f4/core/java/android/app/IUiModeManagerCallback.aidl): contrast and accessibility inversion callbacks are separate from night resources.

## State and scope

`getNightMode()` reports the selected device policy. Configuration resources carry
the applied night bit, which can differ because of an automatic transition waiting
for display-off, a manual override, battery saver, or an application override.

Before an Android selection is saved, a successfully observed Windows
`HKCU\Software\Microsoft\Windows\CurrentVersion\Themes\Personalize\AppsUseLightTheme`
value supplies the desktop user's light/dark preference. An explicit Android
selection takes precedence thereafter. The native registry call returns `-1` for
unknown/unreadable data and retains the last policy; it neither launches `reg.exe`
nor guesses a light result. There are no public synthetic host-input hooks.

The modes retain their Android meanings:

| Mode | Behavior |
| --- | --- |
| `MODE_NIGHT_NO`, `MODE_NIGHT_YES` | Apply explicit light/dark resource configuration immediately. |
| `MODE_NIGHT_AUTO` | Use AOSP's sunrise/sunset calculation with a real Windows location. Missing location retains the last computed value; normal service shutdown saves it for restart. Windows theme selection is not an astronomical input. |
| `MODE_NIGHT_CUSTOM` / schedule | Use the local clock and time zone; default 22:00–06:00. Times are microseconds since midnight, including microsecond precision, and Android's inclusive interval endpoints are retained. The worker evaluates at the first observable millisecond at or after the start and strictly after the end, including when initialized at the exact end. |
| `MODE_NIGHT_CUSTOM` / bedtime | Use the last authorized bedtime activation request. A mismatched subtype returns false but still remembers the bedtime request, as AOSP does. |

Automatic twilight/schedule changes wait for the observed Windows session display
to turn off. `GUID_SESSION_DISPLAY_STATUS` comes from the existing hidden event
window; dimmed remains interactive and an unavailable observation remains unknown.
Explicit activation, bedtime changes and real battery-saver changes apply without
that wait. Windows time, settings, resume, power and display events refresh policy;
one cancellable timer tracks the next boundary, with a bounded hourly refresh for
changes in real location. Events are coalesced and unchanged snapshots do not
rewrite the settings file. Native registry and event observation use no per-call
processes and leak no registry/notification handles.

`setApplicationNightMode()` changes only the package belonging to the authenticated
calling PID and matching UID. A shared UID does not merge package preferences.
Application AUTO/CUSTOM clears the override to inherit the device policy; it does
not change the device's mode. Device preferences are written atomically under
`$ANDROID_DATA/system/ui-mode.properties`. Each application row is bound to PM's
durable installation marker, so an update preserves it and a later installation
cannot inherit an old row. The isolated app driver keeps its settings in its own
app data directory (`no_backup/gesher-ui-mode.properties`) and does not write the
running device's policy. Failed explicit policy writes do not publish a new mode,
override, attention-clear side effect or matched bedtime request. An authorized
bedtime request for an unselected subtype is still remembered, as AOSP requires.

Attention overlays and ordinary computed AUTO/CUSTOM transitions publish their
configuration without requiring unrelated policy-file writes. A derived reset of
a manual override attempts to save the reset, logs a failed write, and retries on
the next update while retaining the current resource configuration. Explicit mode,
schedule, application and manual-override selections retain atomic durable writes.
Like AOSP's shutdown path, the JVM shutdown hook saves AUTO's last computed value;
service close and replacement do the same before releasing the old service. A
failed shutdown write is logged and cleanup still completes. Restart can only
recover the last successfully saved cache; forced process termination does not
run the shutdown hook. Attention and bedtime requests themselves are transient.

## Permissions, lifecycle and capabilities

An unlocked Android night mode allows `setNightMode()` without inventing an extra
permission. Custom-type and attention-overlay access require
`MODIFY_DAY_NIGHT_MODE`; activation without that permission returns false. Invalid
mode values throw `IllegalArgumentException`. Invalid custom times are ignored.
Cross-user callback/contrast/inversion reads enforce the Android incoming-user
permission boundary. An ordinary app in the isolated driver receives no privileged
permission merely because its Binder call stays in the same process.

Every applied configuration uses the shared configuration sequence. Ready
processes receive `ConfigurationChangeItem`; activities declaring `uiMode` receive
`ActivityConfigurationChangeItem`, and unhandled changes use
`ActivityRelaunchItem` with saved state and the preserved window. Stopped
activities defer their update until resumed. Package overrides also reach matching
activities in another package's process, without changing that process's own
configuration. The isolated driver's existing saved-state relaunch path now handles
global configuration changes as well as window changes, deferring a stopped
activity's update until immediately before its next resume.

This device stays in `UI_MODE_TYPE_NORMAL`, with UI type locked; no car/dock mode
is advertised. Default contrast is zero and accessibility force-invert stays off.
Night-resource changes never impersonate contrast or accessibility-inversion
changes and do not call those callbacks. Callback registrations use Binder death
tracking and deduplication; service shutdown removes host/package listeners,
unlinks callbacks and cancels scheduled work.

## Validation and boundaries

`tools/test_ui_mode.py` runs the production service, PackageManager and ATM with
constructed package metadata, explicit instance-owned host inputs and the existing
host JNI identity/initialization boundary. Assertions cover permissions and error
results, scope across packages/shared UIDs, configuration sequence and lifecycle
transaction selection, automatic deferral, manual override reset, schedules and
time zones, actual scheduled-worker boundary transitions, transient publication
with unavailable policy storage, persistent restart, stale-installation rows,
explicit write failures, derived override retry, callback isolation and shutdown.
The host runner also checks AUTO persistence after a child JVM exits through the
production shutdown hook. The solar calculation additionally matches 7,200
results from the unchanged upstream ICU implementation byte for byte at the
millisecond result level; see `tests/ui-mode/aosp/README.md` for its exact provenance.

The native registry adapter also executes against a declared Win32 registry
boundary, covering missing, denied, malformed and known values without Windows
execution. The framework CI runs these host contracts against freshly built app classes.
Windows track 94 runs `UiModeContractTest` through `ServiceFixtureRunner` and a
generated `IUiModeManager` Binder proxy, including Parcel validation. The explicit
fixture inputs are test-only and do not substitute for actual Win32 theme/display
notifications or real Windows/ART rendering, which require their Windows runtime.
