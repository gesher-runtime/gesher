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
import android.app.servertransaction.ActivityResultItem;
import android.app.servertransaction.ClientTransaction;
import android.app.servertransaction.DestroyActivityItem;
import android.app.servertransaction.LaunchActivityItem;
import android.app.servertransaction.NewIntentItem;
import android.app.servertransaction.PauseActivityItem;
import android.app.servertransaction.ResumeActivityItem;
import android.app.servertransaction.StopActivityItem;
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
import android.view.Display;
import android.window.ActivityWindowInfo;
import android.window.IWindowContainerToken;
import android.window.WindowContainerToken;

import com.android.internal.content.ReferrerIntent;
import com.gesher.app.DeviceProfile;

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

    enum State { INITIALIZING, LAUNCHING, RESUMING, RESUMED, PAUSING, PAUSED, STOPPING, STOPPED,
            DESTROYING, DESTROYED }

    /** An app process attached to the server. */
    final class ProcessRecord {
        final int pid;
        final IApplicationThread thread;
        String packageName;  // null until a start binds it to a package
        boolean ready;       // finishAttachApplication came
        boolean exiting;
        boolean spawned;     // started by this server (startProcess), not by itself
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
    /** Processes started for a package and not attached yet, by start sequence number. */
    private final Map<Long, PendingProcess> mStarting = new HashMap<>();
    private long mNextStartSeq = 1;
    /** How app processes are started; null: they start by themselves and attach. */
    private volatile ProcessStarter mStarter;
    /** The exit code of each package's last process, once it is known. */
    private final Map<String, Integer> mLastExit = new HashMap<>();
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
                t.configuration.setTo(DeviceProfile.configuration());
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
        ActivityRecord r = null;
        ActivityRecord deliverTo = null;
        ActivityRecord pause = null;
        ProcessRecord bind = null;
        boolean launchNow = false;
        boolean needProcess = false;
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
                // The activity in front (one activity is resumed at a time) is paused first.
                ActivityRecord front = front(r);
                mActivities.add(r);
                ProcessRecord app = processFor(info.packageName);
                if (app == null) {
                    app = unassignedProcess(callingPid);
                    if (app != null) {
                        app.packageName = info.packageName;
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
                + (r.resultTo != null ? ", result to " + r.resultTo.name() + " (request "
                        + r.requestCode + ")" : ""));
        if (bind != null) bindApplication(bind);
        if (needProcess) startProcess(info.packageName, "activity " + r.name());
        if (pause != null) pause(pause, false);
        if (launchNow) realStartActivity(r);
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
     * The activity in front of the user, other than {@code except}: resumed or on its way
     * to it, or being paused. Guarded by this.
     */
    private ActivityRecord front(ActivityRecord except) {
        for (int i = mActivities.size() - 1; i >= 0; i--) {
            ActivityRecord a = mActivities.get(i);
            if (a == except || a.app == null) continue;
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
        }
        SystemServer.log("starting a process for " + packageName + " (" + reason + "): pid "
                + pid + ", start seq " + seq);
        // A process that dies before it attaches fails what waited for it.
        Thread watch = new Thread(() -> {
            while (true) {
                synchronized (this) {
                    if (mStarting.get(seq) != pending) return;  // attached
                }
                int code = Zygote.exitCode(pid);
                if (code != Zygote.STILL_ACTIVE) {
                    SystemServer.log("process " + pid + " for " + packageName
                            + " exited before attaching (exit code " + code + ")");
                    synchronized (this) {
                        mLastExit.put(packageName, code);
                    }
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
        return mLastExit.get(packageName);
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
                app.packageName = waiting;
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
        try {
            mAm.bindApplication(app.thread, ai);
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
                    synchronized (this) {
                        mLastExit.put(app.packageName, code);
                    }
                    SystemServer.log(app + " exited with code " + code);
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
        Configuration config = DeviceProfile.configuration();
        Rect bounds = new Rect(0, 0, DeviceProfile.WIDTH, DeviceProfile.HEIGHT);
        ActivityWindowInfo windowInfo = new ActivityWindowInfo();
        windowInfo.set(false /* isEmbedded */, bounds, bounds);
        List<ResultInfo> results = takeResults(r);
        ClientTransaction t = new ClientTransaction(r.app.thread);
        t.addTransactionItem(new LaunchActivityItem(r.token, r.intent,
                System.identityHashCode(r), r.info, config, new Configuration(),
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
        if (results != null) t.addTransactionItem(new ActivityResultItem(r.token, results));
        t.addTransactionItem(new ResumeActivityItem(r.token, false /* isForward */,
                false /* shouldSendCompatFakeFocus */));
        synchronized (this) {
            r.stopWhenIdle = false;
            r.idle = false;
        }
        setState(r, State.RESUMING);
        schedule(r, t, (results != null ? "ActivityResultItem, " : "") + "ResumeActivityItem");
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
            if (r.resultTo != null && r.resultTo.state != State.DESTROYED
                    && !r.resultTo.finishing) {
                r.resultTo.pendingResults.add(new ResultInfo(r.resultWho, r.requestCode,
                        resultCode, resultData));
            }
        }
        SystemServer.log("finishing " + r.name() + " (" + reason + ")"
                + (r.resultTo != null ? ", result " + resultCode + " to " + r.resultTo.name()
                        : ""));
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
                stop(r);
                return true;
            case STOPPED:
                destroy(r);
                return true;
            default:
                return true;  // on its way
        }
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
            notifyAll();
        }
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
        synchronized (this) {
            // A pause or a finish may already be on its way (startActivity or finish() in
            // onResume).
            if (r.state == State.LAUNCHING || r.state == State.RESUMING) r.state = State.RESUMED;
            toStop = stopping(r);
            notifyAll();
        }
        SystemServer.log("activityResumed: " + r.name());
        for (ActivityRecord a : toStop) stop(a);
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
     * (it is visible) or idle. Guarded by this.
     */
    private List<ActivityRecord> stopping(ActivityRecord reporting) {
        List<ActivityRecord> toStop = new ArrayList<>();
        for (ActivityRecord a : mActivities) {
            if (a != reporting && a.stopWhenIdle && a.state == State.PAUSED && !a.finishing) {
                a.stopWhenIdle = false;
                toStop.add(a);
            }
        }
        return toStop;
    }

    void activityPaused(ActivityRecord r) {
        List<ActivityRecord> launch = new ArrayList<>();
        ActivityRecord resume = null;
        boolean finishing;
        synchronized (this) {
            r.state = State.PAUSED;
            finishing = r.finishing;
            for (ActivityRecord a : mActivities) {
                if (a.waitingFor == r) {
                    a.waitingFor = null;
                    if (a.state == State.INITIALIZING && a.app != null && a.app.ready) {
                        launch.add(a);
                    }
                }
            }
            if (launch.isEmpty() && finishing) {
                // TaskFragment.resumeTopActivity: the activity under it in the task, else
                // the most recent one of another task.
                resume = topOf(r.taskId);
                if (resume == null) {
                    for (int i = mActivities.size() - 1; i >= 0 && resume == null; i--) {
                        ActivityRecord a = mActivities.get(i);
                        if (!a.finishing && a.state != State.DESTROYED) resume = a;
                    }
                }
                if (resume != null && resume.app == null) resume = null;
                if (resume != null) {
                    switch (resume.state) {
                        case PAUSED:
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
        if (resume != null) resume(resume);
        if (finishing) stop(r);
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

    static int taskOf(ActivityRecord r) {
        return r != null ? r.taskId : ActivityTaskManager.INVALID_TASK_ID;
    }
}
