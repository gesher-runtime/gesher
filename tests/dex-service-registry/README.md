# A03 live Binder registry census

The probe calls the live Android service manager from an isolated Gesher host
shell. It enumerates published names, obtains each handle and asks for its
Binder interface descriptor. The Android reference uses the system's
`adb shell service list`, which reports the corresponding fields.

Build with the compiled framework classes and R8 used for the app boot JAR.

```sh
R8_JAR=/path/to/r8.jar tests/dex-service-registry/build.sh /path/to/framework-classes.jar /tmp/a03-registry
adb -s emulator-5570 shell service list
```

In a disposable Gesher Windows profile, start the device with `gesher-app
start`. Set `GESHER_BINDER` to the namespace in that profile's
`data/system/gesher-device` file and `GESHER_BINDER_UID=2000` for the host shell.
Use the runtime's normal Android root/data/ICU paths and five boot JARs. Invoke
`dalvikvm -Xint -Xusejit:false -cp /path/to/a03-registry.dex RegistryProbe`,
with an explicit `-Xbootclasspath` as used by the other DEX fixtures. Stop the
test device with `gesher-app shutdown`. Use an independent profile for the
other `GESHER_MICROG_SPOOF` setting.

## Observed comparison

Android 16 ARM64 `BE2A.250530.026.D1` published 304 names. Windows 11 ARM64
under Parallels ran the retained native runtime at `156abd94` and the grouped
app boot JAR donor with SHA-256
`ffdf3c90b30d7f2847aa9efd5138632efb89bc557ff363a7189eedf52129b0a8`.
It published 67 names under each setting, with zero missing handles. The sets
and descriptors were identical across the two Windows profiles. They shared
62 names with Android, of which 61 descriptors matched. Android registered
`android.net.IConnectivityManager` under `connectivity` and Gesher published
`com.gesher.net.IConnectivity`. The five extra names belonged to Gesher.
One of them, `gesher.camera_source`, returned an empty interface descriptor.

These counts measure publication and interface identity in the tested shell.
They give no pass result for service methods, third-party provider setup, JNI
registration, callback delivery or app-level permissions.
