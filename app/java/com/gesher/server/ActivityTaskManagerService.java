/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.server;

import android.app.ActivityManager;
import android.app.ActivityTaskManager;
import android.app.IActivityClientController;
import android.app.IActivityTaskManager;
import android.app.IApplicationThread;
import android.app.ProfilerInfo;
import android.app.ResultInfo;
import android.app.WaitResult;
import android.app.servertransaction.ActivityConfigurationChangeItem;
import android.app.servertransaction.ActivityRelaunchItem;
import android.app.servertransaction.ActivityResultItem;
import android.app.servertransaction.ClientTransaction;
import android.app.servertransaction.ConfigurationChangeItem;
import android.app.servertransaction.DestroyActivityItem;
import android.app.servertransaction.LaunchActivityItem;
import android.app.servertransaction.NewIntentItem;
import android.app.servertransaction.PauseActivityItem;
import android.app.servertransaction.ResumeActivityItem;
import android.app.servertransaction.StartActivityItem;
import android.app.servertransaction.StopActivityItem;
import android.app.servertransaction.TopResumedActivityChangeItem;
import android.content.ComponentName;
import android.content.Context;
import android.content.Intent;
import android.content.pm.ActivityInfo;
import android.content.pm.ApplicationInfo;
import android.content.pm.PackageManager;
import android.content.pm.ResolveInfo;
import android.content.res.Configuration;
import android.graphics.Rect;
import android.os.Binder;
import android.os.Bundle;
import android.os.IBinder;
import android.os.RemoteException;
import android.os.SystemClock;
import android.util.MergedConfiguration;
import android.view.Display;
import android.view.Surface;
import android.window.ActivityWindowInfo;
import android.window.IWindowContainerToken;
import android.window.WindowContainerToken;

import com.android.internal.content.ReferrerIntent;
import com.gesher.app.DeviceProfile;
import com.gesher.app.window.TaskConfiguration;

import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.util.function.Consumer;

/**
 * {@code activity_task}: ActivityTaskManagerService, lite. It keeps what ATMS keeps for
 * an app's activities: the app processes ({@link ProcessRecord}: an
 * {@link IApplicationThread} and its package), the activities ({@link ActivityRecord}: a
 * token, an ActivityInfo, a task, a state, who to send a result to), and drives each
 * Activity with the framework's client transactions, as ATMS does:
 * {@code LaunchActivityItem} and {@code ResumeActivityItem} to start one, then
 * {@code PauseActivityItem}, {@code StopActivityItem} and {@code DestroyActivityItem} to
 * finish it, each step after the app reported the previous one through
 * {@link ActivityClientController}.
 *
 * <p>Activities starting activities (ActivityStarter, lite): the intent is resolved
 * (explicit, or implicit through the manifest's intent filters with CATEGORY_DEFAULT); the
 * new activity goes into the task of the activity that started it (a new task with
 * FLAG_ACTIVITY_NEW_TASK or from outside an activity); the activity in front is paused
 * first and the new one launched once it reports {@code activityPaused}; the paused one
 * is stopped once an activity is idle. A singleTop activity (or FLAG_ACTIVITY_SINGLE_TOP)
 * at the top of the task gets a {@code NewIntentItem} instead of a new instance. When an
 * activity finishes, the activity under it in the task is resumed (with the results sent
 * to it, {@code ActivityResultItem}), then the finished one is stopped and destroyed.
 *
 * <p>Processes: an activity start, a service, a broadcast or a provider for a package with
 * no process starts one ({@link #startProcess}, ProcessList.startProcessLocked): a new
 * dalvikvm running {@code android.app.GesherAppProcess} for the package ({@link Zygote}: no
 * fork on Windows), which attaches with the start sequence number it was given
 * ({@code ActivityThread.attach} → {@link ActivityManagerService#attachApplication}) and is
 * bound to the package ({@code IApplicationThread.bindApplication}). A process may also
 * start by itself and offer itself (gesher-app {@code --system-server}): a start asked by
 * such a process goes to it. The activities start once the process reports
 * {@code finishAttachApplication}, as with ActivityManagerService. A process with no
 * activity, service or provider client left is told to exit ({@code scheduleExit},
 * {@link #maybeExit}), as ActivityManager kills the process of a removed task or an empty
 * cached process.
 */
public final class ActivityTaskManagerService extends IActivityTaskManager.Default {
    /** How long startActivityAndWait waits for the activity to be idle. */
    private static final long WAIT_TIMEOUT_MS = 300_000;

    /** STARTED: made visible again without being resumed (StartActivityItem). */
    enum State { INITIALIZING, LAUNCHING, RESUMING, RESUMED, PAUSING, PAUSED, STARTED, STOPPING,
            STOPPED, DESTROYING, DESTROYED }

    /** An app process attached to the server. */
    final class ProcessRecord {
        final int pid;
        final IApplicationThread thread;
        String packageName;  // null until a start binds it to a package
        boolean ready;       // finishAttachApplication came
        boolean exiting;
        boolean spawned;     // started by this server (startProcess), not by itself
        long startSeq;       // distinguishes a late exit from a newer process of this package
        /** The configuration it was given last (bindApplication, ConfigurationChangeItem). */
        Configuration config;
        final List<ActivityRecord> activities = new ArrayList<>();

        ProcessRecord(int pid, IApplicationThread thread) {
            this.pid = pid;
            this.thread = thread;
        }

        @Override
        public String toString() {
            return "process " + pid + (packageName != null ? " (" + packageName + ")" : "");
        }
    }

    /** An activity: what ActivityRecord and its Task keep. */
    final class ActivityRecord {
        final IBinder token;
        final ActivityInfo info;
        final Intent intent;
        final int taskId;
        final int launchedFromPid;
        final String launchedFromPackage;
        /** startActivityForResult: who gets the result, and with which request code. */
        final ActivityRecord resultTo;
        final String resultWho;
        final int requestCode;
        final long startTime = SystemClock.uptimeMillis();
        ProcessRecord app;
        /** What TaskInfo calls the task's WindowContainerToken; nothing stands behind it. */
        final WindowContainerToken taskToken =
                new WindowContainerToken(new IWindowContainerToken.Stub() {});
        State state = State.INITIALIZING;
        boolean idle;
        boolean finishing;
        /** The activity in front, being paused before this one launches. */
        ActivityRecord waitingFor;
        /** Paused because another activity came in front: stopped once one is idle. */
        boolean stopWhenIdle;
        /** Results for it (ActivityResultItem), sent when it is resumed. */
        final List<ResultInfo> pendingResults = new ArrayList<>();
        /** The state it saved when it was last stopped. */
        Bundle savedState;
        /**
         * Its theme is translucent or floating (ActivityTheme): it does not occlude the
         * activities below it, as ActivityRecord.occludesParent; convertToTranslucent and
         * convertFromTranslucent change it.
         */
        boolean translucent;
        /** Resumed once the activity in front ({@link #waitingFor}) is paused. */
        boolean resumeAfterPause;
        /**
         * The configuration it was launched or relaunched with, or told of last: its
         * package's (locale track: LocaleManagerService.configurationFor) with its task's size
         * override (lifecycle track: TaskConfiguration), see {@link #effectiveConfiguration}.
         */
        Configuration config;
        /** A configuration change waits until it is resumed (it was not in front). */
        boolean configPending;
        /** The size buckets its resources care about (reportSizeConfigurations), or null. */
        android.window.SizeConfigurationBuckets sizeBuckets;
        /**
         * The orientation it asks for (windows3 track): its manifest's screenOrientation,
         * then what setRequestedOrientation sets. The task's rotation follows its top
         * activity's ({@link #updateRotationLocked}).
         */
        int requestedOrientation;
        /** The app was told it is the top resumed activity (TopResumedActivityChangeItem). */
        boolean topSent;

        ActivityRecord(ActivityInfo info, Intent intent, int taskId, int launchedFromPid,
                String launchedFromPackage, ActivityRecord resultTo, String resultWho,
                int requestCode) {
            this.info = info;
            this.intent = intent;
            this.taskId = taskId;
            this.launchedFromPid = launchedFromPid;
            this.launchedFromPackage = launchedFromPackage;
            this.resultTo = resultTo;
            this.resultWho = resultWho;
            this.requestCode = requestCode;
            this.token = new Binder("ActivityRecord " + info.name);
            this.requestedOrientation = info.screenOrientation;
        }

        String name() {
            return info.packageName + "/" + info.name;
        }

        ComponentName component() {
            return new ComponentName(info.packageName, info.name);
        }

        @Override
        public String toString() {
            return "ActivityRecord{" + name() + " t" + taskId + " " + state + "}";
        }
    }

    private final PackageManagerService mPm;
    private final ActivityClientController mClientImpl = new ActivityClientController(this);
    private final StandIn<IActivityClientController> mClient =
            StandIn.of(IActivityClientController.class, mClientImpl);
    private ActivityManagerService mAm;

