/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package gesher.services;

import android.app.ActivityManager;
import android.app.IApplicationThread;
import android.app.job.IJobCallback;
import android.app.job.IJobScheduler;
import android.app.job.IJobService;
import android.app.job.JobInfo;
import android.app.job.JobParameters;
import android.app.job.JobScheduler;
import android.app.job.JobWorkItem;
import android.app.job.PendingJobReasonsInfo;
import android.content.ComponentName;
import android.content.Context;
import android.content.ContextWrapper;
import android.content.Intent;
import android.content.pm.ApplicationInfo;
import android.content.pm.Signature;
import com.android.internal.pm.parsing.pkg.PackageImpl;
import com.android.internal.pm.pkg.component.ParsedServiceImpl;
import android.os.Binder;
import android.os.Bundle;
import android.os.IBinder;
import android.os.Parcel;
import android.os.PersistableBundle;
import android.os.RemoteException;
import android.os.SystemClock;
import android.net.Network;
import android.net.NetworkCapabilities;
import android.net.NetworkRequest;

import java.lang.reflect.Constructor;
import java.lang.reflect.Field;
import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Method;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collection;
import java.util.List;
import java.util.Map;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicReference;
import sun.misc.Unsafe;

/**
 * Dalvik contract test for JobScheduler constraint evaluation and active job cancellation.
 * Verifies stopReason propagation on lost constraints (network, charging, idle, battery low),
 * execution timeouts, cancellations, reschedule backoff, and onNetworkChanged notifications.
 */
public final class JobSchedulerConstraintContractTest {
    private static final String PACKAGE = "gesher.jobscheduler.test";
    private static final String NAMESPACE = "constraint-test";
    private static final int PID = 789123;
    private static final int UID = 19842;
    private static final Unsafe UNSAFE;
    public static int checks;

    static {
        try {
            Field f = Unsafe.class.getDeclaredField("theUnsafe");
            f.setAccessible(true);
            UNSAFE = (Unsafe) f.get(null);
        } catch (Exception e) {
            throw new ExceptionInInitializerError(e);
        }
    }

    private static synchronized void check(boolean condition, String message) {
        if (!condition) {
            throw new AssertionError(message);
        }
        checks++;
    }

    private static Field field(Class<?> type, String name) throws Exception {
        Field f = type.getDeclaredField(name);
        f.setAccessible(true);
        return f;
    }

    private static Object get(Object target, String name) throws Exception {
        return field(target.getClass(), name).get(target);
    }

    private static void set(Object target, String name, Object value) throws Exception {
        field(target.getClass(), name).set(target, value);
    }

    private static Class<?> type(String name) throws Exception {
        return Class.forName("com.gesher.server." + name);
    }

    private static Object make(Class<?> type, Class<?>[] params, Object... args) throws Exception {
        Constructor<?> c = type.getDeclaredConstructor(params);
        c.setAccessible(true);
        try {
            return c.newInstance(args);
        } catch (InvocationTargetException e) {
            Throwable cause = e.getCause();
            if (cause instanceof Exception ex) throw ex;
            if (cause instanceof Error err) throw err;
            throw new AssertionError(cause);
        }
    }

    private static Object invoke(Object target, String name, Class<?>[] params, Object... args) throws Exception {
        Method m = target.getClass().getDeclaredMethod(name, params);
        m.setAccessible(true);
        try {
            return m.invoke(target, args);
        } catch (InvocationTargetException e) {
            Throwable cause = e.getCause();
            if (cause instanceof Exception ex) throw ex;
            if (cause instanceof Error err) throw err;
            throw new AssertionError(cause);
        }
    }

    private static class TestService extends IJobService.Stub {
        int starts;
        int stops;
        int networkChanges;
        int lastStopReason = -999;
        int lastInternalStopReason = -999;
        String lastDebugStopReason;
        JobParameters lastStopParams;
        JobParameters lastStartParams;
        boolean acknowledgeStartImmediately = true;
        boolean ongoingOnStart = true;
        boolean failStartCallback;
        boolean acknowledgeImmediately = true;
        boolean failNetworkCallback;
        boolean rescheduleOnStop = false;

        @Override
        public void startJob(JobParameters params) {
            starts++;
            lastStartParams = params;
            if (failStartCallback) throw new IllegalStateException("fixture startJob dispatch failure");
            if (!acknowledgeStartImmediately) return;
            try {
                params.getCallback().acknowledgeStartMessage(params.getJobId(), ongoingOnStart);
            } catch (RemoteException e) {
                throw new RuntimeException(e);
            }
        }

        @Override
        public void stopJob(JobParameters params) {
            stops++;
            lastStopParams = params;
            lastStopReason = params.getStopReason();
            lastInternalStopReason = params.getInternalStopReasonCode();
            lastDebugStopReason = params.getDebugStopReason();
            if (acknowledgeImmediately) {
                try {
                    params.getCallback().acknowledgeStopMessage(params.getJobId(), rescheduleOnStop);
                } catch (RemoteException e) {
                    throw new RuntimeException(e);
                }
            }
        }

        @Override
        public void onNetworkChanged(JobParameters params) throws RemoteException {
            networkChanges++;
            if (failNetworkCallback) throw new RemoteException("fixture disconnected");
        }

        @Override
        public void getTransferredDownloadBytes(JobParameters params, JobWorkItem item) { }

        @Override
        public void getTransferredUploadBytes(JobParameters params, JobWorkItem item) { }
    }

    private static final class TestRunHandle {
        final Object job;
        final Object run;
        final TestService service;
        final JobParameters params;

        TestRunHandle(Object job, Object run, TestService service, JobParameters params) {
            this.job = job;
            this.run = run;
            this.service = service;
            this.params = params;
        }
    }

    private static final class Harness implements AutoCloseable {
        final Object pm, atm, am, scheduler;
        final Class<?> schedulerType, jobType, runType;
        final PackageImpl parsed;

        Harness() throws Exception { this(true); }

        @SuppressWarnings("unchecked")
        Harness(boolean networkPermission) throws Exception {
            parsed = (PackageImpl) PackageImpl.forTesting(PACKAGE, "/fixture/" + PACKAGE + "/base.apk");
            ParsedServiceImpl service = new ParsedServiceImpl();
            service.setName(PACKAGE + ".TestJobService");
            service.setPackageName(PACKAGE);
            service.setProcessName(PACKAGE);
            service.setEnabled(true);
            service.setPermission(android.Manifest.permission.BIND_JOB_SERVICE);
            parsed.addService(service);
            parsed.setEnabled(true);
            parsed.setTargetSdkVersion(36);
            if (networkPermission) parsed.addUsesPermission(new com.android.internal.pm.pkg.component.ParsedUsesPermissionImpl(
                    android.Manifest.permission.ACCESS_NETWORK_STATE, 0, java.util.Set.of()));
            parsed.addUsesPermission(new com.android.internal.pm.pkg.component.ParsedUsesPermissionImpl(
                    "android.permission.RUN_USER_INITIATED_JOBS", 0, java.util.Set.of()));
            parsed.hideAsFinal();

            Object owner = UNSAFE.allocateInstance(type("ApkPackage"));
            set(owner, "packageName", PACKAGE);
            set(owner, "mUid", UID);
            set(owner, "parsed", parsed);
            set(owner, "apkPath", "/fixture/" + PACKAGE + "/base.apk");
            set(owner, "mDataDir", "/fixture/" + PACKAGE + "/data");
            set(owner, "sharedLibraries", field(type("SharedLibraryGraph"), "EMPTY").get(null));
            set(owner, "mSigningIdentityChecked", true);
            set(owner, "mVerifiedSignatures", new Signature[]{new Signature(new byte[]{42})});

            Class<?> pmType = type("PackageManagerService");
            Class<?> atmType = type("ActivityTaskManagerService");
            Class<?> amType = type("ActivityManagerService");
            Class<?> notifType = type("NotificationManagerService");

            pm = make(pmType, new Class<?>[]{Collection.class}, List.of(owner));
            atm = make(atmType, new Class<?>[]{pmType}, pm);
            // Construct only the dependencies these contracts call. This excludes
            // unrelated settings/ICU initialization, not the scheduler or bindings.
            am = UNSAFE.allocateInstance(amType);
            Class<?> callersType = type("CallerIdentity");
            Object callers = make(callersType, new Class<?>[]{pmType, atmType}, pm, atm);
            set(am, "mPm", pm); set(am, "mAtm", atm); set(am, "mCallers", callers);
            Object permissions = invoke(pm, "permissions", new Class<?>[0]);
            set(am, "mPermissions", permissions);
            invoke(permissions, "attach", new Class<?>[]{callersType}, callers);
            Object catalog = get(permissions, "mCatalog");
            set(catalog, "mLoaded", true); set(catalog, "mFrameworkRead", true);
            android.content.pm.PermissionInfo networkDefinition = new android.content.pm.PermissionInfo();
            networkDefinition.name = android.Manifest.permission.ACCESS_NETWORK_STATE;
            networkDefinition.packageName = "android";
            networkDefinition.protectionLevel = android.content.pm.PermissionInfo.PROTECTION_NORMAL;
            ((Map<String, android.content.pm.PermissionInfo>) get(catalog, "mFramework"))
                    .put(networkDefinition.name, networkDefinition);
            android.content.pm.PermissionInfo userInitiatedDefinition = new android.content.pm.PermissionInfo();
            userInitiatedDefinition.name = "android.permission.RUN_USER_INITIATED_JOBS";
            userInitiatedDefinition.packageName = "android";
            userInitiatedDefinition.protectionLevel = android.content.pm.PermissionInfo.PROTECTION_NORMAL;
            ((Map<String, android.content.pm.PermissionInfo>) get(catalog, "mFramework"))
                    .put(userInitiatedDefinition.name, userInitiatedDefinition);
            Object notifications = make(notifType, new Class<?>[]{pmType, amType, java.util.function.ToIntBiFunction.class},
                    pm, am, (java.util.function.ToIntBiFunction<Object, Boolean>) (n, u) -> 0);
            set(am, "mServices", make(type("ActiveServices"), new Class<?>[]{atmType, pmType, notifType}, atm, pm, notifications));
            invoke(atm, "setActivityManager", new Class<?>[]{amType}, am);

            Class<?> processType = type("ActivityTaskManagerService$ProcessRecord");
            Object process = make(processType, new Class<?>[]{atmType, int.class, IApplicationThread.class},
                    atm, PID, new IApplicationThread.Default());
            set(process, "uid", UID);
            set(process, "packageName", PACKAGE);
            set(process, "processName", PACKAGE);
            set(process, "ready", true);
            ((List<Object>) get(atm, "mProcesses")).add(process);
            ((List<Object>) get(process, "activities")).add(UNSAFE.allocateInstance(type("ActivityTaskManagerService$ActivityRecord")));

            schedulerType = type("JobSchedulerService");
            jobType = type("JobSchedulerService$Job");
            runType = type("JobSchedulerService$Run");

            scheduler = make(schedulerType, new Class<?>[]{amType, atmType, pmType}, am, atm, pm);
            // Replace background thread so scheduler acts under deterministic test control.
            set(scheduler, "mThread", new Thread());
        }

