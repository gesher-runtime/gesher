/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package gesher.services;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.job.IJobCallback;
import android.app.job.IJobScheduler;
import android.app.job.IJobService;
import android.app.job.JobInfo;
import android.app.job.JobParameters;
import android.app.job.JobService;
import android.app.job.JobWorkItem;
import android.content.ComponentName;
import android.content.pm.ParceledListSlice;
import android.content.res.AssetManager;
import android.graphics.drawable.Icon;
import android.os.Binder;
import android.os.IBinder;
import android.os.Parcel;
import android.os.RemoteException;
import android.service.notification.StatusBarNotification;
import com.gesher.server.NotificationManagerService;
import java.io.File;
import java.lang.reflect.Constructor;
import java.lang.reflect.Field;
import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Method;
import java.nio.file.Files;
import java.util.Collection;
import java.util.List;
import java.util.Set;
import java.util.function.ToIntBiFunction;

/** Actual job callbacks, notification publication, shared ownership and retained policy. */
public final class JobNotificationLifecycleTest {
    private static final int DETACH = JobService.JOB_END_NOTIFICATION_POLICY_DETACH;
    private static final int REMOVE = JobService.JOB_END_NOTIFICATION_POLICY_REMOVE;
    private static int checks;
    private interface Work { void run() throws Exception; }
    private static void require(boolean condition, String reason) {
        if (!condition) throw new AssertionError(reason);
        checks++;
    }
    private static void rejects(Class<? extends Throwable> type, Work work, String reason) throws Exception {
        try { work.run(); }
        catch (Throwable error) {
            if (error instanceof InvocationTargetException) error = error.getCause();
            if (type.isInstance(error)) { checks++; return; }
            throw new AssertionError(reason + ": wrong exception", error);
        }
        throw new AssertionError(reason);
    }
    private static Field field(Class<?> type, String name) throws Exception {
        Field value = type.getDeclaredField(name); value.setAccessible(true); return value;
    }
    private static Method method(Class<?> type, String name, Class<?>... params) throws Exception {
        Method value = type.getDeclaredMethod(name, params); value.setAccessible(true); return value;
    }
    private static Object make(Class<?> type, Class<?>[] params, Object... args) throws Exception {
        Constructor<?> value = type.getDeclaredConstructor(params); value.setAccessible(true); return value.newInstance(args);
    }
    private static final class JobHandle {
        final int id;
        final Object job, run;
        final IJobCallback callback;
        JobHandle(int id, Object job, Object run) {
            this.id = id; this.job = job; this.run = run;
            IBinder target = ((IJobCallback) run).asBinder();
            // Keep the real generated AIDL request/reply path, even in this one-process fixture.
            callback = IJobCallback.Stub.asInterface(new Binder() {
                @Override protected boolean onTransact(int code, Parcel request, Parcel reply, int flags)
                        throws RemoteException { return target.transact(code, request, reply, flags); }
            });
        }
        void post(int notificationId, Notification notification, int policy) throws Exception {
            callback.setNotification(id, notificationId, notification, policy);
        }
        void finish() throws Exception { callback.jobFinished(id, false); }
    }
    /** A running service that acknowledges its stop only when the test explicitly asks. */
    private static final class DelayedStop extends IJobService.Stub {
        int stops;
        @Override public void startJob(JobParameters parameters) { }
        @Override public void stopJob(JobParameters parameters) { stops++; }
        @Override public void onNetworkChanged(JobParameters parameters) { }
        @Override public void getTransferredDownloadBytes(JobParameters parameters, JobWorkItem item) { }
        @Override public void getTransferredUploadBytes(JobParameters parameters, JobWorkItem item) { }
    }
    private static final class Fixture {
        final Class<?> apkType = Class.forName("com.gesher.server.ApkPackage");
        final Class<?> pmType = Class.forName("com.gesher.server.PackageManagerService");
        final Class<?> atmType = Class.forName("com.gesher.server.ActivityTaskManagerService");
        final Class<?> amType = Class.forName("com.gesher.server.ActivityManagerService");
        final Class<?> schedulerType = Class.forName("com.gesher.server.JobSchedulerService");
        final Class<?> jobType = Class.forName(schedulerType.getName() + "$Job");
        final Class<?> runType = Class.forName(schedulerType.getName() + "$Run");
        final Object owner, pm, scheduler;
        final NotificationManagerService notifications;
        final String pkg;
        Fixture(String apk) throws Exception {
            owner = apkType.getMethod("parse", String.class).invoke(null, apk);
            pkg = (String) apkType.getField("packageName").get(owner);
            pm = pmType.getConstructor(Collection.class).newInstance(List.of(owner));
            Object atm = make(atmType, new Class<?>[]{pmType}, pm);
            ToIntBiFunction<StatusBarNotification, Boolean> sink = (notification, update) -> 0;
            notifications = (NotificationManagerService) make(NotificationManagerService.class,
                    new Class<?>[]{pmType, amType, ToIntBiFunction.class}, pm, null, sink);
            Object am = make(amType, new Class<?>[]{atmType, pmType, NotificationManagerService.class}, atm, pm, notifications);
            Class<?> created = Class.forName("com.gesher.server.Services$Created");
            field(Class.forName("com.gesher.server.Services"), "sLast").set(null,
                    make(created, new Class<?>[]{pmType, atmType, amType}, pm, atm, am));
            Object permissions = pmType.getMethod("permissions").invoke(pm);
            permissions.getClass().getMethod("grantRuntimePermission", String.class, String.class, String.class, int.class)
                    .invoke(permissions, pkg, "android.permission.POST_NOTIFICATIONS", "default:0", 0);
            notifications.createNotificationChannels(pkg, new ParceledListSlice<>(List.of(
                    new NotificationChannel("jobs-a", "Job notifications A", NotificationManager.IMPORTANCE_DEFAULT),
                    new NotificationChannel("jobs-b", "Job notifications B", NotificationManager.IMPORTANCE_DEFAULT))));
            scheduler = make(schedulerType, new Class<?>[]{amType, atmType, pmType}, am, atm, pm);
            Thread background = (Thread) field(schedulerType, "mThread").get(scheduler);
            if (background != null) {
                background.interrupt(); background.join(5000);
                require(!background.isAlive(), "Fixture scheduler polling thread did not stop");
            }
        }
        @SuppressWarnings("unchecked")
        JobHandle run(int id, boolean userInitiated) throws Exception {
            JobInfo.Builder builder = new JobInfo.Builder(id, new ComponentName(pkg, pkg + ".TestJobService"));
            if (userInitiated) builder.setRequiredNetworkType(JobInfo.NETWORK_TYPE_ANY).setUserInitiated(true);
            else builder.setMinimumLatency(60_000);
            Object job = make(jobType, new Class<?>[]{schedulerType, String.class, String.class, JobInfo.class},
                    scheduler, pkg, "notification-contract", builder.build());
            Object run = make(runType, new Class<?>[]{schedulerType, jobType}, scheduler, job);
            synchronized (scheduler) {
                field(jobType, "run").set(job, run);
                ((List<Object>) field(schedulerType, "mJobs").get(scheduler)).add(job);
                Field running = field(schedulerType, "mRunning"); running.setInt(scheduler, running.getInt(scheduler) + 1);
            }
            return new JobHandle(id, job, run);
        }
        Notification notification(String channel) throws Exception {
            Notification value = new Notification();
            field(Notification.class, "mChannelId").set(value, channel);
            value.setSmallIcon(Icon.createWithResource("android", android.R.drawable.ic_dialog_info));
            value.extras = new android.os.Bundle();
            value.extras.putString(Notification.EXTRA_TITLE, "Job notification contract");
            return value;
        }
        StatusBarNotification active(int id) {
            for (Object value : notifications.getAppActiveNotifications(pkg, 0).getList()) {
                StatusBarNotification notification = (StatusBarNotification) value;
                if (notification.getId() == id && notification.getTag() == null) return notification;
            }
            return null;
        }
        void visible(int id, boolean uij) throws Exception {
            StatusBarNotification active = active(id);
            require(active != null && ((active.getNotification().flags & Notification.FLAG_USER_INITIATED_JOB) != 0) == uij,
                    "Wrong live notification or UIJ flag for " + id);
            Class<?> store = Class.forName("com.gesher.server.NotificationStore");
            Object retained = method(store, "load", String.class, String.class).invoke(null, pkg, active.getKey());
            require(retained != null && ((field(retained.getClass(), "flags").getInt(retained)
                    & Notification.FLAG_USER_INITIATED_JOB) != 0) == uij, "Retained notification policy differs for " + id);
        }
        void removed(int id, String key) throws Exception {
            require(active(id) == null, "Completed REMOVE notification is still live: " + id);
            require(method(Class.forName("com.gesher.server.NotificationStore"), "load", String.class, String.class)
                    .invoke(null, pkg, key) == null, "Completed REMOVE retained its toast actions: " + id);
        }
        boolean inUse(String channel) throws Exception {
            return (Boolean) method(schedulerType, "hasUserInitiatedJobNotification", String.class, String.class)
                    .invoke(scheduler, pkg, channel);
        }
        int associations() throws Exception { synchronized (scheduler) { return ((Set<?>) field(schedulerType, "mNotificationRuns").get(scheduler)).size(); } }
        void clear(int id) { notifications.cancelNotificationWithTag(pkg, pkg, null, id, 0); }
    }