    // Guarded by this.
    private final List<ProcessRecord> mProcesses = new ArrayList<>();
    private final List<ActivityRecord> mActivities = new ArrayList<>();
    /** Work for a package's process once it is attached and bound (services, broadcasts). */
    private final List<PendingWork> mPendingWork = new ArrayList<>();
    private int mNextTaskId = 1;
    /** Tasks moved to the back (their window minimized): none of their activities is visible. */
    private final java.util.Set<Integer> mHiddenTasks = new java.util.HashSet<>();
    /** Tasks the user resized (resizeTask): their size in pixels; others are the display's. */
    private final android.util.SparseArray<Rect> mTaskBounds = new android.util.SparseArray<>();
    /**
     * Each task's display rotation (Surface.ROTATION_*, absent: ROTATION_0): the display of a
     * device that rotates, turned for the task's top activity's requested orientation
     * (windows3 track, docs/windows3.md). Each task is a window of its own on the desktop, so
     * each has its own.
     */
    private final android.util.SparseIntArray mTaskRotation = new android.util.SparseIntArray();
    /**
     * Tasks by when they came to the front, the latest last (multi-resume, windows3 track):
     * the top resumed activity is the top of the latest that is not at the back.
     */
    private final List<Integer> mTaskOrder = new ArrayList<>();
    /** The activity told it is the top resumed one (onTopResumedActivityChanged), or null. */
    private ActivityRecord mTopResumed;
    /** Processes started for a package and not attached yet, by start sequence number. */
    private final Map<Long, PendingProcess> mStarting = new HashMap<>();
    private long mNextStartSeq = 1;
    /** How app processes are started; null: they start by themselves and attach. */
    private volatile ProcessStarter mStarter;
    /** The OS exit code of the latest start, unknown until that process actually exits. */
    private final ProcessExitStatus mExitStatus = new ProcessExitStatus();
    /** Since when (uptimeMillis) no app process runs or starts; 0 while some do. */
    private long mIdleSince = SystemClock.uptimeMillis();

    /** Starts a process for a package and returns its pid (ProcessList's zygote request). */
    interface ProcessStarter {
        int start(String packageName, long startSeq, int targetSdkVersion) throws Exception;
    }

    private static final class PendingProcess {
        final String packageName;
        final String reason;
        int pid;

        PendingProcess(String packageName, String reason) {
            this.packageName = packageName;
            this.reason = reason;
        }
    }

    private static final class PendingWork {
        final String packageName;
        final Consumer<ProcessRecord> work;

        PendingWork(String packageName, Consumer<ProcessRecord> work) {
            this.packageName = packageName;
            this.work = work;
        }
    }

    ActivityTaskManagerService(PackageManagerService pm) {
        mPm = pm;
    }

    void setActivityManager(ActivityManagerService am) {
        mAm = am;
    }

    /** Lets the server start app processes on demand ({@link #startProcess}). */
    void setProcessStarter(ProcessStarter starter) {
        mStarter = starter;
    }

    // --- IActivityTaskManager --------------------------------------------------------

    @Override
    public IActivityClientController getActivityClientController() {
        return mClient.asInterface();
    }

    @Override
    public int startActivity(IApplicationThread caller, String callingPackage,
            String callingFeatureId, Intent intent, String resolvedType, IBinder resultTo,
            String resultWho, int requestCode, int flags, ProfilerInfo profilerInfo,
            Bundle options) {
        return startActivityAsUser(caller, callingPackage, callingFeatureId, intent,
                resolvedType, resultTo, resultWho, requestCode, flags, profilerInfo, options, 0);
    }

    @Override
    public int startActivityAsUser(IApplicationThread caller, String callingPackage,
            String callingFeatureId, Intent intent, String resolvedType, IBinder resultTo,
            String resultWho, int requestCode, int flags, ProfilerInfo profilerInfo,
            Bundle options, int userId) {
        ActivityRecord[] started = new ActivityRecord[1];
        return start(caller, intent, resolvedType, callingPackage, resultTo, resultWho,
                requestCode, started);
    }

    /**
     * A stack of activities (TaskStackBuilder, PendingIntent.getActivities), lite: the
     * last one, the one in front, is started, in a task of its own when the first intent
     * asks for a new task. ActivityStarter records the ones below it and creates them when
     * the user goes back; here they are left out, so going back leaves the app
     * (docs/e2e.md).
     */
    @Override
    public int startActivities(IApplicationThread caller, String callingPackage,
            String callingFeatureId, Intent[] intents, String[] resolvedTypes, IBinder resultTo,
            Bundle options, int userId) {
        if (intents == null || intents.length == 0) return ActivityManager.START_SUCCESS;
        int last = intents.length - 1;
        Intent intent = new Intent(intents[last]);
        intent.addFlags(intents[0].getFlags() & Intent.FLAG_ACTIVITY_NEW_TASK);
        if (last > 0) {
            SystemServer.log("startActivities: " + intents.length + " activities; starting the"
                    + " last, " + intent.toShortString(false, true, true, false)
                    + " (the ones below are not recorded)");
        }
        ActivityRecord[] started = new ActivityRecord[1];
        return start(caller, intent, resolvedTypes != null ? resolvedTypes[last] : null,
                callingPackage, resultTo, null, -1, started);
    }

    @Override
    public WaitResult startActivityAndWait(IApplicationThread caller, String callingPackage,
            String callingFeatureId, Intent intent, String resolvedType, IBinder resultTo,
            String resultWho, int requestCode, int flags, ProfilerInfo profilerInfo,
            Bundle options, int userId) {
        WaitResult result = new WaitResult();
        ActivityRecord[] started = new ActivityRecord[1];
        result.result = start(caller, intent, resolvedType, callingPackage, resultTo,
                resultWho, requestCode, started);
        ActivityRecord r = started[0];
        if (r == null) return result;
        result.who = r.component();
        result.launchState = WaitResult.LAUNCH_STATE_COLD;
        long deadline = SystemClock.uptimeMillis() + WAIT_TIMEOUT_MS;
        synchronized (this) {
            // ActivityTaskSupervisor.waitActivityVisibleOrLaunched: until the launch is
            // complete (here: the activity is idle), or it went away.
            while (!r.idle && r.state != State.DESTROYED) {
                long left = deadline - SystemClock.uptimeMillis();
                if (left <= 0) {
                    result.timeout = true;
                    break;
                }
                try {
                    wait(left);
                } catch (InterruptedException e) {
                    Thread.currentThread().interrupt();
                    break;
                }
            }
        }
        result.totalTime = SystemClock.uptimeMillis() - r.startTime;
        return result;
    }

    @Override
    public boolean removeTask(int taskId) {
        List<ActivityRecord> toFinish = new ArrayList<>();
        synchronized (this) {
            for (ActivityRecord r : mActivities) {
                if (r.taskId == taskId && !r.finishing) toFinish.add(r);
            }
        }
        if (toFinish.isEmpty()) return false;
        SystemServer.log("removing task " + taskId);
        // Top first, as the task's activities are finished.
        for (int i = toFinish.size() - 1; i >= 0; i--) {
            finish(toFinish.get(i), "remove-task", android.app.Activity.RESULT_CANCELED, null);
        }
        return true;
    }

    /**
     * No AppTask binders (devices track: CameraManager's getRotationOverride walks
     * ActivityManager.getAppTasks(), which needs a list, not null).
     */
    @Override
    public List<IBinder> getAppTasks(String callingPackage) {
        return new ArrayList<>();
    }

    @Override
    public List<ActivityManager.RunningTaskInfo> getTasks(int maxNum,
            boolean filterOnlyVisibleRecents, boolean keepIntentExtra, int displayId) {
        List<ActivityManager.RunningTaskInfo> tasks = new ArrayList<>();
        synchronized (this) {
            // Most recent first, as RunningTasks orders them.
            for (int i = mActivities.size() - 1; i >= 0 && tasks.size() < maxNum; i--) {
                ActivityRecord r = mActivities.get(i);
                if (r.state == State.DESTROYED) continue;
                boolean seen = false;
                for (ActivityManager.RunningTaskInfo t : tasks) {
                    if (t.taskId == r.taskId) {
                        t.numActivities++;
                        t.baseActivity = r.component();
                        t.baseIntent = keepIntentExtra ? new Intent(r.intent)
                                : new Intent(r.intent).replaceExtras((Bundle) null);
                        seen = true;
                    }
                }
                if (seen) continue;
                ActivityManager.RunningTaskInfo t = new ActivityManager.RunningTaskInfo();
                t.taskId = r.taskId;
                t.id = r.taskId;
                t.baseActivity = r.component();
                t.topActivity = t.baseActivity;
                t.baseIntent = keepIntentExtra ? new Intent(r.intent)
                        : new Intent(r.intent).replaceExtras((Bundle) null);
                t.numActivities = 1;
                t.isRunning = r.app != null;
                t.displayId = Display.DEFAULT_DISPLAY;
                t.userId = 0;
                t.configuration.setTo(LocaleManagerService.configurationFor(r.info.packageName));
                t.token = r.taskToken;  // the task's window container (none of its own)
                tasks.add(t);
            }
        }
        return tasks;
    }

    // --- starting --------------------------------------------------------------------

