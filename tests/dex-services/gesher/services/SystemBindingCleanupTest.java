/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package gesher.services;

import android.accounts.Account;
import android.app.IActivityManager;
import android.app.IApplicationThread;
import android.app.IServiceConnection;
import android.app.job.IJobCallback;
import android.app.job.JobInfo;
import android.content.ComponentName;
import android.content.Context;
import android.content.Intent;
import android.content.ISyncContext;
import android.content.SyncResult;
import android.content.pm.ServiceInfo;
import android.content.res.AssetManager;
import android.content.res.CompatibilityInfo;
import android.os.Binder;
import android.os.Bundle;
import android.os.IBinder;

import java.lang.reflect.Constructor;
import java.lang.reflect.Field;
import java.lang.reflect.Method;
import java.nio.file.Files;
import java.util.Collection;
import java.util.Collections;
import java.util.List;
import java.util.Map;

/** Real service-owner guard and internal job/sync completion cleanup; APK metadata only. */
public final class SystemBindingCleanupTest {
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
    private static final class AppThread extends IApplicationThread.Default {
        final Binder binder = new Binder();
        int created, stopped, exits;
        Runnable onExit;
        @Override public IBinder asBinder() { return binder; }
        @Override public void scheduleCreateService(IBinder token, ServiceInfo info,
                CompatibilityInfo compatibility, int processState) { created++; }
        @Override public void scheduleStopService(IBinder token) { stopped++; }
        @Override public void scheduleExit() {
            exits++;
            if (onExit != null) onExit.run();
        }
    }

    private static final class Harness {
        final Class<?> pmType = Class.forName("com.gesher.server.PackageManagerService");
        final Class<?> atmType = Class.forName("com.gesher.server.ActivityTaskManagerService");
        final Class<?> amType = Class.forName("com.gesher.server.ActivityManagerService");
        final Class<?> processType = Class.forName(atmType.getName() + "$ProcessRecord");
        final Object pm, atm, app, services;
        final IActivityManager am;
        final AppThread thread = new AppThread();
        final String owner;
        final ComponentName component;
        final int appPid = 781231;
        final int appUid;

        @SuppressWarnings("unchecked")
        Harness(Object apk) throws Exception {
            owner = (String) apk.getClass().getField("packageName").get(apk);
            appUid = (Integer) apk.getClass().getMethod("uid").invoke(apk);
            component = new ComponentName(owner, owner + ".TestJobService");
            pm = construct(pmType, new Class<?>[]{Collection.class}, Collections.singletonList(apk));
            atm = construct(atmType, new Class<?>[]{pmType}, pm);
            Class<?> notifications = Class.forName("com.gesher.server.NotificationManagerService");
            am = (IActivityManager) construct(amType, new Class<?>[]{atmType, pmType, notifications},
                    atm, pm, construct(notifications, new Class<?>[0]));
            services = field(amType, "mServices").get(am);
            long original = Binder.clearCallingIdentity();
            try {
                asApp();
                app = construct(processType, new Class<?>[]{atmType, int.class, IApplicationThread.class},
                        atm, appPid, thread);
            } finally { Binder.restoreCallingIdentity(original); }
            field(processType, "packageName").set(app, owner);
            field(processType, "processName").set(app, owner);
            field(processType, "ready").setBoolean(app, true);
            ((List<Object>) field(atmType, "mProcesses").get(atm)).add(app);
        }
        void asApp() {
            // Binder's real thread-local calling identity, not a claimed API argument.
            // This models entry from the attached app's IJobCallback/ISyncContext call.
            Binder.restoreCallingIdentity(((long) appUid << 32) | (appPid & 0xffffffffL));
            require(Binder.getCallingPid() == appPid && Binder.getCallingUid() == appUid,
                    "Native Binder calling identity injection is unavailable");
        }
        void bind(IServiceConnection connection) throws Exception {
            require(am.bindService(null, null, new Intent().setComponent(component), null, connection,
                    Context.BIND_AUTO_CREATE, "android", 0) == 1, "System fixture binding failed");
            require(thread.created == 1 && connectionCount() == 1,
                    "Real ActiveServices did not retain the system-owned auto-create binding");
        }
        int connectionCount() throws Exception {
            return ((Map<?, ?>) field(services.getClass(), "mConnections").get(services)).size();
        }
        void directAppUnbindDenied(IServiceConnection connection) throws Exception {
            try { am.unbindService(connection); }
            catch (SecurityException expected) {
                require(connectionCount() == 1, "Denied direct unbind mutated the owner connection"); return;
            }
            throw new AssertionError("An app directly unbound the system-owned connection");
        }
        /** The idle reaper, as if the process (spawned by the server) had been idle long. */
        void reapNow() throws Exception {
            field(processType, "spawned").setBoolean(app, true);
            field(processType, "lastActive").setLong(app, 0L);
            method(atmType, "reapIdleProcesses", long.class).invoke(atm, 1L);
        }
        void cleaned() throws Exception {
            require(Binder.getCallingPid() == appPid && Binder.getCallingUid() == appUid,
                    "Internal cleanup failed to restore the app's Binder identity");
            require(connectionCount() == 0
                    && ((Map<?, ?>) field(services.getClass(), "mServices").get(services)).isEmpty(),
                    "Completion retained an auto-create connection capable of restarting the app");
            require(thread.stopped == 1 && thread.exits == 1,
                    "Completion did not release its bound-only service and process");
        }
    }

