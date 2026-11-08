/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package gesher.services;

import android.app.ActivityThread;
import android.content.ComponentName;
import android.content.Context;
import android.content.pm.IPackageManager;
import android.content.pm.IDexModuleRegisterCallback;
import android.content.pm.PackageInfo;
import android.content.pm.PackageManager;
import android.content.pm.PackageManager.Property;
import android.content.pm.VersionedPackage;
import android.content.res.AssetManager;
import android.hardware.display.IDisplayManager;
import android.os.Binder;
import android.os.IBinder;
import android.os.IInterface;
import android.os.IServiceManager;
import android.os.Looper;
import android.os.Parcel;
import android.os.RemoteException;
import android.os.ServiceManager;
import android.permission.IPermissionManager;

import com.android.internal.os.ApplicationSharedMemory;
import com.gesher.server.Services;

import java.io.File;
import java.lang.reflect.Constructor;
import java.lang.reflect.Field;
import java.lang.reflect.Method;
import java.nio.file.Files;
import java.nio.file.StandardCopyOption;
import java.util.Arrays;
import java.util.Collection;
import java.util.Collections;
import java.util.List;
import java.util.Map;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.Executor;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicReference;

/** Real manifest -> production service -> AIDL Parcel -> ApplicationPackageManager contracts. */
public final class PackageQueryContractTest {
    private static final String PACKAGE = "com.gesher.components";
    private static final String PROPERTY = PACKAGE + ".CONTRACT_PROPERTY";
    private static int checks;

    private interface Call { void run() throws Exception; }

    private static void require(boolean condition, String message) {
        if (!condition) throw new AssertionError(message);
        ++checks;
    }

    private static void missing(Call call, String message) throws Exception {
        try { call.run(); throw new AssertionError(message); }
        catch (PackageManager.NameNotFoundException expected) { ++checks; }
    }

    private static void nullInput(Call call, String message) throws Exception {
        try { call.run(); throw new AssertionError(message); }
        catch (NullPointerException expected) { ++checks; }
    }

    private static Method method(Class<?> type, String name, Class<?>... params) throws Exception {
        Method method = type.getDeclaredMethod(name, params);
        method.setAccessible(true);
        return method;
    }

    private static void property(Property value, String className, int kind) {
        require(value != null && PROPERTY.equals(value.getName())
                && PACKAGE.equals(value.getPackageName())
                && java.util.Objects.equals(className, value.getClassName()),
                "Property name/package/component changed across the service boundary");
        switch (kind) {
            case PackageManager.TYPE_APPLICATION:
                require(value.isInteger() && value.getInteger() == 73, "Application integer lost");
                break;
            case PackageManager.TYPE_ACTIVITY:
                require(value.isString() && "activity-value".equals(value.getString()),
                        "Activity string lost or replaced by application property");
                break;
            case PackageManager.TYPE_PROVIDER:
                require(value.isBoolean() && value.getBoolean(), "Provider boolean lost");
                break;
            case PackageManager.TYPE_RECEIVER:
                require(value.isFloat() && value.getFloat() == 1.5f, "Receiver float lost");
                break;
            case PackageManager.TYPE_SERVICE:
                require(value.isResourceId() && value.getResourceId() != 0,
                        "Service resource ID was resolved as a string or discarded");
                break;
            default: throw new AssertionError("Unknown fixture kind");
        }
    }

    private static void packageInfo(PackageInfo actual, PackageInfo expected, boolean components) {
        require(actual != null && expected.packageName.equals(actual.packageName)
                && expected.getLongVersionCode() == actual.getLongVersionCode(),
                "Versioned query lost the installed package/version");
        require(actual.applicationInfo.uid == expected.applicationInfo.uid
                && expected.applicationInfo.sourceDir.equals(actual.applicationInfo.sourceDir),
                "Versioned query lost installed identity/paths");
        require(components ? actual.activities != null && actual.activities.length > 0
                : actual.activities == null, "Versioned query did not preserve GET_ACTIVITIES");
    }