        int schedule(JobInfo info) throws Exception {
            return (Integer) invoke(scheduler, "schedule", new Class<?>[]{String.class, JobInfo.class}, NAMESPACE, info);
        }

        void enqueue(JobInfo info, String action) throws Exception {
            check((Integer) invoke(scheduler, "enqueue",
                    new Class<?>[]{String.class, JobInfo.class, JobWorkItem.class},
                    NAMESPACE, info, new JobWorkItem(new Intent(action))) == JobScheduler.RESULT_SUCCESS,
                    "Work enqueue failed: " + action);
        }

        Object findJob(int id) throws Exception {
            return invoke(scheduler, "find", new Class<?>[]{String.class, String.class, int.class}, PACKAGE, NAMESPACE, id);
        }

        JobInfo getPendingJob(int id) throws Exception {
            return (JobInfo) invoke(scheduler, "getPendingJob", new Class<?>[]{String.class, int.class}, NAMESPACE, id);
        }

        void cancel(int id) throws Exception {
            invoke(scheduler, "cancel", new Class<?>[]{String.class, int.class}, NAMESPACE, id);
        }

        int runningCount() throws Exception {
            return (Integer) get(scheduler, "mRunning");
        }

        Object begin(int jobId) throws Exception {
            Object job = findJob(jobId);
            check(job != null, "Scheduled job not found: " + jobId);
            Object run = make(runType, new Class<?>[]{schedulerType, jobType}, scheduler, job);
            synchronized (scheduler) {
                set(job, "run", run);
                set(scheduler, "mRunning", runningCount() + 1);
            }
            return run;
        }

        TestRunHandle start(int jobId, TestService service) throws Exception {
            return start(jobId, service, false);
        }

        TestRunHandle start(int jobId, TestService service, boolean expired) throws Exception {
            Object job = findJob(jobId);
            if (expired) set(job, "latest", SystemClock.elapsedRealtime() - 1);
            Object run = begin(jobId);
            // Exercise production admission-to-start state and parameter construction.
            invoke(run, "started", new Class<?>[]{IBinder.class}, service.asBinder());
            return new TestRunHandle(job, run, service, (JobParameters) get(run, "params"));
        }

        int reason(int id) throws Exception {
            return (Integer) invoke(scheduler, "getPendingJobReason", new Class<?>[]{String.class, int.class}, NAMESPACE, id);
        }

        int[] reasons(int id) throws Exception {
            return (int[]) invoke(scheduler, "getPendingJobReasons",
                    new Class<?>[]{String.class, int.class}, NAMESPACE, id);
        }

        @SuppressWarnings("unchecked")
        List<PendingJobReasonsInfo> history(int id) throws Exception {
            return (List<PendingJobReasonsInfo>) invoke(scheduler, "getPendingJobReasonsHistory",
                    new Class<?>[]{String.class, int.class}, NAMESPACE, id);
        }

        void setNetworkState(Integer state) throws Exception {
            setNetworkSnapshot(state == null ? null : state < 0 || (state & 2) == 0 ? "" :
                    snapshot(41, "wifi", (state & 4) != 0, false));
        }

        void setNetworkSnapshot(String snapshot) throws Exception {
            invoke(scheduler, "setNetworkSnapshotForTest", new Class<?>[]{String.class}, snapshot);
        }

        void setStorageSpace(long total, long usable) throws Exception {
            invoke(scheduler, "setStorageSpaceForTest", new Class<?>[]{long.class, long.class}, total, usable);
        }

        void timeout(Object run, int state) throws Exception {
            invoke(scheduler, "operationTimeout", new Class<?>[]{runType, int.class}, run, state);
        }

        void setPowerStatus(int[] power) throws Exception {
            invoke(scheduler, "setPowerStatusForTest", new Class<?>[]{int[].class}, (Object) power);
        }

        void setDeviceIdleMillis(Long idleMillis) throws Exception {
            invoke(scheduler, "setDeviceIdleMillisForTest", new Class<?>[]{Long.class}, idleMillis);
        }

        void checkTimeouts() throws Exception {
            invoke(scheduler, "checkExecutionTimeouts", new Class<?>[]{});
        }

        @Override
        public void close() throws Exception {
            invoke(scheduler, "cancelAllInNamespace", new Class<?>[]{String.class}, NAMESPACE);
            ((java.util.concurrent.ScheduledThreadPoolExecutor) get(scheduler, "mTimeouts")).shutdownNow();
            ((List<?>) field(type("DeviceWork"), "sSources").get(null)).remove(scheduler);
            field(type("NotificationManagerService"), "sLast").set(null, null);
        }
    }

    private static String snapshot(int index, String kind, boolean metered, boolean roaming) {
        return "internet\t1\nnet\t" + index + "\t" + kind + "\t1500\t100000000\t100000000\t"
                + (metered ? "1" : "0") + "\t" + (roaming ? "1" : "0") + "\t0\t1\t1\n";
    }

    private static ComponentName serviceComponent() {
        return new ComponentName(PACKAGE, PACKAGE + ".TestJobService");
    }

    /**
     * Test job scheduling with NETWORK_TYPE_UNMETERED when unmetered -> job starts.
     * Network transition to metered -> running job receives stopJob with STOP_REASON_CONSTRAINT_CONNECTIVITY.
     */
    private static void testUnmeteredToMetered(Harness h) throws Exception {
        // Initial state: Internet online, unmetered (state 2: bit 2 = internet, bit 4 = metered).
        h.setNetworkState(2);
        JobInfo info = new JobInfo.Builder(101, serviceComponent())
                .setRequiredNetworkType(JobInfo.NETWORK_TYPE_UNMETERED)
                .build();
        check(h.schedule(info) == JobScheduler.RESULT_SUCCESS, "Job 101 scheduling failed");

        TestService service = new TestService();
        service.rescheduleOnStop = true;
        TestRunHandle handle = h.start(101, service);
        check(service.starts == 1, "Job 101 did not start");
        check(service.stops == 0, "Job 101 stopped prematurely");
        check(h.runningCount() == 1, "Wrong running count after start");

        // Transition to metered network (state 6 = 2 | 4).
        h.setNetworkState(6);

        check(service.stops == 1, "Job 101 was not stopped on transition to metered");
        check(service.lastStopReason == JobParameters.STOP_REASON_CONSTRAINT_CONNECTIVITY,
                "Wrong stopReason for connectivity loss: " + service.lastStopReason);
        check(service.lastInternalStopReason == JobParameters.INTERNAL_STOP_REASON_CONSTRAINTS_NOT_SATISFIED,
                "Wrong internal stopReason: " + service.lastInternalStopReason);
        check(service.lastStopParams.getJobId() == 101, "Wrong job ID in stop parameters");

        // A constraint stop is a system stop. The first two do not impose backoff.
        check(h.runningCount() == 0, "Job 101 still counted as running");
        check(h.getPendingJob(101) != null, "Job 101 was dropped despite reschedule=true");
        Object jobObj = h.findJob(101);
        int failures = (Integer) get(jobObj, "failures");
        check(failures == 0, "Constraint stop counted as an application failure: " + failures);
        check((Integer) get(jobObj, "systemStops") == 1, "System stop was not recorded");
        long earliest = (Long) get(jobObj, "earliest");
        check(earliest == 0, "First system stop imposed a retry delay");
    }

    /**
     * Test network transition to disconnected -> stopJob with STOP_REASON_CONSTRAINT_CONNECTIVITY.
     * With onStopJob returning false -> job is dropped.
     */
    private static void testUnmeteredToDisconnected(Harness h) throws Exception {
        h.setNetworkState(2); // Unmetered
        JobInfo info = new JobInfo.Builder(102, serviceComponent())
                .setRequiredNetworkType(JobInfo.NETWORK_TYPE_UNMETERED)
                .build();
        check(h.schedule(info) == JobScheduler.RESULT_SUCCESS, "Job 102 scheduling failed");

        TestService service = new TestService();
        service.rescheduleOnStop = false; // Drop on stop
        TestRunHandle handle = h.start(102, service);
        check(service.starts == 1, "Job 102 did not start");

        // Transition to disconnected (state 0).
        h.setNetworkState(0);

        check(service.stops == 1, "Job 102 was not stopped on network disconnect");
        check(service.lastStopReason == JobParameters.STOP_REASON_CONSTRAINT_CONNECTIVITY,
                "Wrong stopReason: " + service.lastStopReason);
        check(service.lastInternalStopReason == JobParameters.INTERNAL_STOP_REASON_CONSTRAINTS_NOT_SATISFIED,
                "Wrong internal stopReason: " + service.lastInternalStopReason);

        // rescheduleOnStop was false: job must be removed from scheduler.
        check(h.runningCount() == 0, "Running count not 0");
        check(h.getPendingJob(102) == null, "Job 102 was not dropped when reschedule=false");
    }