    @SuppressWarnings("unchecked")
    private static void job(Object apk) throws Exception {
        Harness h = new Harness(apk);
        Class<?> schedulerType = Class.forName("com.gesher.server.JobSchedulerService");
        Class<?> jobType = Class.forName(schedulerType.getName() + "$Job");
        Class<?> runType = Class.forName(schedulerType.getName() + "$Run");
        Object scheduler = construct(schedulerType, new Class<?>[]{h.amType, h.atmType, h.pmType}, h.am, h.atm, h.pm);
        JobInfo info = new JobInfo.Builder(8412, h.component).setMinimumLatency(1).build();
        Object job = construct(jobType, new Class<?>[]{schedulerType, String.class, String.class, JobInfo.class},
                scheduler, h.owner, null, info);
        Object run = construct(runType, new Class<?>[]{schedulerType, jobType}, scheduler, job);
        field(jobType, "run").set(job, run);
        ((List<Object>) field(schedulerType, "mJobs").get(scheduler)).add(job);
        field(schedulerType, "mRunning").setInt(scheduler, 1);
        IServiceConnection connection = (IServiceConnection) field(runType, "connection").get(run);
        // Use the production job bind path, not a test-specific connection registry.
        method(schedulerType, "bind", runType).invoke(scheduler, run);
        require(h.connectionCount() == 1 && h.thread.created == 1, "Job did not establish a real system binding");
        h.reapNow();
        require(h.thread.exits == 0, "The idle reaper ended a process whose service a running job binds");
        long original = Binder.clearCallingIdentity();
        try {
            h.asApp();
            h.directAppUnbindDenied(connection);
            ((IJobCallback) run).jobFinished(info.getId(), false);
            h.cleaned();
            ((IJobCallback) run).jobFinished(info.getId(), false);
            require(h.thread.stopped == 1 && field(schedulerType, "mRunning").getInt(scheduler) == 0
                    && ((List<?>) field(schedulerType, "mJobs").get(scheduler)).isEmpty(),
                    "Duplicate/stale completion changed a finished job's state");
        } finally { Binder.restoreCallingIdentity(original); }
        System.out.println("SystemBindingCleanupTest: app cannot unbind host job connection; jobFinished releases auto-create service and restores caller identity");
    }

