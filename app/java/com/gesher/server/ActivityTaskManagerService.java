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
import android.app.servertransaction.GesherActivityWindowModeItem;
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
import android.content.IIntentSender;
import android.content.pm.ActivityInfo;
import android.content.pm.ApplicationInfo;
import android.content.pm.PackageManager;
import android.content.pm.ResolveInfo;
import android.content.res.Configuration;
import android.graphics.Rect;
import android.net.Uri;
import android.os.Binder;
import android.os.Bundle;
import android.os.IBinder;
import android.os.Parcel;
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
    @Override
    public android.content.pm.ConfigurationInfo getDeviceConfigurationInfo() {
        Configuration config = DeviceProfile.configuration();
        android.content.pm.ConfigurationInfo info = new android.content.pm.ConfigurationInfo();
        info.reqTouchScreen = config.touchscreen;
        info.reqKeyboardType = config.keyboard;
        info.reqNavigation = config.navigation;
        info.reqInputFeatures = 0;
        info.reqGlEsVersion = android.os.SystemProperties.getInt("ro.opengles.version", 0x20000);
        return info;
    }

    /** How long startActivityAndWait waits for the activity to be idle. */
    private static final long WAIT_TIMEOUT_MS = 300_000;

    /**
     * Opens a URL no activity of the device handles (http, https, mailto) with Windows'
     * handler (WindowsHost.openUrl): the user's default browser or mail app. The tests
     * replace it (gesher.services.BrowserIntentTest), so that no browser opens.
     */
    static volatile java.util.function.Predicate<String> sOpenExternalUrl = WindowsHost::openUrl;

    /** Windows owns the toasts' per-app controls; Android's Settings APK is not installed. */
    static volatile java.util.function.Predicate<String> sOpenNotificationSettings = WindowsHost::openUrl;

    /** STARTED: made visible again without being resumed (StartActivityItem). */
    enum State { INITIALIZING, LAUNCHING, RESUMING, RESUMED, PAUSING, PAUSED, STARTED, STOPPING,
            STOPPED, DESTROYING, DESTROYED }

    /** An app process attached to the server. */
    final class ProcessRecord {
        final int pid;
        final int uid;
        final IApplicationThread thread;
        String packageName;  // null until a start binds it to a package
        String processName;  // the manifest component process, within this package UID
        /** Actual mode captured for this server-spawned process, never an Intent extra. */
        String windowMode;
        boolean windowModeExplicit;
        boolean ready;       // finishAttachApplication came
        boolean exiting;
        boolean spawned;     // started by this server (startProcess), not by itself
        long startSeq;       // distinguishes a late exit from a newer process of this package
        ProcessExitStatus.Generation exitGeneration;
        /** The configuration it was given last (bindApplication, ConfigurationChangeItem). */
        Configuration config;
        final List<ActivityRecord> activities = new ArrayList<>();

        ProcessRecord(int pid, IApplicationThread thread) {
            this.pid = pid;
            this.uid = Binder.getCallingUid();
            this.thread = thread;
        }

        @Override
        public String toString() {
            return "process " + pid + (packageName != null ? " (" + processName + ")" : "");
        }
    }

    /** An activity: what ActivityRecord and its Task keep. */
    final class ActivityRecord {
        final IBinder token;
        final ActivityInfo info;
        final ApkPackage targetCode;
        java.util.function.BooleanSupplier sourceLifetime;
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
        ProcessExitStatus.Generation launchGeneration;
        /** What TaskInfo calls the task's WindowContainerToken; nothing stands behind it. */
        final WindowContainerToken taskToken =
                new WindowContainerToken(new IWindowContainerToken.Stub() {});
        State state = State.INITIALIZING;
        boolean idle;
        boolean finishing;
        /** Only an authenticated server chooser may consume its authenticator UI result. */
        Runnable accountCompletion;
        String inheritedWindowMode;
        String activityWindowMode;
        IBinder activityWindowGrant;
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
            this.targetCode = mPm.get(info.packageName);
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
    private final PermissionRequests mPermissionRequests;
    private final ThreadLocal<String> mVerifiedCreator = new ThreadLocal<>();
    private final ThreadLocal<java.util.function.BooleanSupplier> mVerifiedLifetime = new ThreadLocal<>();
    private Runnable mBeforeActivityPublication; // Server-local retirement race fixture.
    private final ThreadLocal<Runnable> mAccountCompletion = new ThreadLocal<>();
    private final Map<IBinder, Object> mAccountChoosers = new HashMap<>();
    private AccountManagerService mAccounts;
    private AccountChooser.Dialog mAccountDialog; // server-local synthetic fixture injection
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

        default int start(String packageName, String processName, long startSeq,
                int targetSdkVersion) throws Exception {
            if (!packageName.equals(processName)) {
                throw new IllegalStateException("Process starter does not support " + processName);
            }
            return start(packageName, startSeq, targetSdkVersion);
        }
        default int start(String packageName, String processName, long startSeq,
                int targetSdkVersion, String windowMode) throws Exception {
            return start(packageName, processName, startSeq, targetSdkVersion);
        }
    }

    private static final class PendingProcess {
        final String packageName;
        final String processName;
        final String reason;
        final String windowMode;
        final boolean windowModeExplicit;
        int pid;
        ProcessExitStatus.Generation exitGeneration;
        boolean cancelled;

        PendingProcess(String packageName, String processName, String reason) {
            this(packageName, processName, reason, null);
        }
        PendingProcess(String packageName, String processName, String reason, String windowMode) {
            this(packageName, processName, reason, windowMode, false);
        }
        PendingProcess(String packageName, String processName, String reason, String windowMode,
                boolean windowModeExplicit) {
            this.packageName = packageName;
            this.processName = processName;
            this.reason = reason;
            this.windowMode = windowMode;
            this.windowModeExplicit = windowModeExplicit;
        }
    }

    private static final class PendingWork {
        final String packageName;
        final String processName;
        final Consumer<ProcessRecord> work;

        PendingWork(String packageName, String processName, Consumer<ProcessRecord> work) {
            this.packageName = packageName;
            this.processName = processName;
            this.work = work;
        }
    }

    ActivityTaskManagerService(PackageManagerService pm) {
        mPm = pm;
        mPermissionRequests = new PermissionRequests(pm);
    }

    void setActivityManager(ActivityManagerService am) {
        mAm = am;
    }

    void setAccountManager(AccountManagerService accounts) { mAccounts = accounts; }

    private int chooseAccount(IApplicationThread caller, String callingPackage, Intent intent,
            IBinder token, String resultWho, int requestCode) {
        mAm.callers().requirePackage(callingPackage);
        final ActivityRecord source;
        final ProcessRecord process;
        final ApkPackage code;
        final Object reservation = new Object();
        synchronized (this) {
            source = forToken(token);
            if (source == null || source.app == null || source.app.thread == null
                    || source.finishing || source.state == State.DESTROYED || source.app.exiting
                    || !mProcesses.contains(source.app) || caller == null || requestCode < 0
                    || source.app.pid != Binder.getCallingPid()
                    || !caller.asBinder().equals(source.app.thread.asBinder())
                    || !source.info.packageName.equals(callingPackage)
                    || !source.info.packageName.equals(source.app.packageName)) {
                throw new SecurityException("Account chooser must come from its own live activity");
            }
            if (mAccounts == null) return ActivityManager.START_INTENT_NOT_RESOLVED;
            if (mAccountChoosers.containsKey(token) || mAccountChoosers.size() >= 16) {
                throw new IllegalStateException("Account chooser already pending");
            }
            process = source.app; code = mPm.get(callingPackage);
            if (code == null) throw new SecurityException("Unknown chooser package");
            mAccountChoosers.put(token, reservation);
        }
        AccountChooser.Lifetime lifetime = new AccountChooser.Lifetime() {
            private boolean valid() {
                return mAccountChoosers.get(token) == reservation && source.app == process
                        && !process.exiting && mProcesses.contains(process)
                        && !source.finishing && source.state != State.DESTROYED
                        && mActivities.contains(source) && mPm.get(callingPackage) == code;
            }
            @Override public boolean active() { synchronized (ActivityTaskManagerService.this) { return valid(); } }
            @Override public boolean whileActive(Runnable action) {
                synchronized (ActivityTaskManagerService.this) {
                    if (!valid()) return false;
                    action.run(); return true;
                }
            }
        };
        try {
            AccountManagerService.ChooserSession session = mAccounts.prepareChooser(callingPackage,
                    process.uid, process.pid, new Intent(intent));
            AccountChooser.Result result = data -> {
                boolean deliver;
                synchronized (ActivityTaskManagerService.this) {
                    deliver = lifetime.active();
                    mAccountChoosers.remove(token, reservation);
                }
                if (deliver) deliverResult(source, resultWho, requestCode, data == null
                        ? android.app.Activity.RESULT_CANCELED : android.app.Activity.RESULT_OK, data);
            };
            AccountChooser.Launcher launcher = (authIntent, finished) -> {
                if (authIntent.getComponent() == null || !lifetime.active()) return false;
                // The authenticator's response validator supplies this explicit component;
                // caller extras cannot access this server-private launch capability.
                String previous = mVerifiedCreator.get();
                Runnable oldCompletion = mAccountCompletion.get();
                mVerifiedCreator.set(authIntent.getComponent().getPackageName());
                mAccountCompletion.set(finished);
                long identity = Binder.clearCallingIdentity();
                try {
                    return start(null, new Intent(authIntent), authIntent.getType(), callingPackage,
                            token, null, -1, new ActivityRecord[1]) == ActivityManager.START_SUCCESS;
                } finally {
                    Binder.restoreCallingIdentity(identity);
                    if (previous == null) mVerifiedCreator.remove(); else mVerifiedCreator.set(previous);
                    if (oldCompletion == null) mAccountCompletion.remove(); else mAccountCompletion.set(oldCompletion);
                }
            };
            AccountChooser chooser = mAccountDialog == null
                    ? new AccountChooser(session, lifetime, launcher, result)
                    : new AccountChooser(session, lifetime, launcher, result, mAccountDialog);
            chooser.start();
            return ActivityManager.START_SUCCESS;
        } catch (RuntimeException | Error e) {
            synchronized (this) { mAccountChoosers.remove(token, reservation); }
            throw e;
        }
    }

    private static void accountActivityFinished(ActivityRecord record) {
        Runnable completion = record.accountCompletion;
        record.accountCompletion = null;
        if (completion != null) completion.run();
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
    public int startActivityIntentSender(IApplicationThread caller, IIntentSender target,
            IBinder allowlistToken, Intent fillIn, String resolvedType, IBinder resultTo,
            String resultWho, int requestCode, int flagsMask, int flagsValues, Bundle options) {
        final ProcessRecord process;
        final ActivityRecord source;
        final ApkPackage callerCode;
        synchronized (this) {
            process = callerProcess(Binder.getCallingPid());
            if (process == null || process.exiting || !process.ready || caller == null
                    || process.thread == null || !process.thread.asBinder().equals(caller.asBinder())) {
                throw new SecurityException("IntentSender must come from an attached application thread");
            }
            mAm.callers().requirePackage(process.packageName);
            callerCode = mPm.get(process.packageName);
            source = resultTo == null ? null : forToken(resultTo);
            if (callerCode == null || mPm.isRemovalPending(process.packageName)
                    || resultTo != null && (source == null || source.app != process || source.finishing
                        || source.state == State.DESTROYED) || requestCode >= 0 && source == null) {
                throw new SecurityException("IntentSender result must belong to its live caller activity");
            }
        }
        java.util.function.BooleanSupplier callerLive = () -> {
            synchronized (ActivityTaskManagerService.this) {
                return mProcesses.contains(process) && !process.exiting
                        && mPm.get(process.packageName) == callerCode && !mPm.isRemovalPending(process.packageName)
                        && (source == null || source.app == process && mActivities.contains(source)
                            && !source.finishing && source.state != State.DESTROYED);
            }
        };
        return mAm.pendingIntents().sendActivity(target, fillIn, resolvedType, (creator, intent, type, creatorLive) -> {
            // URI grant authority cannot be added through flagsMask, including for
            // an otherwise immutable capability. The ordinary fill-in policy stays
            // in PendingIntents alongside one-shot and creator-generation checks.
            int mask = flagsMask & ~Intent.IMMUTABLE_FLAGS;
            intent.setFlags((intent.getFlags() & ~mask) | (flagsValues & mask));
            return startActivityForVerifiedCreator(creator, intent, type,
                    () -> callerLive.getAsBoolean() && creatorLive.getAsBoolean(),
                    resultTo, resultWho, requestCode, options);
        });
    }

    @Override
    public int startActivityAsUser(IApplicationThread caller, String callingPackage,
            String callingFeatureId, Intent intent, String resolvedType, IBinder resultTo,
            String resultWho, int requestCode, int flags, ProfilerInfo profilerInfo,
            Bundle options, int userId) {
        ActivityRecord[] started = new ActivityRecord[1];
        return start(caller, intent, resolvedType, callingPackage, resultTo, resultWho,
                requestCode, launchTaskId(options), started);
    }

    /** ActivityOptions.setLaunchTaskId of a start's options, or -1. */
    private static int launchTaskId(Bundle options) {
        if (options == null) return -1;
        try {
            return android.app.ActivityOptions.fromBundle(options).getLaunchTaskId();
        } catch (RuntimeException e) {
            return -1;
        }
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
        ActivityRecord[] started = new ActivityRecord[1];
        int code = start(caller, intent, resolvedType, callingPackage, resultTo,
                resultWho, requestCode, launchTaskId(options), started);
        return waitForStarted(code, started[0]);
    }

    /** The host launcher receives authority for this actual launch, never a claimed process name. */
    GesherService.LaunchResult startActivityForHost(Intent intent) {
        if (mAm == null || !mAm.callers().isSystem()) throw new SecurityException("Host launch required");
        if (intent == null) throw new IllegalArgumentException("Missing launch intent");
        ActivityRecord[] started = new ActivityRecord[1];
        int code = start(null, new Intent(intent), intent.getType(), "com.gesher.launcher",
                null, null, 0, -1, started);
        ActivityRecord record = started[0];
        WaitResult result = waitForStarted(code, record);
        ProcessExitStatus.Generation generation;
        int task;
        synchronized (this) {
            generation = record == null ? null : record.launchGeneration;
            task = record == null ? -1 : record.taskId;
        }
        return new GesherService.LaunchResult(result, task,
                new LaunchExitReceipt(mExitStatus, generation, mAm.callers()));
    }

    /** A receipt retains no ActivityRecord or Intent, only the captured OS-start generation. */
    private static final class LaunchExitReceipt extends Binder {
        private final ProcessExitStatus status;
        private final ProcessExitStatus.Generation generation;
        private final CallerIdentity callers;
        LaunchExitReceipt(ProcessExitStatus status, ProcessExitStatus.Generation generation,
                CallerIdentity callers) {
            super(GesherService.EXIT_DESCRIPTOR);
            this.status = status; this.generation = generation; this.callers = callers;
        }
        @Override protected boolean onTransact(int code, android.os.Parcel data,
                android.os.Parcel reply, int flags) throws RemoteException {
            if (code != GesherService.EXIT_STATUS) return super.onTransact(code, data, reply, flags);
            data.enforceInterface(GesherService.EXIT_DESCRIPTOR);
            if (!callers.isSystem()) throw new SecurityException("Host exit receipt required");
            data.enforceNoDataAvail();
            if (reply == null) throw new IllegalArgumentException("Exit reply required");
            ProcessExitStatus.Snapshot snapshot = status.snapshot(generation);
            int state = snapshot.state == ProcessExitStatus.State.STARTING ? GesherService.EXIT_STARTING
                    : snapshot.state == ProcessExitStatus.State.RUNNING ? GesherService.EXIT_RUNNING
                    : snapshot.state == ProcessExitStatus.State.FINISHED ? GesherService.EXIT_FINISHED
                    : GesherService.EXIT_UNAVAILABLE;
            reply.writeNoException(); reply.writeInt(state); reply.writeBoolean(snapshot.hasExitCode);
            reply.writeInt(snapshot.exitCode); return true;
        }
    }

    private WaitResult waitForStarted(int code, ActivityRecord r) {
        WaitResult result = new WaitResult(); result.result = code;
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
    /** PendingIntent dispatch uses the validated creator, even when it has no process. */
    int startActivityForVerifiedCreator(String packageName, Intent intent, String resolvedType) {
        ApkPackage captured = mPm.get(packageName);
        return startActivityForVerifiedCreator(packageName, intent, resolvedType,
                () -> captured != null && mPm.get(packageName) == captured && !mPm.isRemovalPending(packageName));
    }
    int startActivityForVerifiedCreator(String packageName, Intent intent, String resolvedType,
            java.util.function.BooleanSupplier lifetime) {
        return startActivityForVerifiedCreator(packageName, intent, resolvedType, lifetime,
                null, null, -1, null);
    }

    private int startActivityForVerifiedCreator(String packageName, Intent intent, String resolvedType,
            java.util.function.BooleanSupplier lifetime, IBinder resultTo, String resultWho,
            int requestCode, Bundle options) {
        if (Binder.getCallingPid() != android.os.Process.myPid() || mPm.get(packageName) == null) {
            throw new SecurityException("Only a validated local capability may supply a creator");
        }
        if (lifetime == null || !lifetime.getAsBoolean()) throw new SecurityException("Delegated creator retired");
        String previous = mVerifiedCreator.get();
        java.util.function.BooleanSupplier oldLifetime = mVerifiedLifetime.get();
        mVerifiedCreator.set(packageName);
        mVerifiedLifetime.set(lifetime);
        try {
            return start(null, intent, resolvedType, packageName, resultTo, resultWho, requestCode,
                    launchTaskId(options), new ActivityRecord[1]);
        } finally {
            if (previous == null) mVerifiedCreator.remove(); else mVerifiedCreator.set(previous);
            if (oldLifetime == null) mVerifiedLifetime.remove(); else mVerifiedLifetime.set(oldLifetime);
        }
    }

    private int requestPermissions(IApplicationThread caller, String callingPackage,
            Intent intent, IBinder resultToToken, String resultWho, int requestCode) {
        mAm.callers().requirePackage(callingPackage);
        final ActivityRecord source;
        final ProcessRecord process;
        final ApkPackage captured;
        synchronized (this) {
            source = forToken(resultToToken);
            if (source == null || source.app == null || source.finishing
                    || source.state == State.DESTROYED || caller == null
                    || source.app.pid != Binder.getCallingPid()
                    || !caller.asBinder().equals(source.app.thread.asBinder())
                    || !source.info.packageName.equals(callingPackage)) {
                throw new SecurityException("Permission request must come from its own activity");
            }
            process = source.app;
            captured = mPm.get(callingPackage);
        }
        if (intent.getIntExtra(PackageManager.EXTRA_REQUEST_PERMISSIONS_DEVICE_ID, 0) != 0) {
            throw new SecurityException("Only the default permission device is supported");
        }
        mPermissionRequests.request(captured,
                intent.getStringArrayExtra(PackageManager.EXTRA_REQUEST_PERMISSIONS_NAMES), data -> {
                    synchronized (ActivityTaskManagerService.this) {
                        if (source.app != process || source.finishing
                                || source.state == State.DESTROYED || !mActivities.contains(source)
                                || mPm.get(callingPackage) != captured) return;
                    }
                    deliverResult(source, resultWho, requestCode, android.app.Activity.RESULT_OK, data);
                });
        return ActivityManager.START_SUCCESS;
    }

    private int start(IApplicationThread caller, Intent intent, String resolvedType,
            String callingPackage, IBinder resultToToken, String resultWho, int requestCode,
            ActivityRecord[] started) {
        return start(caller, intent, resolvedType, callingPackage, resultToToken, resultWho,
                requestCode, -1, started);
    }

    /**
     * {@link #start}, into the task {@code launchTaskId} when it has activities
     * (ActivityOptions.setLaunchTaskId: gesher-app's {@code view} puts the activity that
     * receives a URL from Windows, an OAuth redirect, in the app's task).
     */
    private int start(IApplicationThread caller, Intent intent, String resolvedType,
            String callingPackage, IBinder resultToToken, String resultWho, int requestCode,
            int launchTaskId, ActivityRecord[] started) {
        final int originalPid = Binder.getCallingPid();
        final ProcessRecord originalProcess = callerProcess(originalPid);
        final boolean internalCreator = mVerifiedCreator.get() != null;
        final String originalName = internalCreator ? mVerifiedCreator.get() : callingPackage;
        final ApkPackage originalCallerCode = mPm.get(originalName);
        final java.util.function.BooleanSupplier delegated = mVerifiedLifetime.get();
        String verifiedCaller = mVerifiedCreator.get();
        if (verifiedCaller == null && GmsSignatureSpoofing.isEnabled()
                && mAm != null && !mAm.callers().isSystem()) {
            verifiedCaller = mAm.callers().requirePackage(callingPackage);
            boolean matchingThread = false;
            synchronized (this) {
                for (ProcessRecord app : mProcesses) {
                    if (app.pid == Binder.getCallingPid() && !app.exiting
                            && verifiedCaller.equals(app.packageName) && caller != null
                            && app.thread.asBinder().equals(caller.asBinder())) {
                        matchingThread = true;
                        break;
                    }
                }
            }
            if (!matchingThread) throw new SecurityException("Unrecognized activity caller thread");
        }
        if (intent != null && PackageManager.ACTION_REQUEST_PERMISSIONS.equals(intent.getAction())) {
            return requestPermissions(caller, callingPackage, intent, resultToToken,
                    resultWho, requestCode);
        }
        if (AccountChooser.handles(intent)) {
            return chooseAccount(caller, callingPackage, intent, resultToToken, resultWho, requestCode);
        }
        // ACTION_CHOOSER ("open with", "share via"): the device has no ChooserActivity, so the
        // choice is the first target, as a chooser with one option starts it at once; an
        // http(s) VIEW goes on to the Windows browser below.
        if (mAccountCompletion.get() == null && intent != null && Intent.ACTION_CHOOSER.equals(intent.getAction())) {
            Intent target = intent.getParcelableExtra(Intent.EXTRA_INTENT, Intent.class);
            if (target != null && !Intent.ACTION_CHOOSER.equals(target.getAction())) {
                target = new Intent(target);
                target.addFlags(intent.getFlags());
                SystemServer.log("startActivity: chooser, starting its target " + target);
                return start(caller, target, target.getType(), callingPackage, resultToToken,
                        resultWho, requestCode, launchTaskId, started);
            }
        }
        // Starts from apps resolve implicit intents to filters with CATEGORY_DEFAULT, as
        // ActivityStarter asks (MATCH_DEFAULT_ONLY); gesher-app's launcher (no caller)
        // resolves MAIN/LAUNCHER in a package, as a launcher does.
        ActivityInfo info;
        ApkPackage targetCode;
        synchronized (mPm) {
            info = resolve(intent, resolvedType, caller != null);
            targetCode = info == null ? null : mPm.get(info.packageName);
        }
        if (info == null) {
            if (intent != null) {
                String action = intent.getAction();
                // Apps such as Gmail open a channel's settings without first resolving
                // the intent. With no Settings APK, START_INTENT_NOT_RESOLVED becomes
                // an uncaught ActivityNotFoundException on their UI thread. Windows
                // manages Gesher's app toasts, so open its notification controls. Use
                // a fixed URI: package/channel extras are not shell input. Individual
                // Android channel controls are not provided by this Windows page.
                if (intent.getComponent() == null
                        && (intent.getPackage() == null
                            || "com.android.settings".equals(intent.getPackage()))
                        && ("android.settings.APP_NOTIFICATION_SETTINGS".equals(action)
                            || "android.settings.CHANNEL_NOTIFICATION_SETTINGS".equals(action))) {
                    boolean opened = sOpenNotificationSettings.test("ms-settings:notifications");
                    SystemServer.log("startActivity: " + action
                            + ": Windows notification settings " + (opened ? "opened" : "unavailable"));
                    return opened ? ActivityManager.START_SUCCESS
                            : ActivityManager.START_INTENT_NOT_RESOLVED;
                }
                if (Intent.ACTION_VIEW.equals(action)) {
                    Uri data = intent.getData();
                    if (data != null) {
                        String scheme = data.getScheme();
                        if ("http".equalsIgnoreCase(scheme) || "https".equalsIgnoreCase(scheme)
                                || "mailto".equalsIgnoreCase(scheme)) {
                            // Also a VIEW for the Windows default browser's package
                            // (BrowserPackageSupport, of which resolve() runs nothing).
                            String url = data.toString();
                            SystemServer.log("startActivity: delegating external URL to Windows: " + url);
                            sOpenExternalUrl.test(url);
                            return ActivityManager.START_SUCCESS;
                        }
                    }
                } else if (Intent.ACTION_OPEN_DOCUMENT_TREE.equals(action)
                        || Intent.ACTION_OPEN_DOCUMENT.equals(action)
                        || Intent.ACTION_GET_CONTENT.equals(action)) {
                    SystemServer.log("startActivity: handling document/tree picker intent: " + action);
                    ActivityRecord source = resultToToken != null ? forToken(resultToToken) : null;
                    boolean isTree = Intent.ACTION_OPEN_DOCUMENT_TREE.equals(action);
                    // The Windows dialog is modal and stays up as long as the user takes: not
                    // on this binder thread, which the app's main thread waits on (its UI
                    // would stop, as the app's own startActivityForResult returns at once
                    // on Android). The result comes when the dialog closes.
                    Thread picker = new Thread(() -> {
                        String picked = isTree ? WindowsHost.pickFolder("Select Folder")
                                : WindowsHost.pickFile("Select File");
                        SystemServer.log("startActivity: " + action + " picked "
                                + (picked != null && !picked.isEmpty() ? picked : "nothing"));
                        if (source == null || requestCode < 0) return;
                        Intent resultData = null;
                        int resultCode = android.app.Activity.RESULT_CANCELED;
                        if (picked != null && !picked.isEmpty()) {
                            resultCode = android.app.Activity.RESULT_OK;
                            resultData = new Intent();
                            Uri uri = Uri.fromFile(new java.io.File(Zygote.fromWindowsPath(picked)));
                            resultData.setData(uri);
                            resultData.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION
                                    | Intent.FLAG_GRANT_WRITE_URI_PERMISSION);
                            resultData.setClipData(android.content.ClipData.newUri(null, "document", uri));
                        }
                        deliverResult(source, resultWho, requestCode, resultCode, resultData);
                    }, "document-picker");
                    picker.setDaemon(true);
                    picker.start();
                    return ActivityManager.START_SUCCESS;
                }
            }
            SystemServer.log("startActivity: nothing handles " + intent);
            return ActivityManager.START_INTENT_NOT_RESOLVED;
        }
        if (verifiedCaller != null && !verifiedCaller.equals(info.packageName)) {
            if (!info.exported) throw new SecurityException("Activity is not exported");
            if (info.permission != null && mPm.permissions().checkPermission(verifiedCaller,
                    info.permission) != PackageManager.PERMISSION_GRANTED) {
                throw new SecurityException("Activity permission denied");
            }
        }
        final String attributedCaller = verifiedCaller;
        final java.util.function.BooleanSupplier sourceCurrent = () ->
                (attributedCaller == null || originalCallerCode != null
                    && mPm.get(attributedCaller) == originalCallerCode && !mPm.isRemovalPending(attributedCaller))
                && (delegated == null || delegated.getAsBoolean());
        Intent launch = new Intent(intent);
        launch.setComponent(new ComponentName(info.packageName, info.name));
        int callingPid = Binder.getCallingPid();
        int launchFlags = intent.getFlags();
        ActivityRecord source = resultToToken != null ? forToken(resultToToken) : null;
        // Whether it leaves the activity below it visible (its theme), read before the lock.
        boolean translucent = ActivityTheme.isTranslucentOrFloating(info);
        ActivityRecord r = null;
        ActivityRecord deliverTo = null;
        List<ActivityRecord> clearTop = new ArrayList<>();
        ActivityRecord pause = null;
        ProcessRecord bind = null;
        boolean launchNow = false;
        boolean needProcess = false;
        String sourceWindowMode = null;
        List<ActivityRecord> reconfigure = new ArrayList<>();
        Runnable beforePublication = mBeforeActivityPublication;
        if (beforePublication != null) beforePublication.run();
        synchronized (this) {
            if (targetCode == null || mPm.get(info.packageName) != targetCode
                    || mPm.isRemovalPending(info.packageName) || !sourceCurrent.getAsBoolean()
                    || info.applicationInfo == null || !java.util.Objects.equals(info.applicationInfo.sourceDir, targetCode.apkPath)) {
                return ActivityManager.START_CANCELED;
            }
            if (attributedCaller != null && !internalCreator && (originalProcess == null
                    || callerProcess(originalPid) != originalProcess || originalProcess.exiting)) {
                throw new SecurityException("Activity caller retired before publication");
            }
            // startActivityIntentSender authenticated this result activity before
            // clearing Binder identity. Creator authority still controls the target;
            // window visibility follows the live recipient that requested its UI.
            boolean delegatedResult = internalCreator && source != null;
            sourceWindowMode = sourceWindowMode(delegatedResult ? source.app.thread : caller,
                    delegatedResult ? source.info.packageName : callingPackage, source,
                    delegatedResult ? source.app.pid : callingPid,
                    resultToToken == null, info.packageName,
                    componentProcess(info.packageName, info.processName));
            int taskId = launchTaskId >= 0 && topOf(launchTaskId) != null ? launchTaskId
                    : source != null && (launchFlags & Intent.FLAG_ACTIVITY_NEW_TASK) == 0
                    ? source.taskId : mNextTaskId++;
            ActivityRecord top = topOf(taskId);
            boolean singleTop = mAccountCompletion.get() == null
                    && (info.launchMode == ActivityInfo.LAUNCH_SINGLE_TOP
                    || (launchFlags & Intent.FLAG_ACTIVITY_SINGLE_TOP) != 0);
            ActivityRecord below = (launchFlags & Intent.FLAG_ACTIVITY_CLEAR_TOP) != 0
                    && singleTop ? belowInTask(taskId, launch.getComponent()) : null;
            if (top != null && top.app != null && top.component().equals(launch.getComponent())
                    && singleTop) {
                deliverTo = top;
            } else if (below != null) {
                // FLAG_ACTIVITY_CLEAR_TOP with singleTop (Task.performClearTop): the
                // activities above it in the task finish and it gets the intent (AppAuth's
                // RedirectUriReceiverActivity hands the redirect to the
                // AuthorizationManagementActivity below it this way). Without singleTop,
                // Android would recreate it: that start stays a plain one here.
                deliverTo = below;
                for (int i = mActivities.size() - 1; i >= 0; i--) {
                    ActivityRecord a = mActivities.get(i);
                    if (a == below) break;
                    if (a.taskId == taskId && !a.finishing && a.state != State.DESTROYED) {
                        clearTop.add(a);
                    }
                }
            } else {
                ActivityRecord resultRecord = requestCode >= 0 ? source : null;
                r = new ActivityRecord(info, launch, taskId, callingPid, callingPackage,
                        resultRecord, resultWho, requestCode);
                r.sourceLifetime = sourceCurrent;
                r.accountCompletion = mAccountCompletion.get();
                r.inheritedWindowMode = sourceWindowMode;
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
                ProcessRecord app = processFor(info.packageName, componentProcess(info.packageName, info.processName));
                if (app == null) {
                    app = unassignedProcess(callingPid);
                    if (app != null) {
                        assignPackage(app, info.packageName, componentProcess(info.packageName, info.processName));
                        bind = app;
                    }
                }
                if (app != null) {
                    attach(r, app);
                } else {
                    needProcess = true;
                    for (PendingProcess pending : mStarting.values()) {
                        if (pending.packageName.equals(info.packageName)
                                && pending.processName.equals(componentProcess(info.packageName, info.processName))) {
                            r.launchGeneration = pending.exitGeneration; break;
                        }
                    }
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
            started[0] = deliverTo;
            SystemServer.log("START u0 {" + launch.toShortString(false, true, true, false)
                    + "} from " + callerName(callingPackage, callingPid)
                    + (clearTop.isEmpty() ? ": delivered to the top activity " + deliverTo.name()
                            + " (singleTop)" : ": delivered to " + deliverTo.name()
                            + " (clear top: the " + clearTop.size() + " above it finish)"));
            // Top first, as the task's activities are finished.
            for (ActivityRecord a : clearTop) {
                finish(a, "clear-top", android.app.Activity.RESULT_CANCELED, null);
            }
            deliverNewIntent(deliverTo, launch, callingPackage, sourceCurrent);
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
        if (needProcess) startProcess(info.packageName, componentProcess(info.packageName, info.processName),
                "activity " + r.name(), sourceWindowMode);
        if (pause != null) pause(pause, false);
        if (launchNow) realStartActivity(r);
        for (ActivityRecord a : reconfigure) ensureConfiguration(a);
        updateTopResumed();
        return ActivityManager.START_SUCCESS;
    }

    private static String callerName(String callingPackage, int callingPid) {
        return callingPackage != null ? callingPackage : "pid " + callingPid;
    }

    /** Caller activity authority for mode inheritance only. Guarded by this. */
    private String sourceWindowMode(IApplicationThread caller, String packageName,
            ActivityRecord source, int callingPid, boolean noSourceToken,
            String targetPackage, String targetProcess) {
        if (source == null && noSourceToken && caller != null && packageName != null) {
            ProcessRecord process = processByPid(callingPid);
            if (process == null || process.exiting || process.thread == null
                    || !packageName.equals(process.packageName)
                    || !caller.asBinder().equals(process.thread.asBinder())) {
                traceActivityMode(source, process, noSourceToken, false, false, null,
                        targetPackage, targetProcess);
                return null;
            }
            // Context/Application.startActivity has no result token. Its app must
            // currently own the front activity of a non-hidden task. A service or
            // provider process with no foreground/launching activity cannot inherit.
            for (int i = mActivities.size() - 1; i >= 0; i--) {
                ActivityRecord candidate = mActivities.get(i);
                if (candidate.app != process || candidate.finishing
                        || !packageName.equals(candidate.info.packageName)
                        || mHiddenTasks.contains(candidate.taskId)
                        || topOf(candidate.taskId) != candidate) continue;
                if (candidate.state == State.LAUNCHING || candidate.state == State.RESUMING
                        || candidate.state == State.RESUMED) {
                    source = candidate;
                    break;
                }
            }
        }
        boolean sourceLive = source != null && source.app != null && source.app.thread != null
                && !source.finishing && source.state != State.DESTROYED && !source.app.exiting
                && mActivities.contains(source) && mProcesses.contains(source.app);
        if (!sourceLive || !source.info.packageName.equals(packageName)
                || !source.info.packageName.equals(source.app.packageName)) {
            traceActivityMode(source, source != null ? source.app : processByPid(callingPid),
                    noSourceToken, false, sourceLive, null, targetPackage, targetProcess);
            return null;
        }
        boolean internalAuthenticator = mAccountCompletion.get() != null
                && callingPid == android.os.Process.myPid();
        boolean authenticated = internalAuthenticator || caller != null && source.app.pid == callingPid
                && caller.asBinder().equals(source.app.thread.asBinder());
        String mode = authenticated ? com.gesher.app.window.WindowMode.parse(
                source.activityWindowMode != null ? source.activityWindowMode : source.app.windowMode) : null;
        traceActivityMode(source, source.app, noSourceToken, authenticated, true, mode,
                targetPackage, targetProcess);
        return mode;
    }

    /** Opt-in diagnostics contain only public identity, enums and policy booleans. */
    private void traceActivityMode(ActivityRecord source, ProcessRecord process, boolean noToken,
            boolean authenticated, boolean live, String inherited, String target, String targetProcess) {
        if (!"1".equals(System.getenv("GESHER_TRACE_ACTIVITY_MODE"))) return;
        SystemServer.log("activity-mode source-package=" + (process != null ? process.packageName : null)
                + " source-process=" + (process != null ? process.processName : null)
                + " target-package=" + target + " target-process=" + targetProcess
                + " resultTo-null=" + noToken + " source-state=" + (source != null ? source.state : null)
                + " source-mode=" + (process != null ? process.windowMode : null)
                + " source-authenticated=" + authenticated + " source-live=" + live
                + " explicit-target=" + WindowManagerService.windowMode(target)
                + " inherited=" + inherited);
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
        // The Windows default browser (BrowserPackageSupport) has no activity to run: the
        // start delegates the URL to Windows, as when nothing resolves it.
        if (BrowserPackageSupport.isBrowserPackage(info.packageName)) return null;
        info = new ActivityInfo(info);
        if (info.applicationInfo == null) {
            info.applicationInfo = mPm.getApplicationInfo(info.packageName, flags, 0);
        }
        return info;
    }

    /**
     * The topmost activity of {@code component} in the task below its top activity, not
     * finishing and in a process; or null. Guarded by this.
     */
    private ActivityRecord belowInTask(int taskId, ComponentName component) {
        ActivityRecord top = topOf(taskId);
        for (int i = mActivities.size() - 1; i >= 0; i--) {
            ActivityRecord a = mActivities.get(i);
            if (a == top || a.taskId != taskId || a.finishing || a.state == State.DESTROYED) {
                continue;
            }
            if (a.app != null && a.component().equals(component)) return a;
        }
        return null;
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

    private static String processKey(String packageName, String processName) {
        return packageName + "\n" + processName;
    }

    String componentProcess(String packageName, String declared) {
        if (declared == null || declared.isEmpty()) {
            ApplicationInfo ai = mPm.getApplicationInfo(packageName, 0, 0);
            declared = ai != null ? ai.processName : null;
        }
        if (declared == null || declared.isEmpty()) return packageName;
        return declared.charAt(0) == ':' ? packageName + declared : declared;
    }

    private ProcessRecord processFor(String packageName) {
        return processFor(packageName, componentProcess(packageName, null));
    }

    private ProcessRecord processFor(String packageName, String processName) {
        for (ProcessRecord p : mProcesses) {
            if (packageName.equals(p.packageName) && !p.exiting
                    && processName.equals(componentProcess(p.packageName, p.processName))) return p;
        }
        return null;
    }

    /** Only a package assigned to a live, attached Binder PID can act as that app. */
    synchronized String authenticatedPackageForPid(int pid) {
        for (ProcessRecord app : mProcesses) {
            if (app.pid == pid && !app.exiting && app.packageName != null) {
                return app.packageName;
            }
        }
        return null;
    }

    /** Binds an attached process to a package without reusing an older start's exit. Guarded by this. */
    private void assignPackage(ProcessRecord app, String packageName, String processName) {
        app.packageName = packageName;
        app.processName = processName;
        if (app.startSeq == 0) {
            app.startSeq = mNextStartSeq++;
            mExitStatus.started(processKey(packageName, processName), app.startSeq);
        }
        if (app.exitGeneration == null) {
            app.exitGeneration = mExitStatus.capture(processKey(packageName, processName), app.startSeq);
        }
        mExitStatus.bind(processKey(packageName, processName), app.startSeq, app.pid);
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
        return starting(packageName, componentProcess(packageName, null));
    }

    private boolean starting(String packageName, String processName) {
        for (PendingProcess p : mStarting.values()) {
            if (p.packageName.equals(packageName) && p.processName.equals(processName)) return true;
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
        startProcess(packageName, componentProcess(packageName, null), reason);
    }

    void startProcess(String packageName, String processName, String reason) {
        startProcess(packageName, processName, reason, null);
    }

    private void startProcess(String packageName, String processName, String reason, String sourceMode) {
        ProcessStarter starter = mStarter;
        if (starter == null) return;
        long seq;
        PendingProcess pending;
        synchronized (this) {
            if (mPm.isRemovalPending(packageName)) return;
            if (processFor(packageName, processName) != null || starting(packageName, processName)) return;
            seq = mNextStartSeq++;
            mExitStatus.started(processKey(packageName, processName), seq);
            WindowManagerService.WindowPolicy policy =
                    WindowManagerService.resolveWindowPolicy(packageName, sourceMode);
            pending = new PendingProcess(packageName, processName, reason, policy.mode, policy.explicit);
            pending.exitGeneration = mExitStatus.capture(processKey(packageName, processName), seq);
            for (ActivityRecord record : mActivities) {
                if (record.app == null && record.launchGeneration == null
                        && record.state == State.INITIALIZING && !record.finishing
                        && record.info.packageName.equals(packageName)
                        && componentProcess(packageName, record.info.processName).equals(processName)) {
                    record.launchGeneration = pending.exitGeneration;
                }
            }
            if ("1".equals(System.getenv("GESHER_TRACE_ACTIVITY_MODE"))) {
                SystemServer.log("activity-mode process-start target-package=" + packageName
                        + " target-process=" + processName + " explicit-target="
                        + WindowManagerService.windowMode(packageName) + " inherited=" + sourceMode
                        + " resolved=" + policy.mode + " captured-explicit=" + policy.explicit);
            }
            mStarting.put(seq, pending);
            updateIdle();
        }
        ApplicationInfo ai = mPm.getApplicationInfo(packageName, 0, 0);
        int pid;
        try {
            pid = starter.start(packageName, processName, seq, ai != null ? ai.targetSdkVersion : 0,
                    pending.windowMode);
        } catch (Throwable t) {
            SystemServer.log("starting a process for " + packageName + " (" + reason
                    + ") failed: " + t);
            startFailed(seq, pending);
            return;
        }
        if (pid <= 0) {
            SystemServer.log("process starter returned no PID for " + packageName);
            startFailed(seq, pending); return;
        }
        synchronized (this) {
            pending.pid = pid;
            mExitStatus.bind(pending.exitGeneration, pid);
            if (pending.cancelled) Zygote.kill(pid);
            notifyAll();
        }
        SystemServer.log("starting a process for " + packageName + " (" + reason + "): pid "
                + pid + ", start seq " + seq);
        // A process that dies before it attaches fails what waited for it.
        Thread watch = new Thread(() -> {
            int canceledPolls = 0;
            while (true) {
                int code;
                synchronized (this) {
                    if (mStarting.get(seq) != pending && !pending.cancelled) return;  // attached
                    // exitCode consumes the native handle on completion. Keep
                    // lookup and publication atomic with attach handing it to
                    // processDied's observer, so two readers cannot consume it.
                    code = Zygote.exitCode(pid);
                    if (code != Zygote.STILL_ACTIVE) {
                        if (code == -1) mExitStatus.unavailable(pending.exitGeneration);
                        else mExitStatus.exited(pending.exitGeneration, pid, code);
                    }
                }
                if (code != Zygote.STILL_ACTIVE) {
                    SystemServer.log("process " + pid + " for " + packageName
                            + " exited before attaching" + (code == -1 ? " (OS exit unavailable)"
                                    : " (exit code " + code + ")"));
                    startFailed(seq, pending);
                    return;
                }
                if (pending.cancelled && ++canceledPolls >= 40) {
                    mExitStatus.unavailable(pending.exitGeneration);
                    startFailed(seq, pending); return;
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
            mExitStatus.unavailable(pending.exitGeneration);
            mStarting.remove(seq);
            for (int i = mActivities.size() - 1; i >= 0; i--) {
                ActivityRecord r = mActivities.get(i);
                if (r.app == null && r.info.packageName.equals(pending.packageName)
                        && componentProcess(r.info.packageName, r.info.processName).equals(pending.processName)) {
                    r.state = State.DESTROYED;
                    accountActivityFinished(r);
                    mActivities.remove(i);
                }
            }
            mPendingWork.removeIf(w -> w.packageName.equals(pending.packageName)
                    && w.processName.equals(pending.processName));
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
        return mExitStatus.code(processKey(packageName, componentProcess(packageName, null)));
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
                if (w.packageName.equals(app.packageName)
                        && w.processName.equals(app.processName)) return;
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
        // Also cancel sticky-service restarts when the old process has already died.
        if (mAm != null) mAm.stopServicesForPackage(packageName);
        List<ProcessRecord> stop = new ArrayList<>();
        synchronized (this) {
            mPendingWork.removeIf(work -> packageName.equals(work.packageName));
            for (ActivityRecord activity : new ArrayList<>(mActivities)) {
                if (activity.app != null || !packageName.equals(activity.info.packageName)) continue;
                activity.finishing = true; activity.state = State.DESTROYED;
                mExitStatus.unavailable(activity.launchGeneration);
                accountActivityFinished(activity); mActivities.remove(activity);
            }
            for (ProcessRecord p : mProcesses) {
                if (packageName.equals(p.packageName)) stop.add(p);
            }
            for (Map.Entry<Long, PendingProcess> entry : new ArrayList<>(mStarting.entrySet())) {
                PendingProcess p = entry.getValue();
                if (!p.packageName.equals(packageName)) continue;
                p.cancelled = true;
                if (p.pid > 0) Zygote.kill(p.pid);
                else startFailed(entry.getKey(), p);
            }
            notifyAll();
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
            for (Map.Entry<Long, PendingProcess> entry : new ArrayList<>(mStarting.entrySet())) {
                PendingProcess p = entry.getValue(); p.cancelled = true;
                if (p.pid > 0) Zygote.kill(p.pid);
                else startFailed(entry.getKey(), p);
            }
        }
        endProcesses(all, waitMs);
    }

    /** Retire one unresponsive broadcast generation, retaining normal service recovery. */
    void receiverTimedOut(ProcessRecord app) {
        synchronized (this) {
            if (!mProcesses.contains(app)) return;
        }
        SystemServer.log("manifest receiver timeout in " + app);
        // Remove its authenticated PID before a late finishReceiver/new broadcast can
        // reach it. This is unexpected death, not package force-stop: sticky services
        // and bindings retain their ordinary recovery policy.
        try {
            processDied(app);
        } finally {
            if (app.spawned) {
                Zygote.kill(app.pid);
            } else {
                try { app.thread.scheduleExit(); }
                catch (RemoteException ignored) { }
            }
        }
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
        if (r.launchGeneration == null) r.launchGeneration = app.exitGeneration;
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
            PendingProcess started = startSeq > 0 ? mStarting.get(startSeq) : null;
            if (startSeq > 0) {
                long deadline = SystemClock.uptimeMillis() + 5000;
                while (started != null && started.pid == 0
                        && SystemClock.uptimeMillis() < deadline) {
                    try { wait(Math.max(1, deadline - SystemClock.uptimeMillis())); }
                    catch (InterruptedException interrupted) {
                        Thread.currentThread().interrupt();
                        throw new SecurityException("Interrupted process authentication");
                    }
                }
                if (started == null || started.cancelled || started.pid != pid || mStarting.get(startSeq) != started) {
                    throw new SecurityException("App process does not match its start sequence");
                }
                ApkPackage expected = mPm.get(started.packageName);
                if (GmsSignatureSpoofing.isEnabled()
                        && (expected == null || expected.uid() != app.uid)) {
                    throw new SecurityException("App Binder UID does not match its package");
                }
                mStarting.remove(startSeq);
            } else if (GmsSignatureSpoofing.isEnabled() && mStarter != null) {
                throw new SecurityException("Protected app processes must be started by the server");
            }
            mProcesses.add(app);
            updateIdle();
            String waiting = null;
            String waitingProcess = null;
            if (started != null) {
                app.spawned = true;
                app.startSeq = startSeq;
                app.exitGeneration = started.exitGeneration;
                app.windowMode = started.windowMode;
                app.windowModeExplicit = started.windowModeExplicit;
                waiting = started.packageName;
                waitingProcess = started.processName;
            } else {
                // A start waiting for a process (none being started for its package): the
                // one asked by this process, else the first.
                for (ActivityRecord r : mActivities) {
                    if (r.app != null || r.state != State.INITIALIZING
                            || starting(r.info.packageName, componentProcess(r.info.packageName, r.info.processName))) {
                        continue;
                    }
                    if (r.launchedFromPid == pid) {
                        waiting = r.info.packageName;
                        waitingProcess = componentProcess(r.info.packageName, r.info.processName);
                        break;
                    }
                    if (waiting == null) {
                        waiting = r.info.packageName;
                        waitingProcess = componentProcess(r.info.packageName, r.info.processName);
                    }
                }
                // Else other work waiting for a package's process (a service, a broadcast).
                for (PendingWork w : mPendingWork) {
                    if (waiting != null) break;
                    if (!starting(w.packageName, w.processName)) {
                        waiting = w.packageName;
                        waitingProcess = w.processName;
                    }
                }
            }
            if (waiting != null && processFor(waiting, waitingProcess) == null) {
                assignPackage(app, waiting, waitingProcess);
                for (ActivityRecord r : mActivities) {
                    if (r.app == null && r.state == State.INITIALIZING
                            && app.packageName.equals(r.info.packageName)
                            && app.processName.equals(componentProcess(r.info.packageName, r.info.processName))) {
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
                if (w.packageName.equals(app.packageName) && w.processName.equals(app.processName)) {
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
        withProcess(packageName, null, reason, work);
    }

    void withProcess(String packageName, String declaredProcess, String reason,
            Consumer<ProcessRecord> work) {
        String processName = componentProcess(packageName, declaredProcess);
        ProcessRecord app;
        synchronized (this) {
            if (mPm.isRemovalPending(packageName)) return;
            app = processFor(packageName, processName);
            if (app == null || !app.ready) {
                mPendingWork.add(new PendingWork(packageName, processName, work));
                SystemServer.log("waiting for a process of " + packageName);
            }
        }
        if (app == null) {
            startProcess(packageName, processName, reason);
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
        ai = new ApplicationInfo(ai);
        ai.processName = app.processName;
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
                accountActivityFinished(r);
                mActivities.remove(r);
            }
            updateIdle();
            notifyAll();
        }
        SystemServer.log(app + " died");
        mAm.processDied(app);
        if (!app.spawned) mExitStatus.unavailable(app.exitGeneration);
        if (app.spawned && app.packageName != null) {
            // Its exit code, once Windows has it (binder may tell first).
            Thread t = new Thread(() -> {
                int code = Zygote.STILL_ACTIVE;
                for (int i = 0; i < 40 && code == Zygote.STILL_ACTIVE; i++) {
                    code = Zygote.exitCode(app.pid);
                    if (code == Zygote.STILL_ACTIVE) SystemClock.sleep(250);
                }
                if (code != Zygote.STILL_ACTIVE && code != -1) {
                    boolean published;
                    synchronized (this) {
                        published = mExitStatus.exited(app.exitGeneration, app.pid, code);
                    }
                    if (published) SystemServer.log(app + " exited with code " + code);
                } else {
                    mExitStatus.unavailable(app.exitGeneration);
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

    /** Only new windows in an implicitly headless process may gain inherited graphics. */
    private String promotedActivityMode(ActivityRecord r, ProcessRecord process) {
        if (!com.gesher.app.window.WindowMode.HEADLESS.equals(process.windowMode)) return null;
        String explicit = WindowManagerService.windowMode(r.info.packageName);
        if (com.gesher.app.window.WindowMode.HEADLESS.equals(explicit)) return null;
        if (explicit != null) return explicit; // an explicit hidden mode remains hidden
        if (process.windowModeExplicit) return null;
        String inherited = com.gesher.app.window.WindowMode.parse(r.inheritedWindowMode);
        return com.gesher.app.window.WindowMode.WINDOW.equals(inherited)
                || com.gesher.app.window.WindowMode.HIDDEN.equals(inherited) ? inherited : null;
    }

    /** Private capability accompanies exactly one system-created activity launch. */
    private IBinder createActivityWindowGrant(ActivityRecord r) {
        synchronized (this) {
            if (r.app == null || r.finishing || r.state != State.INITIALIZING
                    || !mActivities.contains(r)) return null;
            String mode = promotedActivityMode(r, r.app);
            if ("1".equals(System.getenv("GESHER_TRACE_ACTIVITY_MODE"))) {
                SystemServer.log("activity-mode new-activity target-package=" + r.info.packageName
                        + " target-process=" + r.app.processName + " startup=" + r.app.windowMode
                        + " captured-explicit=" + r.app.windowModeExplicit + " explicit-target="
                        + WindowManagerService.windowMode(r.info.packageName) + " resolved=" + mode);
            }
            if (mode == null) return null;
            ProcessRecord process = r.app;
            ApkPackage code = mPm.get(r.info.packageName);
            if (code == null) return null;
            r.activityWindowMode = mode;
            Binder grant = new Binder(GesherActivityWindowModeItem.DESCRIPTOR) {
                @Override protected boolean onTransact(int transaction, Parcel data, Parcel reply,
                        int flags) throws RemoteException {
                    if (transaction != GesherActivityWindowModeItem.RESOLVE) {
                        return super.onTransact(transaction, data, reply, flags);
                    }
                    data.enforceInterface(GesherActivityWindowModeItem.DESCRIPTOR);
                    IBinder token = data.readStrongBinder(); data.enforceNoDataAvail();
                    if (Binder.getCallingPid() != process.pid || Binder.getCallingUid() != process.uid) {
                        throw new SecurityException("Activity window capability belongs to its target process");
                    }
                    String permitted = null;
                    synchronized (ActivityTaskManagerService.this) {
                        if (r.activityWindowGrant == this && r.token.equals(token) && r.app == process
                                && !r.finishing && r.state != State.DESTROYED && !process.exiting
                                && mActivities.contains(r) && mProcesses.contains(process)
                                && mPm.get(r.info.packageName) == code) {
                            permitted = promotedActivityMode(r, process);
                        }
                        r.activityWindowMode = permitted;
                    }
                    reply.writeNoException(); reply.writeString(permitted);
                    return true;
                }
            };
            r.activityWindowGrant = grant;
            return grant;
        }
    }

    /** ActivityTaskSupervisor.realStartActivityLocked: launch and resume. */
    private void realStartActivity(ActivityRecord r) {
        boolean current;
        synchronized (this) {
            current = mActivities.contains(r) && r.state != State.DESTROYED && activityCodeCurrent(r)
                    && (r.sourceLifetime == null || r.sourceLifetime.getAsBoolean());
        }
        if (!current) {
            mExitStatus.unavailable(r.launchGeneration); r.finishing = true; remove(r); return;
        }
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
        IBinder windowGrant = createActivityWindowGrant(r);
        if (windowGrant != null) {
            t.addTransactionItem(new GesherActivityWindowModeItem(r.token, windowGrant));
        }
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
    private long mDisplaySeq;

    /**
     * The override of a task {@code task} in size (null: the display's): its size
     * configuration (TaskConfiguration), with a sequence number newer than every one given
     * before and than the package's configuration {@code global}'s: the client drops an
     * override that is not newer than the one it has (a seq of 0 too), and a task's size
     * changes back and forth with the same package configuration.
     */
    private synchronized Configuration taskOverride(Rect task, int rotation,
            Configuration global) {
        Configuration override = TaskConfiguration.override(
                task != null ? task : TaskConfiguration.display(rotation), rotation);
        mConfigSeq = Math.max(mConfigSeq, global.seq) + 1;
        if (mConfigSeq == 0) mConfigSeq = 1;
        override.seq = mConfigSeq;
        return override;
    }

    /** What an activity sees: its package's configuration with its task's override. */
    private static Configuration effectiveConfiguration(Configuration global, Rect task,
            int rotation) {
        Configuration c = new Configuration(global);
        c.updateFrom(TaskConfiguration.override(
                task != null ? task : TaskConfiguration.display(rotation), rotation));
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
        updateTopResumed();
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
        int displayRotation;
        long displaySequence;
        synchronized (this) {
            ActivityRecord want = topResumedCandidateLocked();
            displayRotation = want == null ? Surface.ROTATION_0
                    : mTaskRotation.get(want.taskId, Surface.ROTATION_0);
            displaySequence = ++mDisplaySeq;
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
        com.gesher.app.window.ApplicationDisplay.update(displayRotation, displaySequence);
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
    private boolean activityCodeCurrent(ActivityRecord r) {
        return r.targetCode != null && mPm.get(r.info.packageName) == r.targetCode
                && !mPm.isRemovalPending(r.info.packageName);
    }

    private void deliverNewIntent(ActivityRecord r, Intent intent, String referrer,
            java.util.function.BooleanSupplier sourceCurrent) {
        boolean resumed;
        synchronized (this) {
            if (!mActivities.contains(r) || !activityCodeCurrent(r) || !sourceCurrent.getAsBoolean()) return;
            resumed = r.state == State.RESUMED || r.state == State.LAUNCHING
                    || r.state == State.RESUMING;
        }
        List<ReferrerIntent> intents = new ArrayList<>();
        intents.add(new ReferrerIntent(intent, referrer));
        ClientTransaction t = new ClientTransaction(r.app.thread);
        t.addTransactionItem(new NewIntentItem(r.token, intents, resumed));
        schedule(r, t, "NewIntentItem");
    }

    private void deliverResult(ActivityRecord r, String resultWho, int requestCode,
            int resultCode, Intent resultData) {
        if (r == null) return;
        ResultInfo info = new ResultInfo(resultWho, requestCode, resultCode, resultData);
        boolean resumed;
        synchronized (this) {
            resumed = (r.state == State.RESUMED || r.state == State.LAUNCHING || r.state == State.RESUMING);
            if (!resumed) {
                r.pendingResults.add(info);
                return;
            }
        }
        if (r.app != null && r.app.thread != null) {
            List<ResultInfo> list = new ArrayList<>();
            list.add(info);
            ClientTransaction t = new ClientTransaction(r.app.thread);
            t.addTransactionItem(new ActivityResultItem(r.token, list));
            schedule(r, t, "ActivityResultItem");
        }
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
            accountActivityFinished(r);
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
        synchronized (this) {
            if (r.activityWindowGrant != null) {
                r.activityWindowGrant = null;
                r.activityWindowMode = null;
                t.addTransactionItem(GesherActivityWindowModeItem.revoke(r.token));
            }
        }
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
            accountActivityFinished(r);
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

    /** Whether the activity is its task's root (the bottom one not finishing). */
    boolean isTaskRoot(ActivityRecord r) {
        synchronized (this) {
            for (ActivityRecord a : mActivities) {
                if (a.taskId == r.taskId && !a.finishing && a.state != State.DESTROYED) {
                    return a == r;
                }
            }
            return false;
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
