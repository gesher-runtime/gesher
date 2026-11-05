/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package gesher.services;

import android.app.IActivityManager;
import android.app.IApplicationThread;
import android.content.Intent;
import android.content.pm.ActivityInfo;
import android.content.pm.PackageManager;
import android.content.res.AssetManager;
import android.content.res.CompatibilityInfo;
import android.os.Binder;
import android.os.Bundle;
import android.os.IBinder;
import android.os.RemoteException;

import java.lang.reflect.Constructor;
import java.lang.reflect.Field;
import java.lang.reflect.Method;
import java.nio.file.Files;
import java.util.Collection;
import java.util.Collections;
import java.util.List;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;

/** Actual manifest delivery/keepalive and timeout policy. GmsCore is metadata only. */
public final class BroadcastLifecycleTest {
    private static void require(boolean value, String message) {
        if (!value) throw new AssertionError(message);
    }
    private static Field field(Class<?> type, String name) throws Exception {
        Field result = type.getDeclaredField(name); result.setAccessible(true); return result;
    }
    private static Method method(Class<?> type, String name, Class<?>... parameters) throws Exception {
        Method result = type.getDeclaredMethod(name, parameters); result.setAccessible(true); return result;
    }
    private static Object construct(Class<?> type, Class<?>[] parameters, Object... values) throws Exception {
        Constructor<?> result = type.getDeclaredConstructor(parameters);
        result.setAccessible(true); return result.newInstance(values);
    }
    private interface Operation { void run() throws Exception; }
    private static void denied(Operation operation) throws Exception {
        try { operation.run(); }
        catch (SecurityException expected) { return; }
        throw new AssertionError("Forged or stale receiver completion was accepted");
    }

    private static final class AppThread extends IApplicationThread.Default {
        final Binder binder = new Binder();
        final CountDownLatch exited = new CountDownLatch(1);
        volatile int received;
        volatile int exits;
        boolean failScheduling;
        Runnable onReceive;
        Runnable onExit;
        @Override public IBinder asBinder() { return binder; }
        @Override public void scheduleReceiver(Intent intent, ActivityInfo info,
                CompatibilityInfo compat, int resultCode, String data, Bundle extras,
                boolean ordered, boolean assumeDelivered, int sendingUser,
                int processState, int sendingUid, String sendingPackage) throws RemoteException {
            received++;
            if (onReceive != null) onReceive.run();
            if (failScheduling) throw new RemoteException("fixture dead application thread");
        }
        @Override public void scheduleExit() {
            exits++;
            if (onExit != null) onExit.run();
            exited.countDown();
        }
    }

    private static final class Harness {
        final Object atm;
        final IActivityManager am;
        final Object broadcasts;
        final Class<?> atmType;
        final Class<?> processType;
        final Method hasReceivers;
        final Method broadcast;
        final String owner;
        final int receiverCount;
        final List<Object> processes;
        AppThread thread;
        Object app;

        @SuppressWarnings("unchecked")
        Harness(Object apk, long timeoutMillis) throws Exception {
            Class<?> packageType = Class.forName("com.gesher.server.PackageManagerService");
            atmType = Class.forName("com.gesher.server.ActivityTaskManagerService");
            Class<?> amType = Class.forName("com.gesher.server.ActivityManagerService");
            Class<?> notifications = Class.forName("com.gesher.server.NotificationManagerService");
            Class<?> controller = Class.forName("com.gesher.server.BroadcastController");
            processType = Class.forName(atmType.getName() + "$ProcessRecord");
            owner = (String) apk.getClass().getField("packageName").get(apk);
            Object pm = construct(packageType, new Class<?>[]{Collection.class}, Collections.singletonList(apk));
            atm = construct(atmType, new Class<?>[]{packageType}, pm);
            am = (IActivityManager) construct(amType, new Class<?>[]{atmType, packageType, notifications},
                    atm, pm, construct(notifications, new Class<?>[0]));
            broadcasts = construct(controller, new Class<?>[]{atmType, packageType, long.class}, atm, pm, timeoutMillis);
            field(amType, "mBroadcasts").set(am, broadcasts);
            hasReceivers = method(controller, "hasReceivers", processType);
            broadcast = method(controller, "broadcastIntent", Intent.class, String.class, String.class, int.class);
            processes = (List<Object>) field(atmType, "mProcesses").get(atm);
            receiverCount = ((List<?>) method(packageType, "receivers", Intent.class, String.class, long.class)
                    .invoke(pm, boot(), null, (long) PackageManager.GET_META_DATA)).size();
            require(receiverCount >= 2, "Fixture must have multiple BOOT_COMPLETED receivers");
            newProcess();
        }
        Intent boot() { return new Intent(Intent.ACTION_BOOT_COMPLETED).setPackage(owner); }
        void newProcess() throws Exception {
            thread = new AppThread();
            app = construct(processType, new Class<?>[]{atmType, int.class, IApplicationThread.class},
                    atm, Binder.getCallingPid(), thread);
            field(processType, "packageName").set(app, owner);
            field(processType, "ready").setBoolean(app, true);
            synchronized (atm) { processes.add(app); }
        }
        int queued() throws Exception {
            synchronized (broadcasts) { return ((List<?>) field(broadcasts.getClass(), "mManifest").get(broadcasts)).size(); }
        }
        boolean keptAlive() throws Exception { return (Boolean) hasReceivers.invoke(broadcasts, app); }
        boolean exiting() throws Exception { return field(processType, "exiting").getBoolean(app); }
        void send() throws Exception { broadcast.invoke(broadcasts, boot(), null, null, Binder.getCallingPid()); }
        void finish(IBinder token) throws Exception { am.finishReceiver(token, 0, null, null, false, 0); }
        void finish() throws Exception { finish(thread.binder); }
        void finishAll() throws Exception {
            for (int remaining = receiverCount; remaining > 0; remaining--) {
                require(queued() == remaining && keptAlive(), "Receiver queue lost its keepalive reservation");
                require(thread.received == receiverCount - remaining + 1,
                        "More than one manifest receiver was dispatched before completion");
                finish();
                if (remaining > 1) require(thread.exits == 0 && !exiting(), "A partial batch ended its process");
            }
            require(queued() == 0 && !keptAlive() && thread.exits == 1 && exiting(),
                    "Completed receiver batch retained or prematurely ended the process");
        }
    }

