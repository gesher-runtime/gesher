# A08 local network permission reference

The signed fixture checks Java TCP and UDP from an ordinary application UID
against a test peer. It sends four known bytes and requires the expected
four-byte reply. The peer binds to the test host's loopback interface and
the disposable Android emulator connects through its `10.0.2.2` host alias.

Build with the official Android 37.0 SDK, Build Tools 36.0.0, a JDK and Python 3.
Start the peer in one terminal. Run the reference on a named emulator from
another terminal after it finishes booting.

```sh
tests/local-network-permission/build.sh "$ANDROID_HOME" /tmp/gesher-lan
python3 tests/local-network-permission/peer.py 43781
tests/local-network-permission/run-android.sh emulator-5554 /tmp/gesher-lan 43781 baseline
```

For Android 16, enable the documented compatibility change on the fixture
package, reboot the test emulator and wait for its boot to finish. The runner
restores the package's default compatibility state on exit.

```sh
adb -s emulator-5554 shell am compat enable RESTRICT_LOCAL_NETWORK org.gesher.contracts.lan.t36
adb -s emulator-5554 reboot
# Wait until adb shell getprop sys.boot_completed reports 1.
tests/local-network-permission/run-android.sh emulator-5554 /tmp/gesher-lan 43781 api36-opt-in
```

For an Android 17 emulator, run the two target variants with the same peer.

```sh
tests/local-network-permission/run-android.sh emulator-5556 /tmp/gesher-lan 43781 api37
```

## Observed Android 16 reference

On `Android/sdk_phone64_arm64/emu64a:16/BE2A.250530.026.D1/13818094:userdebug/test-keys`,
target 36 exchanged bytes over both transports with the default compatibility
state. With opt-in enabled, two transports matched in each of three stages.

| Stage | NEARBY_WIFI_DEVICES | TCP | UDP |
| --- | --- | --- | --- |
| Initial denial | Denied | Connect timed out | Send returned EPERM |
| Grant | Granted | Expected reply | Expected reply |
| Revocation | Denied | Connect timed out | Send returned EPERM |

The opt-in run used APK SHA-256
`7094422aec52d412029f00d637f67541948fb1a442e9627046198dca998603f6`.
The API 37 AOSP emulator image r06 failed to finish booting on the test Mac.
The Android target 37 reference is awaiting execution.

## Observed Gesher baseline on Windows

Gesher portable 0.2.0.3123 (source `156abd94bbe733e07171931027d5eb3613565dc1`)
ran both signed APKs on Windows 11 ARM64 build 26300 under Parallels. The
same peer ran inside the Windows guest and bound TCP and UDP to one ephemeral
loopback port. The authenticated host shell delivered the endpoint as Intent
extras and granted and revoked the requested permission in a fresh profile.
Each APK checked its own permission before opening new Java sockets.

| Target SDK | Permission manipulated | Initially denied | Granted | Revoked |
| --- | --- | --- | --- | --- |
| 36 | `NEARBY_WIFI_DEVICES` | TCP and UDP echo | TCP and UDP echo | TCP and UDP echo |
| 37 | `ACCESS_LOCAL_NETWORK` | TCP and UDP echo | TCP and UDP echo | TCP and UDP echo |

Permission checks reported `-1`, `0`, `-1` for the requested permission in
both runs. The peer received six TCP and six UDP requests in total and the app
verified every reply. The target 36 APK matched the Android reference SHA-256
above. The target 37 APK SHA-256 was
`0c4caa9d27c706c4acaef6f12135c8126976f8f352784170cb9659268e396a9d`.

This Windows baseline uses Gesher's own policy and the Windows guest's
loopback path. The opt-in Android 16 case used the emulator's host-side peer
alias. The results establish the observed permission and byte-transfer behavior
on each profile. Enforcement against a common LAN destination, existing sockets,
NDK callers, HttpEngine, WebView, discovery pickers and inbound traffic remain
open.

[Android local network permission contract](https://developer.android.com/privacy-and-security/local-network-permission)
