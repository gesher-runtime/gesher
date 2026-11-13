/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package gesher.services;

import android.app.IApplicationThread;
import android.content.ComponentName;
import android.content.Intent;
import android.content.pm.ActivityInfo;
import android.content.pm.ServiceInfo;
import android.os.Binder;
import android.os.IBinder;
import android.os.SystemClock;

import java.lang.reflect.Constructor;
import java.lang.reflect.Field;
import java.lang.reflect.Method;
import java.lang.reflect.Proxy;
import java.util.Collections;
import java.util.List;
import java.util.Map;

/**
 * The persistent device's idle rule on the actual server classes, no device or APK:
 * ActivityTaskManagerService.idleSince with the real activity manager and services, and
 * DeviceWork.keepsIdleDevice with a scheduler that reschedules itself every 30 s.
 */
public final class DeviceIdleShutdownTest {
    private static final String PKG = "gesher.idle.cached";
    private static final String GMS = "com.google.android.gms";

    private static void require(boolean value, String message) {
        if (!value) throw new AssertionError(message);
    }

    private static Field field(Class<?> type, String name) throws Exception {
        Field field = type.getDeclaredField(name);
        field.setAccessible(true);
        return field;
    }

    private static Object construct(Class<?> type, Class<?>[] parameters, Object... values) throws Exception {
        Constructor<?> constructor = type.getDeclaredConstructor(parameters);
        constructor.setAccessible(true);
        return constructor.newInstance(values);
    }

    private static final class AppThread extends IApplicationThread.Default {
        final Binder binder = new Binder();
        @Override public IBinder asBinder() { return binder; }
    }

    private static Class<?> atmClass, processClass, activityClass, servicesClass, serviceClass;
    private static Object atm, services;
    private static Method idleSince;

    private static long idleSince() throws Exception {
        return (Long) idleSince.invoke(atm);
    }

    private static Object process(int pid, String pkg, boolean ready) throws Exception {
        Object p = construct(processClass,
                new Class<?>[]{atmClass, int.class, IApplicationThread.class}, atm, pid, new AppThread());
        field(processClass, "packageName").set(p, pkg);
        field(processClass, "processName").set(p, pkg);
        field(processClass, "ready").setBoolean(p, ready);
        List<Object> processes = (List<Object>) field(atmClass, "mProcesses").get(atm);
        synchronized (atm) { processes.add(p); }
        return p;
    }

    private static Object service(Object app, String pkg, String name) throws Exception {
        ServiceInfo info = new ServiceInfo();
        info.packageName = pkg;
        info.name = name;
        info.processName = pkg;
        Class<?> apkClass = Class.forName("com.gesher.server.ApkPackage");
        Object r = construct(serviceClass, new Class<?>[]{ServiceInfo.class, apkClass}, info, null);
        field(serviceClass, "app").set(r, app);
        Map<ComponentName, Object> map = (Map<ComponentName, Object>) field(servicesClass, "mServices").get(services);
        synchronized (services) { map.put(new ComponentName(pkg, name), r); }
        return r;
    }

    private static void foreground(Object service, int id) throws Exception {
        synchronized (services) { field(serviceClass, "foregroundId").setInt(service, id); }
    }

