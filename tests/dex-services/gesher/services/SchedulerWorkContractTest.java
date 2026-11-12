/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package gesher.services;

import android.app.ActivityManager;
import android.app.AlarmManager;
import android.app.IActivityManager;
import android.app.IAlarmManager;
import android.app.IApplicationThread;
import android.app.JobSchedulerImpl;
import android.app.PendingIntent;
import android.app.job.IJobCallback;
import android.app.job.IJobScheduler;
import android.app.job.IJobService;
import android.app.job.JobInfo;
import android.app.job.JobParameters;
import android.app.job.JobScheduler;
import android.app.job.JobWorkItem;
import android.content.ComponentName;
import android.content.Context;
import android.content.ContextWrapper;
import android.content.IIntentReceiver;
import android.content.Intent;
import android.content.IntentFilter;
import android.content.pm.ApplicationInfo;
import android.content.pm.ServiceInfo;
import android.content.pm.Signature;
import android.content.res.CompatibilityInfo;
import android.os.Binder;
import android.os.Bundle;
import android.os.IBinder;
import android.os.IInterface;
import android.os.Parcel;
import android.os.PersistableBundle;
import android.os.RemoteException;
import android.service.notification.StatusBarNotification;
import com.android.internal.pm.parsing.pkg.PackageImpl;
import com.android.internal.pm.pkg.component.ParsedServiceImpl;
import java.lang.reflect.Constructor;
import java.lang.reflect.Field;
import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Method;
import java.util.ArrayDeque;
import java.util.Collection;
import java.util.List;
import java.util.Map;
import java.util.Set;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicReference;
import java.util.function.ToIntBiFunction;
import sun.misc.Unsafe;