    private static void dexRegistration(IPackageManager wire, IPackageManager service, String path)
            throws Exception {
        CountDownLatch callbackDone = new CountDownLatch(1);
        AtomicInteger calls = new AtomicInteger();
        AtomicReference<String> failure = new AtomicReference<>();
        Thread caller = Thread.currentThread();
        IDexModuleRegisterCallback.Stub target = new IDexModuleRegisterCallback.Stub() {
            @Override public void onDexModuleRegistered(String actualPath, boolean success, String message) {
                calls.incrementAndGet();
                try {
                    if (!path.equals(actualPath) || success
                            || !"registerDexModule call not supported since Android U".equals(message)) {
                        failure.set("Callback lost request identity or reported unsupported registration as success");
                    } else if (Thread.currentThread() == caller || Thread.holdsLock(service)) {
                        failure.set("Callback ran inline or held the package monitor");
                    } else if (Binder.getCallingUid() != android.os.Process.myUid()
                            || wire.getPackageGids(PACKAGE, 0, 0).length != 0) {
                        failure.set("Callback identity or reentrant package query failed");
                    }
                } catch (Exception error) {
                    failure.set(error.toString());
                } finally {
                    callbackDone.countDown();
                }
            }
        };
        // Force the callback through its generated AIDL proxy/stub too.
        IDexModuleRegisterCallback callback = IDexModuleRegisterCallback.Stub.asInterface(new Binder() {
            @Override protected boolean onTransact(int code, Parcel request, Parcel reply, int flags)
                    throws RemoteException {
                return target.asBinder().transact(code, request, reply, flags);
            }
        });
        wire.registerDexModule(PACKAGE, path, true, callback);
        require(callbackDone.await(5, TimeUnit.SECONDS), "DEX registration callback never completed");
        CountDownLatch drained = new CountDownLatch(1);
        Field executor = service.getClass().getDeclaredField("mPackageEventExecutor");
        executor.setAccessible(true);
        ((Executor) executor.get(service)).execute(drained::countDown);
        require(drained.await(5, TimeUnit.SECONDS) && calls.get() == 1 && failure.get() == null,
                "DEX callback contract failed: " + failure.get());
    }