    public static void main(String[] args) throws Exception {
        Class<?> pmClass = Class.forName("com.gesher.server.PackageManagerService");
        atmClass = Class.forName("com.gesher.server.ActivityTaskManagerService");
        processClass = Class.forName("com.gesher.server.ActivityTaskManagerService$ProcessRecord");
        activityClass = Class.forName("com.gesher.server.ActivityTaskManagerService$ActivityRecord");
        Class<?> amClass = Class.forName("com.gesher.server.ActivityManagerService");
        Class<?> notificationsClass = Class.forName("com.gesher.server.NotificationManagerService");
        servicesClass = Class.forName("com.gesher.server.ActiveServices");
        serviceClass = Class.forName("com.gesher.server.ActiveServices$ServiceRecord");
        Object pm = construct(pmClass, new Class<?>[]{java.util.Collection.class}, Collections.emptyList());
        atm = construct(atmClass, new Class<?>[]{pmClass}, pm);
        Object am = construct(amClass, new Class<?>[]{atmClass, pmClass, notificationsClass},
                atm, pm, construct(notificationsClass, new Class<?>[0]));
        services = field(amClass, "mServices").get(am);
        idleSince = atmClass.getDeclaredMethod("idleSince");
        idleSince.setAccessible(true);
        int checks = 0;

        require(idleSince() != 0, "a device without app processes is not idle");
        checks++;

        // A visible activity keeps the device busy.
        Object app = process(990_001, PKG, true);
        ActivityInfo info = new ActivityInfo();
        info.packageName = PKG;
        info.name = PKG + ".Main";
        info.processName = PKG;
        Class<?> apkClass = Class.forName("com.gesher.server.ApkPackage");
        Object activity = construct(activityClass, new Class<?>[]{atmClass, ActivityInfo.class,
                Intent.class, int.class, int.class, String.class, activityClass, String.class,
                int.class, apkClass}, atm, info, new Intent(), 1, 990_001, PKG, null, null, -1, null);
        List<Object> activities = (List<Object>) field(atmClass, "mActivities").get(atm);
        List<Object> own = (List<Object>) field(processClass, "activities").get(app);
        synchronized (atm) {
            field(activityClass, "app").set(activity, app);
            activities.add(activity);
            own.add(activity);
        }
        require(idleSince() == 0, "a process with an activity did not keep the device busy");
        checks++;

        // The user closes it: the activity is destroyed, the process stays (cached).
        synchronized (atm) {
            activities.remove(activity);
            own.remove(activity);
        }
        long cached = idleSince();
        require(cached != 0, "a cached process without activities or services kept the device busy");
        checks++;

        // A started background service keeps the process, not the device.
        Object background = service(app, PKG, PKG + ".SyncService");
        require(idleSince() == cached, "a started background service restarted the idle clock");
        checks++;

        // A foreground service (navigation, music) keeps the device busy.
        foreground(background, 7);
        require(idleSince() == 0, "a foreground service did not keep the device busy");
        checks++;
        foreground(background, 0);
        long after = idleSince();
        require(after != 0 && after >= cached, "a service leaving the foreground left the device busy");
        checks++;

        // GMS in the foreground serves the apps: it does not hold the device.
        Object gms = process(990_002, GMS, true);
        Object gmsService = service(gms, GMS, "org.microg.gms.PersistentService");
        foreground(gmsService, 1);
        require(idleSince() == after, "a GMS foreground service kept the device busy");
        checks++;

        // A process started for background work (a job) holds the shutdown until it has
        // attached but does not restart the idle clock.
        Object woken = process(990_003, PKG + ".job", false);
        require(idleSince() == 0, "the device was idle while a process was starting");
        synchronized (atm) { field(processClass, "ready").setBoolean(woken, true); }
        require(idleSince() == after, "a background process start restarted the idle clock");
        checks++;

        // A job rescheduled every 30 s by a cached app keeps an idle device one more timeout,
        // not forever; work due now (a running job) keeps it.
        Class<?> work = Class.forName("com.gesher.server.DeviceWork");
        Class<?> source = Class.forName("com.gesher.server.DeviceWork$Source");
        List<Object> sources = (List<Object>) field(work, "sSources").get(null);
        sources.clear();
        long[] due = {Long.MAX_VALUE};
        Object telemetry = Proxy.newProxyInstance(source.getClassLoader(), new Class<?>[]{source},
                (proxy, method, a) -> method.getName().equals("nextDueElapsed") ? (Object) due[0]
                        : method.getName().equals("hashCode") ? (Object) System.identityHashCode(proxy)
                        : method.getName().equals("equals") ? (Object) (proxy == a[0]) : "telemetry");
        Method add = work.getDeclaredMethod("add", source);
        add.setAccessible(true);
        add.invoke(null, telemetry);
        Method keeps = work.getDeclaredMethod("keepsIdleDevice", long.class, long.class);
        long timeout = 600_000;
        require(!(Boolean) keeps.invoke(null, timeout, timeout), "no scheduled work kept the device");
        due[0] = SystemClock.elapsedRealtime() + 30_000;
        require((Boolean) keeps.invoke(null, timeout, timeout),
                "work due within the timeout did not keep the device one more timeout");
        require((Boolean) keeps.invoke(null, 2 * timeout - 5_000, timeout),
                "work due within the timeout did not keep the device one more timeout");
        require(!(Boolean) keeps.invoke(null, 2 * timeout, timeout),
                "a job rescheduled every 30 s kept an idle device beyond twice its timeout");
        require(!(Boolean) keeps.invoke(null, 10 * timeout, timeout),
                "a job rescheduled every 30 s kept an idle device forever");
        due[0] = SystemClock.elapsedRealtime();
        require((Boolean) keeps.invoke(null, 10 * timeout, timeout),
                "a running job did not keep the device");
        due[0] = SystemClock.elapsedRealtime() + 2 * timeout;
        require(!(Boolean) keeps.invoke(null, timeout, timeout), "work due after the timeout kept the device");
        sources.clear();
        checks++;

        System.out.println("DeviceIdleShutdownTest all passed: " + checks);
    }
}