    public static void main(String[] args) throws Exception {
        require(args.length == 1, "Pass an official GmsCore APK for offline receiver metadata");
        System.loadLibrary("android_runtime");
        AssetManager.createSystemAssetsInZygoteLocked(false,
                System.getenv("ANDROID_ROOT") + "/framework/framework-res.apk");
        android.system.Os.setenv("ANDROID_DATA", Files.createTempDirectory("gesher-broadcast-lifecycle-").toString(), true);
        android.system.Os.setenv("GESHER_MICROG_SPOOF", "1", true);
        Object apk = Class.forName("com.gesher.server.ApkPackage").getMethod("parse", String.class).invoke(null, args[0]);

        Harness fast = new Harness(apk, 60_000);
        fast.thread.onReceive = () -> {
            try {
                require(!fast.exiting(), "A fast receiver callback reached an exiting process");
                if (fast.thread.received == 1) require(fast.queued() == fast.receiverCount,
                        "First callback ran before the entire receiver batch was reserved");
                fast.finish();
            } catch (Exception e) { throw new AssertionError(e); }
        };
        fast.thread.onExit = () -> require(fast.thread.received == fast.receiverCount,
                "First receiver completion exited before the remaining receivers ran");
        fast.send();
        require(fast.thread.received == fast.receiverCount && fast.thread.exits == 1 && fast.queued() == 0,
                "Fast manifest completion lost work or leaked reservations");
        System.out.println("BroadcastLifecycleTest: entire BOOT batch reserved before fast first completion; final completion alone releases keepalive");

        Harness authenticated = new Harness(apk, 60_000);
        authenticated.send();
        denied(() -> authenticated.finish(new Binder()));
        synchronized (authenticated.atm) { authenticated.processes.remove(authenticated.app); }
        denied(authenticated::finish);
        synchronized (authenticated.atm) { authenticated.processes.add(authenticated.app); }
        Field uid = field(authenticated.processType, "uid");
        int actualUid = uid.getInt(authenticated.app);
        uid.setInt(authenticated.app, actualUid + 1);
        try { denied(authenticated::finish); } finally { uid.setInt(authenticated.app, actualUid); }
        require(authenticated.queued() == authenticated.receiverCount && authenticated.thread.received == 1,
                "Forged thread, unknown PID or UID mismatch consumed a receiver reservation");
        authenticated.finishAll();
        System.out.println("BroadcastLifecycleTest: wrong thread, unattached PID and UID mismatch cannot complete queued manifest work");

        Harness timedOut = new Harness(apk, 1000);
        AppThread oldThread = timedOut.thread;
        Object oldApp = timedOut.app;
        timedOut.send();
        require(oldThread.exited.await(5, TimeUnit.SECONDS), "Receiver timeout did not retire its app generation");
        require(timedOut.queued() == 0 && oldThread.received == 1 && !timedOut.processes.contains(oldApp),
                "Timeout dispatched later work or retained the authenticated old generation");
        require(!field(timedOut.processType, "exiting").getBoolean(oldApp),
                "Receiver timeout changed unexpected service death into package force-stop");
        timedOut.newProcess();
        timedOut.send();
        denied(() -> timedOut.finish(oldThread.binder));
        require(timedOut.queued() == timedOut.receiverCount && timedOut.thread.received == 1,
                "Late old-generation completion consumed the replacement generation's work");
        timedOut.finishAll();
        System.out.println("BroadcastLifecycleTest: real timeout retires only its generation; late completion cannot consume replacement work");

        Harness dead = new Harness(apk, 60_000);
        dead.send();
        method(dead.atmType, "processDied", dead.processType).invoke(dead.atm, dead.app);
        require(dead.queued() == 0 && !dead.keptAlive(), "Process death retained manifest reservations");
        denied(dead::finish);
        Harness replacement = new Harness(apk, 60_000);
        Object priorGeneration = replacement.app;
        synchronized (replacement.atm) { replacement.processes.remove(priorGeneration); }
        replacement.newProcess();
        field(replacement.processType, "ready").setBoolean(replacement.app, false);
        replacement.send();
        require(replacement.queued() == replacement.receiverCount && replacement.thread.received == 0,
                "Starting-process batch was not reserved before attach");
        method(replacement.am.getClass(), "processDied", replacement.processType)
                .invoke(replacement.am, priorGeneration);
        require(replacement.queued() == replacement.receiverCount,
                "Old-generation cleanup deleted unassigned replacement-generation work");
        method(replacement.atmType, "finishAttachApplication", int.class)
                .invoke(replacement.atm, Binder.getCallingPid());
        replacement.finishAll();
        System.out.println("BroadcastLifecycleTest: late old-process cleanup preserves the replacement process's pre-attach reservation");
        Harness failed = new Harness(apk, 60_000);
        failed.thread.failScheduling = true;
        failed.send();
        require(failed.thread.received == failed.receiverCount && failed.queued() == 0 && failed.thread.exits == 1,
                "Failed Binder scheduling leaked a receiver reservation or process");
        System.out.println("BroadcastLifecycleTest: process death and failed Binder scheduling release outstanding reservations");
        System.out.println("BroadcastLifecycleTest all passed");
    }
}
