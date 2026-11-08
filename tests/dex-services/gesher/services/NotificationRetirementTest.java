/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package gesher.services;

import android.app.Notification;
import android.app.NotificationChannel;
import android.content.pm.ParceledListSlice;
import android.content.pm.PackageManager;
import android.content.res.AssetManager;
import android.os.Bundle;
import android.service.notification.StatusBarNotification;
import java.io.File;
import java.lang.reflect.*;
import java.nio.file.Files;
import java.util.*;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicReference;
import java.util.function.ToIntBiFunction;

/** Real notification/store publication races; the toast sink never calls Windows UI. */
public final class NotificationRetirementTest {
    private static final AtomicInteger checks = new AtomicInteger();
    private static void require(boolean value, String why) { if (!value) throw new AssertionError(why); checks.incrementAndGet(); }
    private static Field field(Class<?> type, String name) throws Exception {
        Field f = type.getDeclaredField(name); f.setAccessible(true); return f;
    }
    private static Method method(Class<?> type, String name, Class<?>... args) throws Exception {
        Method m = type.getDeclaredMethod(name, args); m.setAccessible(true); return m;
    }
    private static Object make(Class<?> type, Class<?>[] args, Object... values) throws Exception {
        Constructor<?> c = type.getDeclaredConstructor(args); c.setAccessible(true); return c.newInstance(values);
    }
    private interface Work { void run() throws Exception; }
    private static Thread worker(String name, AtomicReference<Throwable> failure, Work work) {
        Thread t = new Thread(() -> {
            try { work.run(); }
            catch (InvocationTargetException e) { failure.compareAndSet(null, e.getCause()); }
            catch (Throwable e) { failure.compareAndSet(null, e); }
        }, name);
        t.setDaemon(true); return t;
    }
    private static void await(CountDownLatch latch, String why) {
        try { require(latch.await(5, TimeUnit.SECONDS), why); }
        catch (InterruptedException e) { Thread.currentThread().interrupt(); throw new AssertionError(e); }
    }
    private static void join(Thread t, AtomicReference<Throwable> failure) throws Exception {
        t.join(5000);
        require(!t.isAlive(), "Notification fixture thread did not finish: " + t.getName());
        if (failure.get() != null) throw new AssertionError("Notification fixture worker failed", failure.get());
    }

    private static final class Fixture {
        final Class<?> apk = Class.forName("com.gesher.server.ApkPackage");
        final Class<?> pmType = Class.forName("com.gesher.server.PackageManagerService");
        final Class<?> notifications = Class.forName("com.gesher.server.NotificationManagerService");
        final Class<?> amType = Class.forName("com.gesher.server.ActivityManagerService");
        final Class<?> atmType = Class.forName("com.gesher.server.ActivityTaskManagerService");
        final Object original, fresh, pm, service, policy;
        final String pkg;
        final File data;
        final AtomicInteger shown = new AtomicInteger();

        Fixture(File parent, String source, ToIntBiFunction<StatusBarNotification, Boolean> sink) throws Exception {
            data = Files.createTempDirectory(parent.toPath(), "notification-retirement-").toFile();
            android.system.Os.setenv("ANDROID_DATA", data.getPath(), true);
            File input = new File(source), copy = new File(data, "reinstall/" + input.getName());
            Files.createDirectories(copy.getParentFile().toPath()); Files.copy(input.toPath(), copy.toPath());
            Method parse = apk.getMethod("parse", String.class);
            original = parse.invoke(null, input.getPath()); fresh = parse.invoke(null, copy.getPath());
            pkg = (String)apk.getField("packageName").get(original);
            require(original != fresh, "Reinstall did not create a distinct package generation");
            Method identity = method(notifications, "codeIdentity", apk);
            require(Objects.equals(identity.invoke(null, original), identity.invoke(null, fresh)),
                    "Identical reinstall unexpectedly changed its signer/code identity");
            require(apk.getMethod("uid").invoke(original).equals(apk.getMethod("uid").invoke(fresh)),
                    "Reinstall fixture did not retain the package UID");
            pm = pmType.getConstructor(Collection.class).newInstance(Collections.singletonList(original));
            Object atm = make(atmType, new Class<?>[]{pmType}, pm);
            ToIntBiFunction<StatusBarNotification, Boolean> toast = (n, update) -> {
                if (sink != null) sink.applyAsInt(n, update);
                shown.incrementAndGet(); return 0;
            };
            service = make(notifications, new Class<?>[]{pmType, amType, ToIntBiFunction.class}, pm, null, toast);
            Object am = make(amType, new Class<?>[]{atmType, pmType, notifications}, atm, pm, service);
            Class<?> services = Class.forName("com.gesher.server.Services");
            Class<?> created = Class.forName("com.gesher.server.Services$Created");
            field(services, "sLast").set(null, make(created, new Class<?>[]{pmType, atmType, amType}, pm, atm, am));
            policy = pmType.getMethod("permissions").invoke(pm);
            grant();
        }
        void grant() throws Exception {
            policy.getClass().getMethod("grantRuntimePermission", String.class, String.class, String.class, int.class)
                    .invoke(policy, pkg, "android.permission.POST_NOTIFICATIONS", "default:0", 0);
            require((Integer)policy.getClass().getMethod("checkPermission", String.class, String.class)
                    .invoke(policy, pkg, "android.permission.POST_NOTIFICATIONS") == PackageManager.PERMISSION_GRANTED,
                    "Actual requested notification permission was not granted");
            notifications.getMethod("createNotificationChannels", String.class, ParceledListSlice.class)
                    .invoke(service, pkg, new ParceledListSlice<>(List.of(new NotificationChannel(
                            "retirement", "Retirement fixture", android.app.NotificationManager.IMPORTANCE_DEFAULT))));
        }
        void post(int id) throws Exception {
            Notification n = new Notification(); n.extras = new Bundle();
            field(Notification.class, "mChannelId").set(n, "retirement");
            n.extras.putString(Notification.EXTRA_TITLE, "synthetic notification retirement");
            method(notifications, "post", String.class, String.class, int.class, Notification.class)
                    .invoke(service, pkg, null, id, n);
        }
        void begin() throws Exception { method(pmType, "beginPackageRemoval", apk).invoke(pm, original); }
        void end() throws Exception { method(pmType, "endPackageRemoval", apk).invoke(pm, original); }
        void clear() throws Exception { method(notifications, "packageRemoved", String.class).invoke(service, pkg); }
        void removeAndReinstall() throws Exception {
            policy.getClass().getMethod("forgetPackage", String.class).invoke(policy, pkg);
            method(pmType, "remove", String.class).invoke(pm, pkg);
            method(pmType, "add", apk).invoke(pm, fresh);
        }
        @SuppressWarnings("unchecked") int posted() throws Exception {
            synchronized(service) { return ((Map<String, ?>)field(notifications, "mPosted").get(service)).size(); }
        }
        int stored() {
            File[] files = new File(data, "system/notifications/" + pkg).listFiles();
            return files == null ? 0 : files.length;
        }
    }

