# S04 symmetric key persistence

The signed API 36 fixture runs three times in distinct processes under one app
installation. The first run generates AES-GCM and HMAC-SHA256 keys. It stores
only an IV, ciphertext and MAC in its test file. The second run checks opaque
key handles and KeyInfo, rejects CBC for the GCM-only key, decrypts the original
message, reproduces its MAC and deletes both aliases. The third run verifies
both aliases are gone after another process restart.

Build with Android SDK 36, Build Tools 36.0.0 and JDK 17 or later. The test key
argument should point to a disposable PKCS#12 file outside the checkout.

```sh
tests/apps/keystore-persistence/build.sh "$ANDROID_HOME" /tmp/keystore-persist.apk /tmp/keystore-persist-test.p12
```

Install the APK once in an isolated Android profile. Launch
`com.gesher.keystorepersist/.PersistenceActivity` from ADB three times, using
`am force-stop com.gesher.keystorepersist` between launches. Wait for
`S04Persist: phase=N pid=P PASS` in the app log before moving to the next phase.
Require phases 0, 1 and 2 from different PIDs. Uninstall the fixture at the end.
On Windows, install the same APK in a fresh Gesher profile, use the managed
launcher, and shut down the test device between phases while retaining its
profile. Read the package's app log for the same phase markers.

On Android 16 ARM64 (`BE2A.250530.026.D1`) and on Windows 11 ARM64 under
Parallels, all three phases passed with distinct PIDs and the same APK
SHA-256 `6f68b1076c5174d99ca168b10508b5530a32f9d39b804e0c5b19ba6ac697f2e1`.
The Windows run used the exact Keystore Java classes from PR #638 at
`a11ed79a8f3e96767968b3dc0f6080449bf415c4` in retained Gesher ART
(`156abd94bbe733e07171931027d5eb3613565dc1`). This verifies software-operation
policy and persistence across sequential starts. Broker isolation, parallel
writers, hardware/credential binding, and full package coverage are separate.
