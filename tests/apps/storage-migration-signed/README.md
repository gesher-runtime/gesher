# Signed CE/DE storage migration

This fixture runs in an unlocked profile with distinct credential-protected
and device-protected contexts. It populates both SharedPreferences caches
and creates SQLite databases of the same name with different values in both
areas. The first process moves the credential-protected data to the
device-protected area, replacing its previous values and removing the source.
The second independently checks the persisted values and moves them back.
The third independently verifies the result. The runners require distinct
process IDs and a single persistent fixture ID across all three phases.

Build once with Android SDK 36, Build Tools 36.0.0 and a JDK. Use the same
signed bytes on both platforms.

```sh
bash tests/apps/storage-migration-signed/build.sh "$ANDROID_SDK_ROOT" /tmp/storage-migration
bash tests/apps/storage-migration-signed/run-android.sh "$SERIAL" /tmp/storage-migration/storage-migration-signed.apk
```

The Android runner requires an explicit device serial and a device without
the fixture package installed. It uninstalls only that fixture on exit.

On Windows, the runner copies a prepared portable Gesher runtime's App
directory into a disposable profile and retains the logs.

```powershell
pwsh -NoProfile -File tests/apps/storage-migration-signed/run-windows.ps1 \`
  -PortableApp C:\Path\To\Portable\App \`
  -Apk C:\Path\To\storage-migration-signed.apk
```

The fixture calls the public migration APIs with all database handles closed.
Its results cover an unlocked profile and the app's own storage areas.
[Android 16 ContextImpl migration](https://android.googlesource.com/platform/frameworks/base/+/refs/tags/android-16.0.0_r4/core/java/android/app/ContextImpl.java#719)