    public static void main(String[] args) throws Exception {
        require(args.length == 1, "Pass the current components.apk fixture");
        System.loadLibrary("android_runtime");
        // This VM owns the server side of package-cache invalidation, just as
        // SystemServer does before publishing installation and query services.
        ApplicationSharedMemory.setInstance(ApplicationSharedMemory.create());
        require(ApplicationSharedMemory.getInstance().isMutable(),
                "Package fixture did not acquire writable native cache memory");
        AssetManager.createSystemAssetsInZygoteLocked(false,
                System.getenv("ANDROID_ROOT") + "/framework/framework-res.apk");
        if (Looper.myLooper() == null) Looper.prepareMainLooper();
        File data = Files.createTempDirectory("gesher-package-query-").toFile();
        android.system.Os.setenv("ANDROID_DATA", data.getPath(), true);
        // This isolated metadata test neither launches an app nor uses persistent identities.
        android.system.Os.setenv("GESHER_MICROG_SPOOF", "0", true);
        File apkFile = new File(data, "base.apk");
        Files.copy(new File(args[0]).toPath(), apkFile.toPath(), StandardCopyOption.REPLACE_EXISTING);

        Class<?> apkClass = Class.forName("com.gesher.server.ApkPackage");
        Object apk = apkClass.getMethod("parse", String.class).invoke(null, apkFile.getPath());
        require(PACKAGE.equals(apkClass.getField("packageName").get(apk)), "Wrong fixture package");
        Class<?> pmClass = Class.forName("com.gesher.server.PackageManagerService");
        IPackageManager service = (IPackageManager) pmClass.getConstructor(Collection.class)
                .newInstance(Collections.singletonList(apk));
        Class<?> standInClass = Class.forName("com.gesher.server.StandIn");
        standInClass.getMethod("setStrict", boolean.class).invoke(null, true);
        Object standIn = standInClass.getMethod("of", Class.class, IInterface.class)
                .invoke(null, IPackageManager.class, service);
        IBinder target = (IBinder) standInClass.getMethod("asBinder").invoke(standIn);
        // No attached local interface: exercise the generated proxy/stub and both
        // Parcel directions, without needing a shared external Binder device.
        AtomicInteger transactions = new AtomicInteger();
        IPackageManager wire = IPackageManager.Stub.asInterface(new Binder() {
            @Override protected boolean onTransact(int code, Parcel request, Parcel reply, int flags)
                    throws RemoteException {
                transactions.incrementAndGet();
                return target.transact(code, request, reply, flags);
            }
        });
        // ContextImpl obtains its display through the real framework manager.
        // Publish the production display service before constructing that context,
        // as bindApplication's cache does; no external servicemanager is involved.
        Object display = standInClass.getMethod("of", Class.class, IInterface.class)
                .invoke(null, IDisplayManager.class, new Services.DisplayManager());
        Object permissions = standInClass.getMethod("of", Class.class, IInterface.class)
                .invoke(null, IPermissionManager.class,
                        pmClass.getMethod("permissions").invoke(service));
        IBinder permissionTarget = (IBinder) standInClass.getMethod("asBinder").invoke(permissions);
        AtomicInteger permissionTransactions = new AtomicInteger();
        IBinder permissionWire = new Binder() {
            @Override protected boolean onTransact(int code, Parcel request, Parcel reply, int flags)
                    throws RemoteException {
                permissionTransactions.incrementAndGet();
                return permissionTarget.transact(code, request, reply, flags);
            }
        };
        Map<String, IBinder> services = Map.of("package", wire.asBinder(),
                Context.DISPLAY_SERVICE,
                (IBinder) standInClass.getMethod("asBinder").invoke(display),
                "permissionmgr", permissionWire);
        // PermissionManager also asks for the optional legacy permission manager.
        // Use the production in-process service registry so an absent service has
        // the normal ServiceNotFoundException contract instead of opening /dev/binder.
        Class<?> serviceManagerClass = Class.forName("com.gesher.server.Services$ServiceManagerStandIn");
        Constructor<?> registryConstructor = serviceManagerClass.getDeclaredConstructor(Map.class);
        registryConstructor.setAccessible(true);
        Object registry = standInClass.getMethod("of", Class.class, IInterface.class)
                .invoke(null, IServiceManager.class, registryConstructor.newInstance(services));
        Field serviceManager = ServiceManager.class.getDeclaredField("sServiceManager");
        serviceManager.setAccessible(true);
        serviceManager.set(null, standInClass.getMethod("asInterface").invoke(registry));
        ServiceManager.initServiceCache(services);
        Field field = ActivityThread.class.getDeclaredField("sPackageManager");
        field.setAccessible(true);
        field.set(null, wire);
        Constructor<ActivityThread> constructor = ActivityThread.class.getDeclaredConstructor();
        constructor.setAccessible(true);
        Context context = constructor.newInstance().getSystemContext();
        PackageManager client = context.getPackageManager();
        require(context.getDisplay() != null
                && "android.app.ApplicationPackageManager".equals(client.getClass().getName()),
                "Fixture did not construct the real context, display and public package manager");

        int beforeVisibility = transactions.get();
        require(client.canPackageQuery(PACKAGE, PACKAGE),
                "Public self-package visibility query crashed or returned false");
        require(transactions.get() == beforeVisibility + 1,
                "Public visibility query bypassed the generated package Binder proxy");
        require(Arrays.equals(client.canPackageQuery(PACKAGE,
                new String[]{"android", PACKAGE, "android", PACKAGE}),
                new boolean[]{true, true, true, true}),
                "Bulk visibility query lost installed identities, duplicate positions or array shape");
        require(client.canPackageQuery(PACKAGE, new String[0]).length == 0,
                "Empty visibility query returned null");
        missing(() -> client.canPackageQuery("fixture.not.installed", PACKAGE),
                "Missing visibility source did not produce public NameNotFoundException");
        missing(() -> client.canPackageQuery(PACKAGE, "fixture.not.installed"),
                "Missing visibility target did not produce public NameNotFoundException");
        missing(() -> client.canPackageQuery("fixture.not.installed", new String[0]),
                "Empty visibility query bypassed source validation");
        missing(() -> client.canPackageQuery(PACKAGE, new String[]{PACKAGE, null}),
                "Null bulk target did not preserve the typed package lookup failure");
        nullInput(() -> client.canPackageQuery(null, PACKAGE), "Public null visibility source accepted");
        nullInput(() -> client.canPackageQuery(PACKAGE, (String)null), "Public null visibility target accepted");
        nullInput(() -> client.canPackageQuery(PACKAGE, (String[])null), "Public null visibility array accepted");
        require(Arrays.equals(wire.canPackageQuery(null, new String[]{null, "fixture.absent"}, 10),
                new boolean[2]), "Absent-user visibility query did not preserve result shape and ordering");

        int flags = PackageManager.GET_ACTIVITIES | PackageManager.MATCH_DIRECT_BOOT_AWARE
                | PackageManager.MATCH_DIRECT_BOOT_UNAWARE;
        PackageManager.invalidatePackageInfoCache();
        int beforeQuery = transactions.get();
        PackageInfo expected = client.getPackageInfo(PACKAGE, flags);
        require(transactions.get() == beforeQuery + 1,
                "First public package query did not cross its generated Binder proxy");
        packageInfo(client.getPackageInfo(PACKAGE, flags), expected, true);
        require(transactions.get() == beforeQuery + 1,
                "Initialized native nonce did not permit a real framework cache hit");
        PackageManager.invalidatePackageInfoCache();
        packageInfo(client.getPackageInfo(PACKAGE, flags), expected, true);
        require(transactions.get() == beforeQuery + 2,
                "Native cache invalidation did not force a fresh Binder query");
        packageInfo(client.getPackageInfo(new VersionedPackage(PACKAGE,
                PackageManager.VERSION_CODE_HIGHEST), flags), expected, true);
        packageInfo(client.getPackageInfo(new VersionedPackage(PACKAGE,
                expected.getLongVersionCode()), PackageManager.PackageInfoFlags.of(flags)), expected, true);
        // The requested version selects static libraries on AOSP, not ordinary apps.
        packageInfo(client.getPackageInfo(new VersionedPackage(PACKAGE,
                expected.getLongVersionCode() + 1), 0), expected, false);
        missing(() -> client.getPackageInfo(new VersionedPackage("fixture.not.installed",
                PackageManager.VERSION_CODE_HIGHEST), 0), "Unknown package appeared installed");
        require(wire.getPackageInfoVersioned(new VersionedPackage(PACKAGE,
                PackageManager.VERSION_CODE_HIGHEST), 0, 10) == null,
                "Versioned query escaped the single-user package scope");

        require(client.getPackageGids(PACKAGE).length == 0
                && client.getPackageGids(PACKAGE, 0).length == 0
                && client.getPackageGids(PACKAGE, PackageManager.PackageInfoFlags.of(0)).length == 0,
                "Installed package's absent supplementary groups became NameNotFoundException");
        require(client.getPackageGids("android").length == 0,
                "The platform package became unknown to supplementary-group queries");
        missing(() -> client.getPackageGids("fixture.not.installed"),
                "Unknown package acquired a supplementary-group inventory");
        require(wire.getPackageGids(PACKAGE, 0, 10) == null,
                "Supplementary-group query escaped the single-user package scope");

        int uid = expected.applicationInfo.uid;
        require(Arrays.equals(client.getNamesForUids(new int[]{uid, -1,
                android.os.Process.SYSTEM_UID, uid}),
                new String[]{PACKAGE, null, "shared:android.uid.system", PACKAGE}),
                "Bulk UID names lost installed, unknown, shared-system or duplicate positions");
        require(client.getNamesForUids(new int[0]) == null
                && wire.getNamesForUids(null) == null,
                "Bulk UID names changed Android's absent-input contract");
        String heldPermission = "android.permission.FOREGROUND_SERVICE";
        require(client.checkPermission(heldPermission, PACKAGE) == PackageManager.PERMISSION_GRANTED,
                "Fixture's normal permission was not granted");
        require(permissionTransactions.get() > 0,
                "Public permission query bypassed the generated PermissionPolicy Binder proxy");
        List<PackageInfo> holders = client.getPackagesHoldingPermissions(
                new String[]{"fixture.absent.permission", heldPermission}, 0);
        require(holders.size() == 1 && PACKAGE.equals(holders.get(0).packageName)
                && Arrays.equals(holders.get(0).requestedPermissions, new String[]{heldPermission})
                && holders.get(0).requestedPermissionsFlags == null,
                "Permission-holder query lost the matching subset or fabricated grant metadata");
        holders = client.getPackagesHoldingPermissions(new String[]{heldPermission},
                PackageManager.PackageInfoFlags.of(PackageManager.GET_PERMISSIONS | flags));
        require(holders.size() == 1 && holders.get(0).requestedPermissions.length == 2
                && Arrays.asList(holders.get(0).requestedPermissions).contains(
                        "android.permission.FOREGROUND_SERVICE_SPECIAL_USE")
                && holders.get(0).requestedPermissionsFlags.length == 2
                && (holders.get(0).requestedPermissionsFlags[0] & PackageInfo.REQUESTED_PERMISSION_GRANTED) != 0
                && holders.get(0).activities != null,
                "GET_PERMISSIONS did not retain complete manifest/grant/component metadata");
        holders = client.getPackagesHoldingPermissions(new String[]{heldPermission,
                "fixture.absent.permission", heldPermission}, 0);
        require(holders.size() == 1 && Arrays.equals(holders.get(0).requestedPermissions,
                new String[]{heldPermission, heldPermission}),
                "Matching permission names lost the caller's order or duplicates");
        require(client.getPackagesHoldingPermissions(new String[]{"fixture.absent.permission"}, 0).isEmpty()
                && client.getPackagesHoldingPermissions(new String[0], 0).isEmpty()
                && wire.getPackagesHoldingPermissions(new String[]{heldPermission}, 0, 10).getList().isEmpty(),
                "Empty, ungranted or cross-user permission queries acquired a package");
        require(client.getPackagesHoldingPermissions(new String[]{heldPermission},
                        PackageManager.MATCH_SYSTEM_ONLY).isEmpty(),
                "System-only permission query included an ordinary package");
        missing(() -> client.getPackageGids(PACKAGE, PackageManager.MATCH_SYSTEM_ONLY),
                "System-only group query included an ordinary package");
        require(client.getPackageGids("android", PackageManager.MATCH_SYSTEM_ONLY).length == 0,
                "System-only group query excluded the platform package");
        Method addon = method(apkClass, "setSystemAddon", boolean.class);
        try {
            // components.apk is deliberately unsigned. It must not become a trusted
            // system addon merely to exercise a metadata-query positive branch.
            addon.invoke(apk, true);
            throw new AssertionError("Unverified APK acquired system-addon status");
        } catch (java.lang.reflect.InvocationTargetException rejected) {
            require(rejected.getCause() instanceof SecurityException,
                    "Unsigned addon promotion failed for an unrelated reason");
        }
        require(!((Boolean)apkClass.getMethod("isSystemAddon").invoke(apk))
                        && (client.getApplicationInfo(PACKAGE, 0).flags
                                & android.content.pm.ApplicationInfo.FLAG_SYSTEM) == 0,
                "Rejected addon promotion changed installed system metadata");
        missing(() -> client.getPackageGids(PACKAGE, PackageManager.MATCH_SYSTEM_ONLY),
                "Rejected addon promotion changed the system-only group query");
        require(client.getPackagesHoldingPermissions(new String[]{heldPermission},
                        PackageManager.MATCH_SYSTEM_ONLY).isEmpty(),
                "Rejected addon promotion changed the system-only permission query");
        dexRegistration(wire, service, apkFile.getPath());

        String[] names = {PACKAGE, "fixture.not.installed", PACKAGE};
        require(Arrays.equals(names, client.currentToCanonicalPackageNames(names)),
                "Current-name mapping lost unknown, duplicate or positional entries");
        require(Arrays.equals(names, client.canonicalToCurrentPackageNames(names)),
                "Canonical-name mapping lost unknown, duplicate or positional entries");
        require(client.currentToCanonicalPackageNames(new String[0]).length == 0
                && client.canonicalToCurrentPackageNames(new String[0]).length == 0,
                "Empty canonical-name query returned null");
        String[] local = service.currentToCanonicalPackageNames(names);
        local[0] = "caller.changed";
        require(PACKAGE.equals(names[0]), "Local current-name query returned the caller array");
        local = service.canonicalToCurrentPackageNames(names);
        local[0] = "caller.changed";
        require(PACKAGE.equals(names[0]), "Local canonical-name query returned the caller array");

        property(client.getProperty(PROPERTY, PACKAGE), null, PackageManager.TYPE_APPLICATION);
        int[] kinds = {PackageManager.TYPE_ACTIVITY, PackageManager.TYPE_PROVIDER,
                PackageManager.TYPE_RECEIVER, PackageManager.TYPE_SERVICE};
        String[] classes = {"MainActivity", "ItemsProvider", "ManifestReceiver", "CounterService"};
        for (int i = 0; i < kinds.length; i++) {
            property(client.getProperty(PROPERTY, new ComponentName(PACKAGE, PACKAGE + "." + classes[i])),
                    PACKAGE + "." + classes[i], kinds[i]);
        }
        List<List<Property>> results = Arrays.asList(client.queryApplicationProperty(PROPERTY),
                client.queryActivityProperty(PROPERTY), client.queryProviderProperty(PROPERTY),
                client.queryReceiverProperty(PROPERTY), client.queryServiceProperty(PROPERTY));
        require(results.get(0).size() == 1, "Application property query omitted its parsed value");
        property(results.get(0).get(0), null, PackageManager.TYPE_APPLICATION);
        for (int i = 0; i < kinds.length; i++) {
            require(results.get(i + 1).size() == 1, "Component property query crossed location boundaries");
            property(results.get(i + 1).get(0), PACKAGE + "." + classes[i], kinds[i]);
        }
        missing(() -> client.getProperty("fixture.absent", PACKAGE), "Unknown property appeared present");
        missing(() -> client.getProperty(PROPERTY, "fixture.not.installed"), "Foreign package property leaked");
        missing(() -> client.getProperty(PROPERTY, new ComponentName(PACKAGE, PACKAGE + ".SecondActivity")),
                "Component query fell back to the application's property");
        require(client.queryApplicationProperty("fixture.absent").isEmpty()
                && wire.queryProperty(PROPERTY, -1).getList().isEmpty(),
                "Absent or invalid property location did not return an empty list");
        require(wire.getPropertyAsUser(PROPERTY, PACKAGE, null, 10) == null,
                "Manifest property query escaped the single-user package scope");

        method(pmClass, "beginPackageRemoval", apkClass).invoke(service, apk);
        missing(() -> client.canPackageQuery(PACKAGE, "android"), "Retiring package remained a visibility source");
        missing(() -> client.canPackageQuery("android", PACKAGE), "Retiring package remained a visibility target");
        missing(() -> client.getPackageGids(PACKAGE), "Retiring package kept a group inventory");
        require(wire.getPropertyAsUser(PROPERTY, PACKAGE, null, 0) == null
                && client.queryApplicationProperty(PROPERTY).isEmpty(),
                "Retiring package continued publishing properties");
        require(client.getPackagesHoldingPermissions(new String[]{heldPermission}, 0).isEmpty(),
                "Retiring package continued publishing permission ownership");
        method(pmClass, "endPackageRemoval", apkClass).invoke(service, apk);
        property(client.getProperty(PROPERTY, PACKAGE), null, PackageManager.TYPE_APPLICATION);
        method(pmClass, "remove", String.class).invoke(service, PACKAGE);
        missing(() -> client.canPackageQuery(PACKAGE, PACKAGE), "Removed package retained visibility metadata");
        missing(() -> client.getPackageInfo(PACKAGE, flags),
                "Removed package survived in the public framework package-info cache");
        missing(() -> client.getPackageGids(PACKAGE), "Removed package kept a group inventory");
        require(Arrays.equals(client.getNamesForUids(new int[]{uid}), new String[]{null})
                && client.getPackagesHoldingPermissions(new String[]{heldPermission}, 0).isEmpty(),
                "Removed package retained its UID name or permission ownership");
        missing(() -> client.getProperty(PROPERTY, PACKAGE), "Removed property was cached as installed");
        require(client.queryApplicationProperty(PROPERTY).isEmpty(), "Removed package remained in property index");
        method(pmClass, "add", apkClass).invoke(service, apk);
        require(client.canPackageQuery(PACKAGE, PACKAGE), "Reinstalled package visibility was not republished");
        packageInfo(client.getPackageInfo(PACKAGE, flags), expected, true);
        property(client.getProperty(PROPERTY, PACKAGE), null, PackageManager.TYPE_APPLICATION);
        // PackageImpl.addActivity retains duplicate manifest declarations. AOSP's
        // property index answers the last matching declaration, while bulk queries
        // retain both values in declaration order.
        com.android.internal.pm.parsing.pkg.ParsedPackage parsed =
                (com.android.internal.pm.parsing.pkg.ParsedPackage) apkClass.getField("parsed").get(apk);
        com.android.internal.pm.pkg.component.ParsedActivityImpl later =
                new com.android.internal.pm.pkg.component.ParsedActivityImpl();
        later.setName(PACKAGE + ".MainActivity");
        later.setPackageName(PACKAGE);
        later.addProperty(new PackageManager.Property(PROPERTY, "later-value", PACKAGE,
                PACKAGE + ".MainActivity"));
        parsed.getClass().getMethod("addActivity",
                com.android.internal.pm.pkg.component.ParsedActivity.class).invoke(parsed, later);
        require("later-value".equals(client.getProperty(PROPERTY,
                new ComponentName(PACKAGE, PACKAGE + ".MainActivity")).getString()),
                "Repeated component property did not use the last declaration");
        List<PackageManager.Property> repeated = client.queryActivityProperty(PROPERTY);
        require(repeated.size() == 2 && "activity-value".equals(repeated.get(0).getString())
                && "later-value".equals(repeated.get(1).getString()),
                "Property enumeration lost duplicate declarations or changed their order");
        System.out.println("PackageQueryContractTest all passed: " + checks + " checks");
    }
}