    /**
     * ActivityStarter, lite: resolves the intent, picks the task, delivers it to the top
     * activity (singleTop) or records a new activity, and starts it once the activity in
     * front is paused. Returns the ActivityManager.START_* code; {@code started[0]} is the
     * activity.
     */
    private int start(IApplicationThread caller, Intent intent, String resolvedType,
            String callingPackage, IBinder resultToToken, String resultWho, int requestCode,
            ActivityRecord[] started) {
        // Starts from apps resolve implicit intents to filters with CATEGORY_DEFAULT, as
        // ActivityStarter asks (MATCH_DEFAULT_ONLY); gesher-app's launcher (no caller)
        // resolves MAIN/LAUNCHER in a package, as a launcher does.
        ActivityInfo info = resolve(intent, resolvedType, caller != null);
        if (info == null) {
            SystemServer.log("startActivity: nothing handles " + intent);
            return ActivityManager.START_INTENT_NOT_RESOLVED;
        }
        Intent launch = new Intent(intent);
        launch.setComponent(new ComponentName(info.packageName, info.name));
        int callingPid = Binder.getCallingPid();
        int launchFlags = intent.getFlags();
        ActivityRecord source = resultToToken != null ? forToken(resultToToken) : null;
        // Whether it leaves the activity below it visible (its theme), read before the lock.
        boolean translucent = ActivityTheme.isTranslucentOrFloating(info);
        ActivityRecord r = null;
        ActivityRecord deliverTo = null;
        ActivityRecord pause = null;
        ProcessRecord bind = null;
        boolean launchNow = false;
        boolean needProcess = false;
        List<ActivityRecord> reconfigure = new ArrayList<>();
        synchronized (this) {
            int taskId = source != null && (launchFlags & Intent.FLAG_ACTIVITY_NEW_TASK) == 0
                    ? source.taskId : mNextTaskId++;
            ActivityRecord top = topOf(taskId);
            if (top != null && top.app != null && top.component().equals(launch.getComponent())
                    && (info.launchMode == ActivityInfo.LAUNCH_SINGLE_TOP
                            || (launchFlags & Intent.FLAG_ACTIVITY_SINGLE_TOP) != 0)) {
                deliverTo = top;
            } else {
                ActivityRecord resultRecord = requestCode >= 0 ? source : null;
                r = new ActivityRecord(info, launch, taskId, callingPid, callingPackage,
                        resultRecord, resultWho, requestCode);
                r.translucent = translucent;
                // A start in a task moved to the back brings it to the front.
                mHiddenTasks.remove(taskId);
                // The activity in front in its task is paused first; other tasks' stay
                // resumed (multi-resume: each visible task's top activity is resumed).
                ActivityRecord front = front(r);
                mActivities.add(r);
                focusTaskLocked(taskId);
                // Its orientation may turn the task's display (windows3 track).
                if (updateRotationLocked(taskId)) {
                    for (ActivityRecord a : mActivities) {
                        if (a != r && a.taskId == taskId && !a.finishing && a.app != null) {
                            reconfigure.add(a);
                        }
                    }
                }
                ProcessRecord app = processFor(info.packageName);
                if (app == null) {
                    app = unassignedProcess(callingPid);
                    if (app != null) {
                        assignPackage(app, info.packageName);
                        bind = app;
                    }
                }
                if (app != null) {
                    attach(r, app);
                } else {
                    needProcess = true;
                }
                if (front != null) {
                    r.waitingFor = front;
                    if (front.state != State.PAUSING) pause = front;
                } else {
                    launchNow = app != null && app.ready;
                }
            }
        }
        if (deliverTo != null) {
            SystemServer.log("START u0 {" + launch.toShortString(false, true, true, false)
                    + "} from " + callerName(callingPackage, callingPid)
                    + ": delivered to the top activity " + deliverTo.name() + " (singleTop)");
            deliverNewIntent(deliverTo, launch, callingPackage);
            return ActivityManager.START_DELIVERED_TO_TOP;
        }
        started[0] = r;
        SystemServer.log("START u0 {" + launch.toShortString(false, true, true, false) + "} from "
                + callerName(callingPackage, callingPid) + ": task " + r.taskId
                + (r.app != null ? ", in " + r.app : ", waiting for a process")
                + (r.waitingFor != null ? ", after pausing " + r.waitingFor.name() : "")
                + (r.translucent ? ", translucent or floating (the activity below stays"
                        + " visible)" : "")
                + (r.resultTo != null ? ", result to " + r.resultTo.name() + " (request "
                        + r.requestCode + ")" : ""));
        if (bind != null) bindApplication(bind);
        if (needProcess) startProcess(info.packageName, "activity " + r.name());
        if (pause != null) pause(pause, false);
        if (launchNow) realStartActivity(r);
        for (ActivityRecord a : reconfigure) ensureConfiguration(a);
        updateTopResumed();
        return ActivityManager.START_SUCCESS;
    }

    private static String callerName(String callingPackage, int callingPid) {
        return callingPackage != null ? callingPackage : "pid " + callingPid;
    }

    private ActivityInfo resolve(Intent intent, String resolvedType, boolean defaultOnly) {
        long flags = PackageManager.GET_META_DATA | PackageManager.GET_SHARED_LIBRARY_FILES;
        ActivityInfo info;
        if (intent.getComponent() != null) {
            info = mPm.getActivityInfo(intent.getComponent(), flags, 0);
        } else {
            ResolveInfo ri = mPm.resolveIntent(intent, resolvedType,
                    flags | (defaultOnly ? PackageManager.MATCH_DEFAULT_ONLY : 0), 0);
            info = ri != null ? ri.activityInfo : null;
        }
        if (info == null) return null;
        info = new ActivityInfo(info);
        if (info.applicationInfo == null) {
            info.applicationInfo = mPm.getApplicationInfo(info.packageName, flags, 0);
        }
        return info;
    }

    /** The task's top activity that is not finishing, or null. Guarded by this. */
    private ActivityRecord topOf(int taskId) {
        for (int i = mActivities.size() - 1; i >= 0; i--) {
            ActivityRecord a = mActivities.get(i);
            if (a.taskId == taskId && !a.finishing && a.state != State.DESTROYED) return a;
        }
        return null;
    }

    /**
     * The activity in front in {@code except}'s task, other than {@code except}: resumed or on
     * its way to it, or being paused. Each visible task has one (multi-resume, as Android 10
     * and later resume the top activity of each visible task: windows3 track). Guarded by
     * this.
     */
    private ActivityRecord front(ActivityRecord except) {
        for (int i = mActivities.size() - 1; i >= 0; i--) {
            ActivityRecord a = mActivities.get(i);
            if (a == except || a.app == null || a.taskId != except.taskId) continue;
            switch (a.state) {
                case LAUNCHING:
                case RESUMING:
                case RESUMED:
                    if (!a.finishing) return a;
                    break;
                case PAUSING:
                    return a;
                default:
                    break;
            }
        }
        return null;
    }

    private ProcessRecord processFor(String packageName) {
        for (ProcessRecord p : mProcesses) {
            if (packageName.equals(p.packageName) && !p.exiting) return p;
        }
        return null;
    }

    /** Binds an attached process to a package without reusing an older start's exit. Guarded by this. */
    private void assignPackage(ProcessRecord app, String packageName) {
        app.packageName = packageName;
        if (app.startSeq == 0) {
            app.startSeq = mNextStartSeq++;
            mExitStatus.started(packageName, app.startSeq);
        }
        mExitStatus.bind(packageName, app.startSeq, app.pid);
    }

    /**
     * A process that attached by itself and has no package yet: the one that asked
     * ({@code preferredPid}), else, when this server cannot start processes, the first.
     */
    private ProcessRecord unassignedProcess(int preferredPid) {
        ProcessRecord first = null;
        for (ProcessRecord p : mProcesses) {
            if (p.packageName != null) continue;
            if (p.pid == preferredPid) return p;
            if (first == null) first = p;
        }
        return mStarter == null ? first : null;
    }

    /** Whether a process for the package is being started. Guarded by this. */
    private boolean starting(String packageName) {
        for (PendingProcess p : mStarting.values()) {
            if (p.packageName.equals(packageName)) return true;
        }
        return false;
    }

    /**
     * ProcessList.startProcessLocked: starts a process for {@code packageName} unless one
     * runs or is starting. The process attaches with its start sequence number
     * ({@link #attachApplication}). Without a process starter, a process has to attach by
     * itself.
     */
    void startProcess(String packageName, String reason) {
        ProcessStarter starter = mStarter;
        if (starter == null) return;
        long seq;
        PendingProcess pending;
        synchronized (this) {
            if (processFor(packageName) != null || starting(packageName)) return;
            seq = mNextStartSeq++;
            mExitStatus.started(packageName, seq);
            pending = new PendingProcess(packageName, reason);
            mStarting.put(seq, pending);
            updateIdle();
        }
        ApplicationInfo ai = mPm.getApplicationInfo(packageName, 0, 0);
        int pid;
        try {
            pid = starter.start(packageName, seq, ai != null ? ai.targetSdkVersion : 0);
        } catch (Throwable t) {
            SystemServer.log("starting a process for " + packageName + " (" + reason
                    + ") failed: " + t);
            startFailed(seq, pending);
            return;
        }
        synchronized (this) {
            pending.pid = pid;
            mExitStatus.bind(packageName, seq, pid);
        }
        SystemServer.log("starting a process for " + packageName + " (" + reason + "): pid "
                + pid + ", start seq " + seq);
        // A process that dies before it attaches fails what waited for it.
        Thread watch = new Thread(() -> {
            while (true) {
                int code;
                synchronized (this) {
                    if (mStarting.get(seq) != pending) return;  // attached
                    // exitCode consumes the native handle on completion. Keep
                    // lookup and publication atomic with attach handing it to
                    // processDied's observer, so two readers cannot consume it.
                    code = Zygote.exitCode(pid);
                    if (code != Zygote.STILL_ACTIVE) {
                        mExitStatus.exited(packageName, seq, pid, code);
                    }
                }
                if (code != Zygote.STILL_ACTIVE) {
                    SystemServer.log("process " + pid + " for " + packageName
                            + " exited before attaching (exit code " + code + ")");
                    startFailed(seq, pending);
                    return;
                }
                SystemClock.sleep(250);
            }
        }, "start-watch " + pid);
        watch.setDaemon(true);
        watch.start();
    }

