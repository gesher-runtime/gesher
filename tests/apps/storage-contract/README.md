# App storage and dynamic DEX across process restart

This signed test APK runs the same public Android operations on Android 16 and
Gesher. It opens an app's default credential-encrypted context and an explicit
device-protected context after user unlock. Both use the same file, preference
and SQLite database names. The test checks separation immediately after writes
and persistence when a different process opens the same app data.

Each process also loads a fixture-only class from a separately compiled DEX
archive and from memory through the public
[DexClassLoader](https://developer.android.com/reference/dalvik/system/DexClassLoader)
and [InMemoryDexClassLoader](https://developer.android.com/reference/dalvik/system/InMemoryDexClassLoader)
APIs. The fixture checks parent isolation, class identity, independent static
state, explicit direct-buffer bounds and rejection of a malformed DEX. The
archive is made read-only before its data is written to follow the
[Android 14+ dynamic-code rule](https://developer.android.com/about/versions/14/behavior-changes-14#safer-dynamic-code-loading).

The Android contract is documented by
[Direct Boot](https://developer.android.com/privacy-and-security/direct-boot)
and [Context.createDeviceProtectedStorageContext](https://developer.android.com/reference/android/content/Context#createDeviceProtectedStorageContext()).
An independent writable copy exercises the target SDK boundary of the same
rule. Android 16 is expected to execute the owned fixture archive at target 33
and reject it with `SecurityException` at target 36. The same signed variant
must be used on both platforms for each comparison.

This fixture covers F48, F50, K09 and T02 in [issue #56](https://github.com/gesher-runtime/gesher/issues/56).
A locked-user boot, user unlock transition, migration and multi-user storage
require separate cases.

## Build

Run from this directory with JDK 17+, Python 3, Android SDK Platform 36 and
Build Tools 36.0.0.

```sh
mkdir -p out
./build.sh "$ANDROID_SDK_ROOT" out/storage.apk out/test.p12 36
./build.sh "$ANDROID_SDK_ROOT" out/storage-target33.apk out/test.p12 33
python3 -m unittest -v
```

Use the same signed APK on the two platforms. Record the printed signer
SHA-256, the APK SHA-256 from each platform's staged file and the Android
fingerprint, SDK and ABI. Record the Windows host architecture and the
Gesher release or source revision plus runtime component hashes. Keep the
fixture key and transient captures outside version control.

## Android 16 reference

Run on a disposable emulator or a dedicated test device that contains no
installation of this fixture. Select the device explicitly with `ANDROID_SERIAL`
when more than one is connected. Use a clean install, unlock the user, and wait
for the tagged seed record before force-stopping the package and relaunching.
Capture the two tagged records from logcat using the `tag` output format.

```sh
adb shell getprop ro.build.fingerprint
adb shell getprop ro.build.version.sdk
adb shell getprop ro.product.cpu.abilist
adb push out/storage.apk /data/local/tmp/gesher-storage-contract.apk
adb shell sha256sum /data/local/tmp/gesher-storage-contract.apk
adb shell pm install /data/local/tmp/gesher-storage-contract.apk
adb logcat -T 1 -v tag 'GesherStorageContract:I' '*:S' >out/android.log &
logcat_pid=$!
adb shell am start -W -n org.gesher.contracts.storage/.StorageActivity
# Wait for the tagged seed row to appear in out/android.log.
adb shell am force-stop org.gesher.contracts.storage
adb shell am start -W -n org.gesher.contracts.storage/.StorageActivity
# Wait for the tagged restart row to appear in out/android.log.
kill "$logcat_pid"
```

## Windows / Gesher

Use an empty, dedicated Gesher test profile and a local APK path on the same
drive as the runtime. Use the managed launcher twice with the same profile.
Shut down only that isolated test device between launches so the second result
must come from a new app process. Save only lines starting with
`GesherStorageContract ` from launcher/app stdout for both launches, in sequence.
Look at the profile's app log if startup fails or the launcher omits stdout.
Use the normal launcher and no injected test service. Do not reuse an existing
user profile. The `GESHER_PROFILE` override is available in builds supporting
the portable profile work.

```powershell
$env:GESHER_PROFILE = 'C:\test\storage-new-profile'
Get-FileHash C:\test\storage.apk -Algorithm SHA256
gesher-app install C:\test\storage.apk
gesher-app launch org.gesher.contracts.storage --headless --keep-task
gesher-app shutdown
gesher-app launch org.gesher.contracts.storage --headless --keep-task
gesher-app shutdown
```

Compare independent captures with the matching lower-case APK digests.

```sh
python3 compare.py --android out/android.log --windows out/windows.log \
    --android-apk-sha256 "$ANDROID_APK_SHA256" --windows-apk-sha256 "$WINDOWS_APK_SHA256"
```

Repeat from a clean install and a fresh profile for the target 33 variant,
passing `--target-sdk 33` and its own APK digests to the comparator.

The comparator requires every storage and class-loader check in both phases and verifies that the
startup IDs differ while the storage token persists within each platform.
Retain an unsuccessful startup, blocked result or missing phase as a test
failure to complete. Keep launch-only evidence outside the functional result.

## Recorded comparison

Both platforms executed the same separately signed APK for each target. The
reference was the Android 16 ARM64 system image, API 36 and fingerprint
`Android/sdk_phone64_arm64/emu64a:16/BE2A.250530.026.D1/13818094:userdebug/test-keys`.
Windows was Windows 11 Pro 24H2 ARM64 under Parallels. Gesher's x64 Windows
runtime ran through Windows emulation, in an isolated portable profile with the
managed launcher. The runtime is 0.2.0.3123 from [package run 45668397258](https://github.com/gesher-runtime/gesher/actions/runs/45668397258)
at [156abd94](https://github.com/gesher-runtime/gesher/commit/156abd94bbe733e07171931027d5eb3613565dc1).
Its portable ZIP SHA-256 was
`3efa411cd6538ebca1cbe477a8f97ea8ca31ba1f66676c13906c83fb15fe6c15`.
The shipped `dalvikvm.exe` SHA-256 was
`f8db9f613eb58312998fe9cdfea9b59813e97a0b89906c82da4aa0a5b7c88f35`.

| Target SDK | APK SHA-256 confirmed on both guests | Paired observations |
| --- | --- | --- |
| 33 | `b6ff4bc3039537c54bd39d51a9682b32bad18bb0605bb8f17ba89ae61d8c7289` | 34/34 passed. Both ran the writable test archive. |
| 36 | `763602fdf1c993646be1a05bf127c80e837e4beaf8c6840eebf63127f93c8f01` | 32/34 passed. Android rejected the writable test archive at startup and after restart. Gesher loaded its class both times. |

Both APKs used the test certificate SHA-256
`136b5f0535eec269566e79a2a0bf0bba916258d0bc18ab7dcebf9d6175ea9953`.
The 32 matching checks in each profile cover separate CE/DE data and dynamic
DEX behavior under read-only file and memory loaders. The Windows failure is
scoped to the retained runtime. No exact current-main Windows build or
standalone-Gesher process was run. The Android ART check for read-only DEX is
compiled behind Android target handling. Windows needs equivalent admission
for app-written DEX while ordinary installation and launch continue to use
authorized base and split APKs.