    /**
     * Test job with requireCharging() -> starts on charger, unplugging charger triggers
     * stopJob with STOP_REASON_CONSTRAINT_CHARGING.
     */
    private static void testRequireCharging(Harness h) throws Exception {
        // Initial state: AC plugged in {1, 0, 100}
        h.setPowerStatus(new int[]{1, 0, 100});
        JobInfo info = new JobInfo.Builder(103, serviceComponent())
                .setRequiresCharging(true)
                .build();
        check(h.schedule(info) == JobScheduler.RESULT_SUCCESS, "Job 103 scheduling failed");

        TestService service = new TestService();
        service.rescheduleOnStop = true;
        TestRunHandle handle = h.start(103, service);
        check(service.starts == 1, "Job 103 did not start");

        // Unplug charger (AC offline: power[0] = 0).
        h.setPowerStatus(new int[]{0, 0, 85});

        check(service.stops == 1, "Job 103 was not stopped on unplugging charger");
        check(service.lastStopReason == JobParameters.STOP_REASON_CONSTRAINT_CHARGING,
                "Wrong stopReason for charging loss: " + service.lastStopReason);
        check(service.lastInternalStopReason == JobParameters.INTERNAL_STOP_REASON_CONSTRAINTS_NOT_SATISFIED,
                "Wrong internal stopReason: " + service.lastInternalStopReason);
        check("charging constraint".equals(service.lastDebugStopReason),
                "Wrong debug reason: " + service.lastDebugStopReason);
    }

    /**
     * Test exponential backoff calculation across multiple reschedules vs drop.
     */
    private static void testRescheduleExponentialBackoff(Harness h) throws Exception {
        JobInfo info = new JobInfo.Builder(104, serviceComponent())
                .setBackoffCriteria(10_000, JobInfo.BACKOFF_POLICY_EXPONENTIAL)
                .build();
        check(h.schedule(info) == JobScheduler.RESULT_SUCCESS, "Job 104 scheduling failed");

        // Application-requested retries count as failures immediately.
        TestRunHandle first = h.start(104, new TestService());
        first.params.getCallback().jobFinished(104, true);

        Object jobObj = h.findJob(104);
        check((Integer) get(jobObj, "failures") == 1, "Failures should be 1");
        check((Integer) get(jobObj, "systemStops") == 0, "App retry counted as a system stop");
        checkDelay(jobObj, 10_000, "First application retry");
        check(first.params.getInternalStopReasonCode() == JobParameters.INTERNAL_STOP_REASON_SUCCESSFUL_FINISH,
                "jobFinished did not record its actual completion reason");

        TestRunHandle second = h.start(104, new TestService());
        second.params.getCallback().jobFinished(104, true);

        check((Integer) get(jobObj, "failures") == 2, "Failures should be 2");
        checkDelay(jobObj, 20_000, "Second application retry");

        TestRunHandle third = h.start(104, new TestService());
        third.params.getCallback().jobFinished(104, false);
        check(h.getPendingJob(104) == null, "Job 104 should be dropped when reschedule=false");
    }

    private static void checkDelay(Object job, long expected, String description) throws Exception {
        long delay = (Long) get(job, "earliest") - SystemClock.elapsedRealtime();
        check(delay >= expected - 1_000 && delay <= expected,
                description + " expected " + expected + " ms, got " + delay);
    }

    private static void testSystemStopAndMixedBackoff(Harness h) throws Exception {
        JobInfo info = new JobInfo.Builder(141, serviceComponent()).setRequiresCharging(true)
                .setBackoffCriteria(10_000, JobInfo.BACKOFF_POLICY_EXPONENTIAL).build();
        h.schedule(info);
        Object job = h.findJob(141);
        for (int stops = 1; stops <= 6; stops++) {
            h.setPowerStatus(new int[]{1, 8, 90});
            TestService service = new TestService();
            service.rescheduleOnStop = true;
            h.start(141, service);
            h.setPowerStatus(new int[]{0, 0, 90});
            check(service.stops == 1, "System stop " + stops + " was not dispatched");
            check((Integer) get(job, "failures") == 0, "System stop increased app failures");
            check((Integer) get(job, "systemStops") == stops, "System stop history was lost");
            if (stops < 3) check((Long) get(job, "earliest") == 0, "First two system stops imposed backoff");
            else checkDelay(job, stops < 6 ? 10_000 : 20_000, "System stop " + stops);
        }

        // schedule() creates a fresh history; enqueue() on an equal job retains it.
        h.schedule(info);
        Object fresh = h.findJob(141);
        check(fresh != job && (Integer) get(fresh, "failures") == 0
                        && (Integer) get(fresh, "systemStops") == 0,
                "Fresh schedule inherited retry counters");
        for (int stops = 0; stops < 2; stops++) {
            h.setPowerStatus(new int[]{1, 8, 90});
            TestService service = new TestService(); service.rescheduleOnStop = true;
            h.start(141, service);
            h.setPowerStatus(new int[]{0, 0, 90});
        }
        h.enqueue(info, "retained-history");
        check(h.findJob(141) == fresh && (Integer) get(fresh, "systemStops") == 2,
                "Enqueue reset an existing job's retry history");
        h.setPowerStatus(new int[]{1, 8, 90});
        TestRunHandle failed = h.start(141, new TestService());
        failed.params.getCallback().jobFinished(141, true);
        checkDelay(fresh, 10_000, "One app failure after two system stops");
        TestService stopped = new TestService(); stopped.rescheduleOnStop = true;
        h.start(141, stopped);
        h.setPowerStatus(new int[]{0, 0, 90});
        check((Integer) get(fresh, "failures") == 1 && (Integer) get(fresh, "systemStops") == 3,
                "Mixed application/system history was conflated");
        checkDelay(fresh, 20_000, "One app failure plus three system stops");
        h.cancel(141);

        h.setPowerStatus(new int[]{1, 8, 90});
        h.schedule(new JobInfo.Builder(142, serviceComponent()).setRequiresCharging(true)
                .setBackoffCriteria(10_000, JobInfo.BACKOFF_POLICY_EXPONENTIAL).build());
        TestService pendingStop = new TestService(); pendingStop.acknowledgeImmediately = false;
        TestRunHandle run = h.start(142, pendingStop);
        h.setPowerStatus(new int[]{0, 0, 90});
        run.params.getCallback().jobFinished(142, true);
        check((Integer) get(run.job, "failures") == 1 && (Integer) get(run.job, "systemStops") == 0,
                "jobFinished while stopping inherited the preceding constraint reason");
        check(run.params.getStopReason() == JobParameters.STOP_REASON_UNDEFINED
                        && run.params.getInternalStopReasonCode() == JobParameters.INTERNAL_STOP_REASON_SUCCESSFUL_FINISH,
                "jobFinished did not replace both stop reasons");
        run.params.getCallback().acknowledgeStopMessage(142, true);
        check((Integer) get(run.job, "failures") == 1, "Late stop acknowledgement counted a second retry");
        h.cancel(142);
    }

    private static void testBackoffCapsAndPeriodicReset(Harness h) throws Exception {
        long cap = JobInfo.MAX_BACKOFF_DELAY_MILLIS;
        h.schedule(new JobInfo.Builder(143, serviceComponent())
                .setBackoffCriteria(cap / 2, JobInfo.BACKOFF_POLICY_LINEAR).build());
        for (int attempt = 1; attempt <= 3; attempt++) {
            TestRunHandle run = h.start(143, new TestService());
            run.params.getCallback().jobFinished(143, true);
            checkDelay(run.job, attempt == 1 ? cap / 2 : cap, "Linear cap attempt " + attempt);
        }
        h.cancel(143);
        h.schedule(new JobInfo.Builder(144, serviceComponent())
                .setBackoffCriteria(cap / 2, JobInfo.BACKOFF_POLICY_EXPONENTIAL).build());
        Object capped = h.findJob(144);
        // Extreme historical counters must saturate without negative delays or division by zero.
        set(capped, "failures", Integer.MAX_VALUE);
        set(capped, "systemStops", Integer.MAX_VALUE);
        TestRunHandle extreme = h.start(144, new TestService());
        extreme.params.getCallback().jobFinished(144, true);
        checkDelay(capped, cap, "Saturated exponential retry");
        check((Integer) get(capped, "failures") == Integer.MAX_VALUE, "Failure counter overflowed");
        h.cancel(144);

        h.setPowerStatus(new int[]{1, 8, 90});
        h.schedule(new JobInfo.Builder(145, serviceComponent()).setRequiresCharging(true)
                .setPeriodic(900_000, 300_000).build());
        Object periodic = h.findJob(145);
        long original = (Long) get(periodic, "latest");
        TestRunHandle first = h.start(145, new TestService());
        first.params.getCallback().jobFinished(145, true);
        check((Long) get(periodic, "latest") == Long.MAX_VALUE
                        && (Long) get(periodic, "originalLatest") == original,
                "Periodic retry retained an execution deadline or lost its original window");
        TestService systemStop = new TestService(); systemStop.rescheduleOnStop = true;
        h.start(145, systemStop);
        h.setPowerStatus(new int[]{0, 0, 90});
        check((Integer) get(periodic, "failures") == 1 && (Integer) get(periodic, "systemStops") == 1,
                "Periodic retry did not retain both histories");
        h.setPowerStatus(new int[]{1, 8, 90});
        TestRunHandle success = h.start(145, new TestService());
        check(!success.params.isOverrideDeadlineExpired(), "Retried periodic job reported the old window expired");
        success.params.getCallback().jobFinished(145, false);
        check((Integer) get(periodic, "failures") == 0 && (Integer) get(periodic, "systemStops") == 0
                        && (Integer) get(periodic, "abandonedFailures") == 0,
                "Next periodic window retained failure history");
        check((Long) get(periodic, "latest") == original + 900_000
                        && (Long) get(periodic, "earliest") == original + 600_000,
                "Periodic retry shifted the original cadence");

        // Exercise the same production window calculation with explicit elapsed
        // times. Subtracting 37.5 minutes from a fresh host's uptime creates an
        // invalid negative boundary, so it cannot represent missed valid windows.
        // The full callback, retry and counter-reset path remains exercised above.
        checkPeriodicWindow(h, periodic, 1_000_000, 3_250_000, 3_400_000, 3_700_000,
                "Missed periodic windows retain cadence");
        checkPeriodicWindow(h, periodic, 0, 60_000, 600_000, 900_000,
                "Zero is a valid early-boot periodic boundary");
        checkPeriodicWindow(h, periodic, 1_000_000, 1_590_000, 2_500_000, 2_800_000,
                "Custom flex skips a window too close to completion");
        checkPeriodicWindow(h, periodic, -1, 60_000, 660_000, 960_000,
                "Negative periodic boundary recovers from completion");
        checkPeriodicWindow(h, periodic, Long.MAX_VALUE, 60_000, 660_000, 960_000,
                "Missing periodic boundary recovers from completion");
        h.cancel(145);
    }