    private void startFailed(long seq, PendingProcess pending) {
        synchronized (this) {
            if (mStarting.get(seq) != pending) return;
            mStarting.remove(seq);
            for (int i = mActivities.size() - 1; i >= 0; i--) {
                ActivityRecord r = mActivities.get(i);
                if (r.app == null && r.info.packageName.equals(pending.packageName)) {
                    r.state = State.DESTROYED;
                    mActivities.remove(i);
                }
            }
            mPendingWork.removeIf(w -> w.packageName.equals(pending.packageName));
            updateIdle();
            notifyAll();
        }
    }

    /** Notes since when no process runs or starts. Guarded by this. */
    private void updateIdle() {
        if (mProcesses.isEmpty() && mStarting.isEmpty()) {
            if (mIdleSince == 0) mIdleSince = SystemClock.uptimeMillis();
        } else {
            mIdleSince = 0;
        }
    }

    /** Since when (uptimeMillis) no app process runs or starts; 0 while some do. */
    synchronized long idleSince() {
        return mIdleSince;
    }

    /** The exit code of the package's last process, or null if none ended (or unknown). */
    synchronized Integer lastExitCode(String packageName) {
        return mExitStatus.code(packageName);
    }

    /** The app processes, for getRunningAppProcesses. */
    synchronized List<ProcessRecord> processes() {
        return new ArrayList<>(mProcesses);
    }

    /**
     * Ends the process if nothing keeps it: no activity, no work waiting for it, nothing the
     * activity manager knows of (services, provider clients). It is told to exit.
     */
    void maybeExit(ProcessRecord app) {
        if (app == null) return;
        synchronized (this) {
            if (app.exiting || !app.ready || !app.activities.isEmpty()) return;
            for (PendingWork w : mPendingWork) {
                if (w.packageName.equals(app.packageName)) return;
            }
        }
        if (mAm.keepsAlive(app)) return;
        synchronized (this) {
            if (app.exiting || !app.activities.isEmpty()) return;
            app.exiting = true;
        }
        mAm.processExiting(app);
        SystemServer.log("scheduleExit: " + app + " has no activities, services or provider"
                + " clients left");
        try {
            app.thread.scheduleExit();
        } catch (RemoteException e) {
            SystemServer.log("scheduleExit: " + e);
        }
    }

    /**
     * Ends the package's processes (an update or an uninstall, as
     * ActivityManagerService.forceStopPackage): the ones this server started are killed,
     * others told to exit; waits up to 5 s for them to go.
     */
    void forceStopPackage(String packageName) {
        List<ProcessRecord> stop = new ArrayList<>();
        synchronized (this) {
            for (ProcessRecord p : mProcesses) {
                if (packageName.equals(p.packageName)) stop.add(p);
            }
            for (PendingProcess p : mStarting.values()) {
                if (p.packageName.equals(packageName) && p.pid > 0) Zygote.kill(p.pid);
            }
        }
        if (stop.isEmpty()) return;
        SystemServer.log("force-stopping " + packageName);
        endProcesses(stop, 5000);
    }

    /** Ends every app process (the device shuts down), waiting up to {@code waitMs}. */
    void endAllProcesses(long waitMs) {
        List<ProcessRecord> all;
        synchronized (this) {
            all = new ArrayList<>(mProcesses);
            for (PendingProcess p : mStarting.values()) {
                if (p.pid > 0) Zygote.kill(p.pid);
            }
        }
        endProcesses(all, waitMs);
    }

    private void endProcesses(List<ProcessRecord> processes, long waitMs) {
        for (ProcessRecord p : processes) {
            synchronized (this) {
                p.exiting = true;
            }
            if (p.spawned) {
                Zygote.kill(p.pid);
            } else {
                try {
                    p.thread.scheduleExit();
                } catch (RemoteException e) {
                    // gone already
                }
            }
        }
        long deadline = SystemClock.uptimeMillis() + waitMs;
        synchronized (this) {
            while (true) {
                boolean left = false;
                for (ProcessRecord p : processes) left |= mProcesses.contains(p);
                long wait = deadline - SystemClock.uptimeMillis();
                if (!left || wait <= 0) break;
                try {
                    wait(wait);
                } catch (InterruptedException e) {
                    Thread.currentThread().interrupt();
                    break;
                }
            }
        }
    }

    private void attach(ActivityRecord r, ProcessRecord app) {
        r.app = app;
        app.activities.add(r);
    }

    // --- processes (from ActivityManagerService) ---------------------------------------

    /**
     * IActivityManager.attachApplication: a process this server started for a package
     * ({@code startSeq}), or one that offers itself.
     */
    void attachApplication(IApplicationThread thread, int pid, long startSeq) {
        ProcessRecord app = new ProcessRecord(pid, thread);
        boolean bind = false;
        synchronized (this) {
            mProcesses.add(app);
            updateIdle();
            PendingProcess started = startSeq > 0 ? mStarting.remove(startSeq) : null;
            String waiting = null;
            if (started != null) {
                app.spawned = true;
                app.startSeq = startSeq;
                waiting = started.packageName;
            } else {
                // A start waiting for a process (none being started for its package): the
                // one asked by this process, else the first.
                for (ActivityRecord r : mActivities) {
                    if (r.app != null || r.state != State.INITIALIZING
                            || starting(r.info.packageName)) {
                        continue;
                    }
                    if (r.launchedFromPid == pid) {
                        waiting = r.info.packageName;
                        break;
                    }
                    if (waiting == null) waiting = r.info.packageName;
                }
                // Else other work waiting for a package's process (a service, a broadcast).
                for (PendingWork w : mPendingWork) {
                    if (waiting != null) break;
                    if (!starting(w.packageName)) waiting = w.packageName;
                }
            }
            if (waiting != null && processFor(waiting) == null) {
                assignPackage(app, waiting);
                for (ActivityRecord r : mActivities) {
                    if (r.app == null && r.state == State.INITIALIZING
                            && app.packageName.equals(r.info.packageName)) {
                        attach(r, app);
                    }
                }
                bind = true;
            }
        }
        try {
            thread.asBinder().linkToDeath(() -> processDied(app), 0);
        } catch (RemoteException e) {
            processDied(app);
            return;
        }
        SystemServer.log("attachApplication: " + app
                + (bind ? "" : ", waiting for an activity start"));
        if (bind) bindApplication(app);
    }

    /** IActivityManager.finishAttachApplication: the process bound its application. */
    void finishAttachApplication(int pid) {
        List<ActivityRecord> launch = new ArrayList<>();
        List<Consumer<ProcessRecord>> work = new ArrayList<>();
        ProcessRecord app;
        synchronized (this) {
            app = processByPid(pid);
            if (app == null) return;
            app.ready = true;
            for (ActivityRecord r : app.activities) {
                if (r.state == State.INITIALIZING && r.waitingFor == null) launch.add(r);
            }
            for (int i = 0; i < mPendingWork.size(); i++) {
                PendingWork w = mPendingWork.get(i);
                if (w.packageName.equals(app.packageName)) {
                    work.add(w.work);
                    mPendingWork.remove(i--);
                }
            }
        }
        SystemServer.log("finishAttachApplication: " + app);
        for (ActivityRecord r : launch) realStartActivity(r);
        for (Consumer<ProcessRecord> w : work) w.accept(app);
    }

    /**
     * Runs {@code work} with the process of {@code packageName} once it is attached and
     * bound: now if it is, else when it is (ActivityManagerService would start the
     * process; here it waits for one to attach).
     */
    void withProcess(String packageName, Consumer<ProcessRecord> work) {
        withProcess(packageName, "work for " + packageName, work);
    }

    /** {@link #withProcess(String, Consumer)}, starting a process if none runs. */
    void withProcess(String packageName, String reason, Consumer<ProcessRecord> work) {
        ProcessRecord app;
        synchronized (this) {
            app = processFor(packageName);
            if (app == null || !app.ready) {
                mPendingWork.add(new PendingWork(packageName, work));
                SystemServer.log("waiting for a process of " + packageName);
            }
        }
        if (app == null) {
            startProcess(packageName, reason);
        } else if (app.ready) {
            work.accept(app);
        }
    }

    /** The process with this pid (a caller), or null. */
    ProcessRecord callerProcess(int pid) {
        synchronized (this) {
            return processByPid(pid);
        }
    }

    private void bindApplication(ProcessRecord app) {
        ApplicationInfo ai = mPm.getApplicationInfo(app.packageName,
                PackageManager.GET_META_DATA | PackageManager.GET_SHARED_LIBRARY_FILES, 0);
        Configuration config = LocaleManagerService.configurationFor(app.packageName);
        synchronized (this) {
            app.config = config;
        }
        try {
            mAm.bindApplication(app.thread, ai, config);
            SystemServer.log("bindApplication: " + app);
        } catch (Throwable e) {
            // Reported to the caller (an exception crosses binder), not fatal to the server.
            SystemServer.log("bindApplication " + app + " failed: " + e);
            e.printStackTrace();
            throw new IllegalStateException("bindApplication failed in the system server: "
                    + e, e);
        }
    }

