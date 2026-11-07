/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package gesher.services;

import android.app.ActivityThread;
import android.app.PropertyInvalidatedCache;
import android.content.Intent;
import android.content.pm.IPackageManager;
import android.content.pm.PackageInfo;
import android.content.pm.PackageManager;
import android.content.res.AssetManager;
import com.gesher.server.ApkPackage;
import com.gesher.server.PackageManagerService;
import com.gesher.server.StandIn;
import java.io.File;
import java.lang.reflect.Constructor;
import java.lang.reflect.Field;
import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Method;
import java.nio.file.Files;
import java.util.ArrayList;
import java.util.Collections;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.Executor;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicReference;
import java.util.function.Consumer;

/** Real durable Installer/cache paths; only the final broadcast transport is captured locally. */
public final class PackageInstallEventsTest {
    private static int checks;
    private static void require(boolean value, String reason) {
        if (!value) throw new AssertionError(reason);
        checks++;
    }
    private static Object construct(Class<?> type, Class<?>[] params, Object... args) throws Exception {
        Constructor<?> c = type.getDeclaredConstructor(params); c.setAccessible(true); return c.newInstance(args);
    }
    private static Field field(Class<?> type, String name) throws Exception {
        Field f = type.getDeclaredField(name); f.setAccessible(true); return f;
    }
    private static Method method(Class<?> type, String name, Class<?>... params) throws Exception {
        Method m = type.getDeclaredMethod(name, params); m.setAccessible(true); return m;
    }
    private static void drain(Executor executor) throws Exception {
        CountDownLatch done = new CountDownLatch(1); executor.execute(done::countDown);
        require(done.await(5, TimeUnit.SECONDS), "Package event queue did not drain");
    }
    private static PackageInfo cached(String name) {
        return PackageManager.getPackageInfoAsUserCached(name, 0, 0);
    }
    public static void main(String[] args) throws Exception {
        require(args.length == 2, "Pass same-signer target v1.apk and v2.apk");
        System.loadLibrary("android_runtime");
        AssetManager.createSystemAssetsInZygoteLocked(false,
                System.getenv("ANDROID_ROOT") + "/framework/framework-res.apk");
        File data = Files.createTempDirectory("gesher-package-events-").toFile();
        android.system.Os.setenv("ANDROID_DATA", data.getPath(), true);
        android.system.Os.setenv("GESHER_MICROG_SPOOF", "1", true);
        android.system.Os.setenv("GESHER_BINDER_UID", "1000", true);
        File v1 = new File(args[0]), v2 = new File(args[1]);
        ApkPackage first = ApkPackage.parse(v1.getPath()), second = ApkPackage.parse(v2.getPath());
        String name = first.packageName;
        require(name.equals(second.packageName) && second.versionCode() > first.versionCode(), "Not a versioned target pair");
        PackageManagerService pm = new PackageManagerService(Collections.emptyList());
        Class<?> atmType = Class.forName("com.gesher.server.ActivityTaskManagerService");
        Object atm = construct(atmType, new Class<?>[]{PackageManagerService.class}, pm);
        Class<?> notifications = Class.forName("com.gesher.server.NotificationManagerService");
        Class<?> amType = Class.forName("com.gesher.server.ActivityManagerService");
        Object am = construct(amType, new Class<?>[]{atmType, PackageManagerService.class, notifications},
                atm, pm, construct(notifications, new Class<?>[0]));
        method(amType, "invalidatePermissionCaches").invoke(am);
        field(ActivityThread.class, "sPackageManager").set(null, StandIn.of(IPackageManager.class, pm).asInterface());
        Class<?> installerType = Class.forName("com.gesher.server.Installer");
        Object installer = construct(installerType, new Class<?>[]{PackageManagerService.class, atmType, Map.class},
                pm, atm, new LinkedHashMap<>());
        Method install = method(installerType, "install", String.class);
        Executor executor = (Executor)field(PackageManagerService.class, "mPackageEventExecutor").get(pm);
        List<Intent> events = Collections.synchronizedList(new ArrayList<>());
        AtomicReference<Throwable> callbackFailure = new AtomicReference<>();
        CountDownLatch firstEvent = new CountDownLatch(1);
        field(PackageManagerService.class, "mPackageEventSender").set(pm, (Consumer<Intent>)intent -> {
            try {
                require(!Thread.holdsLock(installer) && !Thread.holdsLock(pm), "Callback holds publication monitor");
                ApkPackage current = pm.get(name);
                require(current != null && new File(current.apkPath).isFile(), "Event preceded published code");
                String registry = Files.readString(new File(data, "system/packages.list").toPath());
                require(registry.contains(name + "\t"), "Event preceded durable package registry");
                require(cached(name).getLongVersionCode() == current.versionCode(), "Event observes stale package cache");
                events.add(new Intent(intent));
                firstEvent.countDown();
            } catch (Throwable failure) { callbackFailure.set(failure); }
        });

        require(cached(name) == null, "Target already installed in fresh fixture");
        synchronized (installer) {
            String[] result = (String[])install.invoke(installer, v1.getPath());
            require("installed".equals(result[1]), "Fresh install did not publish");
            require(!firstEvent.await(150, TimeUnit.MILLISECONDS), "Event escaped outer session publication lock");
        }
        drain(executor);
        require(callbackFailure.get() == null, "Event callback failed: " + callbackFailure.get());
        require(events.size() == 1, "Fresh install must emit exactly one event");
        Intent added = events.get(0);
        require(Intent.ACTION_PACKAGE_ADDED.equals(added.getAction()), "Missing added action");
        require(("package:" + name).equals(added.getDataString()) && added.getPackage() == null, "Wrong actual package URI/scope");
        require(!added.getBooleanExtra(Intent.EXTRA_REPLACING, true) && added.getFlags() == 0, "Fresh event has replacing/background flags");
        require(added.getIntExtra(Intent.EXTRA_UID, -1) == pm.get(name).uid(), "Fresh event has invented UID");
        PackageInfo warm = cached(name);
        require(warm == cached(name) && warm.getLongVersionCode() == first.versionCode(), "Did not establish a real SDK cache hit");
        PropertyInvalidatedCache<?, ?> cache = (PropertyInvalidatedCache<?, ?>)
                field(PackageManager.class, "sPackageInfoCache").get(null);
        long warmEpoch = cache.getNonce();
        ApkPackage beforeUnchanged = pm.get(name);
        String[] same = (String[])install.invoke(installer, v1.getPath());
        drain(executor);
        require("unchanged".equals(same[1]), "Same signed APK was not unchanged");
        require(events.size() == 1, "Unchanged APK emitted a package event");
        PackageInfo settled = cached(name);
        require(settled == cached(name) && settled.getLongVersionCode() == first.versionCode()
                && settled.applicationInfo.uid == warm.applicationInfo.uid,
                "Unchanged APK changed installed version/UID or failed to cache");
        // The existing unchanged path retries a missing dexopt artifact and can
        // republish loader metadata. That publication must invalidate the cache,
        // while still producing no APK-change event.
        if (pm.get(name) != beforeUnchanged) {
            require(cache.getNonce() != warmEpoch && settled != warm,
                    "Republished loader metadata retained the old cache epoch/object");
        } else {
            require(cache.getNonce() == warmEpoch && settled == warm,
                    "Unchanged publication invalidated the cache without a metadata change");
        }
        long settledEpoch = cache.getNonce();
        ApkPackage beforeFailure = pm.get(name);
        File invalid = new File(data, "invalid.apk"); Files.write(invalid.toPath(), new byte[]{1, 2, 3});
        try { install.invoke(installer, invalid.getPath()); throw new AssertionError("Invalid APK installed"); }
        catch (InvocationTargetException expected) { require(expected.getCause() instanceof Exception, "Wrong invalid failure"); }
        drain(executor);
        require(events.size() == 1, "Failed install emitted a package event");
        require(pm.get(name) == beforeFailure && cache.getNonce() == settledEpoch && cached(name) == settled,
                "Failed install changed installed metadata or cache epoch/object");

        String[] updated = (String[])install.invoke(installer, v2.getPath());
        drain(executor);
        require("updated".equals(updated[1]), "Changed signed target did not update");
        require(callbackFailure.get() == null && events.size() == 4, "Update completion sequence failed: " + callbackFailure.get());
        String[] actions = {Intent.ACTION_PACKAGE_ADDED, Intent.ACTION_PACKAGE_REPLACED, Intent.ACTION_MY_PACKAGE_REPLACED};
        for (int i = 0; i < actions.length; i++) require(actions[i].equals(events.get(i + 1).getAction()), "Wrong replacement order");
        for (int i = 1; i <= 2; i++) {
            Intent event = events.get(i);
            require(event.getBooleanExtra(Intent.EXTRA_REPLACING, false), "Update lacks replacing flag");
            require(event.getIntExtra(Intent.EXTRA_UID, -1) == pm.get(name).uid(), "Update UID differs from actual PM");
            require(("package:" + name).equals(event.getDataString()) && event.getPackage() == null && event.getFlags() == 0,
                    "Update broadcast URI/scope/flags differ from pinned Android contract");
        }
        Intent mine = events.get(3);
        require(name.equals(mine.getPackage()) && mine.getData() == null && mine.getExtras() == null,
                "MY_PACKAGE_REPLACED must target only actual updated package");
        PackageInfo refreshed = cached(name);
        require(cache.getNonce() != settledEpoch && refreshed != settled
                && refreshed.getLongVersionCode() == second.versionCode() && refreshed == cached(name),
                "Real SDK package cache did not invalidate then recache updated version");
        System.out.println("PackageInstallEventsTest all passed: " + checks + " checks");
    }
}
