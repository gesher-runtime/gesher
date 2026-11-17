# Desktop app crash notices

An uncaught Java failure reaching `GesherAppProcess.main` or the standalone
`Main.main` is still logged and exits with status 1. If the process has a shown
application window, `AppCrash` writes a UTF-8 text crash report and starts
`gesher-launch --app-crash PACKAGE KIND REPORT` as an independent host dialog.
The dying Android process does not wait for dismissal. This also works after
Explorer's original detached launcher has exited. Hidden/headless runs and
background-only processes do not open dialogs; `GESHER_CRASH_DIALOGS=0` explicitly
suppresses them. Failure to write or start a notice leaves the original failure
and exit path intact.

The helper uses Gesher's host language (English/Hebrew, including RTL flags),
identifies the package, and displays the path of its text report. It accepts
only known diagnosis kinds and never starts PowerShell or relaunches the app.
Application/path arguments are quoted as Windows arguments and substituted once
into localized templates, so braces and Unicode paths stay literal.

## Component audit and diagnosis limits

| Failure/component | Notice | Evidence required |
|---|---|---|
| Bluetooth | No adapter; connect/enable one in Windows and retry | ART's null-receiver exception explicitly names `BluetoothAdapter`, plus a real Windows observation no older than 10 seconds with `ERROR_DEVICE_NOT_CONNECTED` (`0x8007048f`). A missing JNI backend, failed activation, stale cache or an available/off adapter cannot produce this diagnosis. The crash path reads the cache only; it never starts an activation or hardware probe. |
| Security/permissions (including Bluetooth, camera and location operations) | An operation failed a security/permission check; check permissions and the log | An uncaught `SecurityException` in the cause chain. The notice does not claim a particular permission is missing or grant it. |
| Class/method/native-library compatibility | A linkage error; include the log in a report | An uncaught `LinkageError` in the cause chain. This is a diagnostic hint, not a claim that Gesher is necessarily responsible. |
| Camera, microphone, USB, sensors, location and other failures | Generic failure with text report | A null or exception alone cannot prove that a host device is missing. Camera access may instead fail because a device is busy or permission was denied; a missing USB service differs from a missing USB device; an absent sensor can be normal Android behavior. Do not invent device diagnoses from such failures. |

No Android capability, adapter result, callback, permission, application exception
handler or process exit status is changed. In particular, the documented
[`BluetoothAdapter.getDefaultAdapter()`](https://developer.android.com/reference/android/bluetooth/BluetoothAdapter#getDefaultAdapter())
null result without Bluetooth is preserved. There are no package-name exceptions.
Native faults, app-owned exception handlers that terminate the process themselves,
and crashes before any app window is shown are not covered by this Java entry-point
notice. Existing launch-failure reporting remains in place.

## Validation

`python tools/test_app_crash_diagnostics.py` compiles the actual classifier and
reporter against explicit host boundary stand-ins. It covers real failure shapes,
wrapped causes, cycles, unrelated nulls, missing natives, a failed reporter,
background suppression and the explicit opt-out. The separate cache test compiles
the production `WindowsBluetooth` and exercises freshness and invalidation.
`--launcher PATH` additionally opens, inspects and closes all four actual Win32
dialog variants in both languages, including Unicode paths/literal braces and
rejected helper arguments.

`python tools/test_app_crash_runtime.py --runtime RUNTIME --fixture crashnotice.apk
--output OUTPUT` runs on Windows without a Bluetooth radio. The isolated fixture
calls the production Android Bluetooth API after drawing a real application
window. It verifies the original null-receiver exception, exit 1, a readable text
report, the Hebrew Bluetooth notice surviving ART process exit, and explicit
suppression. It uses standalone Android services and a separate profile. It does
not establish multi-process device-mode behavior, native crash handling, or JBL
headphone connectivity. It never uses the synthetic Bluetooth backend.