/** Public scheduler/parameter/alarm calls over real services; constructed package/process metadata. */
public final class SchedulerWorkContractTest {
    private static final String PACKAGE = "gesher.scheduler.contract", NAMESPACE = "work-contract";
    private static final int PID = 873101, UID = 12371;
    private static final Unsafe UNSAFE;
    public static int checks;
    static {
        try { Field f = Unsafe.class.getDeclaredField("theUnsafe"); f.setAccessible(true); UNSAFE = (Unsafe) f.get(null); }
        catch (Exception failure) { throw new ExceptionInInitializerError(failure); }
    }
    private interface Work { void run() throws Exception; }
    private static synchronized void check(boolean value, String why) {
        if (!value) throw new AssertionError(why);
        checks++;
    }
    private static Field field(Class<?> type, String name) throws Exception {
        Field value = type.getDeclaredField(name); value.setAccessible(true); return value;
    }
    private static Object get(Object target, String name) throws Exception { return field(target.getClass(), name).get(target); }
    private static void set(Object target, String name, Object value) throws Exception { field(target.getClass(), name).set(target, value); }
    private static Class<?> type(String name) throws Exception { return Class.forName("com.gesher.server." + name); }
    private static Object make(Class<?> type, Class<?>[] parameters, Object... arguments) throws Exception {
        Constructor<?> value = type.getDeclaredConstructor(parameters); value.setAccessible(true);
        try { return value.newInstance(arguments); }
        catch (InvocationTargetException failure) { return rethrow(failure.getCause()); }
    }
    private static Object invoke(Object target, String name, Class<?>[] parameters, Object... arguments) throws Exception {
        Method value = target.getClass().getDeclaredMethod(name, parameters); value.setAccessible(true);
        try { return value.invoke(target, arguments); }
        catch (InvocationTargetException failure) { return rethrow(failure.getCause()); }
    }
    private static Object rethrow(Throwable failure) throws Exception {
        if (failure instanceof Error error) throw error;
        if (failure instanceof Exception exception) throw exception;
        throw new AssertionError(failure);
    }
    private static void rejects(Class<? extends Throwable> expected, Work work) throws Exception {
        try { work.run(); }
        catch (Throwable failure) { check(expected.isInstance(failure), "Wrong failure: " + failure); return; }
        throw new AssertionError("Expected " + expected.getSimpleName());
    }
    private static final class AppThread extends IApplicationThread.Default {
        final IBinder binder = new Binder();
        int created, stopped;
        @Override public IBinder asBinder() { return binder; }
        @Override public void scheduleCreateService(IBinder token, ServiceInfo info,
                CompatibilityInfo compatibility, int state) { created++; }
        @Override public void scheduleStopService(IBinder token) { stopped++; }
        @Override public void scheduleRegisteredReceiver(IIntentReceiver receiver, Intent intent,
                int code, String data, Bundle extras, boolean ordered, boolean sticky,
                boolean assumed, int user, int state, int uid, String sender) throws RemoteException {
            receiver.performReceive(intent, code, data, extras, ordered, sticky, user);
        }
    }
    private static final class HeldStop extends IJobService.Stub {
        int stops;
        @Override public void startJob(JobParameters parameters) { }
        @Override public void stopJob(JobParameters parameters) { stops++; }
        @Override public void onNetworkChanged(JobParameters parameters) { }
        @Override public void getTransferredDownloadBytes(JobParameters parameters, JobWorkItem work) { }
        @Override public void getTransferredUploadBytes(JobParameters parameters, JobWorkItem work) { }
    }
    private static IBinder transport(IBinder target) {
        return new Binder() {
            @Override protected boolean onTransact(int code, Parcel request, Parcel reply, int flags)
                    throws RemoteException { return target.transact(code, request, reply, flags); }
        };
    }
    private static IBinder serviceBinder(Class<?> contract, IInterface implementation) throws Exception {
        Class<?> standIn = type("StandIn");
        Object delegate = standIn.getMethod("of", Class.class, IInterface.class).invoke(null, contract, implementation);
        return transport((IBinder) standIn.getMethod("asBinder").invoke(delegate));
    }
    private static final class Harness implements AutoCloseable {
        final boolean wire;
        final Object pm, atm, services, scheduler, alarms, notifications, broadcasts;
        final Object previousNotifications;
        final IActivityManager am;
        final Class<?> jobType = type("JobSchedulerService$Job"), runType = type("JobSchedulerService$Run");
        final AppThread thread = new AppThread();
        final JobScheduler manager;
        final AlarmManager alarmManager;
        final AtomicInteger alarmChanges = new AtomicInteger();
        final Context context = new ContextWrapper(null) {
            @Override public String getPackageName() { return PACKAGE; }
            @Override public String getOpPackageName() { return PACKAGE; }
            @Override public int getUserId() { return 0; }
            @Override public ApplicationInfo getApplicationInfo() {
                ApplicationInfo info = new ApplicationInfo(); info.packageName = PACKAGE;
                info.targetSdkVersion = 36; info.uid = UID; return info;
            }
        };
        @SuppressWarnings("unchecked") Harness(boolean wire) throws Exception {
            this.wire = wire;
            PackageImpl parsed = (PackageImpl) PackageImpl.forTesting(PACKAGE, "/fixture/" + PACKAGE + "/base.apk");
            ParsedServiceImpl service = new ParsedServiceImpl();
            service.setName(PACKAGE + ".WorkService"); service.setPackageName(PACKAGE);
            service.setProcessName(PACKAGE); service.setEnabled(true);
            service.setPermission(android.Manifest.permission.BIND_JOB_SERVICE);
            parsed.addService(service); parsed.setEnabled(true); parsed.setTargetSdkVersion(36); parsed.hideAsFinal();
            Object owner = UNSAFE.allocateInstance(type("ApkPackage"));
            set(owner, "packageName", PACKAGE); set(owner, "mUid", UID); set(owner, "parsed", parsed);
            set(owner, "apkPath", "/fixture/" + PACKAGE + "/base.apk"); set(owner, "mDataDir", "/fixture/" + PACKAGE + "/data");
            set(owner, "sharedLibraries", field(type("SharedLibraryGraph"), "EMPTY").get(null));
            set(owner, "mSigningIdentityChecked", true);
            set(owner, "mVerifiedSignatures", new Signature[]{new Signature(new byte[]{7})});
            Class<?> pmType = type("PackageManagerService"), atmType = type("ActivityTaskManagerService"),
                    amType = type("ActivityManagerService"), callersType = type("CallerIdentity"),
                    notificationType = type("NotificationManagerService");
            pm = make(pmType, new Class<?>[]{Collection.class}, List.of(owner));
            atm = make(atmType, new Class<?>[]{pmType}, pm);
            // Isolate unrelated content-provider/settings initialization. All invoked
            // service, caller, queue, notification-fence and binding methods stay real.
            am = (IActivityManager) UNSAFE.allocateInstance(amType);
            Object callers = make(callersType, new Class<?>[]{pmType, atmType}, pm, atm);
            set(am, "mPm", pm); set(am, "mAtm", atm); set(am, "mCallers", callers);
            Object permissions = invoke(pm, "permissions", new Class<?>[0]);
            set(am, "mPermissions", permissions); invoke(permissions, "attach", new Class<?>[]{callersType}, callers);
            Object catalog = get(permissions, "mCatalog"); set(catalog, "mLoaded", true); set(catalog, "mFrameworkRead", true);
            ((Set<String>) get(catalog, "mProtectedBroadcasts")).add(AlarmManager.ACTION_NEXT_ALARM_CLOCK_CHANGED);
            previousNotifications = field(notificationType, "sLast").get(null);
            ToIntBiFunction<StatusBarNotification, Boolean> toast = (value, update) -> 0;
            notifications = make(notificationType, new Class<?>[]{pmType, amType, ToIntBiFunction.class}, pm, am, toast);
            services = make(type("ActiveServices"), new Class<?>[]{atmType, pmType, notificationType}, atm, pm, notifications);
            set(am, "mServices", services);
            broadcasts = make(type("BroadcastController"), new Class<?>[]{atmType, pmType}, atm, pm);
            set(am, "mBroadcasts", broadcasts);
            set(am, "mPendingIntents", make(type("PendingIntents"), new Class<?>[]{amType, pmType}, am, pm));
            invoke(atm, "setActivityManager", new Class<?>[]{amType}, am);
            Class<?> processType = type("ActivityTaskManagerService$ProcessRecord");
            Object process = make(processType, new Class<?>[]{atmType, int.class, IApplicationThread.class}, atm, PID, thread);
            set(process, "uid", UID); set(process, "packageName", PACKAGE); set(process, "processName", PACKAGE); set(process, "ready", true);
            ((List<Object>) get(atm, "mProcesses")).add(process);
            // Keep this constructed process available across separate runs; no activity is executed.
            ((List<Object>) get(process, "activities")).add(UNSAFE.allocateInstance(type("ActivityTaskManagerService$ActivityRecord")));
            scheduler = make(type("JobSchedulerService"), new Class<?>[]{amType, atmType, pmType}, am, atm, pm);
            set(scheduler, "mThread", new Thread()); // Deterministic manual dispatch, no scheduler polling.
            IJobScheduler endpoint = (IJobScheduler) scheduler;
            if (wire) endpoint = IJobScheduler.Stub.asInterface(serviceBinder(IJobScheduler.class, endpoint));
            manager = new JobSchedulerImpl(context, endpoint).forNamespace(NAMESPACE);
            alarms = make(type("AlarmManagerService"), new Class<?>[]{amType, pmType}, am, pm);
            set(alarms, "mThread", new Thread()); // Observe state transitions without real timers firing.
            IAlarmManager alarmEndpoint = (IAlarmManager) alarms;
            if (wire) alarmEndpoint = IAlarmManager.Stub.asInterface(serviceBinder(IAlarmManager.class, alarmEndpoint));
            // PendingIntent-only calls wrap, but never execute, the default Handler.
            // A listener path would fail on this uninitialized Handler instead of
            // silently receiving a fixture executor or an artificial callback.
            alarmManager = (AlarmManager) UNSAFE.allocateInstance(AlarmManager.class);
            set(alarmManager, "mService", alarmEndpoint); set(alarmManager, "mContext", context);
            set(alarmManager, "mPackageName", PACKAGE); set(alarmManager, "mTargetSdkVersion", 36);
            set(alarmManager, "mMainThreadHandler", UNSAFE.allocateInstance(android.os.Handler.class));
            IIntentReceiver receiver = new IIntentReceiver.Stub() {
                @Override public void performReceive(Intent intent, int code, String data,
                        Bundle extras, boolean ordered, boolean sticky, int user) {
                    check(!Thread.holdsLock(alarms), "Alarm notification ran under the alarm monitor");
                    check(AlarmManager.ACTION_NEXT_ALARM_CLOCK_CHANGED.equals(intent.getAction()), "Wrong alarm action");
                    alarmChanges.incrementAndGet();
                }
            };
            invoke(broadcasts, "registerReceiver", new Class<?>[]{IApplicationThread.class, String.class,
                    IIntentReceiver.class, IntentFilter.class, int.class, String.class, int.class},
                    thread, PACKAGE, receiver, new IntentFilter(AlarmManager.ACTION_NEXT_ALARM_CLOCK_CHANGED),
                    PID, null, Context.RECEIVER_EXPORTED);
        }
        JobInfo info(int id) {
            return new JobInfo.Builder(id, new ComponentName(PACKAGE, PACKAGE + ".WorkService"))
                    .setMinimumLatency(3_600_000).build();
        }
        JobWorkItem work(String action) { return new JobWorkItem(new Intent(action)); }
        Object job(int id) throws Exception { return invoke(scheduler, "find", new Class<?>[]{String.class, String.class, int.class}, PACKAGE, NAMESPACE, id); }
        int running() throws Exception { return (Integer) get(scheduler, "mRunning"); }
        int bindings() throws Exception { return ((Map<?, ?>) get(services, "mConnections")).size(); }
        @SuppressWarnings("unchecked") Handle start(int id) throws Exception {
            Object job = job(id); check(job != null, "Scheduled job missing");
            Object run = make(runType, new Class<?>[]{scheduler.getClass(), jobType}, scheduler, job);
            synchronized (scheduler) { set(job, "run", run); set(scheduler, "mRunning", running() + 1); }
            IJobCallback callback = (IJobCallback) run;
            if (wire) callback = IJobCallback.Stub.asInterface(transport(callback.asBinder()));
            JobParameters parameters = new JobParameters(callback.asBinder(), NAMESPACE, id,
                    PersistableBundle.EMPTY, Bundle.EMPTY, null, 0, false, false, false, null, null, null);
            set(run, "params", parameters);
            invoke(scheduler, "bind", new Class<?>[]{runType}, run);
            check(bindings() == 1, "Run did not create its real system service binding");
            return new Handle(job, run, callback, parameters);
        }
        PendingIntent operation(int request) throws Exception {
            return new PendingIntent(am.getIntentSenderWithFeature(ActivityManager.INTENT_SENDER_BROADCAST,
                    PACKAGE, null, null, null, request, new Intent[]{new Intent("gesher.scheduler.ALARM")},
                    null, PendingIntent.FLAG_IMMUTABLE, null, 0));
        }
        @Override public void close() throws Exception {
            field(type("NotificationManagerService"), "sLast").set(null, previousNotifications);
            ((java.util.concurrent.ExecutorService) get(broadcasts, "mTimeouts")).shutdownNow();
            ((java.util.concurrent.ExecutorService) get(services, "mRestarts")).shutdownNow();
            ((java.util.concurrent.ExecutorService) get(pm, "mPackageEventExecutor")).shutdownNow();
            ((java.util.concurrent.ExecutorService) get(get(am, "mPendingIntents"), "mCancelCallbacks")).shutdownNow();
        }
    }
    private record Handle(Object job, Object run, IJobCallback callback, JobParameters parameters) { }
    private static void completed(Harness h, Handle handle, int stopped) throws Exception {
        check((Boolean) get(handle.run, "done") && get(handle.job, "run") == null, "Empty work did not retire the run");
        check(h.running() == 0 && h.manager.getPendingJob(handle.parameters.getJobId()) == null,
                "Exhausted one-shot job retained its slot or scheduled record");
        check(h.bindings() == 0 && h.thread.stopped == stopped, "Exhausted job did not release its binding once");
        check(handle.parameters.dequeueWork() == null, "Completed callback returned work");
        check(h.running() == 0 && h.thread.stopped == stopped, "Duplicate drain repeated cleanup");
    }
    @SuppressWarnings("unchecked") private static void drain(boolean wire) throws Exception {
        try (Harness h = new Harness(wire)) {
            JobInfo info = h.info(101);
            check(h.manager.enqueue(info, h.work("first")) == JobScheduler.RESULT_SUCCESS, "First enqueue rejected");
            check(h.manager.enqueue(info, h.work("second")) == JobScheduler.RESULT_SUCCESS, "Second enqueue rejected");
            Handle handle = h.start(101);
            JobWorkItem first = handle.parameters.dequeueWork(), second = handle.parameters.dequeueWork();
            check("first".equals(first.getIntent().getAction()) && "second".equals(second.getIntent().getAction()), "Work order changed");
            check(first.getDeliveryCount() == 1 && second.getDeliveryCount() == 1, "Work delivery counts wrong");
            check(handle.parameters.dequeueWork() == null && !(Boolean) get(handle.run, "done"), "Pending-empty job finished while work was outstanding");
            handle.parameters.completeWork(second);
            check(handle.parameters.dequeueWork() == null && h.running() == 1, "Out-of-order completion finished outstanding work");
            rejects(IllegalArgumentException.class, () -> handle.parameters.completeWork(second));
            handle.parameters.completeWork(first);
            check(h.running() == 1 && !(Boolean) get(handle.run, "done"), "completeWork alone finished the job");
            // Construct one association without replacing the real lifecycle registry/fence.
            ((Set<Object>) get(h.scheduler, "mNotificationRuns")).add(handle.run);
            set(handle.run, "notificationChannelId", "work");
            long caller = Binder.clearCallingIdentity();
            try {
                Binder.restoreCallingIdentity(((long) UID << 32) | PID);
                check(handle.parameters.dequeueWork() == null, "Drained queue returned work");
                check(Binder.getCallingPid() == PID && Binder.getCallingUid() == UID,
                        "Completion failed to restore the app caller after internal unbind");
            } finally { Binder.restoreCallingIdentity(caller); }
            completed(h, handle, 1);
            check(((Set<?>) get(h.scheduler, "mNotificationRuns")).isEmpty()
                    && get(handle.run, "notificationChannelId") == null, "Drain retained notification association");
            check(h.manager.enqueue(info, h.work("next-run")) == JobScheduler.RESULT_SUCCESS, "Enqueue after drain rejected");
            check(h.job(101) != handle.job, "Enqueue attached to the retired job");
            Handle next = h.start(101); JobWorkItem item = next.parameters.dequeueWork();
            check("next-run".equals(item.getIntent().getAction()) && item.getDeliveryCount() == 1, "Next run lost queued work");
            next.parameters.completeWork(item); check(next.parameters.dequeueWork() == null, "Next run did not drain");
            completed(h, next, 2);
        }
    }
    private static void stopping(boolean wire) throws Exception {
        try (Harness h = new Harness(wire)) {
            JobInfo info = h.info(102);
            h.manager.enqueue(info, h.work("retained")); Handle handle = h.start(102);
            HeldStop service = new HeldStop(); set(handle.run, "service", service);
            invoke(h.scheduler, "stopRun", new Class<?>[]{h.runType, String.class}, handle.run, "fixture constraint change");
            check(service.stops == 1 && (Boolean) get(handle.run, "stopping"), "Stop was not held awaiting acknowledgment");
            check(handle.parameters.dequeueWork() == null, "Stopping job dequeued new work");
            check(((ArrayDeque<?>) get(handle.job, "work")).size() == 1
                    && ((Map<?, ?>) get(handle.job, "delivered")).isEmpty(), "Stopping dequeue consumed pending work");
            handle.callback.acknowledgeStopMessage(102, true);
            check(h.running() == 0 && h.bindings() == 0 && h.job(102) == handle.job, "Stopped run did not preserve rescheduled job");
            Handle next = h.start(102); JobWorkItem item = next.parameters.dequeueWork();
            check(item != null && "retained".equals(item.getIntent().getAction()) && item.getDeliveryCount() == 1,
                    "Work was lost or counted as delivered during stop");
            next.parameters.completeWork(item); next.parameters.dequeueWork(); completed(h, next, 2);
        }
    }
    private static void enqueueAtDrain(boolean wire) throws Exception {
        try (Harness h = new Harness(wire)) {
            JobInfo info = h.info(103); h.manager.schedule(info); Handle handle = h.start(103);
            CountDownLatch entered = new CountDownLatch(1);
            AtomicReference<JobWorkItem> result = new AtomicReference<>();
            AtomicReference<Throwable> failure = new AtomicReference<>();
            Thread reader = new Thread(() -> {
                entered.countDown();
                try { result.set(handle.parameters.dequeueWork()); }
                catch (Throwable error) { failure.set(error); }
            }, "work-final-dequeue");
            reader.setDaemon(true);
            // Channel deletion and completion share this fence. While completion
            // waits for it, enqueue must remain able to add work to the live job.
            synchronized (get(h.notifications, "mPublicationLock")) {
                reader.start(); check(entered.await(5, TimeUnit.SECONDS), "Final dequeue did not enter");
                long until = System.nanoTime() + TimeUnit.SECONDS.toNanos(5);
                while (reader.isAlive() && reader.getState() != Thread.State.BLOCKED && System.nanoTime() < until) Thread.yield();
                check(reader.getState() == Thread.State.BLOCKED, "Final dequeue did not enter the notification lifecycle fence");
                check(h.manager.enqueue(info, h.work("raced")) == JobScheduler.RESULT_SUCCESS, "Concurrent enqueue was rejected");
            }
            reader.join(5000);
            check(!reader.isAlive() && failure.get() == null, "Concurrent dequeue failed: " + failure.get());
            check(result.get() != null && "raced".equals(result.get().getIntent().getAction())
                    && !(Boolean) get(handle.run, "done") && h.running() == 1,
                    "Final dequeue retired a concurrently enqueued work item");
            handle.parameters.completeWork(result.get()); handle.parameters.dequeueWork(); completed(h, handle, 1);
        }
    }
    private static void alarms(boolean wire) throws Exception {
        try (Harness h = new Harness(wire)) {
            long when = System.currentTimeMillis() + 3_600_000;
            PendingIntent first = h.operation(1), second = h.operation(2), ordinary = h.operation(3);
            h.alarmManager.setAlarmClock(new AlarmManager.AlarmClockInfo(when, first), first);
            check(h.alarmChanges.get() == 1 && h.alarmManager.getNextAlarmClock().getTriggerTime() == when, "First clock not announced");
            h.alarmManager.set(AlarmManager.RTC, when, ordinary);
            check(h.alarmChanges.get() == 1, "Unrelated ordinary alarm changed next-clock state");
            h.alarmManager.set(AlarmManager.RTC, when, first);
            check(h.alarmManager.getNextAlarmClock() == null && h.alarmChanges.get() == 2,
                    "Replacing the final alarm clock with an ordinary alarm missed its notification");
            check(get(h.alarms, "mNextAlarmClock") == null, "Replacement left stale next-clock cache");
            h.alarmManager.setAlarmClock(new AlarmManager.AlarmClockInfo(when, first), first);
            h.alarmManager.setAlarmClock(new AlarmManager.AlarmClockInfo(when + 60_000, second), second);
            check(h.alarmChanges.get() == 3, "Later alarm clock changed the next-clock notification");
            h.alarmManager.setExact(AlarmManager.RTC, when, first);
            check(h.alarmChanges.get() == 4 && h.alarmManager.getNextAlarmClock().getTriggerTime() == when + 60_000,
                    "Replacing the first alarm clock failed to publish the second");
            h.alarmManager.cancel(second);
            check(h.alarmChanges.get() == 5 && h.alarmManager.getNextAlarmClock() == null, "Later removal used stale replacement state");
            h.alarmManager.set(AlarmManager.RTC, when + 1, first);
            check(h.alarmChanges.get() == 5, "Ordinary replacement emitted a spurious clock notification");
        }
    }
    public static void contracts(String selected, boolean wire) throws Exception {
        if (selected.equals("all") || selected.equals("drain")) drain(wire);
        if (selected.equals("all") || selected.equals("stopping")) stopping(wire);
        if (selected.equals("all") || selected.equals("race")) enqueueAtDrain(wire);
        if (selected.equals("all") || selected.equals("alarms")) alarms(wire);
    }
    public static void main(String[] args) throws Exception {
        System.loadLibrary("android_runtime");
        android.system.Os.unsetenv("GESHER_BINDER_UID"); // No system-server polling threads in this fresh fixture VM.
        android.system.Os.setenv("GESHER_MICROG_SPOOF", "1", true);
        contracts(args.length == 0 ? "all" : args[0], true);
        System.out.println("SchedulerWorkContractTest all passed: " + checks + " work, completion, binding and alarm checks");
    }
}