    @SuppressWarnings("unchecked")
    private static void sync(Object apk) throws Exception {
        Harness h = new Harness(apk);
        Class<?> syncType = Class.forName("com.gesher.server.SyncManager");
        Class<?> adapterType = Class.forName(syncType.getName() + "$Adapter");
        Class<?> activeType = Class.forName(syncType.getName() + "$Active");
        Object sync = construct(syncType, new Class<?>[]{h.amType, h.pmType}, h.am, h.pm);
        Object adapter = construct(adapterType, new Class<?>[]{String.class, String.class, ComponentName.class,
                boolean.class, boolean.class, boolean.class, boolean.class, String.class},
                "fixture.authority", "fixture.account.type", h.component, true, true, true, false, null);
        Object active = construct(activeType, new Class<?>[]{syncType, String.class, adapterType, Account.class, Bundle.class},
                sync, "fixture-key", adapter, new Account("fixture@example.test", "fixture.account.type"), new Bundle());
        IServiceConnection connection = new IServiceConnection.Stub() {
            @Override public void connected(ComponentName name, IBinder binder,
                    android.app.IBinderSession session, boolean dead) { }
        };
        field(activeType, "connection").set(active, connection);
        ((Map<String, Object>) field(syncType, "mActive").get(sync)).put("fixture-key", active);
        h.bind(connection);
        long original = Binder.clearCallingIdentity();
        try {
            h.asApp();
            h.directAppUnbindDenied(connection);
            ((ISyncContext) active).onFinished(new SyncResult());
            h.cleaned();
            ((ISyncContext) active).onFinished(new SyncResult());
            require(h.thread.stopped == 1 && ((Map<?, ?>) field(syncType, "mActive").get(sync)).isEmpty(),
                    "Duplicate sync completion changed a finished binding");
        } finally { Binder.restoreCallingIdentity(original); }
        System.out.println("SystemBindingCleanupTest: adapter cannot directly unbind system connection; onFinished releases it and restores caller identity");
    }

    /**
     * A service bound only by its own process does not keep the process in use: maybeExit
     * keeps it (hasServices), the idle reaper ends it (a cached process, as OomAdjuster
     * ignores a binding to oneself).
     */
    private static void selfBinding(Object apk) throws Exception {
        Harness h = new Harness(apk);
        IServiceConnection connection = new IServiceConnection.Stub() {
            @Override public void connected(ComponentName name, IBinder binder,
                    android.app.IBinderSession session, boolean dead) { }
        };
        long original = Binder.clearCallingIdentity();
        try {
            h.asApp();
            require(h.am.bindService(h.thread, null, new Intent().setComponent(h.component), null,
                    connection, Context.BIND_AUTO_CREATE, h.owner, 0) == 1, "Self binding failed");
        } finally { Binder.restoreCallingIdentity(original); }
        require(h.thread.created == 1 && h.connectionCount() == 1, "Self binding did not create the service");
        method(h.atmType, "maybeExit", h.processType).invoke(h.atm, h.app);
        require(h.thread.exits == 0, "maybeExit ended a process with a running service");
        field(h.processType, "spawned").setBoolean(h.app, true);
        method(h.atmType, "reapIdleProcesses", long.class).invoke(h.atm, 3_600_000L);
        require(h.thread.exits == 0, "The idle reaper ended a recently used process");
        Map<?, ?> records = (Map<?, ?>) field(h.services.getClass(), "mServices").get(h.services);
        Object oldService = records.get(h.component);
        Object providers = field(h.amType, "mProviders").get(h.am);
        Object broadcasts = field(h.amType, "mBroadcasts").get(h.am);
        IServiceConnection newcomer = new IServiceConnection.Stub() {
            @Override public void connected(ComponentName name, IBinder binder,
                    android.app.IBinderSession session, boolean dead) { }
        };
        h.thread.onExit = () -> {
            require(!Thread.holdsLock(h.services) && !Thread.holdsLock(broadcasts)
                            && !Thread.holdsLock(h.atm) && !Thread.holdsLock(providers),
                    "The reaper invoked application code while controller locks were held");
            try {
                require(records.isEmpty() && h.connectionCount() == 0,
                        "A committed cached exit still exposed its old self-bound service");
                require(h.am.bindService(null, null, new Intent().setComponent(h.component), null,
                        newcomer, Context.BIND_AUTO_CREATE, "android", 0) == 1,
                        "A binding admitted after retirement was lost");
            } catch (Exception failure) { throw new AssertionError("Late service binding failed", failure); }
        };
        h.reapNow();
        require(h.thread.exits == 1, "The idle reaper kept a process only its own binding uses");
        Object replacementService = records.get(h.component);
        require(replacementService != null && replacementService != oldService && h.connectionCount() == 1
                        && field(replacementService.getClass(), "app").get(replacementService) != h.app
                        && h.thread.created == 1,
                "A new client was attached to the exiting process's old service");
        method(h.services.getClass(), "processDied", h.processType).invoke(h.services, h.app);
        require(records.get(h.component) == replacementService && h.connectionCount() == 1,
                "Old process cleanup erased a replacement service's binding");
        System.out.println("SystemBindingCleanupTest: cached exit retires self-bindings; late binding targets replacement and survives old cleanup");
    }