    private static void checkPeriodicWindow(Harness h, Object job, long original, long now,
            long expectedEarliest, long expectedLatest, String scenario) throws Exception {
        synchronized (h.scheduler) {
            set(job, "originalLatest", original);
            invoke(h.scheduler, "reschedulePeriodic", new Class<?>[]{h.jobType, long.class}, job, now);
            long earliest = (Long) get(job, "earliest");
            long latest = (Long) get(job, "latest");
            String context = scenario + " (original=" + original + ", now=" + now + ")";
            check(earliest == expectedEarliest,
                    context + ": earliest " + earliest + ", expected " + expectedEarliest);
            check(latest == expectedLatest,
                    context + ": latest " + latest + ", expected " + expectedLatest);
            check((Long) get(job, "originalLatest") == expectedLatest,
                    context + ": next periodic boundary was not advanced");
        }
    }

    private static void testEffectiveChargingPriority(Harness h) throws Exception {
        h.setPowerStatus(new int[]{1, 8, 90});
        h.schedule(new JobInfo.Builder(146, serviceComponent()).setRequiresCharging(true)
                .setPriority(JobInfo.PRIORITY_HIGH).build());
        Object job = h.findJob(146);
        int[] priorities = {JobInfo.PRIORITY_HIGH, JobInfo.PRIORITY_HIGH, JobInfo.PRIORITY_DEFAULT,
                JobInfo.PRIORITY_DEFAULT, JobInfo.PRIORITY_LOW, JobInfo.PRIORITY_LOW, JobInfo.PRIORITY_MIN};
        for (int failures = 0; failures < priorities.length; failures++) {
            check((Integer) invoke(job, "effectivePriority", new Class<?>[0]) == priorities[failures],
                    "Wrong effective priority after " + failures + " app failures");
            h.setPowerStatus(new int[]{1, 8, 90});
            TestService service = new TestService(); service.rescheduleOnStop = true;
            TestRunHandle run = h.start(146, service);
            set(run.run, "topStarted", true); // Explicit fixture: this attempt started while the app was top.
            h.setPowerStatus(new int[]{1, 2 | 8, 10});
            check(service.stops == (failures < 4 ? 0 : 1),
                    "Charging used raw priority after " + failures + " failures");
            if (service.stops != 0) {
                check((Integer) get(job, "failures") == failures, "Charging system stop decayed priority");
                h.setPowerStatus(new int[]{1, 8, 90});
                run = h.start(146, new TestService());
            }
            run.params.getCallback().jobFinished(146, true);
        }
        h.cancel(146);

        h.setNetworkState(2);
        JobInfo[] privileged = {
                new JobInfo.Builder(147, serviceComponent()).setExpedited(true).build(),
                new JobInfo.Builder(148, serviceComponent()).setUserInitiated(true)
                        .setRequiredNetworkType(JobInfo.NETWORK_TYPE_ANY).build()};
        for (JobInfo info : privileged) {
            h.schedule(info);
            Object privilegedJob = h.findJob(info.getId());
            for (int i = 0; i < 6; i++) {
                TestRunHandle run = h.start(info.getId(), new TestService());
                run.params.getCallback().jobFinished(info.getId(), true);
            }
            int expected = info.isExpedited() ? JobInfo.PRIORITY_HIGH : info.getPriority();
            check((Integer) invoke(privilegedJob, "effectivePriority", new Class<?>[0]) == expected,
                    "Expedited/user-initiated priority exception was lost");
            h.cancel(info.getId());
        }
    }

    private static void testAbandonedTimeout(Harness h) throws Exception {
        h.schedule(new JobInfo.Builder(149, serviceComponent())
                .setBackoffCriteria(10_000, JobInfo.BACKOFF_POLICY_LINEAR).build());
        Object job = h.findJob(149);
        check((Boolean) get(job, "handleAbandonedJobs") == android.app.job.Flags.handleAbandonedJobs(),
                "Default-disabled abandoned override was not read from app compatibility metadata");
        for (int attempt = 1; attempt <= 4; attempt++) {
            TestService service = new TestService(); service.rescheduleOnStop = true;
            TestRunHandle run = h.start(149, service);
            run.params.getCallback().handleAbandonedJob(149);
            check(h.runningCount() == 1 && service.stops == 0 && !(Boolean) get(run.run, "done"),
                    "Abandoned callback prematurely finished execution");
            h.timeout(run.run, 2);
            boolean handled = android.app.job.Flags.handleAbandonedJobs();
            check(service.lastStopReason == (handled ? JobParameters.STOP_REASON_TIMEOUT_ABANDONED
                            : JobParameters.STOP_REASON_TIMEOUT), "Abandoned timeout used wrong public reason");
            check((Integer) get(job, "failures") == attempt && (Integer) get(job, "systemStops") == 0,
                    "Abandoned timeout was not an application failure");
            check((Integer) get(job, "abandonedFailures") == (handled ? attempt : 0),
                    "Abandoned failure counter was not retained");
            checkDelay(job, handled && attempt == 4 ? 80_000 : attempt * 10_000L,
                    "Abandoned timeout backoff " + attempt);
            run.params.getCallback().handleAbandonedJob(149);
            check(h.runningCount() == 0, "Late abandoned callback resurrected a completed run");
        }
        h.cancel(149);
    }

    /**
     * Test execution timeout triggers STOP_REASON_TIMEOUT.
     */
    private static void testExecutionTimeout(Harness h) throws Exception {
        JobInfo info = new JobInfo.Builder(105, serviceComponent()).build();
        check(h.schedule(info) == JobScheduler.RESULT_SUCCESS, "Job 105 scheduling failed");

        TestService service = new TestService();
        service.rescheduleOnStop = true;
        TestRunHandle handle = h.start(105, service);
        check(service.starts == 1, "Job 105 did not start");

        // Simulate 10+ minutes elapsed execution time.
        long tenMinutesAndOneSecond = 10 * 60_000L + 1_000L;
        set(handle.run, "started", SystemClock.elapsedRealtime() - tenMinutesAndOneSecond);

        // Evaluate timeouts
        h.checkTimeouts();

        check(service.stops == 1, "Job 105 was not stopped on execution timeout");
        check(service.lastStopReason == JobParameters.STOP_REASON_TIMEOUT,
                "Wrong stopReason for timeout: " + service.lastStopReason);
        check(service.lastInternalStopReason == JobParameters.INTERNAL_STOP_REASON_TIMEOUT,
                "Wrong internal stopReason: " + service.lastInternalStopReason);
        check("timed out".equals(service.lastDebugStopReason),
                "Wrong debug reason: " + service.lastDebugStopReason);
        check((Integer) get(handle.job, "failures") == 1 && (Integer) get(handle.job, "systemStops") == 0,
                "Execution timeout was not an application failure");
        checkDelay(handle.job, info.getInitialBackoffMillis(), "Execution timeout retry");
        h.cancel(105);
    }

    /**
     * Test job cancellation by app triggers STOP_REASON_CANCELLED_BY_APP.
     */
    private static void testCancelledByApp(Harness h) throws Exception {
        JobInfo info = new JobInfo.Builder(106, serviceComponent()).build();
        check(h.schedule(info) == JobScheduler.RESULT_SUCCESS, "Job 106 scheduling failed");

        TestService service = new TestService();
        TestRunHandle handle = h.start(106, service);
        check(service.starts == 1, "Job 106 did not start");

        // Cancel via JobScheduler
        h.cancel(106);

        check(service.stops == 1, "Job 106 was not stopped on cancel");
        check(service.lastStopReason == JobParameters.STOP_REASON_CANCELLED_BY_APP,
                "Wrong stopReason for cancel: " + service.lastStopReason);
        check(service.lastInternalStopReason == JobParameters.INTERNAL_STOP_REASON_CANCELED,
                "Wrong internal stopReason: " + service.lastInternalStopReason);
    }

    /**
     * Test device idle constraint loss triggers STOP_REASON_CONSTRAINT_DEVICE_IDLE.
     */
    private static void testDeviceIdleConstraint(Harness h) throws Exception {
        // Device idle for 35 minutes (> 31 min AOSP threshold)
        h.setDeviceIdleMillis(35 * 60_000L);
        JobInfo info = new JobInfo.Builder(107, serviceComponent())
                .setRequiresDeviceIdle(true)
                .build();
        check(h.schedule(info) == JobScheduler.RESULT_SUCCESS, "Job 107 scheduling failed");

        TestService service = new TestService();
        TestRunHandle handle = h.start(107, service);
        check(service.starts == 1, "Job 107 did not start");

        // User interacts with device: idle time drops to 0.
        h.setDeviceIdleMillis(0L);

        check(service.stops == 1, "Job 107 was not stopped when device became active");
        check(service.lastStopReason == JobParameters.STOP_REASON_CONSTRAINT_DEVICE_IDLE,
                "Wrong stopReason for device idle loss: " + service.lastStopReason);
        check(service.lastInternalStopReason == JobParameters.INTERNAL_STOP_REASON_CONSTRAINTS_NOT_SATISFIED,
                "Wrong internal stopReason: " + service.lastInternalStopReason);
    }

