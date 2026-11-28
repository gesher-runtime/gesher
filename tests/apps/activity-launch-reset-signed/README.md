# Signed launcher task reset

This fixture uses the ordinary app launcher to open a manifest root with
clearTaskOnLaunch. The root opens a child. A second launcher activation must
finish that child and resume the existing root in the same task. On Android
an intervening explicit launch without RESET_TASK_IF_NEEDED brings the child
forward and preserves the stack. The Windows runner also opens an explicit
component and a URI, checking the received action, flags and data.

Build once with Android SDK 36, Build Tools 36.0.0 and a JDK. The build signs
the APK with a disposable certificate. Use those same signed bytes on Android
and Windows.

### Android

Use an unlocked portrait Android 16 emulator with the stock Launcher3 app
drawer and no copy of this fixture installed. The runner taps the app icon
using the UI hierarchy and removes the test package on exit.

```sh
bash tests/apps/activity-launch-reset-signed/build.sh "$ANDROID_SDK_ROOT" /tmp/launch-reset
bash tests/apps/activity-launch-reset-signed/run-android.sh "$SERIAL" /tmp/launch-reset/activity-launch-reset-signed.apk
```

### Windows

Use a prepared portable runtime's App directory. The runner creates a private
copy of that runtime and starts the normal package launch command used by the
Start menu entry. It uses an isolated profile and retains its logs.

```powershell
pwsh -NoProfile -File tests/apps/activity-launch-reset-signed/run-windows.ps1 `
  -PortableApp C:\Path\To\Portable\App `
  -Apk C:\Path\To\activity-launch-reset-signed.apk
```

[Android 16 launcher intent flags](https://android.googlesource.com/platform/frameworks/base/+/android-16.0.0_r4/services/core/java/com/android/server/pm/LauncherAppsService.java#2002)