    private void processDied(ProcessRecord app) {
        synchronized (this) {
            if (!mProcesses.remove(app)) return;
            for (ActivityRecord r : app.activities) {
                r.state = State.DESTROYED;
                mActivities.remove(r);
            }
            updateIdle();
            notifyAll();
        }
        SystemServer.log(app + " died");
        mAm.processDied(app);
        if (app.spawned && app.packageName != null) {
            // Its exit code, once Windows has it (binder may tell first).
            Thread t = new Thread(() -> {
                int code = Zygote.STILL_ACTIVE;
                for (int i = 0; i < 40 && code == Zygote.STILL_ACTIVE; i++) {
                    code = Zygote.exitCode(app.pid);
                    if (code == Zygote.STILL_ACTIVE) SystemClock.sleep(250);
                }
                if (code != Zygote.STILL_ACTIVE) {
                    boolean published;
                    synchronized (this) {
                        published = mExitStatus.exited(app.packageName, app.startSeq, app.pid, code);
                    }
                    if (published) SystemServer.log(app + " exited with code " + code);
                }
            }, "exit-code " + app.pid);
            t.setDaemon(true);
            t.start();
        }
    }

    private ProcessRecord processByPid(int pid) {
        for (ProcessRecord p : mProcesses) {
            if (p.pid == pid && !p.exiting) return p;
        }
        return null;
    }

    // --- the client transactions -------------------------------------------------------

    /** ActivityTaskSupervisor.realStartActivityLocked: launch and resume. */
    private void realStartActivity(ActivityRecord r) {
        Configuration config = LocaleManagerService.configurationFor(r.info.packageName);
        Rect task;
        int rotation;
        synchronized (this) {
            task = taskBoundsLocked(r.taskId);
            rotation = mTaskRotation.get(r.taskId, Surface.ROTATION_0);
            r.config = effectiveConfiguration(config, task, rotation);
            r.configPending = false;
        }
        // A task the user resized, or turned for its orientation: the task's configuration
        // overrides the display's.
        Configuration override = taskOverride(task, rotation, config);
        if (rotation != Surface.ROTATION_0) {
            SystemServer.log(r.name() + " launches in task " + r.taskId + " turned to "
                    + TaskConfiguration.rotationName(rotation) + " (" + task.width() + "x"
                    + task.height() + ")");
        }
        ActivityWindowInfo windowInfo = windowInfo(task);
        List<ResultInfo> results = takeResults(r);
        ClientTransaction t = new ClientTransaction(r.app.thread);
        t.addTransactionItem(new LaunchActivityItem(r.token, r.intent,
                System.identityHashCode(r), r.info, config, override,
                Context.DEVICE_ID_DEFAULT, r.launchedFromPackage, null /* voiceInteractor */,
                ActivityManager.PROCESS_STATE_TOP, null /* state */, null /* persistentState */,
                results /* pendingResults */, null /* pendingNewIntents */,
                null /* sceneTransitionInfo */, true /* isForward */, null /* profilerInfo */,
                new Binder() /* assistToken */, mClient.asInterface(),
                new Binder() /* shareableActivityToken */, false /* launchedFromBubble */,
                null /* taskFragmentToken */, new Binder() /* initialCallerInfoAccessToken */,
                windowInfo, Display.DEFAULT_DISPLAY));
        t.addTransactionItem(new ResumeActivityItem(r.token, true /* isForward */,
                false /* shouldSendCompatFakeFocus */));
        setState(r, State.LAUNCHING);
        schedule(r, t, "LaunchActivityItem, ResumeActivityItem");
    }

    /** The ActivityWindowInfo of an activity in a task of {@code task} bounds (null: display). */
    private static ActivityWindowInfo windowInfo(Rect task) {
        Rect bounds = task != null ? new Rect(task)
                : new Rect(0, 0, DeviceProfile.WIDTH, DeviceProfile.HEIGHT);
        ActivityWindowInfo info = new ActivityWindowInfo();
        info.set(false /* isEmbedded */, bounds, bounds);
        return info;
    }

    /** The last sequence number given to an activity's override configuration. */
    private int mConfigSeq;

    /**
     * The override of a task {@code task} in size (null: the display's): its size
     * configuration (TaskConfiguration), with a sequence number newer than every one given
     * before and than the package's configuration {@code global}'s: the client drops an
     * override that is not newer than the one it has (a seq of 0 too), and a task's size
     * changes back and forth with the same package configuration.
     */
    private synchronized Configuration taskOverride(Rect task, int rotation,
            Configuration global) {
        Configuration override = task != null ? TaskConfiguration.override(task, rotation)
                : new Configuration();
        mConfigSeq = Math.max(mConfigSeq, global.seq) + 1;
        if (mConfigSeq == 0) mConfigSeq = 1;
        override.seq = mConfigSeq;
        return override;
    }

    /** What an activity sees: its package's configuration with its task's override. */
    private static Configuration effectiveConfiguration(Configuration global, Rect task,
            int rotation) {
        Configuration c = new Configuration(global);
        if (task != null) c.updateFrom(TaskConfiguration.override(task, rotation));
        return c;
    }

    /**
     * The task's bounds: the size the user gave its window, else the display turned to the
     * task's rotation; null for the display as it is. Guarded by this.
     */
    private Rect taskBoundsLocked(int taskId) {
        Rect user = mTaskBounds.get(taskId);
        if (user != null) return user;
        int rotation = mTaskRotation.get(taskId, Surface.ROTATION_0);
        return rotation == Surface.ROTATION_0 ? null : TaskConfiguration.display(rotation);
    }

    /**
     * DisplayRotation.updateOrientation for a task (windows3 track): the orientation its top
     * activity asks for (one asking for {@code behind}, and a translucent one asking for
     * nothing, leave it to the one below, as TaskFragment.getOrientation does) turns the
     * task's display (TaskConfiguration.rotationFor). Returns whether the rotation changed.
     * Guarded by this.
     */
    private boolean updateRotationLocked(int taskId) {
        int requested = ActivityInfo.SCREEN_ORIENTATION_UNSPECIFIED;
        for (int i = mActivities.size() - 1; i >= 0; i--) {
            ActivityRecord a = mActivities.get(i);
            if (a.taskId != taskId || a.finishing || a.state == State.DESTROYED) continue;
            int o = a.requestedOrientation;
            if (o == ActivityInfo.SCREEN_ORIENTATION_BEHIND) continue;
            if (o == ActivityInfo.SCREEN_ORIENTATION_UNSPECIFIED && a.translucent) continue;
            requested = o;
            break;
        }
        int current = mTaskRotation.get(taskId, Surface.ROTATION_0);
        int rotation = TaskConfiguration.rotationFor(requested, current);
        if (rotation == current) return false;
        // The window turns to the display's new size: the size the user gave it goes.
        mTaskBounds.remove(taskId);
        if (rotation == Surface.ROTATION_0) {
            mTaskRotation.delete(taskId);
        } else {
            mTaskRotation.put(taskId, rotation);
        }
        SystemServer.log("task " + taskId + " turned from " + TaskConfiguration.rotationName(current)
                + " to " + TaskConfiguration.rotationName(rotation) + " for orientation "
                + ActivityInfo.screenOrientationToString(requested));
        return true;
    }

    /**
     * ActivityClientController.setRequestedOrientation (Activity.setRequestedOrientation): the
     * activity's orientation request; when it turns its task's display, the task's activities
     * get the configuration (told, or relaunched: now if resumed, else when they are).
     */
    void setRequestedOrientation(ActivityRecord r, int orientation) {
        List<ActivityRecord> reconfigure = new ArrayList<>();
        synchronized (this) {
            if (r.requestedOrientation == orientation) return;
            r.requestedOrientation = orientation;
            if (!updateRotationLocked(r.taskId)) {
                SystemServer.log(r.name() + " asks for orientation "
                        + ActivityInfo.screenOrientationToString(orientation)
                        + ": task " + r.taskId + " stays as it is");
                return;
            }
            for (ActivityRecord a : mActivities) {
                if (a.taskId == r.taskId && !a.finishing && a.app != null) reconfigure.add(a);
            }
        }
        for (ActivityRecord a : reconfigure) ensureConfiguration(a);
    }

    /** ActivityClientController.getRequestedOrientation. */
    synchronized int getRequestedOrientation(ActivityRecord r) {
        return r.requestedOrientation;
    }

    // --- multi-resume (windows3 track) ------------------------------------------------------

    /** The task came to the front (started in, moved to the front). Guarded by this. */
    private void focusTaskLocked(int taskId) {
        mTaskOrder.remove(Integer.valueOf(taskId));
        mTaskOrder.add(taskId);
    }

    /**
     * The activity that should be the top resumed one (ActivityTaskSupervisor's
     * mTopResumedActivity): the top of the task that came to the front last and is not at the
     * back, if it is in a process. Guarded by this.
     */
    private ActivityRecord topResumedCandidateLocked() {
        for (int i = mTaskOrder.size() - 1; i >= 0; i--) {
            int taskId = mTaskOrder.get(i);
            if (mHiddenTasks.contains(taskId)) continue;
            ActivityRecord top = topOf(taskId);
            if (top == null) continue;
            return top.app != null ? top : null;
        }
        return null;
    }