    /** Unordered registered callbacks have no finishReceiver, but still count as recent use. */
    private static void registeredBroadcast(Object apk) throws Exception {
        Harness h = new Harness(apk);
        android.content.IIntentReceiver receiver = new android.content.IIntentReceiver.Default() {
            private final Binder binder = new Binder();
            @Override public IBinder asBinder() { return binder; }
        };
        String action = "gesher.services.IDLE_REAPER_REGISTERED";
        long original = Binder.clearCallingIdentity();
        try {
            h.asApp();
            h.am.registerReceiver(h.thread, h.owner, receiver, new android.content.IntentFilter(action),
                    null, 0, Context.RECEIVER_NOT_EXPORTED);
        } finally { Binder.restoreCallingIdentity(original); }
        Field lastActive = field(h.processType, "lastActive");
        field(h.processType, "spawned").setBoolean(h.app, true);
        lastActive.setLong(h.app, 0);
        h.am.broadcastIntent(null, new Intent(action).setPackage(h.owner), null, null,
                0, null, null, null, android.app.AppOpsManager.OP_NONE, null, false, false, 0);
        require(lastActive.getLong(h.app) > 0, "Registered receiver admission did not refresh process activity");
        method(h.atmType, "reapIdleProcesses", long.class).invoke(h.atm, 3_600_000L);
        require(h.thread.exits == 0, "The reaper ignored an admitted registered receiver's idle grace");
        field(h.processType, "exiting").setBoolean(h.app, true);
        lastActive.setLong(h.app, 0);
        h.am.broadcastIntent(null, new Intent(action).setPackage(h.owner), null, null,
                0, null, null, null, android.app.AppOpsManager.OP_NONE, null, false, false, 0);
        require(lastActive.getLong(h.app) == 0, "Registered receiver admission selected a retired process");
        System.out.println("SystemBindingCleanupTest: registered broadcasts refresh live process activity and reject retired targets");
    }

    /** Pending work becomes recent use before readiness removes its queue reservation. */
    private static void pendingAttachActivity(Object apk) throws Exception {
        Harness h = new Harness(apk);
        Field ready = field(h.processType, "ready");
        Field lastActive = field(h.processType, "lastActive");
        Field exiting = field(h.processType, "exiting");
        ready.setBoolean(h.app, false);
        field(h.processType, "spawned").setBoolean(h.app, true);
        Object providers = field(h.amType, "mProviders").get(h.am);
        Object broadcasts = field(h.amType, "mBroadcasts").get(h.am);
        List<?> pending = (List<?>) field(h.atmType, "mPendingWork").get(h.atm);
        final long idleMs = 3_600_000L;
        java.util.concurrent.atomic.AtomicInteger deliveries =
                new java.util.concurrent.atomic.AtomicInteger();
        java.util.function.Consumer<Object> work = process -> {
            require(process == h.app && !Thread.holdsLock(h.services)
                            && !Thread.holdsLock(broadcasts) && !Thread.holdsLock(h.atm)
                            && !Thread.holdsLock(providers),
                    "Pending attach callback used the wrong process or ran under a controller lock");
            try {
                require(pending.isEmpty(), "Pending callback was not removed before dispatch");
                // Interleave a real sweep after the queue reservation is gone, before
                // the callback establishes a service record or other lasting work.
                method(h.atmType, "reapIdleProcesses", long.class).invoke(h.atm, idleMs);
                require(!exiting.getBoolean(h.app) && h.thread.exits == 0,
                        "The idle reaper retired a process while attached work was admitted");
            } catch (Exception failure) {
                throw new AssertionError("Pending attach activity regression", failure);
            }
            deliveries.incrementAndGet();
        };
        method(h.atmType, "withProcess", String.class, String.class, String.class,
                java.util.function.Consumer.class).invoke(h.atm, h.owner, h.owner,
                        "pending attach activity regression", work);
        require(deliveries.get() == 0 && pending.size() == 1,
                "Unready process work was not retained for attachment");
        // Model a slow bindApplication without sleeps or requiring a long host uptime.
        lastActive.setLong(h.app, android.os.SystemClock.uptimeMillis() - idleMs - 1);
        long beforeAttach = android.os.SystemClock.uptimeMillis();
        method(h.atmType, "finishAttachApplication", int.class).invoke(h.atm, h.appPid);
        require(deliveries.get() == 1 && lastActive.getLong(h.app) >= beforeAttach,
                "Queued work did not receive a fresh attachment activity stamp");
        System.out.println("SystemBindingCleanupTest: queued attach work receives idle grace before dispatch");
    }

