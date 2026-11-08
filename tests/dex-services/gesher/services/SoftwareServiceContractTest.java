/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package gesher.services;

import android.app.admin.DevicePolicyManager;
import android.app.admin.IDevicePolicyManager;
import android.app.usage.IUsageStatsManager;
import android.app.usage.UsageStatsManager;
import android.content.ContextWrapper;
import android.content.Intent;
import android.content.IntentFilter;
import android.content.pm.IShortcutService;
import android.content.pm.ParceledListSlice;
import android.content.pm.ShortcutInfo;
import android.content.res.AssetManager;
import android.os.IThermalService;
import android.os.IThermalStatusListener;
import android.os.RemoteCallback;
import android.os.Temperature;
import android.os.UserHandle;

import java.lang.reflect.Constructor;
import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Method;
import java.util.Collections;
import java.util.concurrent.atomic.AtomicInteger;

/** Object shapes and immediate failure contracts for software-only service policies. */
public final class SoftwareServiceContractTest {
    private static void require(boolean value, String message) {
        if (!value) throw new AssertionError(message);
    }

    private static Object construct(String name, Class<?>[] parameters, Object... arguments)
            throws Exception {
        Constructor<?> ctor = Class.forName("com.gesher.server." + name)
                .getDeclaredConstructor(parameters);
        ctor.setAccessible(true);
        return ctor.newInstance(arguments);
    }

    private static <T> T strict(Class<T> type, Object impl) throws Exception {
        Class<?> wrapper = Class.forName("com.gesher.server.StandIn");
        wrapper.getMethod("setStrict", boolean.class).invoke(null, true);
        Object standIn = wrapper.getMethod("of", Class.class, android.os.IInterface.class)
                .invoke(null, type, impl);
        return type.cast(wrapper.getMethod("asInterface").invoke(standIn));
    }

    private interface Action { void run() throws Exception; }
    private static void denied(Action call, String name) throws Exception {
        long start = System.nanoTime();
        try {
            call.run();
            throw new AssertionError(name + " accepted an unauthorized caller");
        } catch (SecurityException expected) {
            require(System.nanoTime() - start < 1_000_000_000L, name + " did not fail promptly");
        }
    }