    /**
     * ActivityTaskSupervisor.updateTopResumedActivityIfNeeded: the activity that was the top
     * resumed one is told it no longer is (TopResumedActivityChangeItem false), the new one
     * that it is, once it is resumed (the app reports onTopResumedActivityChanged when it
     * resumes, and false again whenever it pauses). Other visible tasks' top activities stay
     * resumed (multi-resume).
     */
    private void updateTopResumed() {
        ActivityRecord lose = null;
        ActivityRecord gain = null;
        synchronized (this) {
            ActivityRecord want = topResumedCandidateLocked();
            if (mTopResumed != null && mTopResumed != want) {
                if (mTopResumed.topSent && mTopResumed.app != null
                        && mTopResumed.state != State.DESTROYED) {
                    lose = mTopResumed;
                }
                mTopResumed.topSent = false;
            }
            mTopResumed = want;
            if (want != null && !want.topSent && want.state == State.RESUMED) {
                want.topSent = true;
                gain = want;
            }
        }
        if (lose != null) {
            ClientTransaction t = new ClientTransaction(lose.app.thread);
            t.addTransactionItem(new TopResumedActivityChangeItem(lose.token, false));
            schedule(lose, t, "TopResumedActivityChangeItem (no longer the top resumed activity)");
        }
        if (gain != null) {
            ClientTransaction t = new ClientTransaction(gain.app.thread);
            t.addTransactionItem(new TopResumedActivityChangeItem(gain.token, true));
            schedule(gain, t, "TopResumedActivityChangeItem (the top resumed activity)");
        }
    }

    /**
     * IActivityTaskManager.resizeTask (the user resized the task's window: its window
     * session tells): the task's configuration is that of its size (TaskConfiguration) over
     * its package's. Each of its activities gets it as for any configuration change
     * ({@link #ensureConfiguration}: told if it handles the changes, else relaunched; now if
     * it is resumed, else when it is). The changes it handles also reach it through its
     * window (ViewRootImpl's activity config callback).
     */
    @Override
    public void resizeTask(int taskId, Rect bounds, int resizeMode) {
        List<ActivityRecord> activities = new ArrayList<>();
        Rect task;
        synchronized (this) {
            // The display's size (turned as the task is) is no size of the user's.
            Rect display = TaskConfiguration.display(mTaskRotation.get(taskId, Surface.ROTATION_0));
            task = bounds == null || bounds.isEmpty()
                    || (bounds.width() == display.width() && bounds.height() == display.height())
                    ? null : new Rect(0, 0, bounds.width(), bounds.height());
            Rect old = mTaskBounds.get(taskId);
            if (old == null ? task == null : old.equals(task)) return;
            if (task != null) {
                mTaskBounds.put(taskId, task);
            } else {
                mTaskBounds.remove(taskId);
            }
            for (ActivityRecord a : mActivities) {
                if (a.taskId == taskId && !a.finishing && a.app != null) activities.add(a);
            }
        }
        SystemServer.log("task " + taskId + " resized to "
                + (task != null ? task.width() + "x" + task.height() : "the display"));
        for (ActivityRecord a : activities) ensureConfiguration(a);
    }

    /** reportSizeConfigurations: the size buckets the activity's resources care about. */
    void reportSizeConfigurations(ActivityRecord r,
            android.window.SizeConfigurationBuckets buckets) {
        synchronized (this) {
            r.sizeBuckets = buckets;
        }
    }

    private List<ResultInfo> takeResults(ActivityRecord r) {
        synchronized (this) {
            if (r.pendingResults.isEmpty()) return null;
            List<ResultInfo> results = new ArrayList<>(r.pendingResults);
            r.pendingResults.clear();
            return results;
        }
    }

    /**
     * Resumes an activity that was paused or stopped, with the results waiting for it
     * (ActivityResultItem, delivered before onResume), as TaskFragment.resumeTopActivity.
     */
    private void resume(ActivityRecord r) {
        List<ResultInfo> results = takeResults(r);
        ClientTransaction t = new ClientTransaction(r.app.thread);
        // The configuration changed while it was not in front (ActivityRecord's
        // ensureActivityConfiguration as it becomes visible): relaunched with its results,
        // or told, before it resumes.
        String config = configurationItem(r, t, results);
        boolean relaunch = config != null && config.startsWith("ActivityRelaunchItem");
        if (results != null && !relaunch) {
            t.addTransactionItem(new ActivityResultItem(r.token, results));
        }
        t.addTransactionItem(new ResumeActivityItem(r.token, false /* isForward */,
                false /* shouldSendCompatFakeFocus */));
        synchronized (this) {
            r.stopWhenIdle = false;
            r.idle = false;
        }
        setState(r, State.RESUMING);
        schedule(r, t, (config != null ? config + ", " : "")
                + (results != null && !relaunch ? "ActivityResultItem, " : "")
                + "ResumeActivityItem");
    }

    /** ActivityRecord.deliverNewIntentLocked: onNewIntent, around a pause if resumed. */
    private void deliverNewIntent(ActivityRecord r, Intent intent, String referrer) {
        boolean resumed;
        synchronized (this) {
            resumed = r.state == State.RESUMED || r.state == State.LAUNCHING
                    || r.state == State.RESUMING;
        }
        List<ReferrerIntent> intents = new ArrayList<>();
        intents.add(new ReferrerIntent(intent, referrer));
        ClientTransaction t = new ClientTransaction(r.app.thread);
        t.addTransactionItem(new NewIntentItem(r.token, intents, resumed));
        schedule(r, t, "NewIntentItem");
    }

    /** Pauses an activity (PauseActivityItem): finishing, or for another in front. */
    private void pause(ActivityRecord r, boolean finishing) {
        synchronized (this) {
            if (!finishing) r.stopWhenIdle = true;
        }
        setState(r, State.PAUSING);
        ClientTransaction t = new ClientTransaction(r.app.thread);
        t.addTransactionItem(new PauseActivityItem(r.token, finishing /* finished */,
                !finishing /* userLeaving */, false /* dontReport */,
                false /* autoEnteringPip */));
        schedule(r, t, "PauseActivityItem");
    }

    /**
     * ActivityRecord.finishIfPossible: a result for the activity that asked for one, then
     * pause, stop and destroy.
     */
    boolean finish(ActivityRecord r, String reason, int resultCode, Intent resultData) {
        State state;
        synchronized (this) {
            if (r.finishing) return true;
            r.finishing = true;
            state = r.state;
            // The activity below may ask for another orientation (it gets the configuration
            // as it resumes).
            updateRotationLocked(r.taskId);
            if (r.resultTo != null && r.resultTo.state != State.DESTROYED
                    && !r.resultTo.finishing) {
                r.resultTo.pendingResults.add(new ResultInfo(r.resultWho, r.requestCode,
                        resultCode, resultData));
            }
        }
        SystemServer.log("finishing " + r.name() + " (" + reason + ")"
                + (r.resultTo != null ? ", result " + resultCode + " to " + r.resultTo.name()
                        : ""));
        updateTopResumed();
        switch (state) {
            case INITIALIZING:
                remove(r);
                return true;
            case LAUNCHING:
            case RESUMING:
            case RESUMED:
                pause(r, true);
                return true;
            case PAUSED:
            case STARTED:
                stop(r);
                return true;
            case STOPPED:
                destroy(r);
                return true;
            default:
                return true;  // on its way
        }
    }

    /**
     * ActivityRecord.navigateUpTo, lite (NavUtils.navigateUpFromSameTask): the activities
     * of the caller's task above the up destination finish. When the destination is not in
     * the task (startActivities records only the top activity), it is started in the task
     * first, as if it had been below, and the caller finishes (exec track, docs/exec.md:
     * Simon Tatham's Puzzles goes up to its chooser on a first launch).
     */
    boolean navigateUpTo(ActivityRecord r, Intent destIntent, String resolvedType,
            int resultCode, Intent resultData) {
        ComponentName dest = destIntent.getComponent();
        if (dest == null) {
            ActivityInfo info = resolve(destIntent, resolvedType, true);
            if (info == null) return false;
            dest = new ComponentName(info.packageName, info.name);
        }
        List<ActivityRecord> above = new ArrayList<>();
        ActivityRecord parent = null;
        synchronized (this) {
            for (int i = mActivities.indexOf(r); i >= 0; i--) {
                ActivityRecord a = mActivities.get(i);
                if (a.taskId != r.taskId || a.finishing || a.state == State.DESTROYED) continue;
                if (a != r && a.component().equals(dest)) {
                    parent = a;
                    break;
                }
                above.add(a);
            }
        }
        SystemServer.log("navigateUpTo " + dest.flattenToShortString() + " from " + r.name()
                + (parent != null ? ": back to it in task " + r.taskId
                        : ": not in task " + r.taskId + ", starting it there"));
        if (parent == null) {
            Intent up = new Intent(destIntent);
            up.setComponent(dest);
            up.removeFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
            ActivityRecord[] started = new ActivityRecord[1];
            int result = start(r.app != null ? r.app.thread : null, up, resolvedType,
                    r.info.packageName, r.token, null, -1, started);
            if (result != ActivityManager.START_SUCCESS) return false;
        }
        for (ActivityRecord a : above) {
            finish(a, "navigateUpTo", a == r ? resultCode : 0 /* RESULT_CANCELED */,
                    a == r ? resultData : null);
        }
        return true;
    }

