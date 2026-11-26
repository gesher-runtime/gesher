# Signed service process recovery

The activity auto-binds to a service in a separate process and keeps that
binding while the service terminates itself. A Binder call confirms the
provider PID before termination. The activity must see one normal disconnect
and reconnect to a different provider PID while its own PID stays the same.
It records a failure if it receives `onNullBinding` or `onBindingDied`.

This fixture covers recovery after process death for an existing auto-create
binding. The provider always publishes a Binder. The component contract
fixture in `tests/dex-services` covers genuine null Binder publication and
successful rebinding after `onUnbind`.

Build once with Android SDK 36, Build Tools 36.0.0 and a JDK. The build signs
the APK with a disposable certificate. Use those same bytes on both platforms.

```sh
bash tests/apps/service-rebind-signed/build.sh "$ANDROID_SDK_ROOT" /tmp/service-rebind
bash tests/apps/service-rebind-signed/run-android.sh "$SERIAL" /tmp/service-rebind/service-rebind-signed.apk
```

The Android runner needs an explicit device serial and a device without the
fixture installed. It uninstalls the fixture after the run.

On Windows, pass the `App` directory of a prepared portable Gesher runtime.
The runner copies the runtime into a disposable profile and retains the logs.

```powershell
pwsh -NoProfile -File tests/apps/service-rebind-signed/run-windows.ps1 `
  -PortableApp C:\Path\To\Portable\App `
  -Apk C:\Path\To\service-rebind-signed.apk
```

The check uses the production binding APIs and waits for the actual provider
process to be replaced. [Android 16 service death handling](https://android.googlesource.com/platform/frameworks/base/+/android-16.0.0_r4/services/core/java/com/android/server/am/ActiveServices.java)

The signed APK with SHA-256
`3d3a347af54fc9afea841ab05b0ea9d831504e9af2a8cfc3fac95b21cbb5f3e8`
passed on Android 16 ARM64 (`BE2A.250530.026.D1/13818094`) and in a Windows
11 Parallels portable profile. Each run observed a new provider process, two
connections, one normal disconnection, and the same client process.
The Windows run used ART SHA-256
`2ccdcda2ea088ee73c67fec9b78767e37455b60a6ef364a37c9af4ab623e0ef8`,
a rebuilt Gesher app JAR and matching Binder daemons.
