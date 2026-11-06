# OsmAnd startup stability, 2026-11-05

APK: `net.osmand-5.4.9.apk`, package `net.osmand`, versionCode `5409`,
target SDK 36. The unmodified installed APK was tested in a separate hidden
ART app process attached to the existing Gesher device. No shared device or
existing app was stopped or restarted. Only the newly created hidden test
processes were ended after their observation window.

## Startup failures found

1. `ExternalSensorsPlugin` creates its BLE callback during
   `OsmandApplication.onCreate`, before checking whether an adapter exists.
   Missing `android.bluetooth.le.ScanCallback` caused `NoClassDefFoundError`.
   Supplying only the callback then failed subclass linking on the missing
   `ScanResult` argument type.
2. The installed system server returned null for
   `getDeviceConfigurationInfo`; `Version.isOpenGlEsVersionSupported` then
   dereferenced `reqGlEsVersion`. The implementation already present in source
   `ActivityTaskManagerService` answers from `DeviceProfile` and
   `ro.opengles.version`. This is an installed-binary/source difference.
3. After reaching MapActivity, `OsmAndDiagnosticThread` called unregistered
   `Debug.getBinderLocalObjectCount` and died with `UnsatisfiedLinkError`.
   The app's main process continued rendering, so a live window alone was not
   treated as complete stability proof.

## Source changes

The eight unmodified AOSP Bluetooth callback/value/helper sources are pinned
to `android-16.0.0_r4`; see
`app/java/android/bluetooth/README.md`. They provide actual result/advertisement
and parcel behavior, not a marker-only `ScanResult`. `BluetoothDevice` supplies
their address/Parcelable/attribution contract. Both adapter entry points remain
null and no BLE hardware feature or scanner is advertised.

`android_runtime.cc` now registers the three existing AOSP Binder object-count
functions from `android_util_Binder.cpp`. Transaction-total methods explicitly
return -1, the same result AOSP returns when the kernel's Binder stats file is
unavailable; Gesher's broker has no such statistics interface.

`FrameworkFixesTest` and track 94 cover callback subclass linking before an
adapter check, real advertisement parsing, device/result parcel round trips,
and Binder local-object growth/release from real flattened Binder objects.

## Observed validation and limits

- All Bluetooth Java sources compile against Gesher's actual compiled
  framework classes without SDK substitute classes.
- The BLE regression ran on the installed ART runtime with an isolated boot
  class path overlay and exited 0. It verifies name/UUID/manufacturer data
  parsing and a non-null device/result parcel round trip as well as subclass
  linking and the absence of an adapter.
- With the BLE overlay and an app-only diagnostic proxy mirroring the existing
  source `getDeviceConfigurationInfo` answer, OsmAnd reached MapActivity,
  presented its first frame, and its native renderer completed a bundled
  basemap. It remained alive for the full 15-second observation. The proxy
  affected only the newly created test app, not the shared server.
- The Binder registration change has Java regression compilation and exact
  AOSP signature/source review. A rebuilt native host and runtime regression
  remain required; no native runtime acceptance is claimed from the old host.
- No installed files were replaced. Normal launches still use the old
  installed runtime until the reviewed changes are built and deployed.

Local evidence and reproducible probe sources are under
`out/osmand-stability/`: `ble-regression.*.log`,
`osmand-source-parity.*.log`, `aosp-source-manifest.json`,
`probe-osmand.ps1`, `BleRegressionRunner.java`, and `OsmandProbeRunner.java`.
