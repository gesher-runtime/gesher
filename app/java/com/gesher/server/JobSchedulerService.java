/*
 * Copyright (C) 2026 The Gesher Authors
 * Copyright (C) 2014 The Android Open Source Project
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
import android.app.job.PendingJobReasonsInfo;
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
 * ({@code stopJob}) when a required constraint is lost, after 10 minutes, or when cancelled
 * or rescheduled. A finished periodic
 * job waits for its next window. Application failures back off immediately; system stops
 * contribute one backoff attempt per three stops, using the job's linear or exponential
 * policy. Persisted jobs are kept in
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
    private static final int SYSTEM_STOP_TO_FAILURE_RATIO = 3;
    private static final int ABANDONED_TIMEOUTS_BEFORE_AGGRESSIVE_BACKOFF = 3;
    private static final long MAX_PERIOD_MS = 365L * 24 * 60 * 60_000;
    private static final long PERIODIC_WINDOW_BUFFER_MS = 30 * 60_000;
    private static final int BINDING = 0, STARTING = 1, EXECUTING = 2, STOPPING = 3, FINISHED = 4;
    /** Fallback observation interval, including data-volume space changes without an event. */
    private static final long CONSTRAINT_POLL_MS = 30_000;
    private static final int OBSERVE_NETWORK = 1;
    private static final int OBSERVE_POWER = 1 << 1;
    private static final int OBSERVE_IDLE = 1 << 2;
    private static final int OBSERVE_STORAGE = 1 << 3;
    private static final int OBSERVE_TOP = 1 << 4;
    private static final int HISTORY_LIMIT = 10;
    private static final int MAX_RUNNING = 4;
    private static final int MAX_JOBS_PER_APP = 150;
    private static final int PERSIST_VERSION = 0x474a4233; // GJB3: code-bound records with admission UID.
    private static final int LEGACY_PERSIST_VERSION = 0x474a4232;

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
        // JobStore counts self-scheduled jobs by scheduling UID. Cross-UID
        // attributed work is exempt, while a same-UID attributed job still counts.
        final int quotaUid;
        long earliest;          // elapsedRealtime; 0: none
        long latest;            // elapsedRealtime; Long.MAX_VALUE: none
        long originalLatest;    // periodic window retained across failure retries
        int failures;
        int systemStops;
        int abandonedFailures;
        final boolean handleAbandonedJobs;
        final int hostInputs;
        // These entries belong to this schedule, never to an ID reused by another schedule.
        ArrayDeque<PendingJobReasonsInfo> pendingHistory; // Guarded by the scheduler monitor.
        int lastPendingReasons = -1;
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
            this(packageName, namespace, info, Integer.MIN_VALUE);
        }

        Job(String packageName, String namespace, JobInfo info, int quotaUid) {
            this.packageName = packageName;
            removalEpoch = mRemovalEpochs.getOrDefault(packageName, 0L);
            this.namespace = namespace;
            this.info = info;
            hostInputs = hostInputsFor(info);
            ApkPackage owner = mPm.get(packageName);
            ownerUid = owner == null ? -1 : owner.uid();
            ownerIdentity = NotificationManagerService.codeIdentity(owner);
            this.quotaUid = quotaUid == Integer.MIN_VALUE ? ownerUid : quotaUid;
            handleAbandonedJobs = owner != null && android.app.job.Flags.handleAbandonedJobs()
                    && java.util.Arrays.binarySearch(CompatChanges.disabledFor(
                            owner.parsed.getTargetSdkVersion()),
                            JobParameters.OVERRIDE_HANDLE_ABANDONED_JOBS) >= 0;
        }

        /** JobStatus decays priority for application failures, not system stops. */
        int effectivePriority() {
            int priority = info.getPriority();
            if (failures < 2 || info.isUserInitiated()) return priority;
            if (info.isExpedited()) return JobInfo.PRIORITY_HIGH;
            if (failures < 4) return Math.min(priority, JobInfo.PRIORITY_DEFAULT);
            if (failures < 6) return Math.min(priority, JobInfo.PRIORITY_LOW);
            return JobInfo.PRIORITY_MIN;
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
        boolean abandoned;
        int stopReason = JobParameters.STOP_REASON_UNDEFINED;
        int internalStopReason = JobParameters.INTERNAL_STOP_REASON_UNKNOWN;
        volatile boolean topStarted;
        int notificationId;
        String notificationChannelId;
        int notificationEndPolicy;
        final IServiceConnection connection = new IServiceConnection.Stub() {
            @Override
            public void connected(ComponentName name, IBinder binder,
                    android.app.IBinderSession session, boolean dead) {
                if (dead) finish(Run.this, true, "its service disconnected");
                else if (binder == null) finish(Run.this, false, "its service returned a null binding");
                else started(binder);
            }
        };

        // The content changes that made it run (JobParameters' triggered URIs and
        // authorities): taken from the job when it starts (with the scheduler's lock).
        final android.net.Uri[] triggeredUris;
        final String[] triggeredAuthorities;
        // The network that met its network constraint (JobParameters.getNetwork), or null.
        android.net.Network network;
        long networkGeneration;

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

        /** Caller holds the scheduler monitor; also records reasons before parameters exist. */
        void setStopReason(int reason, int internalReason, String why) {
            stopReason = reason;
            internalStopReason = internalReason;
            if (params != null) params.setStopReason(reason, internalReason, why);
        }

        void started(IBinder binder) {
            synchronized (dispatch) {
                if (!current(job)) { finish(this, false, "its owner changed while binding"); return; }
                int violation;
                while (true) {
                    long generation;
                    long observation;
                    synchronized (JobSchedulerService.this) {
                        if (done || stopping || state != BINDING || service != null
                                || !isScheduledRunLocked(this)) return;
                        generation = hostRevisionLocked(job.hostInputs);
                        observation = mConstraintObservation;
                    }
                    ConstraintState constraints = readConstraints(job.hostInputs);
                    synchronized (JobSchedulerService.this) {
                        if (!isScheduledRunLocked(this)) return;
                        if (generation != hostRevisionLocked(job.hostInputs)
                                || observation != mConstraintObservation) continue;
                        // This accepted start-time snapshot supersedes older polling
                        // proposals, including those with the same host-event revision.
                        mConstraintObservation++;
                        topStarted |= job.packageName.equals(constraints.topPackage);
                        long now = SystemClock.elapsedRealtime();
                        recordPendingReasons(job, pendingConstraints(job, now, constraints),
                                System.currentTimeMillis());
                        violation = runningConstraintViolation(job, constraints, now);
                        if (violation != JobParameters.STOP_REASON_UNDEFINED) {
                            claimStopLocked(this, violation,
                                    JobParameters.INTERNAL_STOP_REASON_CONSTRAINTS_NOT_SATISFIED,
                                    constraintWhy(violation));
                        } else {
                            service = IJobService.Stub.asInterface(binder);
                            network = satisfyingNetwork(job.info, constraints);
                            networkGeneration = generation;
                            params = new JobParameters(this, job.namespace, job.info.getId(),
                                    job.info.getExtras(), job.info.getTransientExtras(),
                                    job.info.getClipData(), job.info.getClipGrantFlags(),
                                    deadlineExpired, job.info.isExpedited(), job.info.isUserInitiated(),
                                    triggeredUris, triggeredAuthorities, network);
                            state = STARTING;
                            armTimeout(this, CALLBACK_TIMEOUT_MS);
                        }
                        break;
                    }
                }
                if (violation != JobParameters.STOP_REASON_UNDEFINED) {
                    completeClaimedStop(this, BINDING, constraintWhy(violation));
                    return;
                }
                try {
                    binder.linkToDeath(this, 0);
                } catch (RemoteException | RuntimeException e) {
                    finish(this, true, "its service died before startJob: " + e);
                    return;
                }
                boolean ownerCurrent = current(job);
                boolean scheduled;
                synchronized (JobSchedulerService.this) {
                    scheduled = isScheduledRunLocked(this);
                }
                if (!ownerCurrent || !scheduled) {
                    finish(this, false, "its job or owner changed before dispatch");
                    return;
                }
                try {
                    SystemServer.log("jobscheduler: startJob " + job);
                    service.startJob(params);
                } catch (RemoteException | RuntimeException e) {
                    // JobServiceContext keeps the start acknowledgement timer.
                    // A confirmed disconnect retries; an absent acknowledgement
                    // reaches FINISHED/NO-RETRY at the start timeout instead.
                    SystemServer.log("jobscheduler: startJob failed for " + job + ": " + e);
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
                else dispatchNetwork(this);
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
                    setStopReason(JobParameters.STOP_REASON_UNDEFINED,
                            JobParameters.INTERNAL_STOP_REASON_SUCCESSFUL_FINISH,
                            "app called jobFinished");
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
                        setStopReason(JobParameters.STOP_REASON_UNDEFINED,
                                JobParameters.INTERNAL_STOP_REASON_SUCCESSFUL_FINISH,
                                "last work dequeued");
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
            synchronized (JobSchedulerService.this) {
                // JobServiceContext only records this observation. An executing
                // job still owns its binding until completion or its timeout.
                if (!done && state == EXECUTING) abandoned = true;
            }
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
    private final Map<Integer, Integer> mJobsPerUid = new HashMap<>();

    /** Registry publication and admission accounting share the scheduler monitor. */
    private void addJobLocked(Job job) {
        mJobs.add(job);
        if (job.quotaUid >= 0) mJobsPerUid.merge(job.quotaUid, 1, Integer::sum);
    }

    private void removeJobLocked(Job job) {
        int index = mJobs.indexOf(job);
        if (index < 0) return;
        if (job.quotaUid >= 0) {
            // Every registry insertion goes through addJobLocked. A record
            // without a reservation would make this count underflow; fail
            // before mutating either structure instead of skipping it.
            Integer count = mJobsPerUid.get(job.quotaUid);
            if (count == null || count <= 0) {
                throw new IllegalStateException("Unaccounted job " + job + " for uid " + job.quotaUid);
            }
            if (count == 1) mJobsPerUid.remove(job.quotaUid);
            else mJobsPerUid.put(job.quotaUid, count - 1);
        }
        mJobs.remove(index);
    }

    private void removeJobsLocked(List<Job> jobs) {
        for (Job job : jobs) removeJobLocked(job);
    }
    // Associations survive removal from the scheduled-job list until each Run
    // actually completes; canceled/rescheduled services may still be stopping.
    private final Set<Run> mNotificationRuns = new HashSet<>();
    private final Map<String, Long> mRemovalEpochs = new java.util.concurrent.ConcurrentHashMap<>();
    private Runnable mBeforePersist; // Server-local race fixture; never a Binder option.
    private int mRunning;
    private long mHostGeneration; // Guarded by this, shared order for all host signals.
    // Advances for every accepted full constraint observation, including polling reads
    // that have no corresponding host notification/revision.
    private long mConstraintObservation;
    private final long[] mHostRevisions = new long[5]; // Guarded by this, indexed by OBSERVE_* bit.
    private boolean mCheckExecutionAfterResume; // Guarded by this.
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
            addJobLocked(j);
            SystemServer.log("jobscheduler: persisted " + j + " scheduled again");
        }
        WindowsHost.addListener(this);
        mAtm.setTopResumedPackageListener(() -> constraintsChanged(OBSERVE_TOP));
        if (mConnectivity != null) {
            mConnectivity.addSystemListener(() -> constraintsChanged(OBSERVE_NETWORK));
        }
        mThread = new Thread(this::run, "jobs");
        mThread.setDaemon(true);
        mThread.start();
        SystemServer.log("jobscheduler: running" + (mTimeScale != 1
                ? " (times divided by " + mTimeScale + ")" : ""));
    }

    // --- IJobScheduler -----------------------------------------------------------------------

    @Override
    public int schedule(String namespace, JobInfo job) {
        return scheduleInternal(namespace, job, null, false);
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
        return scheduleInternal(namespace, job, null, true);
    }

    @Override
    public int enqueue(String namespace, JobInfo info, JobWorkItem work) {
        if (work == null) throw new IllegalArgumentException("Missing job work item");
        return scheduleInternal(namespace, info, work, false);
    }

    private int scheduleInternal(String namespace, JobInfo info, JobWorkItem work, boolean attributed) {
        if (info == null || info.getService() == null) throw new IllegalArgumentException("Missing job service");
        ComponentName service = info.getService();
        String pkg = service.getPackageName();
        ApkPackage capturedOwner = mPm.get(pkg);
        own(pkg);
        int callingUid = mCallers.callerUid();
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
        List<Job> stale = new ArrayList<>();
        try {
            synchronized (this) {
                if (capturedOwner == null || mPm.get(pkg) != capturedOwner || mPm.isRemovalPending(pkg)) {
                    throw new SecurityException("Job owner changed before publication");
                }
                // Records with this key whose owner code changed can never run or be
                // found again; this schedule supersedes them, so they must not keep
                // holding a reservation alongside the replacement.
                for (Job j : mJobs) {
                    if (j.same(pkg, namespace, info.getId()) && !current(j)) stale.add(j);
                }
                removeJobsLocked(stale);
                Job old = find(pkg, namespace, info.getId());
                if (work != null && old != null && old.info.equals(info)) {
                    // enqueue onto the same job: its work queue grows.
                    job = old;
                } else {
                    // Android 16 r4 tests >150 before publication: 151 distinct jobs
                    // are admitted, then ordinary replacements/new jobs fail too.
                    // The identical-info enqueue fast path above bypasses this check.
                    if (!attributed && mJobsPerUid.getOrDefault(callingUid, 0) > MAX_JOBS_PER_APP) {
                        throw new IllegalStateException("Apps may not schedule more than "
                                + MAX_JOBS_PER_APP + " distinct jobs");
                    }
                    int quotaUid = !attributed || callingUid == capturedOwner.uid() ? callingUid : -1;
                    job = new Job(pkg, namespace, info, quotaUid);
                    timesForNew(job);
                    if (old != null) {
                        removeJobLocked(old);
                        job.work.addAll(old.work);
                        job.nextWorkId = old.nextWorkId;
                        if (old.run != null && !old.run.stopping) stop = old.run;
                    }
                    addJobLocked(job);
                }
                if (work != null) {
                    work.setWorkId(job.nextWorkId++);
                    job.work.add(work);
                }
                notifyAll();
            }
        } finally {
            // Delivered outside the monitor like cancel(), also when admission fails.
            if (!stale.isEmpty()) cancelled(stale, "superseded");
        }
        if (stop != null) stopRun(stop, JobParameters.STOP_REASON_CANCELLED_BY_APP,
                JobParameters.INTERNAL_STOP_REASON_CANCELED, "rescheduled");
        persist(job);
        SystemServer.log("jobscheduler: scheduled " + job + describeTimes(job)
                + constraintsText(info));
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
            removeJobsLocked(cancelled);
            notifyAll();
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
            removeJobsLocked(cancelled);
            notifyAll();
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
        return getPendingJobReasons(namespace, jobId)[0];
    }

    @Override
    public int[] getPendingJobReasons(String namespace, int jobId) {
        String pkg = callerPackage();
        // Retry one concurrent host or schedule change. A diagnostic query
        // must return even when the inputs keep changing.
        for (int attempt = 0; attempt < 2; attempt++) {
            Job observed;
            long revision;
            synchronized (this) {
                observed = find(pkg, namespace, jobId);
                if (observed == null) return new int[] {JobScheduler.PENDING_JOB_REASON_INVALID_JOB_ID};
                if (observed.run != null) return new int[] {JobScheduler.PENDING_JOB_REASON_EXECUTING};
                revision = hostRevisionLocked(observed.hostInputs);
            }
            ConstraintState state = readConstraints(observed.hostInputs);
            synchronized (this) {
                Job job = find(pkg, namespace, jobId);
                if (job == null) return new int[] {JobScheduler.PENDING_JOB_REASON_INVALID_JOB_ID};
                if (job.run != null) return new int[] {JobScheduler.PENDING_JOB_REASON_EXECUTING};
                if (job != observed || revision != hostRevisionLocked(job.hostInputs)) continue;
                int reasons = pendingConstraints(job, SystemClock.elapsedRealtime(), state);
                if (reasons == 0 && mRunning >= MAX_RUNNING) {
                    return new int[] {JobScheduler.PENDING_JOB_REASON_DEVICE_STATE};
                }
                return pendingReasonArray(reasons);
            }
        }
        return pendingReasonArray(0);
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

    @Override
    public List<PendingJobReasonsInfo> getPendingJobReasonsHistory(String namespace, int jobId) {
        String pkg = callerPackage();
        synchronized (this) {
            Job job = find(pkg, namespace, jobId);
            if (job == null) throw new IllegalArgumentException("Invalid job id");
            if (job.pendingHistory == null) return new ArrayList<>();
            List<PendingJobReasonsInfo> result = new ArrayList<>(job.pendingHistory.size());
            for (PendingJobReasonsInfo entry : job.pendingHistory) {
                result.add(new PendingJobReasonsInfo(entry.getTimestampMillis(),
                        entry.getPendingJobReasons().clone()));
            }
            return result;
        }
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
        int inputs = 0;
        if ((events & WindowsHost.EVENT_POWER_STATUS) != 0) inputs |= OBSERVE_POWER;
        if ((events & WindowsHost.EVENT_DISPLAY_STATE) != 0) inputs |= OBSERVE_IDLE;
        boolean resumed = (events & WindowsHost.EVENT_RESUMED) != 0;
        if (resumed) inputs |= OBSERVE_POWER | OBSERVE_IDLE | OBSERVE_STORAGE | OBSERVE_NETWORK;
        if (inputs == 0) return;
        synchronized (this) {
            mCheckExecutionAfterResume |= resumed && mRunning != 0;
            constraintsChanged(inputs);
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
            long period = periodMillis(info);
            long flex = flexMillis(info, period);
            job.latest = now + period;
            job.originalLatest = job.latest;
            job.earliest = job.latest - flex;
        } else {
            job.earliest = info.hasEarlyConstraint() ? now + info.getMinLatencyMillis() : 0;
            job.latest = info.hasLateConstraint() ? now + info.getMaxExecutionDelayMillis()
                    : Long.MAX_VALUE;
        }
    }

    /** Host event threads only signal the scheduler. They never deliver app callbacks. */
    private synchronized void constraintsChanged(int inputs) {
        long revision = ++mHostGeneration;
        for (int bit = 0; bit < mHostRevisions.length; bit++) {
            if ((inputs & (1 << bit)) != 0) mHostRevisions[bit] = revision;
        }
        if (!mJobs.isEmpty()) notifyAll();
    }

    private long hostRevisionLocked(int inputs) {
        long revision = 0;
        for (int bit = 0; bit < mHostRevisions.length; bit++) {
            if ((inputs & (1 << bit)) != 0) revision = Math.max(revision, mHostRevisions[bit]);
        }
        return revision;
    }

    private int hostInputsLocked() {
        int inputs = 0;
        for (Job job : mJobs) inputs |= job.hostInputs;
        return inputs;
    }

    private void run() {
        while (!Thread.currentThread().isInterrupted()) {
            try {
                schedulePass(true);
            } catch (InterruptedException interrupted) {
                Thread.currentThread().interrupt();
            }
        }
    }

    /** One observation and one admission/transition pass, shared with the server-local fixture. */
    private void schedulePass(boolean waitForWork) throws InterruptedException {
        long sampledAfter;
        long sampledObservation;
        int inputs;
        synchronized (this) {
            if (waitForWork && mJobs.isEmpty()) {
                mCheckExecutionAfterResume = false;
                wait();
                return;
            }
            inputs = hostInputsLocked();
            sampledAfter = mHostGeneration;
            sampledObservation = mConstraintObservation;
        }
        // Native calls (and the task-manager lock) stay outside the scheduler monitor.
        ConstraintState state = readConstraints(inputs);
        List<RunConstraintUpdate> updates = new ArrayList<>();
        List<Run> ready = new ArrayList<>();
        boolean checkTimeouts;
        synchronized (this) {
            boolean staleObservation = sampledObservation != mConstraintObservation;
            boolean retry = staleObservation;
            // Polling can observe storage changes without a host event. If another
            // thread accepted a newer snapshot while this read was in flight, this
            // entire older snapshot is stale even when host revisions are identical.
            long observation = staleObservation ? mConstraintObservation : ++mConstraintObservation;
            long now = SystemClock.elapsedRealtime();
            long next = hostPollAt(now, inputs, state);
            List<Job> eligible = new ArrayList<>();
            long timestamp = System.currentTimeMillis();
            for (Job job : mJobs) {
                long revision = hostRevisionLocked(job.hostInputs);
                if (staleObservation || (job.hostInputs & ~inputs) != 0 || revision > sampledAfter) {
                    // The next pass refreshes this job. Changes to one host
                    // input cannot starve jobs using other inputs.
                    retry = true;
                    continue;
                }
                Run run = job.run;
                if (run != null) {
                    // Android records constraint transitions for active jobs too,
                    // before stop dispatch/acknowledgement can complete.
                    recordPendingReasons(job, pendingConstraints(job, now, state), timestamp);
                    if (!run.stopping && !run.done) {
                        int lost = runningConstraintViolation(job, state, now);
                        android.net.Network network = lost == JobParameters.STOP_REASON_UNDEFINED
                                && needsNetwork(job.info) ? satisfyingNetwork(job.info, state) : null;
                        if (lost != JobParameters.STOP_REASON_UNDEFINED
                                || needsNetwork(job.info) && (run.networkGeneration != revision
                                        || !Objects.equals(run.network, network))) {
                            updates.add(new RunConstraintUpdate(run, revision, observation, lost, network));
                        }
                    }
                    if (job.latest > now) next = Math.min(next, job.latest);
                    continue;
                }
                int reasons = pendingConstraints(job, now, state);
                recordPendingReasons(job, reasons, timestamp);
                if (reasons == 0) eligible.add(job);
                if (job.earliest > now) next = Math.min(next, job.earliest);
                if (job.latest > now) next = Math.min(next, job.latest);
                if (hasTriggers(job.info)) {
                    long at = triggerReadyAt(job);
                    if (at > now) next = Math.min(next, at);
                }
            }
            if (waitForWork) admitReady(eligible, ready, state.topPackage);
            checkTimeouts = mCheckExecutionAfterResume;
            mCheckExecutionAfterResume = false;
            if (waitForWork && ready.isEmpty() && updates.isEmpty() && !checkTimeouts && !retry) {
                if (next == Long.MAX_VALUE) wait();
                else wait(Math.max(1, next - now));
                return;
            }
        }
        for (RunConstraintUpdate update : updates) applyConstraintUpdate(update);
        if (checkTimeouts) checkExecutionTimeouts();
        for (Run run : ready) bind(run);
    }

    private static long hostPollAt(long now, int inputs, ConstraintState state) {
        if (inputs == 0) return Long.MAX_VALUE;
        long delay = CONSTRAINT_POLL_MS;
        if (state.idleMillis > 0 && state.idleMillis < IDLE_AFTER_MS) {
            delay = Math.min(delay, IDLE_AFTER_MS - state.idleMillis);
        }
        return now + delay;
    }

    /** Preserves queue order across apps, with preference within the selected app. */
    private void admitReady(List<Job> eligible, List<Run> ready, String topPackage) {
        while (mRunning < MAX_RUNNING && !eligible.isEmpty()) {
            Job selected = eligible.get(0);
            String firstOwner = selected.packageName;
            for (Job job : eligible) {
                if (job.packageName.equals(firstOwner) && preferredWithinApp(job, selected)) selected = job;
            }
            eligible.remove(selected);
            Run run = new Run(selected);
            run.topStarted = selected.packageName.equals(topPackage);
            selected.run = run;
            mRunning++;
            ready.add(run);
        }
    }

    private static boolean preferredWithinApp(Job first, Job second) {
        if (first.info.isUserInitiated() != second.info.isUserInitiated()) {
            return first.info.isUserInitiated();
        }
        if (first.info.isExpedited() != second.info.isExpedited()) return first.info.isExpedited();
        return Objects.equals(first.namespace, second.namespace)
                && first.effectivePriority() > second.effectivePriority();
    }

    private static boolean deadlineSatisfied(Job job, long now) {
        return !job.info.isPeriodic() && job.info.hasLateConstraint() && now >= job.latest;
    }

    private static int reasonBit(int reason) { return 1 << reason; }

    /** Explicit pending constraints, in the order used by Android's JobStatus. Guarded by this. */
    private int pendingConstraints(Job job, long now, ConstraintState state) {
        if (deadlineSatisfied(job, now)) return 0;
        JobInfo info = job.info;
        int reasons = 0;
        if (info.isRequireBatteryNotLow() && !batteryNotLow(state.power)) {
            reasons |= reasonBit(JobScheduler.PENDING_JOB_REASON_CONSTRAINT_BATTERY_NOT_LOW);
        }
        if (info.isRequireCharging() && !charging(job, state)) {
            reasons |= reasonBit(JobScheduler.PENDING_JOB_REASON_CONSTRAINT_CHARGING);
        }
        if (info.isRequireDeviceIdle() && state.idleMillis < IDLE_AFTER_MS) {
            reasons |= reasonBit(JobScheduler.PENDING_JOB_REASON_CONSTRAINT_DEVICE_IDLE);
        }
        if (needsNetwork(info) && satisfyingNetwork(info, state) == null) {
            reasons |= reasonBit(JobScheduler.PENDING_JOB_REASON_CONSTRAINT_CONNECTIVITY);
        }
        if (hasTriggers(info) && triggerReadyAt(job) > now) {
            reasons |= reasonBit(JobScheduler.PENDING_JOB_REASON_CONSTRAINT_CONTENT_TRIGGER);
        }
        if (info.isRequireStorageNotLow() && !state.storageNotLow) {
            reasons |= reasonBit(JobScheduler.PENDING_JOB_REASON_CONSTRAINT_STORAGE_NOT_LOW);
        }
        if (job.earliest > now) {
            reasons |= reasonBit(JobScheduler.PENDING_JOB_REASON_CONSTRAINT_MINIMUM_LATENCY);
        }
        if (reasons != 0 && !info.isPeriodic() && info.hasLateConstraint() && now < job.latest) {
            reasons |= reasonBit(JobScheduler.PENDING_JOB_REASON_CONSTRAINT_DEADLINE);
        }
        return reasons;
    }

    private static final int[] PENDING_REASON_ORDER = {
            JobScheduler.PENDING_JOB_REASON_CONSTRAINT_BATTERY_NOT_LOW,
            JobScheduler.PENDING_JOB_REASON_CONSTRAINT_CHARGING,
            JobScheduler.PENDING_JOB_REASON_CONSTRAINT_DEVICE_IDLE,
            JobScheduler.PENDING_JOB_REASON_CONSTRAINT_CONNECTIVITY,
            JobScheduler.PENDING_JOB_REASON_CONSTRAINT_CONTENT_TRIGGER,
            JobScheduler.PENDING_JOB_REASON_CONSTRAINT_STORAGE_NOT_LOW,
            JobScheduler.PENDING_JOB_REASON_CONSTRAINT_MINIMUM_LATENCY,
            JobScheduler.PENDING_JOB_REASON_CONSTRAINT_DEADLINE,
    };

    private static int[] pendingReasonArray(int reasons) {
        if (reasons == 0) return new int[] {JobScheduler.PENDING_JOB_REASON_UNDEFINED};
        int[] result = new int[Integer.bitCount(reasons)];
        int index = 0;
        for (int reason : PENDING_REASON_ORDER) {
            if ((reasons & reasonBit(reason)) != 0) result[index++] = reason;
        }
        return result;
    }

    private static void recordPendingReasons(Job job, int reasons, long timestamp) {
        if (reasons == job.lastPendingReasons) return;
        job.lastPendingReasons = reasons;
        if (job.pendingHistory == null) job.pendingHistory = new ArrayDeque<>();
        if (job.pendingHistory.size() == HISTORY_LIMIT) job.pendingHistory.removeFirst();
        job.pendingHistory.addLast(new PendingJobReasonsInfo(timestamp, pendingReasonArray(reasons)));
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
        if ((job.packageName.equals(state.topPackage) || run != null && run.topStarted)
                && job.effectivePriority() >= JobInfo.PRIORITY_DEFAULT) return true;
        return batteryNotLow(power)
                && ((power[1] & (128 | 8)) != 0 || power[2] == 100);
    }

    private static final class ConstraintState {
        com.gesher.net.WindowsNetworks.Snapshot networks;
        Map<android.net.NetworkRequest, android.net.Network> networkMatches;
        int[] power;
        long idleMillis;
        boolean storageNotLow;
        String topPackage;
    }

    private static int hostInputsFor(JobInfo info) {
        int inputs = 0;
        if (needsNetwork(info)) inputs |= OBSERVE_NETWORK;
        if (info.isRequireBatteryNotLow()) inputs |= OBSERVE_POWER;
        if (info.isRequireCharging()) inputs |= OBSERVE_POWER | OBSERVE_TOP;
        if (info.isRequireDeviceIdle()) inputs |= OBSERVE_IDLE;
        if (info.isRequireStorageNotLow()) inputs |= OBSERVE_STORAGE;
        return inputs;
    }

    private ConstraintState readConstraints(int inputs) {
        ConstraintState state = new ConstraintState();
        if ((inputs & OBSERVE_NETWORK) != 0) {
            try {
                String snapshot = mNetworkSnapshotOverride;
                if (snapshot == null) snapshot = mConnectivity == null
                        ? com.gesher.net.WindowsNetworks.snapshot() : mConnectivity.snapshotForSystem();
                if (snapshot != null) state.networks = com.gesher.net.WindowsNetworks.parse(snapshot);
            } catch (LinkageError | RuntimeException unavailable) {
                // No observed network means no satisfying network.
            }
        }
        if ((inputs & OBSERVE_POWER) != 0) {
            int[] power = mPowerStatusOverride;
            state.power = power != null ? power : WindowsHost.powerStatus();
        }
        if ((inputs & OBSERVE_IDLE) != 0) {
            Long idle = mIdleMillisOverride;
            state.idleMillis = idle != null ? idle : WindowsHost.jobIdleMillis();
        }
        if ((inputs & OBSERVE_TOP) != 0) state.topPackage = mAtm.topResumedPackage();
        if ((inputs & OBSERVE_STORAGE) != 0) {
            try {
                long[] space = mStorageSpaceOverride;
                File data = android.os.Environment.getDataDirectory();
                long total = space != null ? space[0] : data.getTotalSpace();
                long usable = space != null ? space[1] : data.getUsableSpace();
                // StorageManager's default low threshold: 5%, capped at 500 MiB.
                long low = Math.min(total / 20, 500L * 1024 * 1024);
                state.storageNotLow = total > 0 && usable > low;
            } catch (RuntimeException unavailable) {
                state.storageNotLow = false;
            }
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
            if (any) notifyAll();
        }
        if (any) SystemServer.log("jobscheduler: content changed: " + java.util.Arrays.toString(uris));
    }

    private static boolean needsNetwork(JobInfo info) {
        return info.getRequiredNetwork() != null;
    }

    /** Match the actual request, including transport, roaming and custom capabilities. */
    private static android.net.Network satisfyingNetwork(JobInfo info, ConstraintState state) {
        android.net.NetworkRequest request = info.getRequiredNetwork();
        com.gesher.net.WindowsNetworks.Snapshot snapshot = state.networks;
        if (request == null || snapshot == null) return null;
        if (state.networkMatches == null) state.networkMatches = new HashMap<>();
        if (state.networkMatches.containsKey(request)) return state.networkMatches.get(request);
        android.net.Network network = findSatisfyingNetwork(request, snapshot);
        state.networkMatches.put(request, network);
        return network;
    }

    private static android.net.Network findSatisfyingNetwork(android.net.NetworkRequest request,
            com.gesher.net.WindowsNetworks.Snapshot snapshot) {
        com.gesher.net.WindowsNetworks.Net preferred = snapshot.defaultNetwork();
        if (preferred != null && request.canBeSatisfiedBy(
                android.net.ConnectivityManager.capabilitiesOf(snapshot, preferred))) {
            return new android.net.Network(preferred.netId());
        }
        for (com.gesher.net.WindowsNetworks.Net network : snapshot.networks) {
            if (network != preferred && request.canBeSatisfiedBy(
                    android.net.ConnectivityManager.capabilitiesOf(snapshot, network))) {
                return new android.net.Network(network.netId());
            }
        }
        return null;
    }

    private void bind(Run run) {
        synchronized (run.dispatch) {
            synchronized (this) {
                if (run.done || run.stopping || run.state != BINDING
                        || !isScheduledRunLocked(run)) return;
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
                // AOSP reports binding/start response timeouts as internal ANR.
                // A stop response timeout preserves the reason for the stop.
                if (expectedState == BINDING || expectedState == STARTING) {
                    run.setStopReason(JobParameters.STOP_REASON_UNDEFINED,
                            JobParameters.INTERNAL_STOP_REASON_ANR, "callback timed out");
                }
            }
            switch (expectedState) {
                case BINDING: finish(run, true, "timed out while binding"); break;
                case STARTING: finish(run, false, "no onStartJob acknowledgement"); break;
                case STOPPING: finish(run, true, "no onStopJob acknowledgement"); break;
                case EXECUTING:
                    boolean abandoned;
                    synchronized (this) { abandoned = run.abandoned && run.job.handleAbandonedJobs; }
                    stopRun(run, abandoned ? JobParameters.STOP_REASON_TIMEOUT_ABANDONED
                                    : JobParameters.STOP_REASON_TIMEOUT,
                            abandoned ? JobParameters.INTERNAL_STOP_REASON_TIMEOUT_ABANDONED
                                    : JobParameters.INTERNAL_STOP_REASON_TIMEOUT,
                            abandoned ? "timed out and maybe abandoned" : "timed out");
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
                state = claimStopLocked(run, stopReason, internalStopReason, why);
            }
            completeClaimedStop(run, state, why);
        }
    }

    /** Caller holds both dispatch and the scheduler monitor. */
    private int claimStopLocked(Run run, int reason, int internalReason, String why) {
        if (run.done || run.stopping) return FINISHED;
        run.stopping = true;
        run.setStopReason(reason, internalReason, why);
        return run.state;
    }

    private void completeClaimedStop(Run run, int state, String why) {
        if (state == BINDING) {
            // No onStartJob was delivered, so no onStopJob result exists to
            // discard a job for. Keep it for retry unless canceled/replaced.
            finish(run, true, why + " while binding");
        } else if (state == EXECUTING) {
            dispatchStop(run);
        }
        // STARTING waits for the start acknowledgement or its finite timer.
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

    // Explicit server-local fixture injection; no Binder API or production default.
    void setNetworkSnapshotForTest(String snapshot) {
        mNetworkSnapshotOverride = snapshot;
        observeForTest(OBSERVE_NETWORK);
    }

    void setPowerStatusForTest(int[] power) {
        mPowerStatusOverride = power == null ? null : power.clone();
        observeForTest(OBSERVE_POWER);
    }

    void setDeviceIdleMillisForTest(Long idleMillis) {
        mIdleMillisOverride = idleMillis;
        observeForTest(OBSERVE_IDLE);
    }

    void setStorageSpaceForTest(long total, long usable) {
        mStorageSpaceOverride = new long[] {total, usable};
        observeForTest(OBSERVE_STORAGE);
    }

    private void observeForTest(int inputs) {
        constraintsChanged(inputs);
        try {
            schedulePass(false);
        } catch (InterruptedException impossible) {
            throw new AssertionError(impossible);
        }
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

    private void applyConstraintUpdate(RunConstraintUpdate update) {
        Run run = update.run;
        synchronized (run.dispatch) {
            if (update.stopReason == JobParameters.STOP_REASON_UNDEFINED) {
                updateNetwork(run, update.network, update.generation, update.observation);
            } else {
                String why = constraintWhy(update.stopReason);
                int state;
                synchronized (this) {
                    if (!canApplyConstraintUpdateLocked(run, update.generation, update.observation)) return;
                    state = claimStopLocked(run, update.stopReason,
                            JobParameters.INTERNAL_STOP_REASON_CONSTRAINTS_NOT_SATISFIED, why);
                }
                completeClaimedStop(run, state, why);
            }
        }
    }

    private boolean canApplyConstraintUpdateLocked(Run run, long generation, long observation) {
        return observation == mConstraintObservation
                && generation == hostRevisionLocked(run.job.hostInputs) && !run.done && !run.stopping
                && isScheduledRunLocked(run) && current(run.job);
    }

    private boolean isScheduledRunLocked(Run run) {
        return run.job.run == run && mJobs.contains(run.job);
    }

    /** Accept a fresh proposal while holding the run's dispatch gate. */
    private void updateNetwork(Run run, android.net.Network network, long generation, long observation) {
        synchronized (this) {
            if (!canApplyConstraintUpdateLocked(run, generation, observation)) return;
            run.network = network;
            run.networkGeneration = generation;
        }
        dispatchNetwork(run);
    }

    /** Deliver accepted state after start acknowledgement; caller holds run.dispatch. */
    private void dispatchNetwork(Run run) {
        synchronized (this) {
            // An accepted handover can wait for onStartJob while later polling
            // observations leave its network unchanged. Those observations must
            // not discard the deferred callback as if it were a stale proposal.
            if (run.done || run.stopping || run.state != EXECUTING
                    || run.networkGeneration != hostRevisionLocked(run.job.hostInputs)
                    || !isScheduledRunLocked(run) || !current(run.job)
                    || Objects.equals(run.params.getNetwork(), run.network)) return;
            run.params.setNetwork(run.network);
        }
        try {
            run.service.onNetworkChanged(run.params);
        } catch (RemoteException | RuntimeException e) {
            finish(run, true, "network callback failed: " + e);
        }
    }

    private record RunConstraintUpdate(Run run, long generation, long observation, int stopReason,
            android.net.Network network) {}

    /** The run is over: unbind, then reschedule (periodic, backoff) or drop the job. */
    private void finish(Run run, boolean reschedule, String why) {
        synchronized (run.dispatch) {
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
            removeJobLocked(j);
            keep = false;
        } else if (reschedule) {
            // Preserve the trigger that selected this attempt for the retry.
            if (run.triggeredAuthorities != null) {
                if (j.changedAuthorities.isEmpty()) j.firstChange = now;
                java.util.Collections.addAll(j.changedAuthorities, run.triggeredAuthorities);
                if (run.triggeredUris != null) java.util.Collections.addAll(j.changedUris, run.triggeredUris);
                j.lastChange = now;
            }
            rescheduleForFailure(j, run, now);
            keep = true;
        } else if (j.info.isPeriodic()) {
            reschedulePeriodic(j, now);
            j.failures = 0;
            j.systemStops = 0;
            j.abandonedFailures = 0;
            keep = true;
        } else {
            removeJobLocked(j);
            keep = false;
        }
        if (!keep) {
            // Pending/delivered work alone is not a request to retry. In
            // particular, a missing start acknowledgement is FINISHED/NO-RETRY.
            j.work.clear();
            j.delivered.clear();
        }
        notifyAll();
        return keep ? 1 : 0;
    }

    /** Android 16 JobSchedulerService.getRescheduleJobForFailureLocked defaults. */
    private void rescheduleForFailure(Job job, Run run, long now) {
        int reason = run.internalStopReason;
        if (reason == JobParameters.INTERNAL_STOP_REASON_SUCCESSFUL_FINISH
                || reason == JobParameters.INTERNAL_STOP_REASON_TIMEOUT
                || reason == JobParameters.INTERNAL_STOP_REASON_ANR
                || run.stopReason == JobParameters.STOP_REASON_USER) {
            if (job.failures < Integer.MAX_VALUE) job.failures++;
        } else if (job.handleAbandonedJobs
                && reason == JobParameters.INTERNAL_STOP_REASON_TIMEOUT_ABANDONED) {
            if (job.abandonedFailures < Integer.MAX_VALUE) job.abandonedFailures++;
            if (job.failures < Integer.MAX_VALUE) job.failures++;
        } else if (job.systemStops < Integer.MAX_VALUE) {
            job.systemStops++;
        }
        long attempts = (long) job.failures + job.systemStops / SYSTEM_STOP_TO_FAILURE_RATIO;
        if (attempts == 0) {
            job.earliest = 0; // JobStatus.NO_EARLIEST_RUNTIME
        } else {
            long cap = scaled(JobInfo.MAX_BACKOFF_DELAY_MILLIS);
            long backoff = Math.min(cap, scaled(Math.max(JobInfo.MIN_BACKOFF_MILLIS,
                    job.info.getInitialBackoffMillis())));
            boolean linear = job.info.getBackoffPolicy() == JobInfo.BACKOFF_POLICY_LINEAR
                    && !(job.handleAbandonedJobs && job.abandonedFailures
                            > ABANDONED_TIMEOUTS_BEFORE_AGGRESSIVE_BACKOFF);
            long factor = linear ? attempts : 1L << Math.min(62, attempts - 1);
            long delay = backoff > cap / factor ? cap : backoff * factor;
            job.earliest = now + delay;
        }
        // Retried periodic attempts also have no deadline. Their next regular
        // window is computed separately from the original periodic boundary.
        job.latest = Long.MAX_VALUE;
    }

    private long periodMillis(JobInfo info) {
        return Math.max(1, scaled(Math.max(JobInfo.getMinPeriodMillis(),
                Math.min(MAX_PERIOD_MS, info.getIntervalMillis()))));
    }

    private long flexMillis(JobInfo info, long period) {
        return Math.min(period, Math.max(1, scaled(Math.max(JobInfo.getMinFlexMillis(),
                info.getFlexMillis()))));
    }

    /** Keep the original periodic cadence through retry attempts and missed windows. */
    private void reschedulePeriodic(Job job, long now) {
        long period = periodMillis(job.info);
        long flex = flexMillis(job.info, period);
        long original = job.originalLatest;
        if (original < 0 || original == Long.MAX_VALUE) original = now;
        long difference = Math.abs(now - original);
        long buffer = 0;
        if (now > original) {
            long skipped = difference / period + 1;
            if (period != flex && period - flex - difference % period <= flex / 6) skipped++;
            job.latest = original + period * skipped;
        } else {
            job.latest = original + period;
            long maxBuffer = scaled(PERIODIC_WINDOW_BUFFER_MS);
            if (difference < maxBuffer && difference < period / 6) {
                buffer = Math.min(maxBuffer, period / 6 - difference);
            }
        }
        job.earliest = job.latest - Math.min(flex, period - buffer);
        job.originalLatest = job.latest;
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
            p.writeInt(j.quotaUid);
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
            removeJobsLocked(cancelled);
            notifyAll();
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
                int version = p.readInt();
                if (version != PERSIST_VERSION && version != LEGACY_PERSIST_VERSION) {
                    f.delete(); continue; // An unbound record has no verifiable installed owner.
                }
                String pkg = p.readString();
                String namespace = p.readString();
                JobInfo info = JobInfo.CREATOR.createFromParcel(p);
                if (mPm.get(pkg) == null) {
                    f.delete();  // uninstalled
                    continue;
                }
                int ownerUid = p.readInt();
                String ownerIdentity = p.readString();
                // GJB2 did not record attribution. Retain its verified owner and
                // count it conservatively; never discard valid work for a quota.
                if (version == PERSIST_VERSION && p.dataAvail() < 4) { f.delete(); continue; }
                int quotaUid = version == PERSIST_VERSION ? p.readInt() : ownerUid;
                Job j = new Job(pkg, namespace, info, quotaUid);
                if (quotaUid < -1 || ownerUid != j.ownerUid
                        || !Objects.equals(ownerIdentity, j.ownerIdentity) || !current(j)
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
