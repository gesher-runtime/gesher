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
import java.util.concurrent.ScheduledFuture;
import java.util.concurrent.ScheduledThreadPoolExecutor;
import java.util.concurrent.TimeUnit;

/**
 * {@code jobscheduler}: JobSchedulerService on Windows (devsvc track, docs/services.md
 * "Device services", ADR 0038), what WorkManager schedules its work with. Jobs (by
 * package, namespace and id) wait for their minimum latency, their periodic window
 * (interval and flex, as JobStatus computes them) and their constraints: network (Windows'
 * network list manager: connected to the internet, unmetered when asked), charging (AC
 * power, or a PC without a battery), battery not low, device idle (an observed
 * noninteractive display for the AOSP inactivity threshold) and storage not low. A one-shot override
 * deadline bypasses these explicit constraints; periodic windows do not.
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
    private static final long IDLE_AFTER_MS = 31 * 60_000; // AOSP config_jobSchedulerInactivityIdleThreshold
    private static final long BIND_TIMEOUT_MS = 18_000;
    private static final long CALLBACK_TIMEOUT_MS = 8_000;
    private static final int BINDING = 0, STARTING = 1, EXECUTING = 2, STOPPING = 3, FINISHED = 4;
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
    private final ConnectivityService mConnectivity;
    private final ScheduledThreadPoolExecutor mTimeouts = new ScheduledThreadPoolExecutor(1, task -> {
        Thread thread = new Thread(task, "job-timeouts");
        thread.setDaemon(true);
        return thread;
    });

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
        volatile Run run;       // writes under the scheduler monitor; sampled for constraint queries
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
        final boolean deadlineExpired;
        // Dispatch serializes outbound start/stop/network calls with binding cleanup.
        // Never acquire it while holding the scheduler monitor. Binder callbacks
        // update state through the same gate, without holding the monitor over IPC.
        final Object dispatch = new Object();
        int state = BINDING;
        ScheduledFuture<?> timeout;
        IJobService service;
        JobParameters params;
        boolean stopping;
        boolean done;
        volatile boolean topStarted;
        int notificationId;
        String notificationChannelId;
        int notificationEndPolicy;
        final IServiceConnection connection = new IServiceConnection.Stub() {
            @Override
            public void connected(ComponentName name, IBinder binder,
                    android.app.IBinderSession session, boolean dead) {
                if (dead || binder == null) finish(Run.this, true, "its service disconnected");
                else started(binder);
            }
        };

        // The content changes that made it run (JobParameters' triggered URIs and
        // authorities): taken from the job when it starts (with the scheduler's lock).
        final android.net.Uri[] triggeredUris;
        final String[] triggeredAuthorities;
        // The network that met its network constraint (JobParameters.getNetwork), or null.
        android.net.Network network;

        Run(Job job) {
            this.job = job;
            // JobServiceContext captures this at execution selection, even for a
            // periodic window; the flag itself never grants constraint immunity.
            deadlineExpired = job.latest < started;
            // Assigned from an observed snapshot when the service is ready to start.
            network = null;
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
            synchronized (dispatch) {
                if (!current(job)) { finish(this, false, "its owner changed while binding"); return; }
                synchronized (JobSchedulerService.this) {
                    if (done || stopping || state != BINDING || service != null) return;
                }
                ConstraintState constraints = readConstraints();
                topStarted |= constraints.topUid == job.ownerUid;
                int violation = runningConstraintViolation(job, constraints, SystemClock.elapsedRealtime());
                if (violation != JobParameters.STOP_REASON_UNDEFINED) {
                    stopRun(this, violation, JobParameters.INTERNAL_STOP_REASON_CONSTRAINTS_NOT_SATISFIED,
                            constraintWhy(violation));
                    return;
                }
                synchronized (JobSchedulerService.this) {
                    if (done || stopping) return;
                    service = IJobService.Stub.asInterface(binder);
                    network = satisfyingNetwork(job.info, constraints);
                    params = new JobParameters(this, job.namespace, job.info.getId(),
                            job.info.getExtras(), job.info.getTransientExtras(),
                            job.info.getClipData(), job.info.getClipGrantFlags(),
                            deadlineExpired,
                            job.info.isExpedited(), job.info.isUserInitiated(),
                            triggeredUris, triggeredAuthorities, network);
                    state = STARTING;
                    armTimeout(this, CALLBACK_TIMEOUT_MS);
                }
                try {
                    binder.linkToDeath(this, 0);
                    if (!current(job)) { finish(this, false, "its owner changed before dispatch"); return; }
                    SystemServer.log("jobscheduler: startJob " + job);
                    service.startJob(params);
                } catch (RemoteException | RuntimeException e) {
                    finish(this, true, "startJob failed: " + e);
                }
            }
        }

        @Override
        public void binderDied() {
            finish(this, true, "its process died");
        }

        @Override
        public void acknowledgeStartMessage(int jobId, boolean ongoing) {
            callback(jobId);
            synchronized (dispatch) {
                synchronized (JobSchedulerService.this) {
                    if (done || state != STARTING) return;
                    state = EXECUTING;
                    armTimeout(this, Math.max(1, started + scaled(EXECUTION_LIMIT_MS)
                            - SystemClock.elapsedRealtime()));
                }
                if (!ongoing) finish(this, false, "onStartJob returned false");
                else if (stopping) dispatchStop(this);
                else updateNetwork(this, network);
            }
        }

        @Override
        public void acknowledgeStopMessage(int jobId, boolean reschedule) {
            callback(jobId);
            synchronized (dispatch) {
                synchronized (JobSchedulerService.this) {
                    if (done || state != STOPPING) return;
                }
                finish(this, reschedule, "stopped (onStopJob: reschedule " + reschedule + ")");
            }
        }

        @Override
        public void jobFinished(int jobId, boolean reschedule) {
            callback(jobId);
            synchronized (dispatch) {
                synchronized (JobSchedulerService.this) {
                    if (done || (state != EXECUTING && state != STOPPING)) return;
                }
                finish(this, reschedule, "jobFinished (reschedule " + reschedule + ")");
            }
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
    // Test overrides (isolated to test fixtures; never set in production).
    private volatile String mNetworkSnapshotOverride;
    private volatile int[] mPowerStatusOverride;
    private volatile Long mIdleMillisOverride;
    private volatile long[] mStorageSpaceOverride;

    JobSchedulerService(ActivityManagerService am, ActivityTaskManagerService atm,
            PackageManagerService pm) {
        this(am, atm, pm, null);
    }

    JobSchedulerService(ActivityManagerService am, ActivityTaskManagerService atm,
            PackageManagerService pm, ConnectivityService connectivity) {
        mAm = am;
        mAtm = atm;
        mPm = pm;
        mConnectivity = connectivity;
        mTimeouts.setRemoveOnCancelPolicy(true);
        mCallers = new CallerIdentity(pm, atm);
        mNotifications = NotificationManagerService.sLast;
        if (mNotifications != null) mNotifications.attachJobScheduler(this);
        pm.onPackageRemoval(this::packageRemoved);
        double scale = 1;
        String s = System.getenv("GESHER_JOBS_TIME_SCALE");
        if (s != null) {
            try {
                double requested = Double.parseDouble(s);
                if (Double.isFinite(requested)) scale = Math.max(1, requested);
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
        if (mConnectivity != null) mConnectivity.addSystemListener(this::onNetworkChanged);
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
        if (info.isPersisted()
                && mPm.permissions().checkPermission(pkg, android.Manifest.permission.RECEIVE_BOOT_COMPLETED)
                        != android.content.pm.PackageManager.PERMISSION_GRANTED) {
            throw new IllegalArgumentException("Persisted jobs require RECEIVE_BOOT_COMPLETED");
        }
        if (info.isUserInitiated()
                && !userInitiatedPermission(pkg)) throw new SecurityException("User-initiated job permission denied");
        if (needsNetwork(info) && capturedOwner.parsed.getTargetSdkVersion() >= 34
                && mPm.permissions().checkPermission(pkg, android.Manifest.permission.ACCESS_NETWORK_STATE)
                        != android.content.pm.PackageManager.PERMISSION_GRANTED) {
            throw new SecurityException("Network jobs require ACCESS_NETWORK_STATE");
        }
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
        if (stop != null) stopRun(stop, JobParameters.STOP_REASON_CANCELLED_BY_APP,
                JobParameters.INTERNAL_STOP_REASON_CANCELED, "rescheduled");
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
            if (r != null) stopRun(r, JobParameters.STOP_REASON_CANCELLED_BY_APP,
                    JobParameters.INTERNAL_STOP_REASON_CANCELED, why);
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
        if ((events & (WindowsHost.EVENT_POWER_STATUS | WindowsHost.EVENT_RESUMED | WindowsHost.EVENT_DISPLAY_STATE)) != 0) {
            onPowerStateChanged();
            if ((events & WindowsHost.EVENT_RESUMED) != 0) checkExecutionTimeouts();
        }
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
        return mPm.permissions().checkPermission(pkg,
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
            // One host observation per pass, not one native enumeration per job.
            ConstraintState constraints = readConstraints();
            List<Run> ready = new ArrayList<>();
            List<RunToStop> toStop = new ArrayList<>();
            synchronized (this) {
                long now = SystemClock.elapsedRealtime();
                long next = now + CONSTRAINT_POLL_MS;
                for (Job j : mJobs) {
                    if (j.run != null) continue;
                    if (j.earliest > now && !deadlineSatisfied(j, now)) {
                        next = Math.min(next, j.earliest);
                        continue;
                    }
                    if (mRunning >= MAX_RUNNING) continue;
                    if (waitingReason(j, now, constraints) == JobScheduler.PENDING_JOB_REASON_UNDEFINED) {
                        Run run = new Run(j);
                        run.topStarted = constraints.topUid == j.ownerUid;
                        j.run = run;
                        mRunning++;
                        ready.add(run);
                    }
                }
                for (Job j : mJobs) {
                    if (j.run == null && j.latest > now) next = Math.min(next, j.latest);
                    if (j.run == null && hasTriggers(j.info)) {
                        long at = triggerReadyAt(j);
                        if (at > now && at != Long.MAX_VALUE) next = Math.min(next, at);
                    }
                    if (j.run != null && !j.run.stopping) {
                        int reason = runningConstraintViolation(j, constraints, now);
                        if (reason != JobParameters.STOP_REASON_UNDEFINED) {
                            toStop.add(new RunToStop(j.run, reason,
                                    JobParameters.INTERNAL_STOP_REASON_CONSTRAINTS_NOT_SATISFIED,
                                    constraintWhy(reason)));
                        }
                    }
                }
                if (ready.isEmpty() && toStop.isEmpty()) {
                    try { wait(Math.max(1, next - now)); }
                    catch (InterruptedException e) { return; }
                    continue;
                }
            }
            // IJobService is one-way. A stop is claimed before dispatch, without
            // spawning unbounded per-poll threads for the same run.
            for (RunToStop stop : toStop) stopRun(stop.run, stop.reason, stop.internalReason, stop.why);
            for (Run run : ready) bind(run);
        }
    }

    private static boolean deadlineSatisfied(Job job, long now) {
        return !job.info.isPeriodic() && job.info.hasLateConstraint() && now >= job.latest;
    }

    /** Why a job that may run by time still waits; UNDEFINED when nothing holds it. */
    private int waitingReason(Job job, long now) {
        return waitingReason(job, now, readConstraints());
    }

    private int waitingReason(Job job, long now, ConstraintState state) {
        if (deadlineSatisfied(job, now)) return JobScheduler.PENDING_JOB_REASON_UNDEFINED;
        if (job.earliest > now) return JobScheduler.PENDING_JOB_REASON_CONSTRAINT_MINIMUM_LATENCY;
        switch (runningConstraintViolation(job, state, now)) {
            case JobParameters.STOP_REASON_CONSTRAINT_CONNECTIVITY:
                return JobScheduler.PENDING_JOB_REASON_CONSTRAINT_CONNECTIVITY;
            case JobParameters.STOP_REASON_CONSTRAINT_CHARGING:
                return JobScheduler.PENDING_JOB_REASON_CONSTRAINT_CHARGING;
            case JobParameters.STOP_REASON_CONSTRAINT_BATTERY_NOT_LOW:
                return JobScheduler.PENDING_JOB_REASON_CONSTRAINT_BATTERY_NOT_LOW;
            case JobParameters.STOP_REASON_CONSTRAINT_DEVICE_IDLE:
                return JobScheduler.PENDING_JOB_REASON_CONSTRAINT_DEVICE_IDLE;
            case JobParameters.STOP_REASON_CONSTRAINT_STORAGE_NOT_LOW:
                return JobScheduler.PENDING_JOB_REASON_CONSTRAINT_STORAGE_NOT_LOW;
            default:
                if (hasTriggers(job.info) && triggerReadyAt(job) > now) {
                    return JobScheduler.PENDING_JOB_REASON_CONSTRAINT_CONTENT_TRIGGER;
                }
                return JobScheduler.PENDING_JOB_REASON_UNDEFINED;
        }
    }

    private int runningConstraintViolation(Job job, ConstraintState state, long now) {
        JobInfo info = job.info;
        // AOSP JobStatus treats a periodic latest time as a window boundary, not
        // an override deadline. The live deadline may pass after startJob.
        if (deadlineSatisfied(job, now)) return JobParameters.STOP_REASON_UNDEFINED;
        if (needsNetwork(info) && satisfyingNetwork(info, state) == null) {
            return JobParameters.STOP_REASON_CONSTRAINT_CONNECTIVITY;
        }
        if (info.isRequireCharging() && !charging(job, state)) {
            return JobParameters.STOP_REASON_CONSTRAINT_CHARGING;
        }
        if (info.isRequireBatteryNotLow() && !batteryNotLow(state.power)) {
            return JobParameters.STOP_REASON_CONSTRAINT_BATTERY_NOT_LOW;
        }
        if (info.isRequireDeviceIdle() && state.idleMillis < IDLE_AFTER_MS) {
            return JobParameters.STOP_REASON_CONSTRAINT_DEVICE_IDLE;
        }
        if (info.isRequireStorageNotLow() && !state.storageNotLow) {
            return JobParameters.STOP_REASON_CONSTRAINT_STORAGE_NOT_LOW;
        }
        return JobParameters.STOP_REASON_UNDEFINED;
    }

    private static boolean batteryNotLow(int[] power) {
        return power != null && power.length >= 3 && power[1] >= 0 && power[1] != 255
                && (power[1] & (2 | 4)) == 0;
    }

    private static boolean charging(Job job, ConstraintState state) {
        int[] power = state.power;
        if (power == null || power.length < 3 || power[0] != 1) return false;
        // BatteryController retains the top-app power exemption for that run.
        Run run = job.run;
        if ((state.topUid == job.ownerUid || run != null && run.topStarted)
                && job.info.getPriority() >= JobInfo.PRIORITY_DEFAULT) return true;
        return batteryNotLow(power)
                && ((power[1] & (128 | 8)) != 0 || power[2] == 100);
    }

    private static final class ConstraintState {
        com.gesher.net.WindowsNetworks.Snapshot networks;
        int[] power;
        long idleMillis;
        boolean storageNotLow;
        int topUid;
    }

    private ConstraintState readConstraints() {
        ConstraintState state = new ConstraintState();
        try {
            String text = mNetworkSnapshotOverride;
            if (text == null) text = mConnectivity == null
                    ? com.gesher.net.WindowsNetworks.snapshot() : mConnectivity.snapshotForSystem();
            if (text != null) state.networks = com.gesher.net.WindowsNetworks.parse(text);
        } catch (LinkageError | RuntimeException unavailable) {
            // No observed network means no satisfying network, never a fabricated netId.
        }
        int[] power = mPowerStatusOverride;
        Long idle = mIdleMillisOverride;
        state.power = power != null ? power : WindowsHost.powerStatus();
        state.idleMillis = idle != null ? idle : WindowsHost.jobIdleMillis();
        state.topUid = mAtm.topResumedUid();
        try {
            long[] space = mStorageSpaceOverride;
            File data = android.os.Environment.getDataDirectory();
            long total = space != null ? space[0] : data.getTotalSpace();
            long usable = space != null ? space[1] : data.getUsableSpace();
            // StorageManager's default low threshold: 5%, capped at 500 MiB.
            long low = Math.min(total / 20, 500L * 1024 * 1024);
            state.storageNotLow = total > 0 && usable >= 0 && usable > low;
        } catch (RuntimeException unavailable) {
            state.storageNotLow = false;
        }
        return state;
    }

    private static String constraintWhy(int reason) {
        switch (reason) {
            case JobParameters.STOP_REASON_CONSTRAINT_CONNECTIVITY:
                return "connectivity constraint";
            case JobParameters.STOP_REASON_CONSTRAINT_CHARGING:
                return "charging constraint";
            case JobParameters.STOP_REASON_CONSTRAINT_BATTERY_NOT_LOW:
                return "battery not low constraint";
            case JobParameters.STOP_REASON_DEVICE_STATE:
            case JobParameters.STOP_REASON_CONSTRAINT_DEVICE_IDLE:
                return "device idle constraint";
            case JobParameters.STOP_REASON_CONSTRAINT_STORAGE_NOT_LOW:
                return "storage not low constraint";
            default:
                return "constraint lost";
        }
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
        return info.getRequiredNetwork() != null;
    }

    /** Match the actual request, including transport, roaming and custom capabilities. */
    private static android.net.Network satisfyingNetwork(JobInfo info, ConstraintState state) {
        android.net.NetworkRequest request = info.getRequiredNetwork();
        com.gesher.net.WindowsNetworks.Snapshot snapshot = state.networks;
        if (request == null || snapshot == null) return null;
        com.gesher.net.WindowsNetworks.Net preferred = snapshot.defaultNetwork();
        if (preferred != null && request.canBeSatisfiedBy(
                android.net.ConnectivityManager.capabilitiesOf(snapshot, preferred))) {
            return new android.net.Network(preferred.netId());
        }
        for (com.gesher.net.WindowsNetworks.Net network : snapshot.networks) {
            if (request.canBeSatisfiedBy(android.net.ConnectivityManager.capabilitiesOf(snapshot, network))) {
                return new android.net.Network(network.netId());
            }
        }
        return null;
    }

    private void bind(Run run) {
        synchronized (run.dispatch) {
            synchronized (this) {
                if (run.done || run.stopping || run.state != BINDING) return;
                armTimeout(run, BIND_TIMEOUT_MS);
            }
            Job job = run.job;
            if (!current(job)) { finish(run, false, "its installed owner changed"); return; }
            Intent intent = new Intent().setComponent(job.info.getService());
            SystemServer.log("jobscheduler: running " + job);
            int result;
            long identity = Binder.clearCallingIdentity();
            try {
                result = mAm.bindService(null, null, intent, null, run.connection,
                        Context.BIND_AUTO_CREATE | Context.BIND_NOT_FOREGROUND, "android", 0);
            } catch (RuntimeException e) {
                SystemServer.log("jobscheduler: binding " + job + " failed: " + e);
                result = 0;
            } finally { Binder.restoreCallingIdentity(identity); }
            if (result <= 0) finish(run, true, "its service could not be bound");
        }
    }

    /** Caller holds the scheduler monitor; every state owns at most one timeout. */
    private void armTimeout(Run run, long delay) {
        if (run.timeout != null) run.timeout.cancel(false);
        int expectedState = run.state;
        run.timeout = mTimeouts.schedule(() -> operationTimeout(run, expectedState),
                Math.max(1, delay), TimeUnit.MILLISECONDS);
    }

    private void operationTimeout(Run run, int expectedState) {
        synchronized (run.dispatch) {
            synchronized (this) {
                if (run.done || run.state != expectedState) return;
            }
            switch (expectedState) {
                case BINDING: finish(run, true, "timed out while binding"); break;
                case STARTING: finish(run, false, "no onStartJob acknowledgement"); break;
                case STOPPING: finish(run, true, "no onStopJob acknowledgement"); break;
                case EXECUTING:
                    stopRun(run, JobParameters.STOP_REASON_TIMEOUT,
                            JobParameters.INTERNAL_STOP_REASON_TIMEOUT, "timed out");
                    break;
                default: break;
            }
        }
    }

    void stopRun(Run run, String why) {
        stopRun(run, JobParameters.STOP_REASON_CANCELLED_BY_APP,
                JobParameters.INTERNAL_STOP_REASON_CANCELED, why);
    }

    void stopRun(Run run, int stopReason, int internalStopReason, String why) {
        synchronized (run.dispatch) {
            int state;
            synchronized (this) {
                if (run.done || run.stopping) return;
                run.stopping = true;
                state = run.state;
                if (run.params != null) run.params.setStopReason(stopReason, internalStopReason, why);
            }
            if (state == BINDING) {
                // No onStartJob was delivered, so no onStopJob result exists to
                // discard a job for. Keep it for retry unless canceled/replaced.
                finish(run, true, why + " while binding");
            } else if (state == EXECUTING) {
                dispatchStop(run);
            }
            // STARTING waits for the start acknowledgement (or its finite timer).
        }
    }

    /** Caller holds run.dispatch. */
    private void dispatchStop(Run run) {
        synchronized (this) {
            if (run.done || run.state != EXECUTING || !run.stopping) return;
            run.state = STOPPING;
            armTimeout(run, CALLBACK_TIMEOUT_MS);
        }
        SystemServer.log("jobscheduler: stopJob " + run.job + " (" + run.params.getDebugStopReason() + ")");
        try {
            run.service.stopJob(run.params);
        } catch (RemoteException | RuntimeException e) {
            finish(run, true, "stopJob failed: " + e);
        }
    }

    public void onNetworkChanged() { checkRunningConstraintsAndKick(); }
    public void onPowerStateChanged() { checkRunningConstraintsAndKick(); }
    public void onDeviceIdleChanged() { checkRunningConstraintsAndKick(); }

    // Explicit server-local fixture injection; no Binder API or production default.
    void setNetworkSnapshotForTest(String snapshot) {
        mNetworkSnapshotOverride = snapshot;
        onNetworkChanged();
    }

    void setPowerStatusForTest(int[] power) {
        mPowerStatusOverride = power == null ? null : power.clone();
        onPowerStateChanged();
    }

    void setDeviceIdleMillisForTest(Long idleMillis) {
        mIdleMillisOverride = idleMillis;
        onDeviceIdleChanged();
    }

    void setStorageSpaceForTest(long total, long usable) {
        mStorageSpaceOverride = new long[] {total, usable};
        checkRunningConstraintsAndKick();
    }

    void checkExecutionTimeouts() {
        long now = SystemClock.elapsedRealtime();
        List<Run> expired = new ArrayList<>();
        synchronized (this) {
            for (Job job : mJobs) {
                Run run = job.run;
                if (run != null && run.state == EXECUTING && !run.stopping && !run.done
                        && now - run.started >= scaled(EXECUTION_LIMIT_MS)) expired.add(run);
            }
        }
        for (Run run : expired) operationTimeout(run, EXECUTING);
    }

    void checkRunningConstraintsAndKick() {
        ConstraintState constraints = readConstraints();
        long now = SystemClock.elapsedRealtime();
        List<RunToStop> toStop = new ArrayList<>();
        List<RunNetworkUpdate> updates = new ArrayList<>();
        synchronized (this) {
            for (Job job : mJobs) {
                Run run = job.run;
                if (run == null || run.stopping || run.done) continue;
                int reason = runningConstraintViolation(job, constraints, now);
                if (reason != JobParameters.STOP_REASON_UNDEFINED) {
                    toStop.add(new RunToStop(run, reason,
                            JobParameters.INTERNAL_STOP_REASON_CONSTRAINTS_NOT_SATISFIED,
                            constraintWhy(reason)));
                } else if (needsNetwork(job.info)) {
                    updates.add(new RunNetworkUpdate(run, satisfyingNetwork(job.info, constraints)));
                }
            }
            notifyAll();
        }
        for (RunToStop stop : toStop) stopRun(stop.run, stop.reason, stop.internalReason, stop.why);
        for (RunNetworkUpdate update : updates) updateNetwork(update.run, update.network);
    }

    private void updateNetwork(Run run, android.net.Network network) {
        synchronized (run.dispatch) {
            synchronized (this) {
                if (run.done || run.stopping || !mJobs.contains(run.job) || !current(run.job)) return;
                run.network = network;
                if (run.state != EXECUTING || Objects.equals(run.params.getNetwork(), network)) return;
                run.params.setNetwork(network);
            }
            try {
                run.service.onNetworkChanged(run.params);
            } catch (RemoteException | RuntimeException e) {
                finish(run, true, "network callback failed: " + e);
            }
        }
    }

    private static final class RunToStop {
        final Run run;
        final int reason;
        final int internalReason;
        final String why;

        RunToStop(Run run, int reason, int internalReason, String why) {
            this.run = run;
            this.reason = reason;
            this.internalReason = internalReason;
            this.why = why;
        }
    }

    private static final class RunNetworkUpdate {
        final Run run;
        final android.net.Network network;

        RunNetworkUpdate(Run run, android.net.Network network) {
            this.run = run;
            this.network = network;
        }
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
        run.state = FINISHED;
        if (run.timeout != null) {
            run.timeout.cancel(false);
            run.timeout = null;
        }
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
            // Preserve the trigger that selected this attempt for the retry.
            if (run.triggeredAuthorities != null) {
                if (j.changedAuthorities.isEmpty()) j.firstChange = now;
                java.util.Collections.addAll(j.changedAuthorities, run.triggeredAuthorities);
                if (run.triggeredUris != null) java.util.Collections.addAll(j.changedUris, run.triggeredUris);
                j.lastChange = now;
            }
            if (j.failures < Integer.MAX_VALUE) j.failures++;
            long cap = scaled(JobInfo.MAX_BACKOFF_DELAY_MILLIS);
            long backoff = Math.min(cap, scaled(j.info.getInitialBackoffMillis()));
            long factor = j.info.getBackoffPolicy() == JobInfo.BACKOFF_POLICY_LINEAR
                    ? j.failures : 1L << Math.min(30, j.failures - 1);
            long delay = backoff > cap / factor ? cap : backoff * factor;
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
        synchronized (run.dispatch) {
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
                if (!bound) { f.delete(); continue; } // An unbound record has no verifiable installed owner.
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
                        || mPm.permissions().checkPermission(pkg,
                                android.Manifest.permission.RECEIVE_BOOT_COMPLETED)
                                    != android.content.pm.PackageManager.PERMISSION_GRANTED) {
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
