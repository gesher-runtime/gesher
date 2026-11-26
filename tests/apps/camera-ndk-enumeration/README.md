# Camera NDK enumeration

The signed fixture asks Camera2 and `ACameraManager_getCameraIdList` for the
currently connected cameras. It uses no CAMERA permission and does not open a
camera. It compares the service's ordered IDs with the result obtained in the
Application constructor and again from the Activity. The JNI probe checks
ordinary ELF imports, `dlopen` and `dlsym`, an independent native thread and
snapshots that survive deleting the manager and freeing the other list.

The [Android NDK CameraManager API](https://developer.android.com/ndk/reference/group/camera)
defines the allocation, null argument and snapshot behavior. The NDK list can
omit legacy cameras on Android. Use a Camera2-based reference device when
checking for equality. Compare SDK and NDK IDs within each platform. Hosts with
different connected cameras will have different IDs.

Build with Android SDK 36, Build Tools 36.0.0, an NDK supporting API 24 and JDK
17 or later.

```sh
./build.sh "$ANDROID_SDK_ROOT" "$ANDROID_NDK_ROOT" out out/fixture.p12
adb install out/camera-ndk-enumeration-arm64-v8a.apk
adb shell am start -W -n org.gesher.contracts.camerandk/.EnumerationActivity
adb logcat -d -v tag 'CameraNdkEnumeration:I' '*:S'
```

On Windows use a fresh test profile for each ABI. Install the corresponding APK
with `gesher-app install`, then run
`gesher-app launch org.gesher.contracts.camerandk --headless --keep-task`.
Check for `CameraNdkEnumeration verified` in the launcher output or in the
app log. A host with no cameras must report zero IDs. To exercise storage for
a populated list on a host without hardware, run an additional isolated
profile with `GESHER_CAMERA_TEST=1` and verify at least one ID. Record that
fixture run separately from connected hardware.

The arm64-v8a APK
`5d4862f1ca360dae9c63638ae53b09580e6bac6a3617e4ae79d0d6dca0f28d06`
passed on the Android 16 ARM64 emulator
`BE2A.250530.026.D1/13818094` with one visible camera, ID `1`.
The x86_64 APK SHA-256 is
`4782af292e6c0a4682e5649b90b4f792d7efc9859292608a3d7a4be0e2ef0675`.
Both fixture APKs used signer SHA-256
`1a84f1c899b0eea9f68e6ff1c0b30d88e4de6b1ce4019d02a955e11460b2c5be`.
