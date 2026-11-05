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
        @Override public IBinder asBinder() { return binder; }
        @Override public void scheduleCreateService(IBinder token, ServiceInfo info,
                CompatibilityInfo compatibility, int processState) { created++; }
        @Override public void scheduleStopService(IBinder token) { stopped++; }
        @Override public void scheduleExit() { exits++; }
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
        System.out.println("SystemBindingCleanupTest all passed");
    }
}
