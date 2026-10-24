/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.server;

import android.app.IServiceConnection;
import android.app.job.IJobCallback;
import android.app.job.IJobScheduler;
import android.app.job.IJobService;
import android.app.job.JobInfo;
import android.app.job.JobParameters;
import android.app.job.JobScheduler;
import android.app.job.JobWorkItem;
import android.content.ComponentName;
import android.content.Context;
import android.content.Intent;
import android.content.pm.ParceledListSlice;
import android.content.pm.ServiceInfo;
import android.os.Binder;
import android.os.IBinder;
import android.os.Parcel;
import android.os.RemoteException;
import android.os.SystemClock;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.OutputStream;
import java.util.ArrayDeque;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.util.Objects;

/**
 * {@code jobscheduler}: JobSchedulerService on Windows (devsvc track, docs/services.md
 * "Device services", ADR 0038), what WorkManager schedules its work with. Jobs (by
 * package, namespace and id) wait for their minimum latency, their periodic window
 * (interval and flex, as JobStatus computes them) and their constraints: network (Windows'
 * network list manager: connected to the internet, unmetered when asked), charging (AC
 * power, or a PC without a battery), battery not low, device idle (no user input for
 * 30 minutes) and storage not low (always); an override deadline runs them regardless.
 * Content URI triggers are not watched. A ready job runs in its package's process
 * (started if none runs): the system server binds the JobService (BIND_JOB_SERVICE),
 * {@code startJob}, and the app answers through IJobCallback (acknowledgeStartMessage,
 * jobFinished, dequeueWork and completeWork for enqueued work); a job is stopped
 * ({@code stopJob}) after 10 minutes, when cancelled or rescheduled. A finished periodic
 * job waits for its next window, a job finished with reschedule (or whose process died)
 * backs off (linear or exponential from its initial backoff). Persisted jobs are kept in
 * {@code $ANDROID_DATA/system/jobs} and scheduled again when the device starts.
 *
 * <p>{@code GESHER_JOBS_TIME_SCALE} (a number, tests only) divides periods, flex and
 * backoff (the client already raised periods to JobInfo's 15-minute minimum).
 */