    /**
     * Test battery-not-low constraint loss triggers STOP_REASON_CONSTRAINT_BATTERY_NOT_LOW.
     */
    private static void testBatteryNotLowConstraint(Harness h) throws Exception {
        // Battery normal: discharging, not low {0, 0, 70}
        h.setPowerStatus(new int[]{0, 0, 70});
        JobInfo info = new JobInfo.Builder(108, serviceComponent())
                .setRequiresBatteryNotLow(true)
                .build();
        check(h.schedule(info) == JobScheduler.RESULT_SUCCESS, "Job 108 scheduling failed");

        TestService service = new TestService();
        TestRunHandle handle = h.start(108, service);
        check(service.starts == 1, "Job 108 did not start");

        // Battery level drops low (status bit 2 = low)
        h.setPowerStatus(new int[]{0, 2, 10});

        check(service.stops == 1, "Job 108 was not stopped on battery low");
        check(service.lastStopReason == JobParameters.STOP_REASON_CONSTRAINT_BATTERY_NOT_LOW,
                "Wrong stopReason for battery not low: " + service.lastStopReason);
        check(service.lastInternalStopReason == JobParameters.INTERNAL_STOP_REASON_CONSTRAINTS_NOT_SATISFIED,
                "Wrong internal stopReason: " + service.lastInternalStopReason);
    }

    /**
     * Test that network changes when constraint is STILL satisfied do NOT stop the job,
     * but deliver onNetworkChanged(params).
     */
    private static void testNetworkChangeStillSatisfied(Harness h) throws Exception {
        // Initial state: any network satisfied (state 2: unmetered)
        h.setNetworkState(2);
        JobInfo info = new JobInfo.Builder(109, serviceComponent())
                .setRequiredNetworkType(JobInfo.NETWORK_TYPE_ANY)
                .build();
        check(h.schedule(info) == JobScheduler.RESULT_SUCCESS, "Job 109 scheduling failed");

        TestService service = new TestService();
        TestRunHandle handle = h.start(109, service);
        check(service.starts == 1, "Job 109 did not start");

        // Network changes to metered (state 6). Both have internet, so NETWORK_TYPE_ANY is still met!
        h.setNetworkState(6);

        check(service.stops == 0, "Job 109 should NOT be stopped when network constraint is still satisfied");
        check(h.runningCount() == 1, "Job 109 should still be running");
        h.cancel(109);
        check(h.runningCount() == 0, "Job 109 cleanup failed");
    }

    /**
     * Test that override deadline expiration prevents constraint-loss cancellation.
     */
    private static void testOverrideDeadlineNotCancelled(Harness h) throws Exception {
        h.setNetworkState(2); // Unmetered
        JobInfo info = new JobInfo.Builder(110, serviceComponent())
                .setOverrideDeadline(60_000)
                .setRequiredNetworkType(JobInfo.NETWORK_TYPE_UNMETERED)
                .build();
        check(h.schedule(info) == JobScheduler.RESULT_SUCCESS, "Job 110 scheduling failed");

        TestService service = new TestService();
        // Start job with overrideDeadlineExpired = true
        TestRunHandle handle = h.start(110, service, true);
        check(service.starts == 1, "Job 110 did not start");
        check(handle.params.isOverrideDeadlineExpired(), "Override deadline should be expired");

        // Transition to metered
        h.setNetworkState(6);

        check(service.stops == 0, "Job 110 with override deadline expired should NOT be cancelled on constraint loss");
        check(h.runningCount() == 1, "Job 110 should still be running");
        h.cancel(110);
        check(h.runningCount() == 0, "Job 110 cleanup failed");
    }

    private static void testUnknownConstraints(Harness h) throws Exception {
        h.setNetworkState(-1);
        h.schedule(new JobInfo.Builder(120, serviceComponent()).setRequiredNetworkType(JobInfo.NETWORK_TYPE_ANY).build());
        check(h.reason(120) == JobScheduler.PENDING_JOB_REASON_CONSTRAINT_CONNECTIVITY,
                "Unknown network was advertised as satisfying a job");
        h.cancel(120);
        h.setPowerStatus(new int[0]);
        h.schedule(new JobInfo.Builder(121, serviceComponent()).setRequiresCharging(true).build());
        check(h.reason(121) == JobScheduler.PENDING_JOB_REASON_CONSTRAINT_CHARGING, "Missing power accepted as charging");
        h.setPowerStatus(new int[]{1, 255, 255});
        check(h.reason(121) == JobScheduler.PENDING_JOB_REASON_CONSTRAINT_CHARGING, "Unknown battery flags accepted as stable power");
        h.cancel(121);
        h.schedule(new JobInfo.Builder(122, serviceComponent()).setRequiresBatteryNotLow(true).build());
        check(h.reason(122) == JobScheduler.PENDING_JOB_REASON_CONSTRAINT_BATTERY_NOT_LOW, "Unknown battery accepted as not low");
        h.cancel(122);
        h.setDeviceIdleMillis(0L);
        h.schedule(new JobInfo.Builder(123, serviceComponent()).setRequiresDeviceIdle(true).build());
        check(h.reason(123) == JobScheduler.PENDING_JOB_REASON_CONSTRAINT_DEVICE_IDLE, "Unknown/active device accepted as idle");
        h.cancel(123);
        h.setStorageSpace(0, 0);
        h.schedule(new JobInfo.Builder(124, serviceComponent()).setRequiresStorageNotLow(true).build());
        check(h.reason(124) == JobScheduler.PENDING_JOB_REASON_CONSTRAINT_STORAGE_NOT_LOW, "Unknown storage accepted as available");
        h.cancel(124);

        // A deadline can run without a network, but the app must actually receive null.
        h.schedule(new JobInfo.Builder(125, serviceComponent()).setRequiredNetworkType(JobInfo.NETWORK_TYPE_ANY)
                .setOverrideDeadline(60_000).build());
        TestService service = new TestService();
        TestRunHandle run = h.start(125, service, true);
        check(service.starts == 1 && run.params.isOverrideDeadlineExpired(), "Deadline did not release a one-shot job");
        check(run.params.getNetwork() == null, "Unavailable network replaced by a synthetic Network");
        h.cancel(125);
    }

    private static void testRequestMatchingAndHandover(Harness h) throws Exception {
        h.setNetworkSnapshot(snapshot(41, "wifi", true, false));
        h.schedule(new JobInfo.Builder(126, serviceComponent()).setRequiredNetworkType(JobInfo.NETWORK_TYPE_CELLULAR).build());
        check(h.reason(126) == JobScheduler.PENDING_JOB_REASON_CONSTRAINT_CONNECTIVITY,
                "Metered Wi-Fi was confused with cellular transport");
        h.setNetworkSnapshot(snapshot(42, "cellular", true, false));
        check(h.reason(126) == JobScheduler.PENDING_JOB_REASON_UNDEFINED, "Observed cellular transport not matched");
        TestService cellular = new TestService();
        TestRunHandle cell = h.start(126, cellular);
        check(new Network(142).equals(cell.params.getNetwork()), "Actual interface netId not passed to JobParameters");
        h.setNetworkSnapshot(snapshot(41, "wifi", true, false));
        check(cellular.stops == 1 && cellular.lastStopReason == JobParameters.STOP_REASON_CONSTRAINT_CONNECTIVITY,
                "Lost cellular transport did not stop running job");

        NetworkRequest request = new NetworkRequest.Builder().addTransportType(NetworkCapabilities.TRANSPORT_WIFI)
                .addCapability(NetworkCapabilities.NET_CAPABILITY_INTERNET)
                .addCapability(NetworkCapabilities.NET_CAPABILITY_NOT_ROAMING).build();
        h.setNetworkSnapshot(snapshot(41, "wifi", false, true));
        h.schedule(new JobInfo.Builder(127, serviceComponent()).setRequiredNetwork(request).build());
        check(h.reason(127) == JobScheduler.PENDING_JOB_REASON_CONSTRAINT_CONNECTIVITY,
                "Custom network request ignored roaming requirement");
        h.setNetworkSnapshot(snapshot(41, "wifi", false, false));
        TestService service = new TestService();
        TestRunHandle run = h.start(127, service);
        h.setNetworkSnapshot(snapshot(43, "wifi", false, false));
        check(service.stops == 0 && service.networkChanges == 1, "Satisfied handover did not deliver exactly one network callback");
        check(new Network(143).equals(run.params.getNetwork()), "Handover passed the old network");
        h.setNetworkSnapshot(snapshot(43, "wifi", true, false));
        check(service.networkChanges == 1, "Capability-only change fabricated a different Network");
        service.failNetworkCallback = true;
        h.setNetworkSnapshot(snapshot(44, "wifi", false, false));
        check(h.runningCount() == 0 && h.getPendingJob(127) != null,
                "Network callback death stranded/dropped running job instead of retrying");
        check((Integer) get(run.job, "failures") == 0 && (Integer) get(run.job, "systemStops") == 1,
                "Network callback death fabricated an application completion failure");
        long generation = (Long) get(h.scheduler, "mHostGeneration");
        synchronized (get(run.run, "dispatch")) {
            invoke(h.scheduler, "updateNetwork", new Class<?>[]{h.runType, Network.class, long.class},
                    run.run, new Network(145), generation);
        }
        check(service.networkChanges == 2, "Finished run received a late network callback");
        h.cancel(127);
    }

    private static void testDeadlineBoundaries(Harness h) throws Exception {
        h.setPowerStatus(new int[]{1, 8, 90});
        h.schedule(new JobInfo.Builder(128, serviceComponent()).setRequiresCharging(true).setPeriodic(900_000).build());
        Object job = h.findJob(128);
        set(job, "earliest", 0L);
        set(job, "latest", SystemClock.elapsedRealtime() - 1);
        h.setPowerStatus(new int[]{0, 0, 90});
        check(h.reason(128) == JobScheduler.PENDING_JOB_REASON_CONSTRAINT_CHARGING, "Periodic window bypassed constraints");
        h.setPowerStatus(new int[]{1, 8, 90});
        TestService periodic = new TestService();
        TestRunHandle periodicRun = h.start(128, periodic);
        check(periodicRun.params.isOverrideDeadlineExpired(), "Expired periodic window flag differs from JobServiceContext");
        h.setPowerStatus(new int[]{0, 0, 90});
        check(periodic.stops == 1, "Expired periodic window prevented constraint cancellation");
        h.cancel(128);

        h.setNetworkState(2);
        h.schedule(new JobInfo.Builder(129, serviceComponent()).setRequiredNetworkType(JobInfo.NETWORK_TYPE_ANY)
                .setOverrideDeadline(60_000).build());
        TestService service = new TestService();
        TestRunHandle run = h.start(129, service);
        check(!run.params.isOverrideDeadlineExpired(), "Future deadline reported expired at start");
        set(run.job, "latest", SystemClock.elapsedRealtime() - 1);
        h.setNetworkState(0);
        check(service.stops == 0, "Deadline reached during execution did not override an explicit constraint");
        check(service.networkChanges == 1 && run.params.getNetwork() == null,
                "Deadline run retained a network that no longer exists");
        h.cancel(129);
        check(service.stops == 1 && service.lastStopReason == JobParameters.STOP_REASON_CANCELLED_BY_APP,
                "Deadline immunity incorrectly prevented explicit cancellation");
    }

