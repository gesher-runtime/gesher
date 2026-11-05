/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package gesher.services;

import android.app.IApplicationThread;
import android.app.Service;
import android.app.ServiceStartArgs;
import android.content.ComponentName;
import android.content.Intent;
import android.content.pm.ParceledListSlice;
import android.content.pm.ServiceInfo;
import android.content.res.CompatibilityInfo;
import android.os.Binder;
import android.os.IBinder;

import java.lang.reflect.Constructor;
import java.lang.reflect.Field;
import java.lang.reflect.Method;
import java.util.Collections;
import java.util.List;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;

/** Actual ActiveServices lifecycle callbacks with local fake application threads; no APK code runs. */
public final class ServiceRecoveryTest {
    private static void require(boolean value, String message) {
        if (!value) throw new AssertionError(message);
    }

    private static Field field(Class<?> type, String name) throws Exception {
        Field field = type.getDeclaredField(name);
        field.setAccessible(true);
        return field;
    }

    private static Method method(Class<?> type, String name, Class<?>... parameters) throws Exception {
        Method method = type.getDeclaredMethod(name, parameters);
        method.setAccessible(true);
        return method;
    }

    private static Object construct(Class<?> type, Class<?>[] parameters, Object... values) throws Exception {
        Constructor<?> constructor = type.getDeclaredConstructor(parameters);
        constructor.setAccessible(true);
        return constructor.newInstance(values);
    }

    private static final class AppThread extends IApplicationThread.Default {
        final Binder binder = new Binder();
        final CountDownLatch started = new CountDownLatch(1);
        IBinder service;
        ServiceStartArgs args;
        @Override public IBinder asBinder() { return binder; }
        @Override public void scheduleCreateService(IBinder token, ServiceInfo info,
                CompatibilityInfo compat, int processState) { service = token; }
        @Override public void scheduleServiceArgs(IBinder token, ParceledListSlice starts) {
            List<ServiceStartArgs> items = starts.getList();
            if (!items.isEmpty()) args = items.get(items.size() - 1);
            started.countDown();
        }
    }

    private static void exercise(Object apk, Class<?> apkClass, int result,
            boolean exiting, boolean forceStop) throws Exception {
        Class<?> pmClass = Class.forName("com.gesher.server.PackageManagerService");
        Class<?> atmClass = Class.forName("com.gesher.server.ActivityTaskManagerService");
        Class<?> processClass = Class.forName("com.gesher.server.ActivityTaskManagerService$ProcessRecord");
        Class<?> servicesClass = Class.forName("com.gesher.server.ActiveServices");
        Class<?> notificationsClass = Class.forName("com.gesher.server.NotificationManagerService");
        Object pm = construct(pmClass, new Class<?>[]{java.util.Collection.class}, Collections.singletonList(apk));
        Object atm = construct(atmClass, new Class<?>[]{pmClass}, pm);
        Object services = construct(servicesClass, new Class<?>[]{atmClass, pmClass, notificationsClass},
                atm, pm, construct(notificationsClass, new Class<?>[0]));
        String pkg = (String) apkClass.getField("packageName").get(apk);
        List<Object> processes = (List<Object>) field(atmClass, "mProcesses").get(atm);
        AppThread oldThread = new AppThread();
        Object oldProcess = construct(processClass,
                new Class<?>[]{atmClass, int.class, IApplicationThread.class}, atm, 999_991, oldThread);
        field(processClass, "packageName").set(oldProcess, pkg);
        field(processClass, "ready").setBoolean(oldProcess, true);
        synchronized (atm) { processes.add(oldProcess); }
        Intent start = new Intent().setComponent(new ComponentName(pkg, "org.microg.gms.gcm.McsService"))
                .putExtra("recovery-probe", "preserved");
        method(servicesClass, "startService", IApplicationThread.class, Intent.class,
                String.class, boolean.class, String.class).invoke(services, null, start, null, false, pkg);
        require(oldThread.started.await(2, TimeUnit.SECONDS), "initial service start did not run");
        method(servicesClass, "serviceDoneExecuting", IBinder.class, int.class, int.class, int.class)
                .invoke(services, oldThread.service, 1, oldThread.args.startId, result);
        synchronized (atm) {
            processes.remove(oldProcess);
            field(processClass, "exiting").setBoolean(oldProcess, exiting);
        }
        method(servicesClass, "processDied", processClass).invoke(services, oldProcess);
        if (forceStop) method(servicesClass, "stopPackage", String.class).invoke(services, pkg);

        AppThread nextThread = new AppThread();
        Object nextProcess = construct(processClass,
                new Class<?>[]{atmClass, int.class, IApplicationThread.class}, atm, 999_992, nextThread);
        field(processClass, "packageName").set(nextProcess, pkg);
        field(processClass, "ready").setBoolean(nextProcess, true);
        synchronized (atm) { processes.add(nextProcess); }
        boolean restarted = nextThread.started.await(1800, TimeUnit.MILLISECONDS);
        boolean expected = !exiting && !forceStop && result != Service.START_NOT_STICKY;
        require(restarted == expected, "restart policy mismatch for result=" + result
                + " exiting=" + exiting + " forceStop=" + forceStop);
        if (restarted && result == Service.START_REDELIVER_INTENT) {
            require(nextThread.args.args != null
                            && "preserved".equals(nextThread.args.args.getStringExtra("recovery-probe"))
                            && (nextThread.args.flags & Service.START_FLAG_REDELIVERY) != 0,
                    "redelivery lost the original intent/flag");
        }
        if (restarted && result == Service.START_STICKY) {
            require(nextThread.args.args == null, "sticky restart must receive a null intent");
        }
        synchronized (atm) { field(processClass, "ready").setBoolean(nextProcess, false); }
        method(servicesClass, "stopPackage", String.class).invoke(services, pkg);
    }

    public static void main(String[] args) throws Exception {
        Class<?> apkClass = Class.forName("com.gesher.server.ApkPackage");
        Object apk = apkClass.getMethod("parse", String.class).invoke(null, args[0]);
        exercise(apk, apkClass, Service.START_REDELIVER_INTENT, false, false);
        exercise(apk, apkClass, Service.START_STICKY, false, false);
        exercise(apk, apkClass, Service.START_NOT_STICKY, false, false);
        exercise(apk, apkClass, Service.START_REDELIVER_INTENT, true, false);
        exercise(apk, apkClass, Service.START_REDELIVER_INTENT, false, true);
        System.out.println("ServiceRecoveryTest: redelivery, sticky, non-sticky and force-stop policies passed");
    }
}