final class JobSchedulerService extends IJobScheduler.Default
        implements DeviceWork.Source, WindowsHost.Listener {
    private static final long EXECUTION_LIMIT_MS = 10 * 60_000;
    private static final long IDLE_AFTER_MS = 30 * 60_000;
    /** How often waiting constraints (network, charging, idle) are looked at again. */
    private static final long CONSTRAINT_POLL_MS = 30_000;
    private static final int MAX_RUNNING = 4;

    private final ActivityManagerService mAm;
    private final ActivityTaskManagerService mAtm;
    private final PackageManagerService mPm;
    private final double mTimeScale;

    /** A scheduled job: JobStatus. */
    private final class Job {
        final String packageName;
        final String namespace;
        final JobInfo info;
        long earliest;          // elapsedRealtime; 0: none
        long latest;            // elapsedRealtime; Long.MAX_VALUE: none
        int failures;
        final ArrayDeque<JobWorkItem> work = new ArrayDeque<>();
        final Map<Integer, JobWorkItem> delivered = new HashMap<>();
        int nextWorkId = 1;
        Run run;                // while it runs

        Job(String packageName, String namespace, JobInfo info) {
            this.packageName = packageName;
            this.namespace = namespace;
            this.info = info;
        }

        boolean same(String pkg, String ns, int id) {
            return (pkg == null || packageName.equals(pkg)) && Objects.equals(namespace, ns)
                    && info.getId() == id;
        }

        @Override
        public String toString() {
            return "job " + info.getId() + (namespace != null ? " (" + namespace + ")" : "")
                    + " of " + packageName + " (" + info.getService().getShortClassName()
                    + (info.isPeriodic() ? ", periodic" : "") + ")";
        }
    }

    /** A job running: its connection to the JobService and its callback. */
    private final class Run extends IJobCallback.Stub implements IBinder.DeathRecipient {
        final Job job;
        final long started = SystemClock.elapsedRealtime();
        IJobService service;
        JobParameters params;
        boolean stopping;
        boolean done;
        final IServiceConnection connection = new IServiceConnection.Stub() {
            @Override
            public void connected(ComponentName name, IBinder binder,
                    android.app.IBinderSession session, boolean dead) {
                if (binder != null) started(binder);
            }
        };

        Run(Job job) {
            this.job = job;
        }

        void started(IBinder binder) {
            synchronized (JobSchedulerService.this) {
                if (done || service != null) return;
                service = IJobService.Stub.asInterface(binder);
                params = new JobParameters(this, job.namespace, job.info.getId(),
                        job.info.getExtras(), job.info.getTransientExtras(),
                        job.info.getClipData(), job.info.getClipGrantFlags(),
                        SystemClock.elapsedRealtime() >= job.latest /* deadline expired */,
                        job.info.isExpedited(), job.info.isUserInitiated(),
                        null /* triggered content uris */, null /* authorities */,
                        null /* network: the default one (ConnectivityManager's) */);
            }
            try {
                binder.linkToDeath(this, 0);
            } catch (RemoteException e) {
                finish(this, true, "its process died");
                return;
            }
            SystemServer.log("jobscheduler: startJob " + job);
            try {
                service.startJob(params);
            } catch (RemoteException e) {
                finish(this, true, "startJob failed: " + e);
            }
        }

        @Override
        public void binderDied() {
            finish(this, true, "its process died");
        }

        @Override
        public void acknowledgeStartMessage(int jobId, boolean ongoing) {
            if (!ongoing) finish(this, false, "onStartJob returned false");
        }

        @Override
        public void acknowledgeStopMessage(int jobId, boolean reschedule) {
            finish(this, reschedule, "stopped (onStopJob: reschedule " + reschedule + ")");
        }

        @Override
        public void jobFinished(int jobId, boolean reschedule) {
            finish(this, reschedule, "jobFinished (reschedule " + reschedule + ")");
        }

        @Override
        public JobWorkItem dequeueWork(int jobId) {
            synchronized (JobSchedulerService.this) {
                JobWorkItem w = job.work.poll();
                if (w == null) return null;
                w.bumpDeliveryCount();
                job.delivered.put(w.getWorkId(), w);
                return w;
            }
        }

        @Override
        public boolean completeWork(int jobId, int workId) {
            synchronized (JobSchedulerService.this) {
                return job.delivered.remove(workId) != null;
            }
        }

        @Override
        public void handleAbandonedJob(int jobId) {
            finish(this, false, "abandoned");
        }

        @Override
        public void acknowledgeGetTransferredDownloadBytesMessage(int jobId, int workId,
                long transferredBytes) {
        }

        @Override
        public void acknowledgeGetTransferredUploadBytesMessage(int jobId, int workId,
                long transferredBytes) {
        }

        @Override
        public void updateEstimatedNetworkBytes(int jobId, JobWorkItem item,
                long downloadBytes, long uploadBytes) {
        }

        @Override
        public void updateTransferredNetworkBytes(int jobId, JobWorkItem item,
                long transferredDownloadBytes, long transferredUploadBytes) {
        }

        @Override
        public void setNotification(int jobId, int notificationId,
                android.app.Notification notification, int jobEndNotificationPolicy) {
            // User-initiated jobs' notifications: posted as the app's.
            NotificationManagerService n = NotificationManagerService.sLast;
            if (n != null && notification != null) {
                n.post(job.packageName, null, notificationId, notification);
            }
        }
    }

    // Guarded by this.
    private final List<Job> mJobs = new ArrayList<>();
    private int mRunning;
    private Thread mThread;

    JobSchedulerService(ActivityManagerService am, ActivityTaskManagerService atm,
            PackageManagerService pm) {
        mAm = am;
        mAtm = atm;
        mPm = pm;
        double scale = 1;
        String s = System.getenv("GESHER_JOBS_TIME_SCALE");
        if (s != null) {
            try {
                scale = Math.max(1, Double.parseDouble(s));
            } catch (NumberFormatException e) {
                // not a number: no scale
            }
        }
        mTimeScale = scale;
        DeviceWork.add(this);
        // Persisted jobs run again from the system server's start.
        if (WindowsHost.inSystemServer()) start();
    }

    private long scaled(long ms) {
        return (long) (ms / mTimeScale);
    }

    /** Starts the scheduling thread, with the persisted jobs (on first use, or at boot). */
    synchronized void start() {
        if (mThread != null) return;
        for (Job j : loadPersisted()) {
            mJobs.add(j);
            SystemServer.log("jobscheduler: persisted " + j + " scheduled again");
        }
        WindowsHost.addListener(this);
        mThread = new Thread(this::run, "jobs");
        mThread.setDaemon(true);
        mThread.start();
        SystemServer.log("jobscheduler: running" + (mTimeScale != 1
                ? " (times divided by " + mTimeScale + ")" : ""));
    }

    // --- IJobScheduler -----------------------------------------------------------------------

    @Override
    public int schedule(String namespace, JobInfo job) {
        return enqueue(namespace, job, null);
    }

    @Override
    public int scheduleAsPackage(String namespace, JobInfo job, String packageName, int userId,
            String tag) {
        return enqueue(namespace, job, null);
    }

    @Override
    public int enqueue(String namespace, JobInfo info, JobWorkItem work) {
        start();
        ComponentName service = info.getService();
        ServiceInfo si = mPm.getServiceInfo(service, 0, 0);
        if (si == null) {
            throw new IllegalArgumentException("No such service " + service);
        }
        if (!android.Manifest.permission.BIND_JOB_SERVICE.equals(si.permission)) {
            throw new IllegalArgumentException("Scheduled service " + service
                    + " does not require android.permission.BIND_JOB_SERVICE permission");
        }
        String pkg = service.getPackageName();
        Run stop = null;
        Job job;
        synchronized (this) {
            Job old = find(pkg, namespace, info.getId());
            if (work != null && old != null && old.info.equals(info)) {
                // enqueue onto the same job: its work queue grows.
                job = old;
            } else {
                job = new Job(pkg, namespace, info);
                timesForNew(job);
                if (old != null) {
                    mJobs.remove(old);
                    job.work.addAll(old.work);
                    job.nextWorkId = old.nextWorkId;
                    if (old.run != null && !old.run.stopping) stop = old.run;
                }
                mJobs.add(job);
            }
            if (work != null) {
                work.setWorkId(job.nextWorkId++);
                job.work.add(work);
            }
        }
        if (stop != null) stopRun(stop, "rescheduled");
        persist(job);
        SystemServer.log("jobscheduler: scheduled " + job + describeTimes(job)
                + constraintsText(info));
        kick();
        return JobScheduler.RESULT_SUCCESS;
    }

    @Override
    public void cancel(String namespace, int jobId) {
        String pkg = callerPackage();
        List<Job> cancelled = new ArrayList<>();
        synchronized (this) {
            for (Job j : mJobs) {
                if (j.same(pkg, namespace, jobId)) cancelled.add(j);
            }
            mJobs.removeAll(cancelled);
        }
        cancelled(cancelled, "cancelled");
    }

    @Override
    public void cancelAll() {
        cancelWhere(callerPackage(), null, true);
    }

    @Override
    public void cancelAllInNamespace(String namespace) {
        cancelWhere(callerPackage(), namespace, false);
    }

    private void cancelWhere(String pkg, String namespace, boolean allNamespaces) {
        List<Job> cancelled = new ArrayList<>();
        synchronized (this) {
            for (Job j : mJobs) {
                if ((pkg == null || j.packageName.equals(pkg))
                        && (allNamespaces || Objects.equals(j.namespace, namespace))) {
                    cancelled.add(j);
                }
            }
            mJobs.removeAll(cancelled);
        }
        cancelled(cancelled, "cancelled");
    }

    private void cancelled(List<Job> jobs, String why) {
        for (Job j : jobs) {
            SystemServer.log("jobscheduler: " + why + " " + j);
            unpersist(j);
            Run r;
            synchronized (this) {
                r = j.run;
            }
            if (r != null) stopRun(r, why);
        }
        kick();
    }

    @Override
    public Map<String, ParceledListSlice<JobInfo>> getAllPendingJobs() {
        String pkg = callerPackage();
        Map<String, List<JobInfo>> byNamespace = new HashMap<>();
        synchronized (this) {
            for (Job j : mJobs) {
                if (pkg != null && !j.packageName.equals(pkg)) continue;
                byNamespace.computeIfAbsent(j.namespace, k -> new ArrayList<>()).add(j.info);
            }
        }
        Map<String, ParceledListSlice<JobInfo>> result = new HashMap<>();
        for (Map.Entry<String, List<JobInfo>> e : byNamespace.entrySet()) {
            result.put(e.getKey(), new ParceledListSlice<>(e.getValue()));
        }
        return result;
    }

    @Override
    public ParceledListSlice<JobInfo> getAllPendingJobsInNamespace(String namespace) {
        String pkg = callerPackage();
        List<JobInfo> result = new ArrayList<>();
        synchronized (this) {
            for (Job j : mJobs) {
                if ((pkg == null || j.packageName.equals(pkg))
                        && Objects.equals(j.namespace, namespace)) {
                    result.add(j.info);
                }
            }
        }
        return new ParceledListSlice<>(result);
    }

    @Override
    public JobInfo getPendingJob(String namespace, int jobId) {
        synchronized (this) {
            Job j = find(callerPackage(), namespace, jobId);
            return j != null ? j.info : null;
        }
    }

    @Override
    public int getPendingJobReason(String namespace, int jobId) {
        Job j;
        synchronized (this) {
            j = find(callerPackage(), namespace, jobId);
            if (j == null) return JobScheduler.PENDING_JOB_REASON_UNDEFINED;
            if (j.run != null) return JobScheduler.PENDING_JOB_REASON_EXECUTING;
        }
        return waitingReason(j, SystemClock.elapsedRealtime());
    }

    @Override
    public int[] getPendingJobReasons(String namespace, int jobId) {
        return new int[] {getPendingJobReason(namespace, jobId)};
    }

    @Override
    public boolean canRunUserInitiatedJobs(String packageName) {
        return true;
    }

    @Override
    public boolean hasRunUserInitiatedJobsPermission(String packageName, int userId) {
        return true;
    }

    @Override
    public List<JobInfo> getStartedJobs() {
        List<JobInfo> result = new ArrayList<>();
        synchronized (this) {
            for (Job j : mJobs) {
                if (j.run != null) result.add(j.info);
            }
        }
        return result;
    }

    // --- DeviceWork.Source ---------------------------------------------------------------------

    @Override
    public synchronized long nextDueElapsed() {
        long next = Long.MAX_VALUE;
        for (Job j : mJobs) {
            if (j.run != null) return SystemClock.elapsedRealtime();
            next = Math.min(next, Math.max(j.earliest, 0));
        }
        return next;
    }

    @Override
    public void onWindowsEvents(int events) {
        if ((events & (WindowsHost.EVENT_POWER_STATUS | WindowsHost.EVENT_RESUMED)) != 0) kick();
    }

    // --- scheduling ---------------------------------------------------------------------------

    private String callerPackage() {
        ActivityTaskManagerService.ProcessRecord p = mAtm.callerProcess(Binder.getCallingPid());
        return p != null ? p.packageName : null;  // a process that is not an app's: all
    }

    private Job find(String pkg, String namespace, int id) {
        for (Job j : mJobs) {
            if (j.same(pkg, namespace, id)) return j;
        }
        return null;
    }

    /** JobStatus.createFromJobInfo's times: latency and deadline, or the first period. */
    private void timesForNew(Job job) {
        long now = SystemClock.elapsedRealtime();
        JobInfo info = job.info;
        if (info.isPeriodic()) {
            long period = scaled(info.getIntervalMillis());
            long flex = Math.min(period, scaled(info.getFlexMillis()));
            job.latest = now + period;
            job.earliest = job.latest - flex;
        } else {
            job.earliest = info.hasEarlyConstraint() ? now + info.getMinLatencyMillis() : 0;
            job.latest = info.hasLateConstraint() ? now + info.getMaxExecutionDelayMillis()
                    : Long.MAX_VALUE;
        }
    }

    private void kick() {
        synchronized (this) {
            notifyAll();
        }
    }

    private void run() {
        while (true) {
            List<Job> ready = new ArrayList<>();
            long wait;
            synchronized (this) {
                long now = SystemClock.elapsedRealtime();
                long next = now + CONSTRAINT_POLL_MS;
                for (Job j : mJobs) {
                    if (j.run != null) continue;
                    if (j.earliest > now) {
                        next = Math.min(next, j.earliest);
                        continue;
                    }
                    if (mRunning + ready.size() >= MAX_RUNNING) continue;
                    boolean overdue = now >= j.latest;
                    if (overdue || waitingReason(j, now) == JobScheduler.PENDING_JOB_REASON_UNDEFINED) {
                        ready.add(j);
                    }
                }
                for (Job j : ready) {
                    j.run = new Run(j);
                    mRunning++;
                }
                for (Job j : mJobs) {
                    if (j.run == null && j.latest > now) next = Math.min(next, j.latest);
                    if (j.run != null && now - j.run.started >= EXECUTION_LIMIT_MS
                            && !j.run.stopping) {
                        Run r = j.run;
                        new Thread(() -> stopRun(r, "timed out")).start();
                    } else if (j.run != null) {
                        next = Math.min(next, j.run.started + EXECUTION_LIMIT_MS);
                    }
                }
                wait = ready.isEmpty() ? Math.max(1, next - now) : 0;
                if (wait > 0) {
                    try {
                        wait(wait);
                    } catch (InterruptedException e) {
                        return;
                    }
                    continue;
                }
            }
            for (Job j : ready) bind(j.run);
        }
    }

    /** Why a job that may run by time still waits; UNDEFINED when nothing holds it. */
    private int waitingReason(Job j, long now) {
        JobInfo info = j.info;
        if (j.earliest > now) return JobScheduler.PENDING_JOB_REASON_CONSTRAINT_MINIMUM_LATENCY;
        if (now >= j.latest) return JobScheduler.PENDING_JOB_REASON_UNDEFINED;
        if (needsNetwork(info) && !networkOk(info)) {
            return JobScheduler.PENDING_JOB_REASON_CONSTRAINT_CONNECTIVITY;
        }
        int[] power = WindowsHost.powerStatus();
        boolean noBattery = power == null || (power[1] & 128) != 0 || power[1] == 255;
        if (info.isRequireCharging() && power != null && power[0] != 1) {
            return JobScheduler.PENDING_JOB_REASON_CONSTRAINT_CHARGING;
        }
        if (info.isRequireBatteryNotLow() && !noBattery && (power[1] & (2 | 4)) != 0
                && power[0] != 1) {
            return JobScheduler.PENDING_JOB_REASON_CONSTRAINT_BATTERY_NOT_LOW;
        }
        if (info.isRequireDeviceIdle() && WindowsHost.available()
                && WindowsHost.idleMillis() < IDLE_AFTER_MS) {
            return JobScheduler.PENDING_JOB_REASON_CONSTRAINT_DEVICE_IDLE;
        }
        JobInfo.TriggerContentUri[] triggers = info.getTriggerContentUris();
        if (triggers != null && triggers.length > 0) {
            return JobScheduler.PENDING_JOB_REASON_CONSTRAINT_CONTENT_TRIGGER;
        }
        return JobScheduler.PENDING_JOB_REASON_UNDEFINED;
    }

    private static boolean needsNetwork(JobInfo info) {
        try {
            return info.getRequiredNetwork() != null;
        } catch (LinkageError e) {
            return false;
        }
    }

    /** The job's network constraint against Windows' networks (unknown: met). */
    private static boolean networkOk(JobInfo info) {
        int state = WindowsHost.networkState();
        if (state < 0) return true;
        boolean internet = (state & 2) != 0;
        boolean metered = (state & 4) != 0;
        int type;
        try {
            type = info.getNetworkType();
        } catch (LinkageError e) {
            type = JobInfo.NETWORK_TYPE_ANY;  // the connectivity classes are not there
        }
        switch (type) {
            case JobInfo.NETWORK_TYPE_UNMETERED: return internet && !metered;
            case JobInfo.NETWORK_TYPE_CELLULAR: return internet && metered;
            default: return internet;
        }
    }

    private void bind(Run run) {
        Job j = run.job;
        Intent intent = new Intent().setComponent(j.info.getService());
        SystemServer.log("jobscheduler: running " + j);
        int result;
        try {
            result = mAm.bindService(null, null, intent, null, run.connection,
                    Context.BIND_AUTO_CREATE | Context.BIND_NOT_FOREGROUND, "android", 0);
        } catch (Throwable t) {
            SystemServer.log("jobscheduler: binding " + j + " failed: " + t);
            result = 0;
        }
        if (result == 0) finish(run, false, "its service could not be bound");
    }

    private void stopRun(Run run, String why) {
        IJobService service;
        JobParameters params;
        synchronized (this) {
            if (run.done || run.stopping) return;
            run.stopping = true;
            service = run.service;
            params = run.params;
        }
        if (service == null) {
            finish(run, false, why + " before it started");
            return;
        }
        SystemServer.log("jobscheduler: stopJob " + run.job + " (" + why + ")");
        try {
            service.stopJob(params);
        } catch (RemoteException e) {
            finish(run, false, why);
        }
        // An app that never acknowledges the stop is unbound anyway.
        Thread t = new Thread(() -> {
            SystemClock.sleep(10_000);
            finish(run, false, why + " (no acknowledgement)");
        }, "job-stop");
        t.setDaemon(true);
        t.start();
    }

    /** The run is over: unbind, then reschedule (periodic, backoff) or drop the job. */
    private void finish(Run run, boolean reschedule, String why) {
        Job j = run.job;
        boolean keep;
        synchronized (this) {
            if (run.done) return;
            run.done = true;
            if (j.run == run) {
                j.run = null;
                mRunning--;
            }
            // Delivered work not completed goes back to the queue.
            for (JobWorkItem w : j.delivered.values()) j.work.addFirst(w);
            j.delivered.clear();
            long now = SystemClock.elapsedRealtime();
            boolean inList = mJobs.contains(j);
            if (!inList) {
                keep = false;
            } else if (reschedule) {
                j.failures++;
                long backoff = scaled(j.info.getInitialBackoffMillis());
                long delay = j.info.getBackoffPolicy() == JobInfo.BACKOFF_POLICY_LINEAR
                        ? backoff * j.failures
                        : backoff * (1L << Math.min(30, j.failures - 1));
                delay = Math.min(delay, scaled(JobInfo.MAX_BACKOFF_DELAY_MILLIS));
                j.earliest = now + delay;
                if (!j.info.isPeriodic()) j.latest = Long.MAX_VALUE;
                keep = true;
            } else if (j.info.isPeriodic()) {
                // JobSchedulerService.getRescheduleJobForPeriodic: the next window after
                // the one it ran in.
                long period = scaled(j.info.getIntervalMillis());
                long flex = Math.min(period, scaled(j.info.getFlexMillis()));
                long latest = Math.max(j.latest, now) + period;
                if (j.latest < now) latest = now + period;
                j.latest = latest;
                j.earliest = latest - flex;
                j.failures = 0;
                keep = true;
            } else if (!j.work.isEmpty()) {
                keep = true;  // enqueued work left: run again
            } else {
                mJobs.remove(j);
                keep = false;
            }
            notifyAll();
        }
        SystemServer.log("jobscheduler: " + j + " finished: " + why
                + (keep ? "; next" + describeTimes(j) : ""));
        if (run.service != null) {
            try {
                run.service.asBinder().unlinkToDeath(run, 0);
            } catch (Throwable t) {
                // not linked
            }
        }
        try {
            mAm.unbindService(run.connection);
        } catch (Throwable t) {
            // not bound
        }
        if (!keep) unpersist(j);
    }

    private String describeTimes(Job j) {
        long now = SystemClock.elapsedRealtime();
        StringBuilder b = new StringBuilder();
        if (j.earliest > now) b.append(" in ").append(j.earliest - now).append(" ms");
        if (j.latest != Long.MAX_VALUE) b.append(", by ").append(j.latest - now).append(" ms");
        return b.toString();
    }

    private static String constraintsText(JobInfo info) {
        List<String> c = new ArrayList<>();
        if (needsNetwork(info)) c.add("network");
        if (info.isRequireCharging()) c.add("charging");
        if (info.isRequireBatteryNotLow()) c.add("battery not low");
        if (info.isRequireDeviceIdle()) c.add("idle");
        if (info.isRequireStorageNotLow()) c.add("storage not low");
        if (info.isPersisted()) c.add("persisted");
        return c.isEmpty() ? "" : " (" + String.join(", ", c) + ")";
    }

    // --- persisted jobs -----------------------------------------------------------------------

    private static File persistDir() {
        return new File(Device.dataDir(), "system/jobs");
    }

    private static File persistFile(Job j) {
        String name = j.packageName + "-" + (j.namespace != null
                ? Integer.toHexString(j.namespace.hashCode()) : "") + "-" + j.info.getId();
        return new File(persistDir(), name + ".job");
    }

    /** Writes a persisted job (package, namespace and the JobInfo's Parcel). */
    private void persist(Job j) {
        if (!j.info.isPersisted()) return;
        Parcel p = Parcel.obtain();
        try {
            p.writeString(j.packageName);
            p.writeString(j.namespace);
            j.info.writeToParcel(p, 0);
            File f = persistFile(j);
            f.getParentFile().mkdirs();
            try (OutputStream out = new FileOutputStream(f)) {
                out.write(p.marshall());
            }
        } catch (Throwable t) {
            SystemServer.log("jobscheduler: persisting " + j + " failed: " + t);
        } finally {
            p.recycle();
        }
    }

    private void unpersist(Job j) {
        if (j.info.isPersisted()) persistFile(j).delete();
    }

    private List<Job> loadPersisted() {
        List<Job> jobs = new ArrayList<>();
        File[] files = persistDir().listFiles();
        if (files == null) return jobs;
        for (File f : files) {
            if (!f.getName().endsWith(".job")) continue;
            Parcel p = Parcel.obtain();
            try (InputStream in = new FileInputStream(f)) {
                byte[] data = in.readAllBytes();
                p.unmarshall(data, 0, data.length);
                p.setDataPosition(0);
                String pkg = p.readString();
                String namespace = p.readString();
                JobInfo info = JobInfo.CREATOR.createFromParcel(p);
                if (mPm.get(pkg) == null) {
                    f.delete();  // uninstalled
                    continue;
                }
                Job j = new Job(pkg, namespace, info);
                timesForNew(j);
                jobs.add(j);
            } catch (Throwable t) {
                SystemServer.log("jobscheduler: " + f.getName() + " not loaded: " + t);
            } finally {
                p.recycle();
            }
        }
        return jobs;
    }
}
