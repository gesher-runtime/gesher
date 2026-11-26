# Signed activity restoration after process death

This fixture runs on Android 16 and Gesher Windows. The parent saves a Bundle
and view state after displaying a child activity in another process. The test
terminates the stopped parent and lets the still-live child return a result.
The original task must recreate the parent under a new PID, restore its saved
text and deliver that result once. The release file is a rendezvous used only
by this fixture.

Build once with an Android 36 SDK, Build Tools 36.0.0 and JDK 21 or newer. The
build signs the APK with a disposable certificate. Use those same signed bytes
in both runs.

```sh
bash tests/apps/activity-restore-signed/build.sh "$ANDROID_SDK_ROOT" /tmp/restore
bash tests/apps/activity-restore-signed/run-android.sh "$SERIAL" /tmp/restore/activity-restore-signed.apk
```

The Android runner expects a disposable debug-capable emulator with no copy
of this fixture installed. It verifies the two exact process names and kills
the parent through the fixture's own UID. It removes the package after the run.

On Windows, use the path to a prepared portable runtime's `App` directory.
The runner copies it and uses a fresh profile. Before terminating the parent,
it verifies that both reported PIDs belong to the test runtime copy. It leaves
its test work directory and logs to support diagnosis.

```powershell
pwsh -NoProfile -File tests/apps/activity-restore-signed/run-windows.ps1 `
  -PortableApp C:\Path\To\Portable\App `
  -Apk C:\Path\To\activity-restore-signed.apk
```

This covers an actual stopped activity, process death and pending result with
a plain Bundle and view hierarchy. Locked-user behavior, disk-backed task
history and AndroidX SavedStateHandle require separate fixtures.
