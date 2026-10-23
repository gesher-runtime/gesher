# gesher-app: the app driver

`gesher-app <app.apk>` starts an Android app and drives its launcher
Activity through attach, create, start and resume on a main `Looper`,
headless, then exits with code 0. It is the driver of
[0012](../docs/decisions/0012-headless-framework.md) and step 7 of
[the framework natives plan](../docs/framework-natives.md). It does what
Ravenwood's `RavenwoodAppDriver` and `RavenwoodActivityDriver`
(`frameworks/base/ravenwood/junit-impl-src/android/app`) do for host tests,
on Android's own runtime and framework.jar instead of a JVM and hoststubgen.

```
dalvikvm -Xbootclasspath:core-oj.jar:core-libart.jar:core-icu4j.jar:framework.jar:gesher-app.jar \
  -Xint -Xusejit:false com.gesher.app.Main [options] app.apk
```

Options: `--build-prop FILE` (default `$ANDROID_ROOT/build.prop`),
`--framework-res FILE` (default `$ANDROID_ROOT/framework/framework-res.apk`),
`--activity CLASS` (default: the MAIN/LAUNCHER activity), `--data-dir DIR`
(default `$ANDROID_DATA/data/<package>`), `--strict-services` (see below).
On Windows, `tools/package/gesher-app.cmd` sets up the directories and the
boot class path and runs it.

## Where it runs from

`gesher-app.jar` goes on the **boot class path, after framework.jar**. The
driver's core, `android.app.GesherAppDriver`, is in `android.app` for
ActivityThread's package-private API (`AppBindData`, `LoadedApk`'s
constructor, `ContextImpl.createActivityContext`, `Instrumentation.basicInit`,
`Activity.attach` and `perform*`), as Ravenwood's drivers are; package access
needs the same class loader as framework.jar. Being on the boot class path
also exempts its reflection (a few private fields) from hidden-API checks.
It goes after framework.jar because it also carries placeholders for classes
framework.jar lacks (below), which must never shadow real ones.

## What it does

`com.gesher.app.Main`, on the main thread:

0. `Digests.install()`: the JCA providers that exist on Gesher's boot class
   path (libcore's list names Conscrypt and BouncyCastle, which are not
   there yet), and SHA-1, SHA-256 and MD5 digests, which the framework needs
   from its first steps.
1. `System.loadLibrary("android_runtime")`: the framework's natives, a builtin
   library on Windows.
2. Loads the build.prop (`app/build.prop`, android-16.0.0_r4's
   `BP4A.251205.006`, SDK 36) into `SystemProperties`, before anything
   initializes `android.os.Build`.
3. `Looper.prepareMainLooper()`.
4. `AssetManager.createSystemAssetsInZygoteLocked(false, <framework-res.apk>)`:
   the system assets, from `$ANDROID_ROOT`'s copy. AssetManager's own path,
   `/system/framework/framework-res.apk`, is a path on the current drive on
   Windows ([0010](../docs/decisions/0010-paths.md)); creating the system
   AssetManager first makes `Resources.getSystem()` and every app
   AssetManager use this one.
5. Reads the APK's binary manifest with `ApkAssets.openXml`
   (`AppManifest`): package, `uses-sdk`, the `<application>`'s class, label,
   theme and flags, and each `<activity>`'s name, label, theme and intent
   filters. It builds the `ApplicationInfo`, `ActivityInfo`s and
   `PackageInfo` PackageManagerService would. (Splits, shared libraries,
   permissions, providers, services, receivers, aliases and meta-data are not
   read; `ParsingPackageUtils` can replace it later.)
   The APK's native libraries, `lib/x86_64/*.so`, are copied into
   `ApplicationInfo.nativeLibraryDir` (`<data-dir>/lib`, `NativeLibs`), the
   app class loader's library path, as the package manager does for
   `extractNativeLibs`; dalvikvm loads them with Gesher's ELF loader
   ([native-libs.md](../docs/native-libs.md)).
6. Installs the stand-in services (below) through
   `ServiceManager.initServiceCache`.