    private void stop(ActivityRecord r) {
        setState(r, State.STOPPING);
        ClientTransaction t = new ClientTransaction(r.app.thread);
        t.addTransactionItem(new StopActivityItem(r.token));
        schedule(r, t, "StopActivityItem");
    }

    private void destroy(ActivityRecord r) {
        setState(r, State.DESTROYING);
        ClientTransaction t = new ClientTransaction(r.app.thread);
        t.addTransactionItem(new DestroyActivityItem(r.token, r.finishing));
        schedule(r, t, "DestroyActivityItem");
    }

    private void schedule(ActivityRecord r, ClientTransaction t, String what) {
        SystemServer.log("scheduleTransaction " + what + " to " + r.name() + " in " + r.app);
        try {
            r.app.thread.scheduleTransaction(t);
        } catch (RemoteException e) {
            SystemServer.log("scheduleTransaction to " + r.app + " failed: " + e);
        }
    }

    private void setState(ActivityRecord r, State state) {
        synchronized (this) {
            r.state = state;
            notifyAll();
        }
    }

    private void remove(ActivityRecord r) {
        ProcessRecord check = null;
        synchronized (this) {
            r.state = State.DESTROYED;
            mActivities.remove(r);
            for (ActivityRecord a : mActivities) {
                if (a.waitingFor == r) a.waitingFor = null;
            }
            if (r.app != null) {
                r.app.activities.remove(r);
                // ActivityManagerService kills the process of a removed task; here the
                // process is told to exit, unless something else keeps it.
                if (r.app.activities.isEmpty() && r.finishing) check = r.app;
            }
            if (r == mTopResumed) mTopResumed = null;
            if (topOf(r.taskId) == null) {
                mTaskOrder.remove(Integer.valueOf(r.taskId));
                mTaskRotation.delete(r.taskId);
            }
            notifyAll();
        }
        updateTopResumed();
        maybeExit(check);
    }

    /** The record of a token, or null for a token this server did not hand out. */
    ActivityRecord forToken(IBinder token) {
        synchronized (this) {
            for (ActivityRecord r : mActivities) {
                if (r.token == token) return r;
            }
        }
        return null;
    }

    /** Whether the activity is the top of its task (and so visible once resumed). */
    boolean isTopOfTask(ActivityRecord r) {
        synchronized (this) {
            return topOf(r.taskId) == r;
        }
    }

    // --- reports from the app (ActivityClientController) --------------------------------

    void activityResumed(ActivityRecord r) {
        List<ActivityRecord> toStop;
        List<ActivityRecord> toStart;
        synchronized (this) {
            // A pause or a finish may already be on its way (startActivity or finish() in
            // onResume).
            if (r.state == State.LAUNCHING || r.state == State.RESUMING) r.state = State.RESUMED;
            toStop = stopping(r);
            toStart = starting();
            notifyAll();
        }
        SystemServer.log("activityResumed: " + r.name());
        for (ActivityRecord a : toStop) stop(a);
        for (ActivityRecord a : toStart) makeVisible(a);
        boolean pending;
        synchronized (this) {
            // The configuration changed (a locale change, its task resized) while it was on
            // its way to the front.
            pending = r.configPending && r.state == State.RESUMED;
        }
        if (pending) ensureConfiguration(r);
        updateTopResumed();
    }

    void activityIdle(ActivityRecord r) {
        List<ActivityRecord> toStop;
        synchronized (this) {
            r.idle = true;
            toStop = stopping(r);
            notifyAll();
        }
        SystemServer.log("activityIdle: " + r.name());
        for (ActivityRecord a : toStop) stop(a);
    }

    /**
     * ActivityTaskSupervisor.processStoppingAndFinishingActivities: the activities paused
     * for another one are stopped once an activity in front of them reports it is resumed
     * (it is visible) or idle, if they are no longer visible ({@link #isVisible}: an
     * activity under a translucent or floating one stays paused, visible). Guarded by this.
     */
    private List<ActivityRecord> stopping(ActivityRecord reporting) {
        List<ActivityRecord> toStop = new ArrayList<>();
        for (ActivityRecord a : mActivities) {
            if (a != reporting && a.stopWhenIdle && a.state == State.PAUSED && !a.finishing
                    && !isVisible(a)) {
                a.stopWhenIdle = false;
                toStop.add(a);
            }
        }
        return toStop;
    }

    /**
     * EnsureActivitiesVisibleHelper, lite: whether an activity is visible. A task moved to
     * the back (its window minimized) shows nothing; in a task, an activity is visible
     * unless one above it occludes it (one that is not translucent or floating:
     * ActivityRecord.occludesParent). Tasks are windows of their own on the Windows
     * desktop (Android's desktop windowing), so another task never hides this one's
     * activities. Guarded by this.
     */
    private boolean isVisible(ActivityRecord a) {
        if (a.finishing || a.state == State.DESTROYED || a.state == State.DESTROYING) return false;
        if (mHiddenTasks.contains(a.taskId)) return false;
        for (int i = mActivities.size() - 1; i >= 0; i--) {
            ActivityRecord above = mActivities.get(i);
            if (above == a) return true;
            if (above.taskId != a.taskId || above.finishing || above.state == State.DESTROYED) {
                continue;
            }
            if (!above.translucent) return false;
        }
        return true;
    }

    /** Whether the activity is visible (willActivityBeVisible). */
    synchronized boolean willBeVisible(ActivityRecord a) {
        return isVisible(a);
    }

    /**
     * The stopped activities that are visible again (the one above them finished, went
     * translucent, or their task came to the front): to be started, not resumed. Guarded
     * by this.
     */
    private List<ActivityRecord> starting() {
        List<ActivityRecord> toStart = new ArrayList<>();
        for (ActivityRecord a : mActivities) {
            if (a.state == State.STOPPED && a.app != null && a.app.ready && !a.finishing
                    && isVisible(a)) {
                toStart.add(a);
            }
        }
        return toStart;
    }

    /**
     * ActivityRecord.makeActiveIfNeeded for an activity that is visible but not in front:
     * StartActivityItem (onRestart, onStart; its window shows again), not resumed.
     */
    private void makeVisible(ActivityRecord r) {
        synchronized (this) {
            if (r.state != State.STOPPED || r.finishing) return;
            r.state = State.STARTED;
            notifyAll();
        }
        // A configuration change that came meanwhile waits until it is resumed (configPending).
        ClientTransaction t = new ClientTransaction(r.app.thread);
        t.addTransactionItem(new StartActivityItem(r.token, null /* sceneTransitionInfo */));
        schedule(r, t, "StartActivityItem (visible, not resumed)");
    }

    /**
     * ActivityClientController.moveActivityTaskToBack (the user minimized the task's window,
     * or the activity asked: Activity.moveTaskToBack): the task is no longer visible. Its
     * resumed activity is paused, then stopped; the other visible ones are stopped (each
     * saves its state, ActivityThread's handleStopActivity).
     */
    boolean moveTaskToBack(ActivityRecord r, boolean nonRoot) {
        List<ActivityRecord> toPause = new ArrayList<>();
        List<ActivityRecord> toStop = new ArrayList<>();
        int taskId = r.taskId;
        synchronized (this) {
            if (!nonRoot) {
                // Only the task's root activity may move it back (Android's rule).
                ActivityRecord root = null;
                for (ActivityRecord a : mActivities) {
                    if (a.taskId == taskId && !a.finishing) {
                        root = a;
                        break;
                    }
                }
                if (root != r) return false;
            }
            if (!mHiddenTasks.add(taskId)) return true;
            for (ActivityRecord a : mActivities) {
                if (a.taskId != taskId || a.finishing || a.app == null) continue;
                switch (a.state) {
                    case LAUNCHING:
                    case RESUMING:
                    case RESUMED:
                        toPause.add(a);
                        break;
                    case PAUSED:
                    case STARTED:
                        toStop.add(a);
                        break;
                    default:
                        break;
                }
            }
        }
        SystemServer.log("moving task " + taskId + " to the back: " + toPause.size()
                + " to pause, " + toStop.size() + " to stop");
        // The top resumed activity is another visible task's now, if any.
        updateTopResumed();
        for (ActivityRecord a : toPause) pause(a, false);
        for (ActivityRecord a : toStop) stop(a);
        return true;
    }

    /**
     * IActivityTaskManager.moveTaskToFront (the user restored or activated the task's
     * window): the task is visible again and the focused one; its top activity is resumed
     * if it was not (other tasks' top activities stay resumed: multi-resume), the ones
     * visible under it started, and it becomes the top resumed activity.
     */
    @Override
    public void moveTaskToFront(IApplicationThread app, String callingPackage, int taskId,
            int flags, Bundle options) {
        ActivityRecord top;
        boolean resumeNow = false;
        List<ActivityRecord> toStart;
        boolean wasHidden;
        boolean refocused;
        synchronized (this) {
            wasHidden = mHiddenTasks.remove(taskId);
            top = topOf(taskId);
            if (top == null || top.app == null || !top.app.ready) return;
            refocused = mTaskOrder.isEmpty() || mTaskOrder.get(mTaskOrder.size() - 1) != taskId;
            focusTaskLocked(taskId);
            switch (top.state) {
                case PAUSED:
                case STARTED:
                case STOPPING:
                case STOPPED:
                    resumeNow = true;
                    break;
                default:
                    // Resumed, or on its way to it (or being paused in its own task).
                    if (!wasHidden && !refocused) return;  // in front already
                    top = null;
            }
            toStart = starting();
            if (top != null) toStart.remove(top);
        }
        SystemServer.log("moving task " + taskId + " to the front"
                + (top != null ? ": resuming " + top.name() : ""));
        if (resumeNow) resume(top);
        for (ActivityRecord a : toStart) makeVisible(a);
        updateTopResumed();
    }