    private static void testStartStopOrdering(Harness h) throws Exception {
        h.schedule(new JobInfo.Builder(130, serviceComponent()).build());
        TestService delayed = new TestService();
        delayed.acknowledgeStartImmediately = false;
        TestRunHandle run = h.start(130, delayed);
        IJobCallback callback = run.params.getCallback();
        callback.acknowledgeStopMessage(130, false);
        callback.jobFinished(130, false);
        check(h.runningCount() == 1, "Unsolicited finish/stop acknowledgement retired a starting run");
        h.cancel(130);
        check(delayed.stops == 0, "stopJob preceded acknowledgement of onStartJob");
        callback.acknowledgeStartMessage(130, true);
        check(delayed.stops == 1 && h.runningCount() == 0 && h.getPendingJob(130) == null,
                "Cancellation while starting did not stop after start acknowledgement");
        callback.acknowledgeStartMessage(130, true);
        callback.acknowledgeStopMessage(130, true);
        check(delayed.stops == 1 && h.getPendingJob(130) == null, "Late acknowledgement resurrected canceled work");

        h.setNetworkState(2);
        h.schedule(new JobInfo.Builder(131, serviceComponent()).setRequiredNetworkType(JobInfo.NETWORK_TYPE_ANY).build());
        TestService starting = new TestService();
        starting.acknowledgeStartImmediately = false;
        TestRunHandle pending = h.start(131, starting);
        h.setNetworkSnapshot(snapshot(48, "wifi", false, false));
        check(starting.networkChanges == 0, "Network callback arrived before start acknowledgement");
        pending.params.getCallback().acknowledgeStartMessage(131, true);
        check(starting.networkChanges == 1 && new Network(148).equals(pending.params.getNetwork()),
                "Deferred network handover was not delivered after start acknowledgement");
        h.cancel(131);
    }

    private static void testBindingAndCallbackTimeouts(Harness h) throws Exception {
        h.setPowerStatus(new int[]{1, 8, 90});
        h.schedule(new JobInfo.Builder(132, serviceComponent()).setRequiresCharging(true).build());
        Object binding = h.begin(132);
        h.setPowerStatus(new int[]{0, 0, 90});
        check(h.runningCount() == 0 && h.getPendingJob(132) != null, "Constraint loss during bind discarded a pending job");
        Object bindingJob = h.findJob(132);
        check((Integer) get(bindingJob, "failures") == 0 && (Integer) get(bindingJob, "systemStops") == 1
                        && (Long) get(bindingJob, "earliest") == 0,
                "Binding cleanup lost its constraint stop reason before JobParameters existed");
        TestService late = new TestService();
        invoke(binding, "started", new Class<?>[]{IBinder.class}, late.asBinder());
        check(late.starts == 0, "Retired binding delivered late onStartJob");
        h.cancel(132);

        h.schedule(new JobInfo.Builder(133, serviceComponent()).build());
        Object unbound = h.begin(133);
        h.timeout(unbound, 0); // Production binding timeout boundary, without sleeping.
        check(h.runningCount() == 0 && h.getPendingJob(133) != null, "Binding timeout did not preserve job for retry");
        Object timedOutJob = h.findJob(133);
        check((Integer) get(timedOutJob, "failures") == 1 && (Integer) get(timedOutJob, "systemStops") == 0,
                "Binding ANR timeout was not counted as an application failure");
        h.cancel(133);

        h.schedule(new JobInfo.Builder(134, serviceComponent()).build());
        TestService noStartAck = new TestService();
        noStartAck.acknowledgeStartImmediately = false;
        TestRunHandle start = h.start(134, noStartAck);
        h.timeout(start.run, 1);
        check(h.runningCount() == 0 && h.getPendingJob(134) == null, "Start callback timeout did not retire unresponsive job");
        start.params.getCallback().acknowledgeStartMessage(134, true);
        check(noStartAck.stops == 0, "Expired starting run delivered a late stop");

        h.setPowerStatus(new int[]{1, 8, 90});
        h.schedule(new JobInfo.Builder(135, serviceComponent()).setRequiresCharging(true).build());
        TestService noStopAck = new TestService();
        noStopAck.acknowledgeImmediately = false;
        TestRunHandle stop = h.start(135, noStopAck);
        h.setPowerStatus(new int[]{0, 0, 90});
        h.setPowerStatus(new int[]{0, 0, 90});
        check(noStopAck.stops == 1 && h.runningCount() == 1, "Repeated constraint event duplicated stopJob");
        h.timeout(stop.run, 3);
        check(h.runningCount() == 0 && h.getPendingJob(135) != null, "Stop callback timeout discarded work instead of retrying");
        check((Integer) get(stop.job, "failures") == 0 && (Integer) get(stop.job, "systemStops") == 1
                        && (Long) get(stop.job, "earliest") == 0,
                "Stop acknowledgement timeout replaced the original constraint reason");
        check(get(stop.run, "timeout") == null, "Completed run retained its timer");
        check(((java.util.concurrent.ScheduledThreadPoolExecutor) get(h.scheduler, "mTimeouts")).getQueue().isEmpty(),
                "Completed attempts retained canceled timers");
        h.cancel(135);
    }

    private static void testWorkCompletionOutcomes(Harness h) throws Exception {
        // Explicit completion and missing start acknowledgement both discard an
        // unfinished nonperiodic queue unless the application requests a retry.
        for (int outcome = 0; outcome < 4; outcome++) {
            int id = 150 + outcome;
            h.setPowerStatus(new int[]{1, 8, 90});
            JobInfo info = new JobInfo.Builder(id, serviceComponent()).setRequiresCharging(true).build();
            h.enqueue(info, "delivered");
            h.enqueue(info, "pending");
            TestService service = new TestService();
            service.acknowledgeStartImmediately = outcome != 0;
            service.ongoingOnStart = outcome != 1;
            TestRunHandle run = h.start(id, service);
            if (outcome != 1) {
                check(run.params.dequeueWork() != null, "Fixture did not obtain outstanding work");
                if (outcome == 0) h.timeout(run.run, 1);
                else if (outcome == 2) run.params.getCallback().jobFinished(id, false);
                else h.setPowerStatus(new int[]{0, 0, 90});
            }
            check(h.getPendingJob(id) == null && h.runningCount() == 0,
                    "Nonretry completion retained unfinished work for outcome " + outcome);
            check(((Collection<?>) get(run.job, "work")).isEmpty()
                            && ((Map<?, ?>) get(run.job, "delivered")).isEmpty(),
                    "Retired run retained pending/delivered work references");
            run.params.getCallback().acknowledgeStartMessage(id, true);
            run.params.getCallback().jobFinished(id, true);
            check(run.params.dequeueWork() == null && h.getPendingJob(id) == null,
                    "Late callback resurrected discarded work");
        }

        h.setPowerStatus(new int[]{1, 8, 90});
        JobInfo info = new JobInfo.Builder(154, serviceComponent()).setRequiresCharging(true).build();
        h.enqueue(info, "retry-delivered"); h.enqueue(info, "retry-pending");
        TestService service = new TestService(); service.rescheduleOnStop = true;
        TestRunHandle first = h.start(154, service);
        JobWorkItem delivered = first.params.dequeueWork();
        h.setPowerStatus(new int[]{0, 0, 90});
        check(h.getPendingJob(154) != null, "Requested retry discarded unfinished work");
        h.setPowerStatus(new int[]{1, 8, 90});
        TestRunHandle retry = h.start(154, new TestService());
        JobWorkItem redelivered = retry.params.dequeueWork();
        check(redelivered != null && delivered.getWorkId() == redelivered.getWorkId()
                        && redelivered.getDeliveryCount() == 2,
                "Retry did not redeliver outstanding work with its original ID");
        check("retry-pending".equals(retry.params.dequeueWork().getIntent().getAction()),
                "Retry lost the pending work behind delivered work");
        retry.params.getCallback().jobFinished(154, false);
        check(h.getPendingJob(154) == null, "A final no-retry completion retained redelivered work");

        JobInfo failedInfo = new JobInfo.Builder(155, serviceComponent()).build();
        h.enqueue(failedInfo, "start-exception");
        TestService failing = new TestService(); failing.failStartCallback = true;
        TestRunHandle failed = h.start(155, failing);
        check(h.runningCount() == 1 && (Integer) get(failed.run, "state") == 1,
                "startJob exception skipped the start acknowledgement timeout");
        h.timeout(failed.run, 1);
        check(h.getPendingJob(155) == null && h.runningCount() == 0,
                "A start exception without acknowledgement retried queued work");

        JobInfo nullInfo = new JobInfo.Builder(156, serviceComponent()).build();
        h.enqueue(nullInfo, "null-binding");
        Object nullRun = h.begin(156);
        ((android.app.IServiceConnection) get(nullRun, "connection"))
                .connected(serviceComponent(), null, null, false);
        check(h.getPendingJob(156) == null && h.runningCount() == 0,
                "Explicit null binding retried queued work");

        JobInfo deadInfo = new JobInfo.Builder(157, serviceComponent()).build();
        h.enqueue(deadInfo, "binding-died");
        Object deadRun = h.begin(157);
        ((android.app.IServiceConnection) get(deadRun, "connection"))
                .connected(serviceComponent(), null, null, true);
        Object deadJob = h.findJob(157);
        check(deadJob != null && (Integer) get(deadJob, "systemStops") == 1
                        && (Integer) get(deadJob, "failures") == 0,
                "Confirmed binding death did not preserve a retry with its system reason");
        h.cancel(157);
    }