    private static void capturedPost(File data, String apk) throws Exception {
        Fixture f = new Fixture(data, apk, null);
        f.post(10);
        require(f.shown.get() == 1 && f.posted() == 1 && f.stored() == 1,
                "Granted baseline did not exercise real state/store and injected toast");
        CountDownLatch captured = new CountDownLatch(1), resume = new CountDownLatch(1);
        field(f.notifications, "mBeforePostPublication").set(f.service, (Runnable)() -> {
            captured.countDown(); await(resume, "Paused post was not released");
        });
        AtomicReference<Throwable> failure = new AtomicReference<>();
        Thread poster = worker("stale-notification-post", failure, () -> f.post(11));
        poster.start();
        try {
            await(captured, "Post did not reach pre-publication pause");
            f.begin();
            try { f.clear(); f.removeAndReinstall(); } finally { f.end(); }
            f.grant(); // A fresh real grant cannot authorize the previous package generation.
            require(f.posted() == 0 && f.stored() == 0, "Removal did not clear the old notification/store");
        } finally { resume.countDown(); }
        join(poster, failure);
        require(f.shown.get() == 1 && f.posted() == 0 && f.stored() == 0,
                "Paused original-generation post resurrected state, toast or stored contents");
        field(f.notifications, "mBeforePostPublication").set(f.service, null);
        f.post(12);
        require(f.shown.get() == 2 && f.posted() == 1 && f.stored() == 1,
                "Fresh reinstalled generation could not publish after its new grant");
        f.clear();
    }

    private static void activePublication(File data, String apk) throws Exception {
        CountDownLatch inToast = new CountDownLatch(1), resume = new CountDownLatch(1), removing = new CountDownLatch(1);
        Fixture f = new Fixture(data, apk, (n, update) -> {
            inToast.countDown(); await(resume, "Synthetic toast publication was not released"); return 0;
        });
        AtomicReference<Throwable> failure = new AtomicReference<>();
        Thread poster = worker("active-notification-post", failure, () -> f.post(20));
        Thread removal = worker("notification-removal", failure, () -> {
            f.begin();
            try { removing.countDown(); f.clear(); f.removeAndReinstall(); } finally { f.end(); }
        });
        poster.start();
        try {
            await(inToast, "Post did not enter synthetic host publication");
            require(f.posted() == 1 && f.stored() == 1,
                    "Paused host publication did not include real notification/store state");
            removal.start(); await(removing, "Removal did not start");
            long deadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(3);
            while (removal.isAlive() && removal.getState() != Thread.State.BLOCKED && System.nanoTime() < deadline) {
                Thread.sleep(5);
            }
            require(removal.isAlive() && removal.getState() == Thread.State.BLOCKED,
                    "Removal passed a host publication still holding its fence");
        } finally { resume.countDown(); }
        join(poster, failure); join(removal, failure);
        require(f.shown.get() == 1 && f.posted() == 0 && f.stored() == 0,
                "Completed publication/removal left or recreated notification contents");
    }

    public static void main(String[] args) throws Exception {
        require(args.length == 1, "Pass the synthetic devsvc APK requesting POST_NOTIFICATIONS");
        require("1000".equals(System.getenv("GESHER_BINDER_UID")), "Explicit system-UID service fixture required for real store path");
        System.loadLibrary("android_runtime");
        AssetManager.createSystemAssetsInZygoteLocked(false,
                System.getenv("ANDROID_ROOT") + "/framework/framework-res.apk");
        File data = new File(System.getenv("ANDROID_DATA")); Files.createDirectories(data.toPath());
        Class<?> services = Class.forName("com.gesher.server.Services");
        Field last = field(services, "sLast"); Object previous = last.get(null);
        try { capturedPost(data, args[0]); activePublication(data, args[0]); }
        finally { last.set(null, previous); }
        System.out.println("NotificationRetirementTest all passed: " + checks.get() + " checks");
    }
}
