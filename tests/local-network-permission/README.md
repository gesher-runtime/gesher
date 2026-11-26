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
The target 37 cases are built and awaiting execution. Gesher comparison,
native sockets, HttpEngine, WebView, discovery pickers and inbound traffic
also remain open.

[Android local network permission contract](https://developer.android.com/privacy-and-security/local-network-permission)