    /**
     * Activity.setTranslucent (convertToTranslucent / convertFromTranslucent): whether the
     * activity occludes the ones below it changes; those now visible are started, those
     * now hidden stopped.
     */
    boolean setTranslucent(ActivityRecord r, boolean translucent) {
        List<ActivityRecord> toStop = new ArrayList<>();
        List<ActivityRecord> toStart;
        synchronized (this) {
            if (r.translucent == translucent) return true;
            r.translucent = translucent;
            for (ActivityRecord a : mActivities) {
                if (a != r && (a.state == State.PAUSED || a.state == State.STARTED)
                        && !a.finishing && a.app != null && !isVisible(a)) {
                    a.stopWhenIdle = false;
                    toStop.add(a);
                }
            }
            toStart = starting();
        }
        SystemServer.log(r.name() + (translucent ? " converted to translucent"
                : " converted from translucent"));
        for (ActivityRecord a : toStop) stop(a);
        for (ActivityRecord a : toStart) makeVisible(a);
        return true;
    }

    void activityPaused(ActivityRecord r) {
        List<ActivityRecord> launch = new ArrayList<>();
        List<ActivityRecord> resumeAfter = new ArrayList<>();
        ActivityRecord resume = null;
        boolean finishing;
        boolean stopNow;
        synchronized (this) {
            r.state = State.PAUSED;
            finishing = r.finishing;
            // Paused as its task went to the back: stopped now (nothing in front of it will
            // report idle).
            stopNow = !finishing && mHiddenTasks.contains(r.taskId);
            if (stopNow) r.stopWhenIdle = false;
            for (ActivityRecord a : mActivities) {
                if (a.waitingFor == r) {
                    a.waitingFor = null;
                    if (a.state == State.INITIALIZING && a.app != null && a.app.ready) {
                        launch.add(a);
                    } else if (a.resumeAfterPause) {
                        a.resumeAfterPause = false;
                        if (!a.finishing && a.app != null) resumeAfter.add(a);
                    }
                }
            }
            if (launch.isEmpty() && finishing) {
                // TaskFragment.resumeTopActivity: the activity under it in the task, else
                // the most recent one of another task.
                resume = topOf(r.taskId);
                if (resume == null) {
                    // Another visible task's top (with multi-resume, usually resumed already).
                    ActivityRecord next = topResumedCandidateLocked();
                    if (next != null && next.taskId != r.taskId) resume = next;
                }
                if (resume != null && resume.app == null) resume = null;
                if (resume != null) {
                    switch (resume.state) {
                        case PAUSED:
                        case STARTED:
                        case STOPPING:
                        case STOPPED:
                            break;
                        default:
                            resume = null;  // already on its way to the front
                    }
                }
            }
            notifyAll();
        }
        SystemServer.log("activityPaused: " + r.name());
        for (ActivityRecord a : launch) realStartActivity(a);
        for (ActivityRecord a : resumeAfter) resume(a);
        if (resume != null) resume(resume);
        if (finishing || stopNow) stop(r);
    }

    void activityStopped(ActivityRecord r, Bundle savedState) {
        boolean destroy;
        synchronized (this) {
            r.savedState = savedState;
            // A resume may have been scheduled after the stop (the activity in front
            // finished first): it stays on its way to the front.
            if (r.state == State.STOPPING) r.state = State.STOPPED;
            destroy = r.finishing;
            notifyAll();
        }
        SystemServer.log("activityStopped: " + r.name());
        if (destroy) destroy(r);
    }

    void activityDestroyed(ActivityRecord r) {
        SystemServer.log("activityDestroyed: " + r.name());
        remove(r);
    }

    // --- configuration changes (locale track, ADR 0049) --------------------------------

    /**
     * The configuration changed (Windows' languages or formats; a package's app locales):
     * each running process of {@code packageName} (null: every package) gets the new one
     * (ConfigurationChangeItem: its resources, default locales and non-UI components'
     * onConfigurationChanged), then each of its activities is relaunched, or told
     * (ActivityConfigurationChangeItem: onConfigurationChanged) when it handles every change
     * ({@code android:configChanges}), as WindowProcessController.dispatchConfiguration and
     * ActivityRecord.ensureActivityConfiguration do. An activity that is not resumed is
     * relaunched or told when it is.
     */
    void dispatchConfiguration(String packageName) {
        List<ProcessRecord> processes = new ArrayList<>();
        synchronized (this) {
            for (ProcessRecord p : mProcesses) {
                if (p.exiting || !p.ready || p.packageName == null || p.config == null) continue;
                if (packageName == null || packageName.equals(p.packageName)) processes.add(p);
            }
        }
        for (ProcessRecord p : processes) {
            Configuration config = LocaleManagerService.configurationFor(p.packageName);
            List<ActivityRecord> activities;
            int diff;
            synchronized (this) {
                diff = p.config.diff(config);
                p.config = config;
                activities = new ArrayList<>(p.activities);
            }
            if (diff != 0) {
                ClientTransaction t = new ClientTransaction(p.thread);
                t.addTransactionItem(new ConfigurationChangeItem(config,
                        Context.DEVICE_ID_DEFAULT));
                SystemServer.log("scheduleTransaction ConfigurationChangeItem to " + p + ": "
                        + config.getLocales().toLanguageTags() + " (changes 0x"
                        + Integer.toHexString(diff) + ", seq " + config.seq + ")");
                try {
                    p.thread.scheduleTransaction(t);
                } catch (RemoteException e) {
                    SystemServer.log("scheduleTransaction to " + p + " failed: " + e);
                }
            }
            for (ActivityRecord r : activities) ensureConfiguration(r);
        }
    }

    /** ActivityRecord.ensureActivityConfiguration: now if resumed, else when it is. */
    private void ensureConfiguration(ActivityRecord r) {
        synchronized (this) {
            if (r.config == null || r.app == null || r.finishing) return;
            if (r.state != State.RESUMED) {
                r.configPending = true;
                return;
            }
        }
        ClientTransaction t = new ClientTransaction(r.app.thread);
        String what = configurationItem(r, t, null);
        if (what == null) return;
        if (what.startsWith("ActivityRelaunchItem")) {
            t.addTransactionItem(new ResumeActivityItem(r.token, false /* isForward */,
                    false /* shouldSendCompatFakeFocus */));
            synchronized (this) {
                r.idle = false;
            }
            setState(r, State.RESUMING);
            what += ", ResumeActivityItem";
        }
        schedule(r, t, what);
    }

    /**
     * Adds what the activity needs for the package's current configuration to {@code t}:
     * nothing (null), an ActivityConfigurationChangeItem when it handles every change, else
     * an ActivityRelaunchItem (with {@code results}); returns its description.
     */
    private String configurationItem(ActivityRecord r, ClientTransaction t,
            List<ResultInfo> results) {
        // One configuration for both tracks: the package's (locales, formats: locale track)
        // with the task's size override (a resized window: lifecycle track).
        Configuration global = LocaleManagerService.configurationFor(r.info.packageName);
        Configuration config;
        Rect task;
        int rotation;
        int diff;
        synchronized (this) {
            if (r.config == null) return null;
            task = taskBoundsLocked(r.taskId);
            rotation = mTaskRotation.get(r.taskId, Surface.ROTATION_0);
            config = effectiveConfiguration(global, task, rotation);
            // Size changes within the buckets its resources care about do not count.
            diff = android.window.SizeConfigurationBuckets.filterDiff(
                    r.config.diffPublicOnly(config), r.config, config, r.sizeBuckets);
            r.config = config;
            r.configPending = false;
        }
        if (diff == 0) return null;
        Configuration override = taskOverride(task, rotation, global);
        ActivityWindowInfo windowInfo = windowInfo(task);
        int handled = r.info.getRealConfigChanged();
        String changes = "changes 0x" + Integer.toHexString(diff) + ", handles 0x"
                + Integer.toHexString(handled) + ", " + config.getLocales().toLanguageTags()
                + ", " + config.screenWidthDp + "x" + config.screenHeightDp + " dp"
                + (rotation != Surface.ROTATION_0 ? ", " + TaskConfiguration.rotationName(rotation)
                        : "");
        if ((diff & ~handled) == 0) {
            t.addTransactionItem(new ActivityConfigurationChangeItem(r.token, override,
                    windowInfo, Display.DEFAULT_DISPLAY));
            return "ActivityConfigurationChangeItem (" + changes + ")";
        }
        t.addTransactionItem(new ActivityRelaunchItem(r.token, results,
                null /* pendingNewIntents */, diff,
                new MergedConfiguration(global, override),
                true /* preserveWindow */, windowInfo, Display.DEFAULT_DISPLAY));
        return "ActivityRelaunchItem (" + changes + ")";
    }

    static int taskOf(ActivityRecord r) {
        return r != null ? r.taskId : ActivityTaskManager.INVALID_TASK_ID;
    }
}
