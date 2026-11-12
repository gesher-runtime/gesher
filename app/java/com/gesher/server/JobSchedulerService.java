/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.server;

import android.app.IServiceConnection;
import android.app.Notification;
import android.app.NotificationChannel;
import android.app.job.IJobCallback;
import android.app.job.IJobScheduler;
import android.app.job.IJobService;
import android.app.job.JobInfo;
import android.app.job.JobParameters;
import android.app.job.JobScheduler;
import android.app.job.JobService;
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
import java.util.HashSet;
import java.util.List;
import java.util.Map;
import java.util.Objects;
import java.util.Set;

/**
 * {@code jobscheduler}: JobSchedulerService on Windows (devsvc track, docs/services.md
 * "Device services", ADR 0038), what WorkManager schedules its work with. Jobs (by
 * package, namespace and id) wait for their minimum latency, their periodic window
 * (interval and flex, as JobStatus computes them) and their constraints: network (Windows'
 * network list manager: connected to the internet, unmetered when asked), charging (AC
 * power, or a PC without a battery), battery not low, device idle (no user input for
 * 30 minutes) and storage not low (always); an override deadline runs them regardless.
 * Content URI triggers ({@code addTriggerContentUri}) watch {@link ContentService}'s changes
 * (a provider's {@code notifyChange}): such a job runs once its update delay passed without
 * another change, or its maximum delay after the first, with the changed URIs and
 * authorities in its JobParameters (hwsvc2, ADR 0051). A ready job runs in its package's process
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
    private static final int PERSIST_VERSION = 0x474a4232; // GJB2: code-bound records.

    private final ActivityManagerService mAm;
    private final ActivityTaskManagerService mAtm;
    private final PackageManagerService mPm;
    private final CallerIdentity mCallers;
    private final NotificationManagerService mNotifications;
    private final double mTimeScale;

    /** A scheduled job: JobStatus. */
    private final class Job {
        final String packageName;
        final long removalEpoch;
        final String namespace;
        final JobInfo info;
        final int ownerUid;
        final String ownerIdentity;
        long earliest;          // elapsedRealtime; 0: none
        long latest;            // elapsedRealtime; Long.MAX_VALUE: none
        int failures;
        final ArrayDeque<JobWorkItem> work = new ArrayDeque<>();
        final Map<Integer, JobWorkItem> delivered = new HashMap<>();
        int nextWorkId = 1;
        Run run;                // while it runs
        // Content URI triggers: the changes seen since it last ran (elapsedRealtime).
        final java.util.LinkedHashSet<android.net.Uri> changedUris = new java.util.LinkedHashSet<>();
        final java.util.LinkedHashSet<String> changedAuthorities = new java.util.LinkedHashSet<>();
        long firstChange;
        long lastChange;

        Job(String packageName, String namespace, JobInfo info) {
            this.packageName = packageName;
            removalEpoch = mRemovalEpochs.getOrDefault(packageName, 0L);
            this.namespace = namespace;
            this.info = info;
            ApkPackage owner = mPm.get(packageName);
            ownerUid = owner == null ? -1 : owner.uid();
            ownerIdentity = NotificationManagerService.codeIdentity(owner);
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
        int notificationId;
        String notificationChannelId;
        int notificationEndPolicy;
        final IServiceConnection connection = new IServiceConnection.Stub() {
            @Override
            public void connected(ComponentName name, IBinder binder,
                    android.app.IBinderSession session, boolean dead) {
                if (binder != null) started(binder);
            }
        };

        // The content changes that made it run (JobParameters' triggered URIs and
        // authorities): taken from the job when it starts (with the scheduler's lock).
        final android.net.Uri[] triggeredUris;
        final String[] triggeredAuthorities;
        // The network that met its network constraint (JobParameters.getNetwork), or null.
        final android.net.Network network;

        Run(Job job) {
            this.job = job;
            android.net.Network n = null;
            if (needsNetwork(job.info)) {
                try {
                    n = satisfyingNetwork(job.info);
                } catch (LinkageError | RuntimeException e) {
                    // no connectivity classes
                }
            }
            network = n;
            if (job.changedAuthorities.isEmpty()) {
                triggeredUris = null;
                triggeredAuthorities = null;
            } else {
                // JobStatus.MAX_URIS_TO_REPORT: more than that and only the authorities.
                triggeredUris = job.changedUris.size() <= 50
                        ? job.changedUris.toArray(new android.net.Uri[0]) : null;
                triggeredAuthorities = job.changedAuthorities.toArray(new String[0]);
                job.changedUris.clear();
                job.changedAuthorities.clear();
                job.firstChange = 0;
                job.lastChange = 0;
            }
        }

        void started(IBinder binder) {
            // A binding may finish after an APK replacement. Do not hand the old
            // JobParameters/extras/work to the newly installed service code.
            if (!current(job)) { finish(this, false, "its owner changed while binding"); return; }
            synchronized (JobSchedulerService.this) {
                if (done || service != null) return;
                service = IJobService.Stub.asInterface(binder);
                params = new JobParameters(this, job.namespace, job.info.getId(),
                        job.info.getExtras(), job.info.getTransientExtras(),
                        job.info.getClipData(), job.info.getClipGrantFlags(),
                        SystemClock.elapsedRealtime() >= job.latest /* deadline expired */,
                        job.info.isExpedited(), job.info.isUserInitiated(),
                        triggeredUris, triggeredAuthorities, network);
            }
            try {
                binder.linkToDeath(this, 0);
            } catch (RemoteException e) {
                finish(this, true, "its process died");
                return;
            }
            SystemServer.log("jobscheduler: startJob " + job);
            try {
                if (!current(job)) { finish(this, false, "its owner changed before dispatch"); return; }
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
            callback(jobId);
            if (!ongoing) finish(this, false, "onStartJob returned false");
        }

        @Override
        public void acknowledgeStopMessage(int jobId, boolean reschedule) {
            callback(jobId);
            finish(this, reschedule, "stopped (onStopJob: reschedule " + reschedule + ")");
        }

        @Override
        public void jobFinished(int jobId, boolean reschedule) {
            callback(jobId);
            finish(this, reschedule, "jobFinished (reschedule " + reschedule + ")");
        }

        @Override
        public JobWorkItem dequeueWork(int jobId) {
            callback(jobId);
            JobWorkItem[] result = {null};
            int[] transition = {-1};
            Runnable dequeue = () -> {
                synchronized (JobSchedulerService.this) {
                    if (done || stopping) return;
                    JobWorkItem work = job.work.poll();
                    if (work != null) {
                        work.bumpDeliveryCount();
                        job.delivered.put(work.getWorkId(), work);
                        result[0] = work;
                    } else if (job.delivered.isEmpty()) {
                        // Enqueue uses this same monitor. Retire an exhausted run
                        // before another enqueue can attach to its old callback.
                        transition[0] = finishState(this, false);
                    }
                }
                if (transition[0] >= 0) releaseNotification(this);
            };
            try {
                // Match finish/channel deletion's lifecycle -> scheduler lock order.
                if (mNotifications == null) dequeue.run();
                else mNotifications.withChannelLifecycle(dequeue);
            } finally {
                if (transition[0] >= 0) {
                    finishBinding(this, transition[0] != 0, "last work dequeued");
                }
            }
            return result[0];
        }

        @Override
        public boolean completeWork(int jobId, int workId) {
            callback(jobId);
            synchronized (JobSchedulerService.this) {
                if (done) return false;
                return job.delivered.remove(workId) != null;
            }
        }

        @Override
        public void handleAbandonedJob(int jobId) {
            callback(jobId);
            finish(this, false, "abandoned");
        }

        @Override
        public void acknowledgeGetTransferredDownloadBytesMessage(int jobId, int workId,
                long transferredBytes) {
            callback(jobId);
        }

        @Override
        public void acknowledgeGetTransferredUploadBytesMessage(int jobId, int workId,
                long transferredBytes) {
            callback(jobId);
        }

        @Override
        public void updateEstimatedNetworkBytes(int jobId, JobWorkItem item,
                long downloadBytes, long uploadBytes) {
            callback(jobId);
        }

        @Override
        public void updateTransferredNetworkBytes(int jobId, JobWorkItem item,
                long transferredDownloadBytes, long transferredUploadBytes) {
            callback(jobId);
        }

        @Override
        public void setNotification(int jobId, int notificationId,
                Notification notification, int jobEndNotificationPolicy) {
            ApkPackage captured = mPm.get(job.packageName);
            callback(jobId);
            synchronized (JobSchedulerService.this) { if (done) return; }
            Objects.requireNonNull(notification, "notification");
            Notification posted = notification.clone();
            if (posted.getSmallIcon() == null) throw new IllegalArgumentException("small icon required");
            if (jobEndNotificationPolicy != JobService.JOB_END_NOTIFICATION_POLICY_DETACH
                    && jobEndNotificationPolicy != JobService.JOB_END_NOTIFICATION_POLICY_REMOVE) {
                throw new IllegalArgumentException("invalid job end notification policy");
            }
            if (mNotifications == null) throw new IllegalStateException("Notification service unavailable");
            mNotifications.withChannelLifecycle(() -> {
                synchronized (JobSchedulerService.this) {
                    if (done || !current(job) || mPm.get(job.packageName) != captured) return;
                }
                NotificationChannel channel = mNotifications.getNotificationChannel(
                        job.packageName, 0, job.packageName, posted.getChannelId());
                if (channel == null) throw new IllegalArgumentException("invalid notification channel");
                boolean changedId;
                synchronized (JobSchedulerService.this) {
                    changedId = mNotificationRuns.contains(this) && this.notificationId != notificationId;
                }
                if (changedId) releaseNotification(this);
                synchronized (JobSchedulerService.this) {
                    this.notificationId = notificationId;
                    notificationChannelId = channel.getId();
                    notificationEndPolicy = jobEndNotificationPolicy;
                    mNotificationRuns.add(this);
                    // UIJ status belongs to the active jobs, not caller-supplied flags.
                    posted.flags &= ~Notification.FLAG_USER_INITIATED_JOB;
                    if (notificationHasUijLocked(job.packageName, notificationId)) {
                        posted.flags |= Notification.FLAG_USER_INITIATED_JOB;
                    }
                }
                mNotifications.postFor(captured, job.packageName, null, notificationId, posted);
            });
        }

        private void callback(int jobId) {
            // Completed tokens expose no work and every callback is a no-op. A
            // duplicate completion may arrive after the app was marked exiting.
            synchronized (JobSchedulerService.this) { if (done) return; }
            mCallers.requireSystemOrPackage(job.packageName);
            if (jobId != job.info.getId() || !current(job)) {
                throw new SecurityException("Job callback owner or job ID changed");
            }
        }
    }

    // Guarded by this.
    private final List<Job> mJobs = new ArrayList<>();
    // Associations survive removal from the scheduled-job list until each Run
    // actually completes; canceled/rescheduled services may still be stopping.
    private final Set<Run> mNotificationRuns = new HashSet<>();
    private final Map<String, Long> mRemovalEpochs = new java.util.concurrent.ConcurrentHashMap<>();
    private Runnable mBeforePersist; // Server-local race fixture; never a Binder option.
    private int mRunning;
    private Thread mThread;

    JobSchedulerService(ActivityManagerService am, ActivityTaskManagerService atm,
            PackageManagerService pm) {
        mAm = am;
        mAtm = atm;
        mPm = pm;
        mCallers = new CallerIdentity(pm, atm);
        mNotifications = NotificationManagerService.sLast;
        if (mNotifications != null) mNotifications.attachJobScheduler(this);
        pm.onPackageRemoval(this::packageRemoved);
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
        // Content URI triggers watch the content service's changes (hwsvc2, ADR 0051).
        ContentService.addChangeListener(this::contentChanged);
        // Persisted jobs run again from the system server's start.
        if (WindowsHost.inSystemServer()) start();
    }

    synchronized boolean hasUserInitiatedJobNotification(String pkg, String channelId) {
        for (Run run : mNotificationRuns) {
            if (!run.done && run.job.packageName.equals(pkg) && run.job.info.isUserInitiated()
                    && current(run.job) && Objects.equals(run.notificationChannelId, channelId)) return true;
        }
        return false;
    }

    /** Guarded by this; notification IDs are scoped to a package and user 0. */
    private boolean notificationHasUijLocked(String pkg, int notificationId) {
        for (Run run : mNotificationRuns) {
            if (!run.done && run.job.packageName.equals(pkg) && run.notificationId == notificationId
                    && run.job.info.isUserInitiated() && current(run.job)) return true;
        }
        return false;
    }

    /** Called under the notification lifecycle fence, never with the job monitor held. */
    private void releaseNotification(Run run) {
        String pkg = run.job.packageName;
        int id;
        boolean remove, stripUij, ownerCurrent;
        synchronized (this) {
            if (!mNotificationRuns.remove(run)) return;
            id = run.notificationId;
            boolean associated = false;
            for (Run other : mNotificationRuns) {
                if (!other.done && other.job.packageName.equals(pkg) && other.notificationId == id
                        && current(other.job)) { associated = true; break; }
            }
            // The final associated job's policy wins. An earlier REMOVE cannot
            // cancel a notification still used by another ordinary or UIJ run.
            remove = !associated && run.notificationEndPolicy == JobService.JOB_END_NOTIFICATION_POLICY_REMOVE;
            stripUij = !notificationHasUijLocked(pkg, id);
            ownerCurrent = current(run.job);
            run.notificationChannelId = null;
        }
        // Delayed callbacks from a removed/replaced owner cannot mutate a new
        // package's notifications after the package-removal epoch advances.
        if (!ownerCurrent) return;
        if (remove) mNotifications.cancel(pkg, null, id);
        else if (stripUij) mNotifications.removeUserInitiatedJobFlag(pkg, id);
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
        return scheduleInternal(namespace, job, null);
    }

    @Override
    public int scheduleAsPackage(String namespace, JobInfo job, String packageName, int userId,
            String tag) {
        system(); user(userId);
        // Cross-package source attribution is not implemented. Never reinterpret
        // a claimed source package as authority to execute another app's service.
        if (job == null || job.getService() == null || packageName == null
                || !packageName.equals(job.getService().getPackageName())) {
            throw new SecurityException("Job attribution must match its service owner");
        }
        return scheduleInternal(namespace, job, null);
    }

    @Override
    public int enqueue(String namespace, JobInfo info, JobWorkItem work) {
        if (work == null) throw new IllegalArgumentException("Missing job work item");
        return scheduleInternal(namespace, info, work);
    }

    private int scheduleInternal(String namespace, JobInfo info, JobWorkItem work) {
        if (info == null || info.getService() == null) throw new IllegalArgumentException("Missing job service");
        ComponentName service = info.getService();
        String pkg = service.getPackageName();
        ApkPackage capturedOwner = mPm.get(pkg);
        own(pkg);
        ServiceInfo si = checkedService(pkg, info);
        if (si == null) {
            throw new IllegalArgumentException("No such service " + service);
        }
        if (!android.Manifest.permission.BIND_JOB_SERVICE.equals(si.permission)) {
            throw new IllegalArgumentException("Scheduled service " + service
                    + " does not require android.permission.BIND_JOB_SERVICE permission");
        }
        if (GmsSignatureSpoofing.isEnabled() && info.isPersisted()
                && mPm.permissions().checkPermission(pkg, android.Manifest.permission.RECEIVE_BOOT_COMPLETED)
                        != android.content.pm.PackageManager.PERMISSION_GRANTED) {
            throw new SecurityException("Persisted jobs require RECEIVE_BOOT_COMPLETED");
        }
        if (GmsSignatureSpoofing.isEnabled() && info.isUserInitiated()
                && !userInitiatedPermission(pkg)) throw new SecurityException("User-initiated job permission denied");
        start();
        Run stop = null;
        Job job;
        synchronized (this) {
            if (capturedOwner == null || mPm.get(pkg) != capturedOwner || mPm.isRemovalPending(pkg)) {
                throw new SecurityException("Job owner changed before publication");
            }
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
                if (!current(j)) continue;
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
                if (!current(j)) continue;
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
        String pkg = callerPackage();
        synchronized (this) {
            Job j = find(pkg, namespace, jobId);
            return j != null ? j.info : null;
        }
    }

    @Override
    public int getPendingJobReason(String namespace, int jobId) {
        String pkg = callerPackage();
        Job j;
        synchronized (this) {
            j = find(pkg, namespace, jobId);
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
        own(packageName);
        return userInitiatedPermission(packageName);
    }

    @Override
    public boolean hasRunUserInitiatedJobsPermission(String packageName, int userId) {
        user(userId); own(packageName);
        return userInitiatedPermission(packageName);
    }

    @Override
    public List<JobInfo> getStartedJobs() {
        system();
        List<JobInfo> result = new ArrayList<>();
        synchronized (this) {
            for (Job j : mJobs) {
                if (j.run != null && current(j)) result.add(j.info);
            }
        }
        return result;
    }

    @Override public List<android.app.job.PendingJobReasonsInfo> getPendingJobReasonsHistory(
            String namespace, int jobId) {
        callerPackage();
        // Historical reasons are not retained. An empty history exposes no other jobs.
        return java.util.Collections.emptyList();
    }

    @Override public ParceledListSlice getAllJobSnapshots() {
        system();
        throw new UnsupportedOperationException("Job snapshots are not implemented");
    }

    @Override public void registerUserVisibleJobObserver(android.app.job.IUserVisibleJobObserver observer) {
        system();
    }

    @Override public void unregisterUserVisibleJobObserver(android.app.job.IUserVisibleJobObserver observer) {
        system();
    }

    @Override public void notePendingUserRequestedAppStop(String packageName, int userId, String reason) {
        system(); user(userId); own(packageName);
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
        return mCallers.isSystem() ? null : mCallers.callerPackage();
    }

    private void system() {
        if (!mCallers.isSystem()) throw new SecurityException("Job administration requires the host shell");
    }

    private static void user(int userId) {
        if (userId != 0) throw new SecurityException("Only user 0 is available");
    }

    private void own(String pkg) {
        if (pkg == null || mPm.get(pkg) == null) throw new SecurityException("Unknown job owner");
        mCallers.requireSystemOrPackage(pkg);
    }

    private boolean userInitiatedPermission(String pkg) {
        return !GmsSignatureSpoofing.isEnabled() || mPm.permissions().checkPermission(pkg,
                "android.permission.RUN_USER_INITIATED_JOBS") == android.content.pm.PackageManager.PERMISSION_GRANTED;
    }

    private ServiceInfo checkedService(String pkg, JobInfo info) {
        ComponentName component = info.getService();
        if (component == null || !pkg.equals(component.getPackageName())) return null;
        ServiceInfo si = mPm.getServiceInfo(component, 0, 0);
        return si != null && pkg.equals(si.packageName) && component.getClassName().equals(si.name)
                ? si : null;
    }

    private boolean current(Job job) {
        ApkPackage owner = mPm.get(job.packageName);
        ServiceInfo service = checkedService(job.packageName, job.info);
        return owner != null && !mPm.isRemovalPending(job.packageName)
                && job.removalEpoch == mRemovalEpochs.getOrDefault(job.packageName, 0L)
                && owner.uid() == job.ownerUid && job.ownerIdentity != null
                && job.ownerIdentity.equals(NotificationManagerService.codeIdentity(owner))
                && service != null && android.Manifest.permission.BIND_JOB_SERVICE.equals(service.permission);
    }

    private Job find(String pkg, String namespace, int id) {
        for (Job j : mJobs) {
            if (j.same(pkg, namespace, id) && current(j)) return j;
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
                    if (j.run == null && hasTriggers(j.info)) {
                        long at = triggerReadyAt(j);
                        if (at > now && at != Long.MAX_VALUE) next = Math.min(next, at);
                    }
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
        if (hasTriggers(info) && triggerReadyAt(j) > now) {
            return JobScheduler.PENDING_JOB_REASON_CONSTRAINT_CONTENT_TRIGGER;
        }
        return JobScheduler.PENDING_JOB_REASON_UNDEFINED;
    }

    // --- content URI triggers (ContentObserverController) --------------------------------------

    /** ContentObserverController's defaults for a trigger's update and maximum delays. */
    private static final long DEFAULT_TRIGGER_UPDATE_DELAY_MS = 10_000;
    private static final long DEFAULT_TRIGGER_MAX_DELAY_MS = 2 * 60_000;

    private static boolean hasTriggers(JobInfo info) {
        JobInfo.TriggerContentUri[] t = info.getTriggerContentUris();
        return t != null && t.length > 0;
    }

    /**
     * When a job with content URI triggers may run (elapsedRealtime): once no change came
     * for its update delay, or its maximum delay after the first change; never without a
     * change (Long.MAX_VALUE). Guarded by this.
     */
    private long triggerReadyAt(Job j) {
        if (j.changedAuthorities.isEmpty()) return Long.MAX_VALUE;
        long update = j.info.getTriggerContentUpdateDelay();
        long max = j.info.getTriggerContentMaxDelay();
        if (update < 0) update = DEFAULT_TRIGGER_UPDATE_DELAY_MS;
        if (max < 0) max = DEFAULT_TRIGGER_MAX_DELAY_MS;
        return Math.min(j.lastChange + update, j.firstChange + max);
    }

    /** ContentService's changes: jobs whose triggers they match take note, and may run. */
    private void contentChanged(android.net.Uri[] uris, int flags) {
        boolean any = false;
        long now = SystemClock.elapsedRealtime();
        synchronized (this) {
            for (Job j : mJobs) {
                JobInfo.TriggerContentUri[] triggers = j.info.getTriggerContentUris();
                if (triggers == null) continue;
                for (android.net.Uri uri : uris) {
                    for (JobInfo.TriggerContentUri t : triggers) {
                        boolean descendants = (t.getFlags()
                                & JobInfo.TriggerContentUri.FLAG_NOTIFY_FOR_DESCENDANTS) != 0;
                        if (!ContentService.matches(t.getUri(), descendants, uri, flags)) {
                            continue;
                        }
                        if (j.changedAuthorities.isEmpty()) j.firstChange = now;
                        j.lastChange = now;
                        j.changedUris.add(uri);
                        j.changedAuthorities.add(uri.getAuthority());
                        any = true;
                        break;
                    }
                }
            }
        }
        if (any) {
            SystemServer.log("jobscheduler: content changed: " + java.util.Arrays.toString(uris));
            kick();
        }
    }

    private static boolean needsNetwork(JobInfo info) {
        try {
            return info.getRequiredNetwork() != null;
        } catch (LinkageError e) {
            return false;
        }
    }

    /**
     * The network that satisfies the job's NetworkRequest, as ConnectivityManager describes
     * Windows' networks (the net track's connectivity: capabilities from the snapshot of
     * Windows' interfaces and the Network List Manager; hwsvc2, ADR 0051): the default
     * network if it does, else another; null if none does. Throws LinkageError or
     * RuntimeException when the connectivity classes cannot answer.
     */
    private static android.net.Network satisfyingNetwork(JobInfo info) {
        android.net.NetworkRequest request = info.getRequiredNetwork();
        com.gesher.net.WindowsNetworks.Snapshot s = com.gesher.net.WindowsNetworks.parse(
                com.gesher.net.WindowsNetworks.snapshot());
        com.gesher.net.WindowsNetworks.Net chosen = null;
        com.gesher.net.WindowsNetworks.Net def = s.defaultNetwork();
        if (def != null && request.canBeSatisfiedBy(
                android.net.ConnectivityManager.capabilitiesOf(s, def))) {
            chosen = def;
        } else {
            for (com.gesher.net.WindowsNetworks.Net n : s.networks) {
                if (request.canBeSatisfiedBy(android.net.ConnectivityManager.capabilitiesOf(s, n))) {
                    chosen = n;
                    break;
                }
            }
        }
        return chosen != null ? new android.net.Network(chosen.netId()) : null;
    }

    /** The job's network constraint against Windows' networks (unknown: met). */
    private static boolean networkOk(JobInfo info) {
        try {
            return satisfyingNetwork(info) != null;
        } catch (LinkageError | RuntimeException e) {
            // No connectivity classes: the Network List Manager's view, below.
        }
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
        if (!current(j)) { finish(run, false, "its installed owner changed"); return; }
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
        int[] transition = {-1};
        Runnable complete = () -> {
            transition[0] = finishState(run, reschedule);
            if (transition[0] >= 0) releaseNotification(run);
        };
        try {
            if (mNotifications == null) complete.run();
            else mNotifications.withChannelLifecycle(complete);
        } finally {
            // Notification storage/listener failures must not strand a completed
            // service binding or skip the existing persisted-job cleanup.
            if (transition[0] >= 0) finishBinding(run, transition[0] != 0, why);
        }
    }

    /** -1 for an already completed callback; otherwise whether the job remains scheduled. */
    private synchronized int finishState(Run run, boolean reschedule) {
        Job j = run.job;
        boolean keep;
        if (run.done) return -1;
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
        if (!inList || !current(j)) {
            mJobs.remove(j);
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
        return keep ? 1 : 0;
    }

    private void finishBinding(Run run, boolean keep, String why) {
        Job j = run.job;
        SystemServer.log("jobscheduler: " + j + " finished: " + why
                + (keep ? "; next" + describeTimes(j) : ""));
        if (run.service != null) {
            try {
                run.service.asBinder().unlinkToDeath(run, 0);
            } catch (Throwable t) {
                // not linked
            }
        }
        // The verified Run owns a system-created binding. A job callback arrives
        // under the app's Binder identity; it cannot directly unbind that connection.
        long identity = Binder.clearCallingIdentity();
        try {
            mAm.unbindService(run.connection);
        } catch (Throwable t) {
            // not bound
        } finally { Binder.restoreCallingIdentity(identity); }
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
        if (hasTriggers(info)) {
            for (JobInfo.TriggerContentUri t : info.getTriggerContentUris()) {
                c.add("content " + t.getUri());
            }
        }
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
        Runnable beforePersist = mBeforePersist;
        if (beforePersist != null) beforePersist.run();
        synchronized (this) { persistLocked(j); }
    }

    private void persistLocked(Job j) {
        ApkPackage owner = mPm.get(j.packageName);
        if (!mJobs.contains(j) || mPm.isRemovalPending(j.packageName) || owner == null
                || j.removalEpoch != mRemovalEpochs.getOrDefault(j.packageName, 0L)
                || owner.uid() != j.ownerUid || !Objects.equals(j.ownerIdentity, NotificationManagerService.codeIdentity(owner))) return;
        if (!j.info.isPersisted()) return;
        Parcel p = Parcel.obtain();
        try {
            p.writeInt(PERSIST_VERSION);
            p.writeString(j.packageName);
            p.writeString(j.namespace);
            j.info.writeToParcel(p, 0);
            p.writeInt(j.ownerUid);
            p.writeString(j.ownerIdentity);
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

    private synchronized void unpersist(Job j) {
        if (j.removalEpoch != mRemovalEpochs.getOrDefault(j.packageName, 0L)) return;
        for (Job current : mJobs) {
            if (current != j && current.same(j.packageName, j.namespace, j.info.getId())) return;
        }
        if (j.info.isPersisted()) persistFile(j).delete();
    }

    private void packageRemoved(String name) {
        List<Job> cancelled = new ArrayList<>();
        synchronized (this) {
            mRemovalEpochs.put(name, mRemovalEpochs.getOrDefault(name, 0L) + 1);
            for (Job job : mJobs) if (name.equals(job.packageName)) cancelled.add(job);
            mJobs.removeAll(cancelled);
            File[] persisted = persistDir().listFiles();
            if (persisted != null) for (File file : persisted) {
                if (!file.getName().startsWith(name + "-") || !file.getName().endsWith(".job")) continue;
                try { java.nio.file.Files.deleteIfExists(file.toPath()); }
                catch (java.io.IOException failed) {
                    throw new IllegalStateException("Could not revoke persisted package jobs", failed);
                }
            }
        }
        cancelled(cancelled, "package removed");
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
                boolean bound = p.readInt() == PERSIST_VERSION;
                if (!bound) {
                    if (GmsSignatureSpoofing.isEnabled()) { f.delete(); continue; }
                    p.setDataPosition(0); // Deliberate legacy opt-out compatibility.
                }
                String pkg = p.readString();
                String namespace = p.readString();
                JobInfo info = JobInfo.CREATOR.createFromParcel(p);
                if (mPm.get(pkg) == null) {
                    f.delete();  // uninstalled
                    continue;
                }
                Job j = new Job(pkg, namespace, info);
                if ((bound && (p.readInt() != j.ownerUid
                        || !Objects.equals(p.readString(), j.ownerIdentity))) || !current(j)
                        || (GmsSignatureSpoofing.isEnabled() && mPm.permissions().checkPermission(pkg,
                                android.Manifest.permission.RECEIVE_BOOT_COMPLETED)
                                    != android.content.pm.PackageManager.PERMISSION_GRANTED)) {
                    f.delete(); continue;
                }
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
