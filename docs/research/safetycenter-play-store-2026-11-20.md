# android.safetycenter and the Play Store, 2026-11-20

The Play Store (54.1.11) died whenever any package was installed while its main
process ran: its dynamically registered `PACKAGE_ADDED`/`PACKAGE_REMOVED` receiver
resolves `SafetyCenterManager` (`Context.getSystemService(SafetyCenterManager.class)`,
then `isSafetyCenterEnabled()` when the manager is non-null), and the class was
absent:

```
java.lang.NoClassDefFoundError: Failed resolution of: Landroid/safetycenter/SafetyCenterManager;
  ... at aodj.onReceive  (Play's package-change receiver)
System.exit called, status: 1
```

After an install from the Store the Store disappeared and its "open when installed"
step never ran. Installing any APK from the command line while the Store was open
reproduced it.

`android.safetycenter` is a System API of the Permission module
(framework-permission-s), a boot class path jar on Android 13+. framework.jar
compiles against the module's stubs but does not contain it.

## What Gesher supplies

- The module's value classes, from `packages/modules/Permission` at
  android-16.0.0_r4 (`framework-s/java/android/safetycenter`, including `config/`),
  in gesher-app.jar. They are unchanged apart from dropping the compile-time
  `@RequiresApi` annotations and using the framework's jarjar'd `SdkLevel`
  (`android.internal.modules.utils.build`), as the Wi-Fi ports do.
- `SafetyCenterManager` with the SDK 36 API surface (constants, listener
  interface, methods). Gesher runs no Safety Center service and
  `SafetyCenterFrameworkInitializer` registers no service wrapper, so
  `getSystemService` returns `null`, as `SystemServiceRegistry` does on a device
  whose service is not published. The Store checks for `null` and skips Safety
  Center reporting. No manager instance is created and no Safety Center state is
  reported.

## Validation

- Before: with the Store open, `gesher-launch install lifeapp.apk` gave
  `PACKAGE_ADDED` and then `process (com.android.vending) exited with code 1`, with the
  NoClassDefFoundError above.
- After (gesher-app.jar swapped into the installed Gesher): the Store's
  receiver logged `DTU: Received onPackageAdded` for an install made from the Store
  (Firefox) and for a later install, and its process kept running.
- `SafetyCenterContractTest` (track 96), on real ART with the boot jars: the classes
  load from the boot class path, `getSystemService` returns `null` by class and by
  name, and SafetyEvent, SafetySourceStatus, SafetySourceData and
  SafetySourceErrorDetails build, validate and round-trip through Parcel. Against
  the previous gesher-app.jar it fails with the same NoClassDefFoundError.

## Other missing SDK classes

The installed apps' dex files were compared with the public and system API lists
(`api/current.txt`, `api/system-current.txt`) of frameworks/base, libcore, ICU,
conscrypt and the boot-class-path modules. Classes missing from the boot jars that
the installed apps reference include `android.app.role.RoleManager` (Play Store,
Gmail, Firefox), `android.app.appsearch.*` (Play Store, Gmail, Google),
`android.os.flagging.AconfigPackage`, `android.os.ProfilingManager`,
`android.adservices.*`, `android.health.connect.*`, `android.net.wifi.p2p.*`,
`android.media.MediaParser`, `android.media.ApplicationMediaCapabilities`,
`android.nfc.tech.*` and `android.scheduling.RebootReadinessManager`. Each needs its
own contract decision. They are not part of this change.
