/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package gesher.services;

import android.app.ActivityThread;
import android.app.admin.DevicePolicyManager;
import android.content.Context;
import android.content.pm.IPackageManager;
import android.content.pm.SystemFeaturesCache;
import android.content.res.AssetManager;
import android.os.IBinder;
import android.os.Looper;
import android.os.PowerManager;
import android.os.ServiceManager;

import java.lang.reflect.Constructor;
import java.lang.reflect.Field;
import java.util.Collections;
import java.util.Map;

/** Constructs real ContextImpl managers in an isolated test process, without opening an app. */
public final class ManagerConstructionBatchTest {
    private static void require(boolean value, String message) {
        if (!value) throw new AssertionError(message);
    }

    public static void main(String[] args) throws Exception {
        System.loadLibrary("android_runtime");
        AssetManager.createSystemAssetsInZygoteLocked(false,
                System.getenv("ANDROID_ROOT") + "/framework/framework-res.apk");
        if (Looper.myLooper() == null) Looper.prepareMainLooper();
        Class<?> services = Class.forName("com.gesher.server.Services");
        @SuppressWarnings("unchecked")
        Map<String, IBinder> created = (Map<String, IBinder>) services
                .getMethod("create", java.util.Collection.class).invoke(null, Collections.emptyList());
        Field cacheField = ServiceManager.class.getDeclaredField("sCache");
        cacheField.setAccessible(true);
        @SuppressWarnings("unchecked")
        Map<String, IBinder> cache = (Map<String, IBinder>) cacheField.get(null);
        cache.putAll(created);
        SystemFeaturesCache.setInstance(new SystemFeaturesCache(IPackageManager.Stub
                .asInterface(created.get("package")).getSystemAvailableFeatures().getList()));
        require(created.get(Context.DEVICE_IDLE_CONTROLLER) != null, "deviceidle not registered");
        Constructor<ActivityThread> threadConstructor = ActivityThread.class.getDeclaredConstructor();
        threadConstructor.setAccessible(true);
        ActivityThread thread = threadConstructor.newInstance();
        Context context = thread.getSystemContext();
        String[] names = {Context.DEVICE_IDLE_CONTROLLER, Context.POWER_EXEMPTION_SERVICE,
                Context.POWER_SERVICE, Context.ALARM_SERVICE, Context.USER_SERVICE,
                Context.APP_OPS_SERVICE, Context.DEVICE_POLICY_SERVICE, Context.SHORTCUT_SERVICE,
                Context.USAGE_STATS_SERVICE, Context.DROPBOX_SERVICE, Context.SEARCH_SERVICE,
                Context.NOTIFICATION_SERVICE, Context.CLIPBOARD_SERVICE, Context.STORAGE_SERVICE,
                Context.DISPLAY_SERVICE, Context.UI_MODE_SERVICE, Context.AUDIO_SERVICE};
        for (String name : names) {
            Object manager = context.getSystemService(name);
            require(manager != null, name + " constructed null");
            require(manager == context.getSystemService(name), name + " did not cache its manager");
            System.out.println("manager constructed: " + name + " -> " + manager.getClass().getName());
        }
        PowerManager power = context.getSystemService(PowerManager.class);
        require(!power.isDeviceIdleMode(), "unexpected Android doze");
        require(power.getThermalHeadroomThresholds().isEmpty(), "invented headroom thresholds");
        require(!power.isIgnoringBatteryOptimizations("missing.package"), "invented exemption");
        DevicePolicyManager policy = context.getSystemService(DevicePolicyManager.class);
        long begin = System.nanoTime();
        try {
            policy.setPermissionGrantState(null, "gesher.services", "android.permission.CAMERA",
                    DevicePolicyManager.PERMISSION_GRANT_STATE_GRANTED);
            throw new AssertionError("manager granted device policy permission without an owner");
        } catch (SecurityException expected) {
            require(System.nanoTime() - begin < 1_000_000_000L,
                    "manager waited for a callback after a denied policy mutation");
        }
        System.out.println("ManagerConstructionBatchTest all passed (" + names.length + " managers)");
        // Services.create owns periodic workers; this isolated fixture has no event loop.
        System.exit(0);
    }
}