7. Posts the launch to the main Looper and runs `Looper.loop()`. The main
   Looper may not quit, so the launch ends by posting a message that throws
   out of `Looper.loop()`: the process exits once the messages it left behind
   have run, as with `quitSafely()`. Any exception, in the launch or in those
   messages, exits with code 1.

`android.app.GesherAppDriver.launch` is a scaled-down
`ActivityThread.handleBindApplication` and `performLaunchActivity`, as
`RavenwoodAppDriver`'s constructor and `createResumedActivity` are:

- **Process:** `new ActivityThread()`, made current
  (`sCurrentActivityThread`) with a `ConfigurationController`, as `attach()`
  does; `VMRuntime.setTargetSdkVersion`; an `AppBindData`
  (`mBoundApplication`), so `currentPackageName()` and friends answer; the
  configuration from `DeviceProfile` applied to the `ConfigurationController`
  and `ResourcesManager`; the default `LocaleList`.
- **Package:** a `LoadedApk` for the APK. Its class loader is a
  `PathClassLoader` over the APK whose parent is the boot class loader, set
  into the LoadedApk directly: LoadedApk's own path (`ApplicationLoaders`,
  `ClassLoaderFactory`) also creates a linker namespace for the app's native
  libraries with the native `createClassloaderNamespace`, which Gesher does
  not register yet. The LoadedApk is put in `mPackages`.
- **Application:** a plain `Instrumentation` (`basicInit`), then
  `LoadedApk.makeApplicationInner` (the app's `ContextImpl` and its
  `Application` class from the manifest) and
  `Instrumentation.callApplicationOnCreate`.
- **Activity:** an Intent for `ACTION_MAIN`/`CATEGORY_LAUNCHER`, a `Binder`
  token, `ContextImpl.createActivityContext`, `Instrumentation.newActivity`,
  the title from the label resource, `Activity.attach` (the 20-argument form
  `performLaunchActivity` uses), `setTheme` with the activity's (or the
  application's) theme, `callActivityOnCreate`, `performStart`,
  `callActivityOnPostCreate`, `performResume`. Not `makeVisible`: no window
  is added while headless, and the decor view is never created.

It prints `gesher-app: <Activity> attach/onCreate/onStart/onResume`, and the
Application's `onCreate`, on stdout.

Not done (yet), compared with a device: no `ActivityClientRecord` in
`mActivities` (so `ActivityThread.getActivity(token)` returns null), no
`ApplicationThread` binding, no content providers, no instrumentation
package, no pause/stop/destroy, no `performTopResumedActivityChanged`.

## The stand-in services

`StandInServices` registers in-process services under the names an app looks
up, the way `RavenwoodSystemServer` does. Each is a subclass of the AIDL
interface's generated `IFoo.Default` that overrides what the lifecycle calls,
so javac checks every override against the interface. `StandIn` puts a
`java.lang.reflect.Proxy` in front of it and attaches the proxy to a local
`Binder` under the interface's descriptor, so `IFoo.Stub.asInterface(binder)`
returns it with no Parcel round trip. A call to a method the stand-in does not
override is logged once on stderr
(`gesher-app: stand-in IFoo.bar not implemented; returning a default`) and
gets `Default`'s answer (null, 0, false); with `--strict-services` (or
`GESHER_APP_STRICT_SERVICES=1`) it throws instead. The log is the list of
what to implement next.

| Name | Interface | Answers |
|---|---|---|
| `activity` | `IActivityManager` | `getContentProvider` → null (no providers: Settings and friends fall back to defaults), `checkPermission` → granted, `getCurrentUser` → user 0 |
| `activity_task` | `IActivityTaskManager` | `getActivityClientController` → a stand-in `IActivityClientController` (`getDisplayId` → 0, `getTaskForActivity` → 1) |
| `package` | `IPackageManager` | `getPackageInfo`, `getApplicationInfo`, `getActivityInfo` for the app's package (from the manifest); `hasSystemFeature` → false; `getNameForUid`, `getPackagesForUid` → the app |
| `window` | `IWindowManager` | `openSession` → a stand-in `IWindowSession` (all defaults); display sizes; no navigation bar; animator scales 1 |
| `display` | `IDisplayManager` | one display (`DeviceProfile`: 1080×2400, 420 dpi, 60 Hz, on); callbacks accepted and never called |
| `user` | `IUserManager` | user 0, the system user, running and unlocked |