    /** Concurrent uses must retain the newest stamp regardless of write order. */
    private static void activityClock(Object apk) throws Exception {
        Harness h = new Harness(apk);
        Method markActive = method(h.processType, "markActive", long.class);
        Field lastActive = field(h.processType, "lastActive");
        java.util.concurrent.CyclicBarrier barrier = new java.util.concurrent.CyclicBarrier(3);
        java.util.concurrent.atomic.AtomicReference<Throwable> failure =
                new java.util.concurrent.atomic.AtomicReference<>();
        Thread[] workers = new Thread[2];
        for (int i = 0; i < workers.length; i++) {
            final long time = i + 1;
            workers[i] = new Thread(() -> {
                try {
                    for (int round = 0; round < 512; round++) {
                        barrier.await(3, java.util.concurrent.TimeUnit.SECONDS);
                        markActive.invoke(h.app, time);
                        barrier.await(3, java.util.concurrent.TimeUnit.SECONDS);
                    }
                } catch (Throwable problem) { failure.compareAndSet(null, problem); }
            }, "process-activity-clock-" + time);
            workers[i].setDaemon(true);
            workers[i].start();
        }
        try {
            for (int round = 0; round < 512; round++) {
                lastActive.setLong(h.app, 0);
                barrier.await(3, java.util.concurrent.TimeUnit.SECONDS);
                barrier.await(3, java.util.concurrent.TimeUnit.SECONDS);
                require(lastActive.getLong(h.app) == 2, "Concurrent activity moved the process clock backward");
            }
            markActive.invoke(h.app, 1L);
            require(lastActive.getLong(h.app) == 2, "An older activity moved the process clock backward");
        } finally {
            for (Thread worker : workers) worker.interrupt();
            for (Thread worker : workers) worker.join(3_000);
        }
        if (failure.get() != null) throw new AssertionError("Concurrent activity clock failed", failure.get());
        require(!workers[0].isAlive() && !workers[1].isAlive(), "Activity clock workers did not finish");
        System.out.println("SystemBindingCleanupTest: concurrent and older activity stamps remain monotonic");
    }

    public static void main(String[] args) throws Exception {
        require(args.length == 1, "Pass the local devsvc.apk fixture (metadata only)");
        System.loadLibrary("android_runtime");
        AssetManager.createSystemAssetsInZygoteLocked(false,
                System.getenv("ANDROID_ROOT") + "/framework/framework-res.apk");
        android.system.Os.setenv("ANDROID_DATA", Files.createTempDirectory("gesher-system-binding-cleanup-").toString(), true);
        android.system.Os.setenv("GESHER_MICROG_SPOOF", "1", true);
        Object apk = Class.forName("com.gesher.server.ApkPackage").getMethod("parse", String.class).invoke(null, args[0]);
        job(apk);
        sync(apk);
        selfBinding(apk);
        registeredBroadcast(apk);
        pendingAttachActivity(apk);
        activityClock(apk);
        System.out.println("SystemBindingCleanupTest all passed");
    }
}