    private static void contracts(Fixture f) throws Exception {
        JobHandle validation = f.run(701, true);
        validation.post(101, f.notification("jobs-a"), REMOVE);
        f.visible(101, true);
        require(f.inUse("jobs-a") && f.associations() == 1, "UIJ was not associated with its channel");
        rejects(NullPointerException.class, () -> validation.post(102, null, REMOVE), "Null notification accepted");
        Notification noIcon = f.notification("jobs-a"); noIcon.setSmallIcon(null);
        rejects(IllegalArgumentException.class, () -> validation.post(102, noIcon, REMOVE), "Missing small icon accepted");
        rejects(IllegalArgumentException.class, () -> validation.post(102, f.notification("missing-channel"), REMOVE), "Missing channel accepted");
        rejects(IllegalArgumentException.class, () -> validation.post(102, f.notification("jobs-a"), 99), "Unknown end policy accepted");
        rejects(SecurityException.class, () -> validation.callback.setNotification(999, 102, f.notification("jobs-a"), REMOVE), "Foreign job ID accepted");
        require(f.active(102) == null && f.associations() == 1, "Invalid requests changed publication or associations");
        validation.post(101, f.notification("jobs-a"), DETACH);
        validation.finish(); f.visible(101, false);
        require(!f.inUse("jobs-a") && f.associations() == 0, "DETACH retained UIJ ownership");
        validation.post(102, null, 99); validation.finish();
        require(f.active(102) == null && f.associations() == 0, "Completed callback changed state");
        f.clear(101);

        JobHandle ordinary = f.run(702, false);
        Notification forged = f.notification("jobs-a"); forged.flags |= Notification.FLAG_USER_INITIATED_JOB;
        ordinary.post(201, forged, REMOVE); f.visible(201, false);
        String removedKey = f.active(201).getKey(); ordinary.finish(); f.removed(201, removedKey);

        JobHandle drained = f.run(713, false);
        drained.post(202, f.notification("jobs-a"), REMOVE); f.visible(202, false);
        removedKey = f.active(202).getKey();
        JobParameters workParameters = new JobParameters(drained.callback.asBinder(),
                "notification-contract", drained.id, android.os.PersistableBundle.EMPTY,
                android.os.Bundle.EMPTY, null, 0, false, false, false, null, null, null);
        require(workParameters.dequeueWork() == null, "An exhausted work queue returned a work item");
        f.removed(202, removedKey);
        require(f.associations() == 0 && field(f.schedulerType, "mRunning").getInt(f.scheduler) == 0,
                "Automatic work completion retained its notification or running slot");
        require(workParameters.dequeueWork() == null && f.associations() == 0,
                "Repeated drain reactivated a finished notification association");

        JobHandle first = f.run(703, true), last = f.run(704, true);
        first.post(301, f.notification("jobs-a"), REMOVE);
        last.post(301, f.notification("jobs-a"), DETACH);
        first.finish(); f.visible(301, true);
        require(f.inUse("jobs-a") && f.associations() == 1, "Earlier REMOVE broke a shared UIJ notification");
        last.finish(); f.visible(301, false); f.clear(301);

        JobHandle uij = f.run(705, true), optional = f.run(706, false);
        uij.post(401, f.notification("jobs-a"), REMOVE);
        optional.post(401, f.notification("jobs-a"), DETACH); f.visible(401, true);
        uij.finish(); f.visible(401, false);
        require(!f.inUse("jobs-a") && f.associations() == 1, "Last UIJ completion lost the ordinary association");
        optional.finish(); f.visible(401, false); f.clear(401);

        first = f.run(707, true); last = f.run(708, false);
        first.post(501, f.notification("jobs-a"), DETACH);
        last.post(501, f.notification("jobs-a"), REMOVE);
        first.finish(); f.visible(501, false);
        removedKey = f.active(501).getKey(); last.finish(); f.removed(501, removedKey);

        JobHandle switching = f.run(709, true);
        switching.post(601, f.notification("jobs-a"), REMOVE);
        removedKey = f.active(601).getKey();
        switching.post(602, f.notification("jobs-a"), DETACH); f.removed(601, removedKey); f.visible(602, true);
        switching.post(603, f.notification("jobs-b"), REMOVE); f.visible(602, false); f.visible(603, true);
        require(!f.inUse("jobs-a") && f.inUse("jobs-b") && f.associations() == 1, "Changing IDs/channels retained old active ownership");
        removedKey = f.active(603).getKey(); switching.finish(); f.removed(603, removedKey); f.clear(602);

        JobHandle channelChange = f.run(710, true);
        channelChange.post(701, f.notification("jobs-a"), REMOVE);
        channelChange.post(701, f.notification("jobs-b"), DETACH);
        require(!f.inUse("jobs-a") && f.inUse("jobs-b") && f.associations() == 1,
                "Same-ID update retained a replaced channel association");
        channelChange.finish(); f.visible(701, false); f.clear(701);

        JobHandle stopping = f.run(711, true), sharing = f.run(712, false);
        stopping.post(801, f.notification("jobs-a"), REMOVE);
        sharing.post(801, f.notification("jobs-a"), DETACH);
        DelayedStop delayed = new DelayedStop(); field(f.runType, "service").set(stopping.run, delayed);
        ((IJobScheduler) f.scheduler).cancel("notification-contract", stopping.id);
        require(delayed.stops == 1 && !((List<?>) field(f.schedulerType, "mJobs").get(f.scheduler)).contains(stopping.job)
                && f.inUse("jobs-a") && f.associations() == 2, "Scheduled-list removal prematurely retired a running association");
        sharing.finish(); f.visible(801, true);
        removedKey = f.active(801).getKey();
        stopping.callback.acknowledgeStopMessage(stopping.id, false); f.removed(801, removedKey);
        require(!f.inUse("jobs-a") && f.associations() == 0 && field(f.schedulerType, "mRunning").getInt(f.scheduler) == 0,
                "Completed/canceled runs retained notification ownership or running slots");
    }

    public static void main(String[] args) throws Exception {
        require(args.length == 1, "Pass the source-built devsvc APK");
        require("1000".equals(System.getenv("GESHER_BINDER_UID")), "System-UID fixture required for retained notifications");
        System.loadLibrary("android_runtime");
        AssetManager.createSystemAssetsInZygoteLocked(false, System.getenv("ANDROID_ROOT") + "/framework/framework-res.apk");
        File parent = new File(System.getenv("ANDROID_DATA")); Files.createDirectories(parent.toPath());
        File data = Files.createTempDirectory(parent.toPath(), "job-notification-").toFile();
        android.system.Os.setenv("ANDROID_DATA", data.getPath(), true);
        android.system.Os.setenv("GESHER_MICROG_SPOOF", "1", true);
        Field services = field(Class.forName("com.gesher.server.Services"), "sLast");
        Object previous = services.get(null);
        try { contracts(new Fixture(args[0])); }
        finally { services.set(null, previous); }
        System.out.println("JobNotificationLifecycleTest all passed: " + checks + " checks");
    }
}