    public static void main(String[] args) throws Exception {
        System.loadLibrary("android_runtime");
        AssetManager.createSystemAssetsInZygoteLocked(false,
                System.getenv("ANDROID_ROOT") + "/framework/framework-res.apk");
        IThermalService thermal = strict(IThermalService.class,
                construct("DeviceServices$ThermalService", new Class<?>[0]));
        require(thermal.getCurrentTemperatures().length == 0, "invented temperature sensors");
        require(thermal.getCurrentTemperaturesWithType(Temperature.TYPE_CPU).length == 0,
                "invented CPU sensor");
        require(thermal.getCurrentCoolingDevices().length == 0, "invented cooling devices");
        require(thermal.getCurrentCoolingDevicesWithType(0).length == 0, "null cooling array");
        require(Float.isNaN(thermal.getThermalHeadroom(0)), "unsupported headroom must be NaN");
        float[] thresholds = thermal.getThermalHeadroomThresholds();
        require(thresholds.length > Temperature.THROTTLING_SHUTDOWN, "short threshold array");
        for (int i = Temperature.THROTTLING_LIGHT; i <= Temperature.THROTTLING_SHUTDOWN; i++) {
            require(Float.isNaN(thresholds[i]), "invented thermal threshold");
        }
        AtomicInteger events = new AtomicInteger();
        IThermalStatusListener listener = new IThermalStatusListener.Stub() {
            @Override public void onStatusChange(int status) { events.incrementAndGet(); }
        };
        require(!thermal.registerThermalStatusListener(listener), "accepted unavailable listener");
        require(!thermal.unregisterThermalStatusListener(listener), "invented registration");
        require(events.get() == 0, "unavailable listener emitted a fake event");

        Class<?> pmType = Class.forName("com.gesher.server.PackageManagerService");
        Object pm = construct("PackageManagerService", new Class<?>[] {java.util.Collection.class},
                Collections.emptyList());
        IUsageStatsManager usage = strict(IUsageStatsManager.class,
                construct("DeviceServices$UsageStatsService", new Class<?>[] {pmType}, pm));
        require(!usage.isAppStandbyEnabled(), "Android app standby is not a Windows policy");
        require(usage.queryUsageStats(0, 0, 100, "gesher.services", 0).getList().isEmpty(),
                "unexpected usage history");
        require(!usage.queryEvents(0, 100, "gesher.services").hasNextEvent(), "fake usage events");
        require(!usage.queryEventsForPackage(0, 100, "gesher.services").hasNextEvent(),
                "fake self events");
        require(usage.queryBroadcastResponseStats(null, 0, "gesher.services", 0).getList().isEmpty(),
                "null broadcast response object");
        require(usage.getAppStandbyBuckets("gesher.services", 0).getList().isEmpty(),
                "unexpected standby packages");
        try {
            usage.getAppStandbyBucket("missing.package", "gesher.services", 0);
            throw new AssertionError("unknown package acquired a standby bucket");
        } catch (IllegalArgumentException expected) { }
        denied(() -> usage.registerAppUsageObserver(1, new String[] {"gesher.services"},
                1, null, "gesher.services"), "usage observer");
        denied(() -> usage.registerUsageSessionObserver(2, new String[] {"gesher.services"},
                1, 1, null, null, "gesher.services"), "session observer");
        denied(() -> usage.registerAppUsageLimitObserver(3, new String[] {"gesher.services"},
                1, 0, null, "gesher.services"), "limit observer");
        usage.unregisterAppUsageObserver(1, "gesher.services");

        require(args.length == 1, "supply the test APK fixture path");
        Class<?> apkType = Class.forName("com.gesher.server.ApkPackage");
        Object apk = apkType.getMethod("parse", String.class).invoke(null, args[0]);
        String packageName = (String) apkType.getField("packageName").get(apk);
        Method add = pmType.getDeclaredMethod("add", apkType);
        add.setAccessible(true);
        add.invoke(pm, apk);
        require(usage.getAppStandbyBucket(packageName, packageName, 0)
                == UsageStatsManager.STANDBY_BUCKET_ACTIVE, "installed package has wrong bucket");
        require(!usage.isAppInactive(packageName, 0, packageName), "inactive on disabled standby");
        require(usage.getAppStandbyBuckets(packageName, 0).getList().size() == 1,
                "installed package omitted from buckets");

        IShortcutService shortcuts = strict(IShortcutService.class,
                construct("DeviceServices$ShortcutService", new Class<?>[] {pmType}, pm));
        require(!shortcuts.isRequestPinItemSupported(0, 1), "invented pin capability");
        ShortcutInfo shortcut = new ShortcutInfo.Builder(new ContextWrapper(null) {
            @Override public String getPackageName() { return packageName; }
            @Override public int getUserId() { return 0; }
        }, "test").setShortLabel("test").setIntent(new Intent(Intent.ACTION_VIEW)).build();
        require(!shortcuts.requestPinShortcut(packageName, shortcut, null, 0),
                "pin request reported success without a launcher");
        require(shortcuts.createShortcutResultIntent(packageName, shortcut, 0) == null,
                "unsupported legacy pin returned a success intent");
        require(shortcuts.getShortcuts(packageName, 15, 0).getList().isEmpty(), "fake shortcuts");
        require(shortcuts.getShareTargets(packageName, new IntentFilter(Intent.ACTION_SEND), 0)
                .getList().isEmpty(), "fake share targets");
        require(shortcuts.setDynamicShortcuts(packageName, ParceledListSlice.emptyList(), 0),
                "dynamic shortcut publishing should succeed as a no-op");
        ParceledListSlice<ShortcutInfo> published = new ParceledListSlice<>(Collections.singletonList(shortcut));
        require(shortcuts.setDynamicShortcuts(packageName, published, 0), "nonempty shortcut set threw or failed");
        require(shortcuts.addDynamicShortcuts(packageName, published, 0), "nonempty shortcut add threw or failed");
        require(shortcuts.updateShortcuts(packageName, published, 0), "nonempty shortcut update threw or failed");
        shortcuts.pushDynamicShortcut(packageName, shortcut, 0);
        require(shortcuts.getShortcuts(packageName, 15, 0).getList().size() == 1
                && !shortcuts.isRequestPinItemSupported(0, 1), "dynamic shortcut state or pin capability is wrong");
        shortcuts.removeAllDynamicShortcuts(packageName, 0);
        require(shortcuts.getShortcuts(packageName, 15, 0).getList().isEmpty(), "dynamic shortcuts were not removed");
        Action[] malformed = {
                () -> shortcuts.setDynamicShortcuts(packageName, null, 0),
                () -> shortcuts.addDynamicShortcuts(packageName, null, 0),
                () -> shortcuts.updateShortcuts(packageName, null, 0),
                () -> shortcuts.pushDynamicShortcut(packageName, null, 0)};
        for (Action call : malformed) {
            try { call.run(); throw new AssertionError("null shortcut payload accepted"); }
            catch (NullPointerException expected) { }
        }
        Action[] unknown = {
                () -> shortcuts.setDynamicShortcuts("missing.package", published, 0),
                () -> shortcuts.addDynamicShortcuts(packageName, published, 10),
                () -> shortcuts.updateShortcuts("missing.package", published, 0),
                () -> shortcuts.pushDynamicShortcut(packageName, shortcut, 10)};
        for (Action call : unknown) {
            try { call.run(); throw new AssertionError("shortcut publication accepted unknown package/user"); }
            catch (IllegalArgumentException expected) { }
        }
        denied(shortcuts::resetThrottling, "shortcut throttling administration");
        denied(() -> shortcuts.getBackupPayload(0), "shortcut backup administration");
        try {
            shortcuts.getShortcuts("missing.package", 15, 0);
            throw new AssertionError("shortcut query accepted an unknown package");
        } catch (IllegalArgumentException expected) { }

        IDevicePolicyManager policy = strict(IDevicePolicyManager.class,
                construct("DeviceServices$DevicePolicyService", new Class<?>[0]));
        require(!policy.isDeviceManaged() && !policy.hasDeviceOwner(), "invented owner");
        require(policy.getDeviceOwnerUserId() == UserHandle.USER_NULL, "user 0 is not an owner");
        require(policy.getActiveAdmins(0) == null, "Android no-admin representation changed");
        require(policy.getStorageEncryptionStatus("gesher.services", 0)
                == DevicePolicyManager.ENCRYPTION_STATUS_UNSUPPORTED, "claimed Android encryption");
        AtomicInteger results = new AtomicInteger();
        RemoteCallback callback = new RemoteCallback(result -> results.incrementAndGet());
        denied(() -> policy.setPermissionGrantState(null, "gesher.services", "gesher.services",
                "android.permission.CAMERA", DevicePolicyManager.PERMISSION_GRANT_STATE_GRANTED,
                callback), "permission grant");
        denied(() -> policy.getRemoveWarning(null, callback, 0), "remove warning");
        denied(() -> policy.installUpdateFromFile(null, "gesher.services", null, null),
                "system update");
        require(results.get() == 0, "denied admin call also emitted a success callback");
        System.out.println("SoftwareServiceContractTest all passed");
    }
}