`ServiceManager` asks `IServiceManager` for any name not in its cache.
Without a binder driver there is no servicemanager to ask, so a stand-in
`IServiceManager` (knowing the services above, and any `addService`d later)
replaces `ServiceManager.sServiceManager`; other names get null, as on a
device for a service that does not exist.

## Classes framework.jar does not have

`SystemServiceRegistry`'s static initializer calls each mainline module's
`*FrameworkInitializer.registerServiceWrappers()` (or `initialize()`) and
names a few classes from outside framework.jar as class literals.
framework.jar is compiled against those modules' API stubs
([framework-build.md](../docs/framework-build.md)) and does not contain them,
so without them the initializer, and with it every `getSystemService`, would
fail. `app/compat-java` has a no-op initializer for each module not in
framework.jar (Connectivity, Wi-Fi, Bluetooth, NFC, AppSearch, Health,
Stats, Media, Role, Scheduling, SDK sandbox, AdServices, UWB, Safety Center,
Nearby, OnDevicePersonalization, OnDeviceIntelligence, DeviceLock,
Virtualization, ConfigInfrastructure, EnhancedConfirmation, Profiling,
Ranging, and the telephony module's `TelephonyServicesInitializer`) and
empty placeholders for `LocationManager`, `CountryDetector` and
`TetheringManager`. Those services are simply not registered. When
framework.jar (or a module jar ahead of gesher-app.jar on the boot class path)
brings the real class, it wins.

One placeholder has behaviour: `android.provider.DeviceConfig`, through which
framework.jar reads its server-side flags (autofill reads them on resume). It
keeps flags in the process: nothing sets one, so each read answers the
caller's default; `setProperty` sets one and tells the namespace's listeners.

## Building

```
app/build.sh <out> --framework-classes <framework classes dir|jar> [--core-classes <libcore java.base dir>]
tests/apps/lifecycle/build.sh <out> --framework-res <framework-res.apk> \
  --framework-classes <dir|jar> [--core-classes <dir>]
```

`app/build.sh` writes `gesher-app.jar` (one `classes.dex`) and `build.prop`;
the test app's script writes `lifecycle.apk` (aapt2 against
framework-res.apk, javac, d8, zipalign; unsigned, as gesher-app does not
verify signatures). Both download d8 9.5.26 (or use `R8_JAR`); the test app's
also downloads build-tools r36.1 for aapt2 and zipalign (or uses
`BUILD_TOOLS_DIR`).

## The test app

`tests/apps/lifecycle` is one launcher Activity, `LifecycleActivity`, and an
`Application` class, both named in the manifest. Each callback prints a line;
on resume the Activity prints its title (from `@string/activity_label`), a
string resource, an attribute set by its own theme (`gesherMarker`) and one
inherited from the theme's framework parent
(`Theme.Material.Light.NoActionBar`'s `windowActionBar`):

```
LifecycleApplication onCreate, package=com.gesher.lifecycle, app_name=Gesher Lifecycle
gesher-app: com.gesher.lifecycle.LifecycleApplication onCreate
gesher-app: com.gesher.lifecycle.LifecycleActivity attach
LifecycleActivity onCreate, savedInstanceState=null
gesher-app: com.gesher.lifecycle.LifecycleActivity onCreate
LifecycleActivity onStart
gesher-app: com.gesher.lifecycle.LifecycleActivity onStart
LifecycleActivity onPostCreate
LifecycleActivity onResume, label=Lifecycle Activity, greeting=Hello from a resource, theme attr=from-theme, windowActionBar=false
gesher-app: com.gesher.lifecycle.LifecycleActivity onResume
gesher-app: done
```