    private static void testStorageAndStablePower(Harness h) throws Exception {
        long gib = 1024L * 1024 * 1024;
        h.setStorageSpace(20 * gib, gib);
        h.schedule(new JobInfo.Builder(136, serviceComponent()).setRequiresStorageNotLow(true).build());
        TestService storage = new TestService();
        h.start(136, storage);
        h.setStorageSpace(20 * gib, 400L * 1024 * 1024);
        check(storage.stops == 1 && storage.lastStopReason == JobParameters.STOP_REASON_CONSTRAINT_STORAGE_NOT_LOW,
                "Low data-volume storage did not stop a storage-constrained job");

        h.setPowerStatus(new int[]{1, 2 | 8, 10});
        h.schedule(new JobInfo.Builder(137, serviceComponent()).setRequiresBatteryNotLow(true).build());
        check(h.reason(137) == JobScheduler.PENDING_JOB_REASON_CONSTRAINT_BATTERY_NOT_LOW,
                "AC power hid a genuinely low battery");
        h.cancel(137);
        h.schedule(new JobInfo.Builder(138, serviceComponent()).setRequiresCharging(true).build());
        check(h.reason(138) == JobScheduler.PENDING_JOB_REASON_CONSTRAINT_CHARGING,
                "Low charging battery accepted as stable power for a background job");
        h.setPowerStatus(new int[]{1, 128, 255});
        check(h.reason(138) == JobScheduler.PENDING_JOB_REASON_UNDEFINED, "Observed AC desktop without battery was blocked");
        h.setPowerStatus(new int[]{255, 128, 255});
        check(h.reason(138) == JobScheduler.PENDING_JOB_REASON_CONSTRAINT_CHARGING, "Unknown AC fabricated charging on a desktop");
        h.cancel(138);
    }

    private static void testTopPackageChargingExemption(Harness h) throws Exception {
        int id = 170;
        h.setPowerStatus(new int[]{1, 2 | 8, 10});
        h.schedule(new JobInfo.Builder(id, serviceComponent()).setRequiresCharging(true).build());
        Object record = UNSAFE.allocateInstance(type("ActivityTaskManagerService$ActivityRecord"));
        Object process = UNSAFE.allocateInstance(type("ActivityTaskManagerService$ProcessRecord"));
        set(process, "uid", UID); // The legacy profile may assign unrelated packages the same UID.
        set(process, "packageName", "different.top.package");
        set(record, "app", process);
        set(h.atm, "mTopResumed", record);
        try {
            check(h.reason(id) == JobScheduler.PENDING_JOB_REASON_CONSTRAINT_CHARGING,
                    "A different top package granted the charging exemption through a shared UID");
            set(process, "packageName", PACKAGE);
            check(h.reason(id) == JobScheduler.PENDING_JOB_REASON_UNDEFINED,
                    "The actual top job package did not receive its charging exemption");
        } finally {
            set(h.atm, "mTopResumed", null);
            h.cancel(id);
        }
    }

    private static void testStoragePressureWithoutNotification(Harness h) throws Exception {
        int jobId = 171;
        long gib = 1024L * 1024 * 1024;
        long lowThreshold = 500L * 1024 * 1024;
        h.setStorageSpace(20 * gib, gib);
        h.schedule(new JobInfo.Builder(jobId, serviceComponent()).setRequiresStorageNotLow(true).build());
        TestService service = new TestService();
        service.rescheduleOnStop = true;
        TestRunHandle run = h.start(jobId, service);

        // Other Windows processes can consume disk space without sending a Gesher event.
        set(h.scheduler, "mStorageSpaceOverride", new long[]{20 * gib, lowThreshold});
        invoke(h.scheduler, "schedulePass", new Class<?>[]{boolean.class}, false);
        check(service.stops == 1 && service.lastStopReason == JobParameters.STOP_REASON_CONSTRAINT_STORAGE_NOT_LOW,
                "Periodic host observation missed the low storage threshold");
        List<PendingJobReasonsInfo> activeHistory = h.history(jobId);
        check(!activeHistory.isEmpty() && Arrays.equals(
                        activeHistory.get(activeHistory.size() - 1).getPendingJobReasons(),
                        new int[]{JobScheduler.PENDING_JOB_REASON_CONSTRAINT_STORAGE_NOT_LOW}),
                "Active-run constraint loss was not recorded before stop acknowledgement");
        check(h.reason(jobId) == JobScheduler.PENDING_JOB_REASON_CONSTRAINT_STORAGE_NOT_LOW,
                "Retry lost its original storage requirement");
        check((Integer) get(run.job, "systemStops") == 1, "Storage loss was not recorded as a system stop");
        h.setStorageSpace(20 * gib, lowThreshold + 1);
        check(h.reason(jobId) == JobScheduler.PENDING_JOB_REASON_UNDEFINED,
                "Recovered storage did not make the job eligible again");
        h.cancel(jobId);
    }

    private static void testPendingReasonsAndHistory(Harness h) throws Exception {
        int id = 172;
        long gib = 1024L * 1024 * 1024;
        h.setNetworkState(0);
        h.setPowerStatus(new int[]{0, 2, 10});
        h.setStorageSpace(20 * gib, 0);
        JobInfo job = new JobInfo.Builder(id, serviceComponent())
                .setRequiredNetworkType(JobInfo.NETWORK_TYPE_ANY)
                .setRequiresCharging(true).setRequiresBatteryNotLow(true)
                .setRequiresStorageNotLow(true).setMinimumLatency(60_000)
                .setOverrideDeadline(120_000)
                .build();
        h.schedule(job);
        h.setStorageSpace(20 * gib, 0); // Record the production scheduler observation.
        int[] blocked = {
                JobScheduler.PENDING_JOB_REASON_CONSTRAINT_BATTERY_NOT_LOW,
                JobScheduler.PENDING_JOB_REASON_CONSTRAINT_CHARGING,
                JobScheduler.PENDING_JOB_REASON_CONSTRAINT_CONNECTIVITY,
                JobScheduler.PENDING_JOB_REASON_CONSTRAINT_STORAGE_NOT_LOW,
                JobScheduler.PENDING_JOB_REASON_CONSTRAINT_MINIMUM_LATENCY,
                JobScheduler.PENDING_JOB_REASON_CONSTRAINT_DEADLINE,
        };
        check(Arrays.equals(h.reasons(id), blocked), "Pending API did not report all active constraints");
        check(h.reason(id) == blocked[0], "Legacy pending API diverged from the all-reasons API");
        List<PendingJobReasonsInfo> history = h.history(id);
        check(history.size() == 1 && Arrays.equals(history.get(0).getPendingJobReasons(), blocked),
                "Missing initial pending constraint history");
        check(Math.abs(history.get(0).getTimestampMillis() - System.currentTimeMillis()) < 10_000,
                "History timestamp is not wall-clock time");
        history.get(0).getPendingJobReasons()[0] = -999;
        check(Arrays.equals(h.history(id).get(0).getPendingJobReasons(), blocked),
                "Local client could mutate server history");
        h.setStorageSpace(20 * gib, 0);
        check(h.history(id).size() == 1, "Identical host observation duplicated history");
        for (int i = 0; i < 14; i++) h.setStorageSpace(20 * gib, i % 2 == 0 ? gib : 0);
        history = h.history(id);
        check(history.size() == 10, "History did not enforce Android's ten-entry bound");
        check(Arrays.equals(history.get(9).getPendingJobReasons(), blocked),
                "History omitted most recent constraint state");
        long previous = 0;
        for (PendingJobReasonsInfo entry : history) {
            check(entry.getTimestampMillis() >= previous, "History is not returned in observation order");
            previous = entry.getTimestampMillis();
        }
        h.cancel(id);
        check(Arrays.equals(h.reasons(id), new int[]{JobScheduler.PENDING_JOB_REASON_INVALID_JOB_ID}),
                "Canceled ID was reported as merely unknown");
        try {
            h.history(id);
            throw new AssertionError("Canceled ID exposed a history");
        } catch (IllegalArgumentException expected) { checks++; }
        h.schedule(job);
        h.setStorageSpace(20 * gib, gib);
        check(h.history(id).size() == 1, "Replacement inherited history for its reused ID");
        h.cancel(id);
    }

    private static void testAdmissionOrderAndCapacity(Harness h) throws Exception {
        List<Object> eligible = new ArrayList<>();
        h.schedule(new JobInfo.Builder(173, serviceComponent()).setPriority(JobInfo.PRIORITY_LOW).build());
        h.schedule(new JobInfo.Builder(174, serviceComponent()).setPriority(JobInfo.PRIORITY_HIGH).build());
        h.schedule(new JobInfo.Builder(175, serviceComponent()).setExpedited(true).build());
        for (int id = 173; id <= 175; id++) eligible.add(h.findJob(id));
        List<Object> ready = new ArrayList<>();
        synchronized (h.scheduler) {
            set(h.scheduler, "mRunning", 2); // Two occupied slots from other jobs.
            invoke(h.scheduler, "admitReady", new Class<?>[]{List.class, List.class, String.class},
                    eligible, ready, null);
        }
        check(ready.size() == 2, "Admission did not respect its available slots");
        JobInfo first = (JobInfo) get(get(ready.get(0), "job"), "info");
        JobInfo second = (JobInfo) get(get(ready.get(1), "job"), "info");
        check(first.getId() == 175 && second.getId() == 174,
                "Expedited and priority jobs were not selected ahead of regular jobs");
        check(h.reason(173) == JobScheduler.PENDING_JOB_REASON_DEVICE_STATE
                        && h.reason(174) == JobScheduler.PENDING_JOB_REASON_EXECUTING,
                "Admission capacity and executing reasons were not observable");
        for (int id = 173; id <= 175; id++) h.cancel(id);
        set(h.scheduler, "mRunning", 0);
    }

