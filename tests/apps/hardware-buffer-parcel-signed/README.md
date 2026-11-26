# Signed HardwareBuffer Binder contract

The activity sends an ImageReader buffer with a real two-color Canvas frame
to an unexported service in a different app process. Before the transaction
it closes the hardware buffer, image, and reader. The service reads the
buffer from the Parcel, checks its description, and returns its ID, two
sampled colors, and its process ID. The sender checks the exact values,
distinct processes, and the fields around the descriptor.

Build with the Android 36 SDK and a disposable fixture signer.

```sh
bash tests/apps/hardware-buffer-parcel-signed/build.sh "$ANDROID_HOME" /tmp/hardware-parcel-contract
```

On Android 16, install the resulting APK and run this component.

```sh
adb install /tmp/hardware-parcel-contract/hardware-buffer-parcel-signed.apk
adb shell am start -W -n gesher.contract.hardwareparcel/.ProbeActivity
adb logcat -d -s HardwareParcelBinder:I
```

On Windows, use a fresh Gesher profile and matching runtime, Binder daemon
and service manager. Install the same APK with `gesher-app install` and run
`gesher-app launch gesher.contract.hardwareparcel --headless --keep-task`.
Look for `HardwareParcelBinder: PASS sender=... receiver=... id=...` in the
app log and verify that the process IDs differ.

Signed APK SHA-256
`89e4711025b1687ea5aa2e84097ba869f73887214c61225e185001b86357b5a1`
passed on Android 16 ARM64 (`BE2A.250530.026.D1/13818094`) and Windows 11
Parallels with ART SHA-256
`f69369af820d35cc583d2e63f696179a89469884d10dbae5b5d4c3bbf339ac06`.
The test covers a Canvas-backed RGBA ImageReader buffer. Other formats and
independent GPU writers have their own transfer requirements.
