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
import java.util.Collection;
import java.util.List;
import java.util.Map;
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
        boolean acknowledgeImmediately = true;
        boolean failNetworkCallback;
        boolean rescheduleOnStop = false;

        @Override
        public void startJob(JobParameters params) {
            starts++;
            lastStartParams = params;
            if (!acknowledgeStartImmediately) return;
            try {
                params.getCallback().acknowledgeStartMessage(params.getJobId(), true);
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

        // rescheduleOnStop was true: job must remain pending with rescheduled delay.
        check(h.runningCount() == 0, "Job 101 still counted as running");
        check(h.getPendingJob(101) != null, "Job 101 was dropped despite reschedule=true");
        Object jobObj = h.findJob(101);
        int failures = (Integer) get(jobObj, "failures");
        check(failures == 1, "Failure count not incremented: " + failures);
        long earliest = (Long) get(jobObj, "earliest");
        check(earliest > SystemClock.elapsedRealtime(), "Rescheduled job earliest delay not set");
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
        h.setPowerStatus(new int[]{1, 0, 100});
        JobInfo info = new JobInfo.Builder(104, serviceComponent())
                .setRequiresCharging(true)
                .setBackoffCriteria(10_000, JobInfo.BACKOFF_POLICY_EXPONENTIAL)
                .build();
        check(h.schedule(info) == JobScheduler.RESULT_SUCCESS, "Job 104 scheduling failed");

        // First run & stop with reschedule=true
        TestService service1 = new TestService();
        service1.rescheduleOnStop = true;
        h.start(104, service1);
        h.setPowerStatus(new int[]{0, 0, 80}); // triggers stop
        check(service1.stops == 1, "Stop 1 not received");

        Object jobObj = h.findJob(104);
        check((Integer) get(jobObj, "failures") == 1, "Failures should be 1");
        long delay1 = (Long) get(jobObj, "earliest") - SystemClock.elapsedRealtime();
        check(delay1 >= 9_000 && delay1 <= 11_000, "First backoff delay should be ~10s: " + delay1);

        // Plug back in
        h.setPowerStatus(new int[]{1, 0, 100});
        set(jobObj, "earliest", 0L); // allow immediate run for test

        // Second run & stop with reschedule=true
        TestService service2 = new TestService();
        service2.rescheduleOnStop = true;
        h.start(104, service2);
        h.setPowerStatus(new int[]{0, 0, 80}); // triggers stop
        check(service2.stops == 1, "Stop 2 not received");

        check((Integer) get(jobObj, "failures") == 2, "Failures should be 2");
        long delay2 = (Long) get(jobObj, "earliest") - SystemClock.elapsedRealtime();
        check(delay2 >= 19_000 && delay2 <= 21_000, "Second exponential backoff should be ~20s: " + delay2);

        // Third run & stop with reschedule=false (dropped)
        h.setPowerStatus(new int[]{1, 0, 100});
        set(jobObj, "earliest", 0L);
        TestService service3 = new TestService();
        service3.rescheduleOnStop = false;
        h.start(104, service3);
        h.setPowerStatus(new int[]{0, 0, 80});
        check(service3.stops == 1, "Stop 3 not received");
        check(h.getPendingJob(104) == null, "Job 104 should be dropped when reschedule=false");
    }

    /**
     * Test execution timeout triggers STOP_REASON_TIMEOUT.
     */
    private static void testExecutionTimeout(Harness h) throws Exception {
        JobInfo info = new JobInfo.Builder(105, serviceComponent()).build();
        check(h.schedule(info) == JobScheduler.RESULT_SUCCESS, "Job 105 scheduling failed");

        TestService service = new TestService();
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
        invoke(h.scheduler, "updateNetwork", new Class<?>[]{h.runType, Network.class}, run.run, new Network(145));
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
        TestService late = new TestService();
        invoke(binding, "started", new Class<?>[]{IBinder.class}, late.asBinder());
        check(late.starts == 0, "Retired binding delivered late onStartJob");
        h.cancel(132);

        h.schedule(new JobInfo.Builder(133, serviceComponent()).build());
        Object unbound = h.begin(133);
        h.timeout(unbound, 0); // Production binding timeout boundary, without sleeping.
        check(h.runningCount() == 0 && h.getPendingJob(133) != null, "Binding timeout did not preserve job for retry");
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
        check(get(stop.run, "timeout") == null, "Completed run retained its timer");
        check(((java.util.concurrent.ScheduledThreadPoolExecutor) get(h.scheduler, "mTimeouts")).getQueue().isEmpty(),
                "Completed attempts retained canceled timers");
        h.cancel(135);
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
            testStorageAndStablePower(h);
            testNetworkPermission(h);
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