    private static void testSupersededConstraintObservation(Harness h) throws Exception {
        int id = 176;
        h.setNetworkState(2);
        h.schedule(new JobInfo.Builder(id, serviceComponent())
                .setRequiredNetworkType(JobInfo.NETWORK_TYPE_ANY).build());
        TestService service = new TestService();
        TestRunHandle handle = h.start(id, service);
        Class<?> updateType = type("JobSchedulerService$RunConstraintUpdate");
        Class<?>[] signature = {h.runType, long.class, long.class, int.class, Network.class};
        long generation = (Long) get(h.scheduler, "mHostGeneration");
        long observation = (Long) get(h.scheduler, "mConstraintObservation");
        Object obsoleteStop = make(updateType, signature, handle.run, generation, observation,
                JobParameters.STOP_REASON_CONSTRAINT_CONNECTIVITY, null);
        Object obsoleteNetwork = make(updateType, signature, handle.run, generation, observation,
                JobParameters.STOP_REASON_UNDEFINED, new Network(999));
        h.setNetworkSnapshot(snapshot(59, "wifi", false, false));
        check(handle.params.getNetwork() != null && handle.params.getNetwork().getNetId() == 159,
                "New network assignment did not reach the job");
        invoke(h.scheduler, "applyConstraintUpdate", new Class<?>[]{updateType}, obsoleteStop);
        invoke(h.scheduler, "applyConstraintUpdate", new Class<?>[]{updateType}, obsoleteNetwork);
        long currentGeneration = (Long) get(h.scheduler, "mHostGeneration");
        long currentObservation = (Long) get(h.scheduler, "mConstraintObservation");
        Object stalePoll = make(updateType, signature, handle.run, currentGeneration,
                currentObservation - 1, JobParameters.STOP_REASON_CONSTRAINT_CONNECTIVITY, null);
        invoke(h.scheduler, "applyConstraintUpdate", new Class<?>[]{updateType}, stalePoll);
        check(service.stops == 0 && service.networkChanges == 1
                        && handle.params.getNetwork().getNetId() == 159,
                "A superseded host or same-revision polling snapshot stopped or rebound an active job");
        handle.params.getCallback().jobFinished(id, false);
    }

    private static void awaitBlocked(Thread thread, String message) throws InterruptedException {
        long until = System.nanoTime() + TimeUnit.SECONDS.toNanos(5);
        while (thread.isAlive() && thread.getState() != Thread.State.BLOCKED && System.nanoTime() < until) {
            Thread.sleep(1);
        }
        check(thread.getState() == Thread.State.BLOCKED, message);
    }

    private static void testCancelDuringStartObservation(Harness h) throws Exception {
        int id = 178;
        h.setPowerStatus(new int[]{1, 8, 70});
        h.schedule(new JobInfo.Builder(id, serviceComponent()).setRequiresCharging(true).build());
        Object run = h.begin(id);
        TestService service = new TestService();
        AtomicReference<Throwable> failure = new AtomicReference<>();
        Thread starter = new Thread(() -> {
            try {
                invoke(run, "started", new Class<?>[]{IBinder.class}, service.asBinder());
            } catch (Throwable unexpected) {
                failure.compareAndSet(null, unexpected);
            }
        }, "job-start-observation-test");
        Thread canceller = new Thread(() -> {
            try {
                h.cancel(id);
            } catch (Throwable unexpected) {
                failure.compareAndSet(null, unexpected);
            }
        }, "job-cancel-observation-test");
        starter.setDaemon(true);
        canceller.setDaemon(true);
        synchronized (h.atm) {
            starter.start();
            awaitBlocked(starter, "Job did not wait for the top activity observation");
            canceller.start();
            awaitBlocked(canceller, "Cancellation did not wait for the run dispatch gate");
            check(h.getPendingJob(id) == null, "Cancellation did not retire the schedule");
        }
        starter.join(5_000);
        canceller.join(5_000);
        check(!starter.isAlive() && !canceller.isAlive() && failure.get() == null,
                "Start or cancellation did not finish: " + failure.get());
        check(service.starts == 0 && service.stops == 0 && h.runningCount() == 0,
                "Canceled binding still called the job service");
    }

    private static void testPendingReasonRefreshesObservation(Harness h) throws Exception {
        int id = 179;
        h.setNetworkState(0);
        h.setPowerStatus(new int[]{1, 8, 70});
        h.schedule(new JobInfo.Builder(id, serviceComponent()).setRequiresCharging(true)
                .setRequiredNetworkType(JobInfo.NETWORK_TYPE_ANY).build());
        AtomicReference<int[]> reasons = new AtomicReference<>();
        AtomicReference<Throwable> failure = new AtomicReference<>();
        Thread reader = new Thread(() -> {
            try {
                reasons.set(h.reasons(id));
            } catch (Throwable unexpected) {
                failure.set(unexpected);
            }
        }, "job-pending-observation-test");
        reader.setDaemon(true);
        synchronized (h.atm) {
            reader.start();
            awaitBlocked(reader, "Pending query did not wait for the top activity observation");
            // Publish the native network event without performing a synchronous test pass.
            set(h.scheduler, "mNetworkSnapshotOverride", snapshot(60, "wifi", false, false));
            int network = field(h.schedulerType, "OBSERVE_NETWORK").getInt(null);
            invoke(h.scheduler, "constraintsChanged", new Class<?>[]{int.class}, network);
        }
        reader.join(5_000);
        check(!reader.isAlive() && failure.get() == null, "Pending query failed: " + failure.get());
        check(Arrays.equals(reasons.get(), new int[]{JobScheduler.PENDING_JOB_REASON_UNDEFINED}),
                "Pending query returned a superseded network observation");
        h.cancel(id);
    }

    private static void testWindowsPowerEventWakesWorker(Harness h) throws Exception {
        int id = 177;
        h.setPowerStatus(new int[]{1, 8, 70});
        h.schedule(new JobInfo.Builder(id, serviceComponent()).setRequiresCharging(true).build());
        CountDownLatch stopped = new CountDownLatch(1);
        AtomicReference<Thread> stopThread = new AtomicReference<>();
        TestService service = new TestService() {
            @Override public void stopJob(JobParameters params) {
                super.stopJob(params);
                stopThread.set(Thread.currentThread());
                stopped.countDown();
            }
        };
        h.start(id, service);
        AtomicReference<Throwable> failure = new AtomicReference<>();
        Thread worker = new Thread(() -> {
            try {
                invoke(h.scheduler, "run", new Class<?>[0]);
            } catch (Throwable unexpected) {
                failure.set(unexpected);
                stopped.countDown();
            }
        }, "job-event-test");
        worker.setDaemon(true);
        worker.start();
        try {
            long waitUntil = System.nanoTime() + TimeUnit.SECONDS.toNanos(2);
            while (worker.getState() != Thread.State.TIMED_WAITING && System.nanoTime() < waitUntil) {
                Thread.sleep(1);
            }
            check(worker.getState() == Thread.State.TIMED_WAITING,
                    "Scheduler did not enter its constraint wait");
            // Isolate the native power source but deliver the real Windows event entry point.
            set(h.scheduler, "mPowerStatusOverride", new int[]{0, 0, 70});
            int event = field(type("WindowsHost"), "EVENT_POWER_STATUS").getInt(null);
            invoke(h.scheduler, "onWindowsEvents", new Class<?>[]{int.class}, event);
            check(stopped.await(5, TimeUnit.SECONDS), "Windows event did not wake the scheduler worker");
            check(failure.get() == null, "Scheduler worker failed: " + failure.get());
            check(stopThread.get() == worker, "Windows event thread delivered an app callback");
            check(service.lastStopReason == JobParameters.STOP_REASON_CONSTRAINT_CHARGING,
                    "Windows event omitted the charging stop reason");
        } finally {
            worker.interrupt();
            worker.join(5_000);
        }
        check(!worker.isAlive(), "Scheduler did not leave its wait when interrupted");
    }

    private static void testNetworkPermission(Harness ignored) throws Exception {
        try (Harness h = new Harness(false)) {
            h.setNetworkState(2);
            JobInfo network = new JobInfo.Builder(139, serviceComponent()).setRequiredNetworkType(JobInfo.NETWORK_TYPE_ANY).build();
            try { h.schedule(network); throw new AssertionError("Target 34+ network job accepted without ACCESS_NETWORK_STATE"); }
            catch (SecurityException expected) { checks++; }
            check(h.getPendingJob(139) == null, "Denied network job was published");
            h.parsed.setTargetSdkVersion(33);
            check(h.schedule(network) == JobScheduler.RESULT_SUCCESS, "Legacy app received target 34 permission requirement");
            h.cancel(139);
            try {
                h.schedule(new JobInfo.Builder(140, serviceComponent()).setPersisted(true).build());
                throw new AssertionError("Persisted job accepted without RECEIVE_BOOT_COMPLETED");
            } catch (IllegalArgumentException expected) { checks++; }
        }
    }

    public static void contracts() throws Exception {
        try (Harness h = new Harness()) {
            testUnmeteredToMetered(h);
            testUnmeteredToDisconnected(h);
            testRequireCharging(h);
            testRescheduleExponentialBackoff(h);
            testSystemStopAndMixedBackoff(h);
            testBackoffCapsAndPeriodicReset(h);
            testEffectiveChargingPriority(h);
            testAbandonedTimeout(h);
            testExecutionTimeout(h);
            testCancelledByApp(h);
            testDeviceIdleConstraint(h);
            testBatteryNotLowConstraint(h);
            testNetworkChangeStillSatisfied(h);
            testOverrideDeadlineNotCancelled(h);
            testUnknownConstraints(h);
            testRequestMatchingAndHandover(h);
            testDeadlineBoundaries(h);
            testStartStopOrdering(h);
            testBindingAndCallbackTimeouts(h);
            testWorkCompletionOutcomes(h);
            testStorageAndStablePower(h);
            testTopPackageChargingExemption(h);
            testStoragePressureWithoutNotification(h);
            testPendingReasonsAndHistory(h);
            testAdmissionOrderAndCapacity(h);
            testSupersededConstraintObservation(h);
            testCancelDuringStartObservation(h);
            testPendingReasonRefreshesObservation(h);
            testNetworkPermission(h);
        }
        try (Harness h = new Harness()) {
            testWindowsPowerEventWakesWorker(h);
        }
    }

    public static void main(String[] args) throws Exception {
        System.loadLibrary("android_runtime");
        android.system.Os.unsetenv("GESHER_BINDER_UID");
        android.system.Os.setenv("GESHER_MICROG_SPOOF", "1", true);
        contracts();
        System.out.println("JobSchedulerConstraintContractTest all passed: " + checks + " checks");
    }
}
