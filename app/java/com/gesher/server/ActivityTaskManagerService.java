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
import android.app.WaitResult;
import android.app.servertransaction.ClientTransaction;
import android.app.servertransaction.DestroyActivityItem;
import android.app.servertransaction.LaunchActivityItem;
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

import com.gesher.app.DeviceProfile;

import java.util.ArrayList;
import java.util.List;

/**
 * {@code activity_task}: ActivityTaskManagerService, lite. It keeps what ATMS keeps for
 * the headless lifecycle: the app processes ({@link ProcessRecord}: an
 * {@link IApplicationThread} and its package), the activities ({@link ActivityRecord}: a
 * token, an ActivityInfo, a task, a state), and drives each Activity with the framework's
 * client transactions, as ATMS does: {@code LaunchActivityItem} and
 * {@code ResumeActivityItem} to start one, then {@code PauseActivityItem},
 * {@code StopActivityItem} and {@code DestroyActivityItem} to finish it, each step after
 * the app reported the previous one through {@link ActivityClientController}.
 *
 * <p>Processes: Gesher's system server cannot fork app processes (there is no zygote).
 * An app process is started by gesher-app's launcher and attaches itself
 * ({@code ActivityThread.attach} → {@link ActivityManagerService#attachApplication});
 * an activity start for a package with no process takes such a process, the one that
 * asked for the start if it is one (gesher-app asks for its own launch, as {@code am
 * start} would), else the first unassigned one, and binds it to the package
 * ({@code IApplicationThread.bindApplication}), as ActivityManagerService binds a process
 * zygote forked for it. Either may come first. The activities start once the process
 * reports {@code finishAttachApplication}, as with ActivityManagerService. A process whose
 * last activity is destroyed is told to exit ({@code scheduleExit}), as ActivityManager
 * kills the process of a removed task.
 */
public final class ActivityTaskManagerService extends IActivityTaskManager.Default {
    /** How long startActivityAndWait waits for the activity to be idle. */
    private static final long WAIT_TIMEOUT_MS = 300_000;

    enum State { INITIALIZING, LAUNCHING, RESUMED, PAUSING, PAUSED, STOPPING, STOPPED, DESTROYING,
            DESTROYED }

    /** An app process attached to the server. */
    final class ProcessRecord {
        final int pid;
        final IApplicationThread thread;
        String packageName;  // null until a start binds it to a package
        boolean ready;       // finishAttachApplication came
        boolean exiting;
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
        final long startTime = SystemClock.uptimeMillis();
        ProcessRecord app;
        State state = State.INITIALIZING;
        boolean idle;
        boolean finishing;

        ActivityRecord(ActivityInfo info, Intent intent, int taskId, int launchedFromPid,
                String launchedFromPackage) {
            this.info = info;
            this.intent = intent;
            this.taskId = taskId;
            this.launchedFromPid = launchedFromPid;
            this.launchedFromPackage = launchedFromPackage;
            this.token = new Binder("ActivityRecord " + info.name);
        }

        String name() {
            return info.packageName + "/" + info.name;
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
    private int mNextTaskId = 1;

    ActivityTaskManagerService(PackageManagerService pm) {
        mPm = pm;
    }

    void setActivityManager(ActivityManagerService am) {
        mAm = am;
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
        ActivityRecord r = start(intent, resolvedType, callingPackage);
        return r != null ? ActivityManager.START_SUCCESS : ActivityManager.START_INTENT_NOT_RESOLVED;
    }

    @Override
    public WaitResult startActivityAndWait(IApplicationThread caller, String callingPackage,
            String callingFeatureId, Intent intent, String resolvedType, IBinder resultTo,
            String resultWho, int requestCode, int flags, ProfilerInfo profilerInfo,
            Bundle options, int userId) {
        WaitResult result = new WaitResult();
        ActivityRecord r = start(intent, resolvedType, callingPackage);
        if (r == null) {
            result.result = ActivityManager.START_INTENT_NOT_RESOLVED;
            return result;
        }
        result.result = ActivityManager.START_SUCCESS;
        result.who = new ComponentName(r.info.packageName, r.info.name);
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
        for (ActivityRecord r : toFinish) finish(r, "remove-task");
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
                        seen = true;
                    }
                }
                if (seen) continue;
                ActivityManager.RunningTaskInfo t = new ActivityManager.RunningTaskInfo();
                t.taskId = r.taskId;
                t.id = r.taskId;
                t.baseActivity = new ComponentName(r.info.packageName, r.info.name);
                t.topActivity = t.baseActivity;
                t.baseIntent = keepIntentExtra ? new Intent(r.intent)
                        : new Intent(r.intent).replaceExtras((Bundle) null);
                t.numActivities = 1;
                t.isRunning = r.app != null;
                t.displayId = Display.DEFAULT_DISPLAY;
                t.userId = 0;
                tasks.add(t);
            }
        }
        return tasks;
    }

    // --- starting --------------------------------------------------------------------

    /** ActivityStarter, lite: resolves the intent, records the activity, starts it. */
    private ActivityRecord start(Intent intent, String resolvedType, String callingPackage) {
        ActivityInfo info = resolve(intent, resolvedType);
        if (info == null) {
            SystemServer.log("startActivity: nothing handles " + intent);
            return null;
        }
        Intent launch = new Intent(intent);
        launch.setComponent(new ComponentName(info.packageName, info.name));
        int callingPid = Binder.getCallingPid();
        ActivityRecord r;
        ProcessRecord bind = null;
        ProcessRecord launchIn = null;
        synchronized (this) {
            r = new ActivityRecord(info, launch, mNextTaskId++, callingPid, callingPackage);
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
                if (app.ready) launchIn = app;
            }
        }
        SystemServer.log("START u0 {" + launch.toShortString(false, true, true, false) + "} from "
                + (callingPackage != null ? callingPackage : "pid " + callingPid) + ": task "
                + r.taskId + (r.app != null ? ", in " + r.app : ", waiting for a process"));
        if (bind != null) bindApplication(bind);
        if (launchIn != null) realStartActivity(r);
        return r;
    }

    private ActivityInfo resolve(Intent intent, String resolvedType) {
        long flags = PackageManager.GET_META_DATA | PackageManager.GET_SHARED_LIBRARY_FILES;
        ActivityInfo info;
        if (intent.getComponent() != null) {
            info = mPm.getActivityInfo(intent.getComponent(), flags, 0);
        } else {
            ResolveInfo ri = mPm.resolveIntent(intent, resolvedType, flags, 0);
            info = ri != null ? ri.activityInfo : null;
        }
        if (info == null) return null;
        info = new ActivityInfo(info);
        if (info.applicationInfo == null) {
            info.applicationInfo = mPm.getApplicationInfo(info.packageName, flags, 0);
        }
        return info;
    }

    private ProcessRecord processFor(String packageName) {
        for (ProcessRecord p : mProcesses) {
            if (packageName.equals(p.packageName) && !p.exiting) return p;
        }
        return null;
    }

    private ProcessRecord unassignedProcess(int preferredPid) {
        ProcessRecord first = null;
        for (ProcessRecord p : mProcesses) {
            if (p.packageName != null) continue;
            if (p.pid == preferredPid) return p;
            if (first == null) first = p;
        }
        return first;
    }

    private void attach(ActivityRecord r, ProcessRecord app) {
        r.app = app;
        app.activities.add(r);
    }

    // --- processes (from ActivityManagerService) ---------------------------------------

    /** IActivityManager.attachApplication: a process offers itself. */
    void attachApplication(IApplicationThread thread, int pid) {
        ProcessRecord app = new ProcessRecord(pid, thread);
        boolean bind = false;
        synchronized (this) {
            mProcesses.add(app);
            // A start waiting for a process: the one asked by this process, else the first.
            ActivityRecord waiting = null;
            for (ActivityRecord r : mActivities) {
                if (r.app != null || r.state != State.INITIALIZING) continue;
                if (r.launchedFromPid == pid) {
                    waiting = r;
                    break;
                }
                if (waiting == null) waiting = r;
            }
            if (waiting != null) {
                app.packageName = waiting.info.packageName;
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
        ProcessRecord app;
        synchronized (this) {
            app = processByPid(pid);
            if (app == null) return;
            app.ready = true;
            for (ActivityRecord r : app.activities) {
                if (r.state == State.INITIALIZING) launch.add(r);
            }
        }
        SystemServer.log("finishAttachApplication: " + app);
        for (ActivityRecord r : launch) realStartActivity(r);
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
            notifyAll();
        }
        SystemServer.log(app + " died");
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
        ClientTransaction t = new ClientTransaction(r.app.thread);
        t.addTransactionItem(new LaunchActivityItem(r.token, r.intent,
                System.identityHashCode(r), r.info, config, new Configuration(),
                Context.DEVICE_ID_DEFAULT, r.launchedFromPackage, null /* voiceInteractor */,
                ActivityManager.PROCESS_STATE_TOP, null /* state */, null /* persistentState */,
                null /* pendingResults */, null /* pendingNewIntents */,
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

    /** ActivityRecord.finishIfPossible: pause, then stop, then destroy. */
    boolean finish(ActivityRecord r, String reason) {
        State state;
        synchronized (this) {
            if (r.finishing) return true;
            r.finishing = true;
            state = r.state;
        }
        SystemServer.log("finishing " + r.name() + " (" + reason + ")");
        switch (state) {
            case INITIALIZING:
                remove(r);
                return true;
            case LAUNCHING:
            case RESUMED:
                setState(r, State.PAUSING);
                ClientTransaction t = new ClientTransaction(r.app.thread);
                t.addTransactionItem(new PauseActivityItem(r.token, true /* finished */,
                        false /* userLeaving */, false /* dontReport */,
                        false /* autoEnteringPip */));
                schedule(r, t, "PauseActivityItem");
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
        ProcessRecord exit = null;
        synchronized (this) {
            r.state = State.DESTROYED;
            mActivities.remove(r);
            if (r.app != null) {
                r.app.activities.remove(r);
                if (r.app.activities.isEmpty() && r.finishing) {
                    // ActivityManagerService kills the process of a removed task; here the
                    // process is told to exit.
                    r.app.exiting = true;
                    exit = r.app;
                }
            }
            notifyAll();
        }
        if (exit != null) {
            SystemServer.log("scheduleExit: " + exit + " has no activities left");
            try {
                exit.thread.scheduleExit();
            } catch (RemoteException e) {
                SystemServer.log("scheduleExit: " + e);
            }
        }
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

    // --- reports from the app (ActivityClientController) --------------------------------

    void activityResumed(ActivityRecord r) {
        setState(r, State.RESUMED);
        SystemServer.log("activityResumed: " + r.name());
    }

    void activityIdle(ActivityRecord r) {
        synchronized (this) {
            r.idle = true;
            notifyAll();
        }
        SystemServer.log("activityIdle: " + r.name());
    }

    void activityPaused(ActivityRecord r) {
        setState(r, State.PAUSED);
        SystemServer.log("activityPaused: " + r.name());
        if (r.finishing) stop(r);
    }

    void activityStopped(ActivityRecord r) {
        setState(r, State.STOPPED);
        SystemServer.log("activityStopped: " + r.name());
        if (r.finishing) destroy(r);
    }

    void activityDestroyed(ActivityRecord r) {
        SystemServer.log("activityDestroyed: " + r.name());
        remove(r);
    }

    static int taskOf(ActivityRecord r) {
        return r != null ? r.taskId : ActivityTaskManager.INVALID_TASK_ID;
    }
}
