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
import android.content.IntentFilter;
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
import android.os.PersistableBundle;
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
import java.util.concurrent.atomic.AtomicLongFieldUpdater;
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

    private static final AtomicLongFieldUpdater<ProcessRecord> LAST_ACTIVE =
            AtomicLongFieldUpdater.newUpdater(ProcessRecord.class, "lastActive");

    /**
     * Opens a URL no activity of the device handles (http, https, mailto) with Windows'
     * handler (WindowsHost.openUrl): the user's default browser or mail app. The tests
     * replace it (gesher.services.BrowserIntentTest), so that no browser opens.
     */
    static volatile java.util.function.Predicate<String> sOpenExternalUrl = WindowsHost::openUrl;

    /** Windows owns the toasts' per-app controls; Android's Settings APK is not installed. */
    static volatile java.util.function.Predicate<String> sOpenNotificationSettings = WindowsHost::openUrl;

    /** A fixed ms-settings: page for an Android Settings screen (SettingsIntents). */
    static volatile java.util.function.Predicate<String> sOpenSettingsPage = WindowsHost::openUrl;

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
        ApkPackage packageCode;
        java.util.function.BooleanSupplier enabledProcessLifetime;
        /** Actual mode captured for this server-spawned process, never an Intent extra. */
        String windowMode;
        boolean windowModeExplicit;
        boolean ready;       // finishAttachApplication came
        /** Told to exit (or dying): no longer handed out. Read without the lock (providers). */
        volatile boolean exiting;
        /** Removed by physical-death cleanup; checked by provider admission after authentication. */
        volatile boolean dead;
        /** A later attachment claimed this PID; it can never belong to this record again. Guarded by ATM. */
        boolean pidSuperseded;
        /** PID replacement is not an intentional stop of a previously running process. */
        volatile boolean recoverAfterPidSupersession;
        /** Ended by the idle reaper: its services bound by others start again (ActiveServices). */
        volatile boolean reaped;
        boolean spawned;     // started by this server (startProcess), not by itself
        /** Original retained native child identity, copied from its PendingProcess. */
        long nativeStartTime;
        long startSeq;       // distinguishes a late exit from a newer process of this package
        /** When something last used it (uptime): the idle reaper's clock. */
        volatile long lastActive = SystemClock.uptimeMillis();

        /** Used at {@code time}, unless a later use is already noted. */
        void markActive(long time) {
            long observed = lastActive;
            while (time > observed) {
                if (LAST_ACTIVE.compareAndSet(this, observed, time)) return;
                observed = lastActive;
            }
        }
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

    /** Server-side origin retained until a result is delivered or forwarded. */
    private static final class PendingActivityResult extends ResultInfo {
        final ActivityRecord source;
        PendingActivityResult(ActivityRecord source, int code, Intent data, IBinder callerToken) {
            super(source.resultWho, source.requestCode, code, data, callerToken);
            this.source = source;
        }
    }

    /** An activity: what ActivityRecord and its Task keep. */
    final class ActivityRecord {
        final IBinder token;
        final IBinder initialCallerInfoAccessToken = new Binder("Initial activity caller");
        final ActivityCallerState callerState = new ActivityCallerState();
        final ActivityInfo info;
        final ApkPackage targetCode;
        java.util.function.BooleanSupplier sourceLifetime;
        final Intent intent;
        final int taskId;
        final int launchedFromPid;
        final String launchedFromPackage;
        /** startActivityForResult: who gets the result, and with which request code. */
        // Result ownership can be transferred by FLAG_ACTIVITY_FORWARD_RESULT.
        // Writes hold the lifecycle monitor; snapshots outside it remain visible.
        volatile ActivityRecord resultTo;
        final String resultWho;
        final int requestCode;
        final long startTime = SystemClock.uptimeMillis();
        // Record construction is guarded by the task-manager monitor. Clock ticks
        // can be shared by several activities and do not establish launch order.
        final long startOrder = mNextActivityOrder++;
        boolean launchFailed;
        ProcessRecord app;
        ProcessExitStatus.Generation launchGeneration;
        /** What TaskInfo calls the task's WindowContainerToken; nothing stands behind it. */
        final WindowContainerToken taskToken =
                new WindowContainerToken(new IWindowContainerToken.Stub() {});
        State state = State.INITIALIZING;
        boolean idle;
        boolean finishing;
        boolean preserveProcessOnFinish;
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
        /** Intents delivered to a history record before its first client launch. */
        final List<ReferrerIntent> pendingNewIntents = new ArrayList<>();
        /** The state it saved when it was last stopped. */
        Bundle savedState;
        PersistableBundle persistentState;
        /**
         * Its theme is translucent or floating (ActivityTheme): it does not occlude the
         * activities below it, as ActivityRecord.occludesParent; convertToTranslucent and
         * convertFromTranslucent change it.
         */
        boolean translucent;
        /** Resumed once the activity in front ({@link #waitingFor}) is paused. */
        boolean resumeAfterPause;
        /** Identity of the launch batch that created this record; never reused for another batch. */
        Object batchOwner;
        /** Published task history; no app instance/process is needed until it is visible. */
        boolean deferredHistory;
        boolean historyEntry;
        boolean launchScheduled;
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
            this(info, intent, taskId, launchedFromPid, launchedFromPackage, resultTo,
                    resultWho, requestCode, mPm.get(info.packageName));
        }

        ActivityRecord(ActivityInfo info, Intent intent, int taskId, int launchedFromPid,
                String launchedFromPackage, ActivityRecord resultTo, String resultWho,
                int requestCode, ApkPackage targetCode) {
            this.info = info;
            this.targetCode = targetCode;
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
    private final ThreadLocal<java.util.function.BooleanSupplier> mVerifiedPublication = new ThreadLocal<>();
    private Runnable mBeforeActivityPublication; // Server-local retirement race fixture.
    private Runnable mBeforeActivityOwnerAuthentication; // Server-local late-finish race fixture.
    private Runnable mBeforeShortcutResolution; // Server-local shortcut publisher race fixture.
    private final ThreadLocal<Runnable> mAccountCompletion = new ThreadLocal<>();
    private final Map<IBinder, Object> mAccountChoosers = new HashMap<>();
    private AccountManagerService mAccounts;
    private AccountChooser.Dialog mAccountDialog; // server-local synthetic fixture injection
    private int mNextTaskId = 1;
    private long mNextActivityOrder = 1;
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
        long nativeStartTime;
        ProcessExitStatus.Generation exitGeneration;
        volatile boolean cancelled;
        ApkPackage owner;
        java.util.function.BooleanSupplier enabledLifetime;

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
        final ApkPackage owner;
        final String component;
        final ApkPackage.ComponentKind kind;
        final Consumer<ProcessRecord> work;
        final Runnable canceled;
        ProcessExitStatus.Generation launchGeneration;

        PendingWork(ApkPackage owner, String component, ApkPackage.ComponentKind kind,
                String processName, Consumer<ProcessRecord> work, Runnable canceled) {
            this.owner = owner;
            this.component = component;
            this.kind = kind;
            this.packageName = owner.packageName;
            this.processName = processName;
            this.work = work;
            this.canceled = canceled;
        }

        boolean current(PackageManagerService pm) {
            return pm.get(packageName) == owner && !pm.isRemovalPending(packageName)
                    && (component == null ? owner.matchesEnabledApplication(0)
                            : owner.componentEnabled(component, kind));
        }
    }

    ActivityTaskManagerService(PackageManagerService pm) {
        mPm = pm;
        mPermissionRequests = new PermissionRequests(pm);
    }

    void setActivityManager(ActivityManagerService am) {
        mAm = am;
    }

    UriGrants uriGrants() {
        return mAm != null ? mAm.uriGrants() : null;
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
        return mAm.pendingIntents().sendActivity(target, fillIn, resolvedType, options,
                (creator, intents, types, creatorLive, shareIdentity, mergedOptions) -> {
            // URI grant authority cannot be added through flagsMask, including for
            // an otherwise immutable capability. The ordinary fill-in policy stays
            // in PendingIntents alongside one-shot and creator-generation checks.
            int mask = flagsMask & ~Intent.IMMUTABLE_FLAGS;
            Intent intent = intents[intents.length - 1];
            intent.setFlags((intent.getFlags() & ~mask) | (flagsValues & mask));
            Bundle launchOptions = activityOptions(mergedOptions, shareIdentity);
            if (intents.length > 1) {
                return withVerifiedCreator(creator, creatorLive, callerLive,
                        () -> startActivities(null, creator, null, intents, types, resultTo, launchOptions, 0));
            }
            return startActivityForVerifiedCreator(creator, intent, types[0],
                    creatorLive, resultTo, resultWho, requestCode, launchOptions, callerLive);
        });
    }

    @Override
    public int startActivityAsUser(IApplicationThread caller, String callingPackage,
            String callingFeatureId, Intent intent, String resolvedType, IBinder resultTo,
            String resultWho, int requestCode, int flags, ProfilerInfo profilerInfo,
            Bundle options, int userId) {
        ActivityRecord[] started = new ActivityRecord[1];
        return start(caller, intent, resolvedType, callingPackage, resultTo, resultWho,
                requestCode, launchTaskId(options), shareIdentity(options), started);
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

    private static boolean shareIdentity(Bundle options) {
        return options != null && android.app.ActivityOptions.fromBundle(options).isShareIdentityEnabled();
    }

    /** SafeActivityOptions checks task-placement authority for each options supplier. */
    static void checkLaunchTaskOptions(Bundle options, PackageManagerService packages,
            String packageName, boolean system) {
        if (options == null || android.app.ActivityOptions.fromBundle(options).getLaunchTaskId()
                == ActivityTaskManager.INVALID_TASK_ID || system) return;
        if (packageName == null || packages.permissions().checkPermission(packageName,
                "android.permission.START_TASKS_FROM_RECENTS") != PackageManager.PERMISSION_GRANTED) {
            throw new SecurityException("launchTaskId requires START_TASKS_FROM_RECENTS");
        }
    }

    private static Bundle activityOptions(Bundle options, boolean shareIdentity) {
        android.app.ActivityOptions parsed = options == null
                ? android.app.ActivityOptions.makeBasic() : android.app.ActivityOptions.fromBundle(options);
        return parsed.setShareIdentityEnabled(shareIdentity).toBundle();
    }

    /**
     * ActivityStartController.startActivities: publish history before realizing the
     * visible activities. In an opaque stack only the final activity is created;
     * Back realizes each preceding record when it first becomes visible.
     */
    @Override
    public int startActivities(IApplicationThread caller, String callingPackage,
            String callingFeatureId, Intent[] intents, String[] resolvedTypes, IBinder resultTo,
            Bundle options, int userId) {
        java.util.Objects.requireNonNull(intents, "intents is null");
        java.util.Objects.requireNonNull(resolvedTypes, "resolvedTypes is null");
        if (resolvedTypes.length != intents.length) {
            throw new IllegalArgumentException("intents are length different than resolvedTypes");
        }
        if (userId != 0) throw new SecurityException("Only user 0 is available");
        // Validate descriptors before publishing any activity, as AOSP does.
        List<Intent> members = new ArrayList<>(intents.length);
        for (Intent intent : intents) {
            if (intent == null) continue;
            if (intent.hasFileDescriptors()) {
                throw new IllegalArgumentException("File descriptors passed in Intent");
            }
            members.add(new Intent(intent));
        }
        // PendingIntents separately checks the original creator and sender option
        // suppliers before dispatch clears Binder identity. Direct calls check here.
        if (mVerifiedCreator.get() == null) {
            boolean system = mAm != null && mAm.callers().isSystem();
            checkLaunchTaskOptions(options, mPm,
                    system ? callingPackage : mAm.callers().requirePackage(callingPackage), system);
        }
        Object batchOwner = new Object();
        java.util.LinkedHashSet<Integer> tasks = new java.util.LinkedHashSet<>();
        IBinder nextSource = resultTo;
        String creator = mVerifiedCreator.get();
        ApkPackage creatorCode = creator == null ? null : mPm.get(creator);
        int sourceUid = creatorCode != null ? creatorCode.uid()
                : creator != null ? android.os.Process.SYSTEM_UID : Binder.getCallingUid();
        final boolean sharedIdentity = shareIdentity(options);
        ActivityRecord[] created = new ActivityRecord[1];
        synchronized (this) { mStackPublications++; }
        try {
            // Serialize publication with ordinary starts, without holding the
            // lifecycle monitor while package resolution/URI checks take locks.
            synchronized (mLaunchLock) {
                for (int i = 0; i < members.size(); i++) {
                    Intent member = members.get(i);
                    created[0] = null;
                    StackLaunch previous = mStackLaunch.get();
                    mStackLaunch.set(new StackLaunch(member, batchOwner));
                    int result;
                    try {
                        result = start(caller, member, resolvedTypes[i], callingPackage,
                                nextSource, null, -1, i == members.size() - 1 ? launchTaskId(options) : -1,
                                i == members.size() - 1 && sharedIdentity, created);
                    } finally {
                        if (previous == null) mStackLaunch.remove(); else mStackLaunch.set(previous);
                    }
                    if (created[0] != null) tasks.add(created[0].taskId);
                    if (result < ActivityManager.START_SUCCESS) return result;
                    ActivityRecord activity = created[0];
                    if (activity != null && activity.info.applicationInfo.uid == sourceUid) {
                        nextSource = activity.token;
                    } else {
                        nextSource = resultTo;
                        if (i + 1 < members.size()) members.get(i + 1).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
                    }
                }
            }
            return ActivityManager.START_SUCCESS;
        } finally {
            synchronized (this) {
                mStackPublications--;
                for (ActivityRecord record : mActivities) {
                    if (record.batchOwner == batchOwner) {
                        record.batchOwner = null;
                        // Each member already passed creator/capability admission.
                        // Accepted task history belongs to its target; a later
                        // creator update must not erase its uninstantiated parents.
                        // Target generation checks still run when history is realized.
                        record.sourceLifetime = null;
                        tasks.add(record.taskId);
                    }
                    if (mStackPublications == 0 && record.deferredHistory) tasks.add(record.taskId);
                }
            }
            // Android may leave a successfully published prefix after a failed
            // member. Do not destroy old tasks or pretend the batch is atomic.
            for (int task : tasks) realizeTaskHistory(task);
        }
    }

    /** Scoped only to this exact member Intent: reentrant starts cannot inherit the batch. */
    private static final class StackLaunch {
        final Intent intent;
        final Object owner;
        StackLaunch(Intent intent, Object owner) {
            this.intent = intent;
            this.owner = owner;
        }
    }
    private final ThreadLocal<StackLaunch> mStackLaunch = new ThreadLocal<>();
    private final Object mLaunchLock = new Object();
    private int mStackPublications; // Guarded by this; callbacks cannot realize a half-published stack.
    private final java.util.LinkedHashSet<Integer> mHistoryToRealize = new java.util.LinkedHashSet<>();
    private boolean mRealizingHistory; // Drain reentrant lifecycle work without recursive launches.

    @Override
    public WaitResult startActivityAndWait(IApplicationThread caller, String callingPackage,
            String callingFeatureId, Intent intent, String resolvedType, IBinder resultTo,
            String resultWho, int requestCode, int flags, ProfilerInfo profilerInfo,
            Bundle options, int userId) {
        ActivityRecord[] started = new ActivityRecord[1];
        int code = start(caller, intent, resolvedType, callingPackage, resultTo,
                resultWho, requestCode, launchTaskId(options), shareIdentity(options), started);
        return waitForStarted(code, started[0]);
    }

    /** Read the stored capability on the server and retain its publisher through resolution. */
    GesherService.LaunchResult startShortcutForHost(String pkg, String key) {
        if (mAm == null || !mAm.callers().isSystem()) throw new SecurityException("Host shortcut launch required");
        final ApkPackage owner = mPm.get(pkg);
        if (owner == null || mPm.isRemovalPending(pkg)) throw new SecurityException("Shortcut publisher unavailable");
        Intent intent = com.gesher.app.shell.AppShell.dynamicShortcutIntent(pkg, key, owner.codeIdentity());
        java.util.function.BooleanSupplier previous = mVerifiedLifetime.get();
        String previousCreator = mVerifiedCreator.get();
        long identity = Binder.clearCallingIdentity();
        mVerifiedCreator.set(pkg);
        mVerifiedLifetime.set(() -> mPm.get(pkg) == owner && !mPm.isRemovalPending(pkg)
                && (previous == null || previous.getAsBoolean()));
        try {
            Runnable before = mBeforeShortcutResolution;
            if (before != null) before.run();
            return startActivityForHost(intent);
        }
        finally {
            if (previous == null) mVerifiedLifetime.remove(); else mVerifiedLifetime.set(previous);
            if (previousCreator == null) mVerifiedCreator.remove(); else mVerifiedCreator.set(previousCreator);
            Binder.restoreCallingIdentity(identity);
        }
    }

    /** The host launcher receives authority for this actual launch, never a claimed process name. */
    GesherService.LaunchResult startActivityForHost(Intent intent) {
        if (mAm == null || !mAm.callers().isSystem()) throw new SecurityException("Host launch required");
        if (intent == null) throw new IllegalArgumentException("Missing launch intent");
        ActivityRecord[] started = new ActivityRecord[1];
        String creator = mVerifiedCreator.get();
        int code = start(null, new Intent(intent), intent.getType(), creator == null ? "com.gesher.launcher" : creator,
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
            // complete (here: the activity is idle), or it went away. A trampoline (a
            // launcher activity that starts the app's real one and stays below it,
            // stopped, never idle) completes the launch with that activity: one started
            // after it in its task that is idle (Android's launch transition ends when
            // the task's windows are drawn).
            while (!r.idle && r.state != State.DESTROYED && idleAbove(r) == null) {
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
            // A rejected native start or unexpected pre-idle process death is
            // different from an activity deliberately finishing in onCreate.
            if (r.launchFailed && !r.idle && idleAbove(r) == null) {
                result.result = ActivityManager.START_CANCELED;
            }
        }
        result.totalTime = SystemClock.uptimeMillis() - r.startTime;
        return result;
    }

    /** An activity started after {@code r} in its task that is idle, or null. Guarded by this. */
    private ActivityRecord idleAbove(ActivityRecord r) {
        for (int i = mActivities.size() - 1; i >= 0; i--) {
            ActivityRecord a = mActivities.get(i);
            if (a != r && a.taskId == r.taskId && a.idle && a.state != State.DESTROYED
                    && a.startOrder > r.startOrder) {
                return a;
            }
        }
        return null;
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
        java.util.Collections.reverse(toFinish);
        // Retire the complete history before a fast pause callback can expose
        // and instantiate a parent from the task being removed.
        finishTogether(toFinish.get(0), toFinish, "remove-task",
                android.app.Activity.RESULT_CANCELED, null);
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
        if (maxNum <= 0 || (displayId != Display.INVALID_DISPLAY
                && displayId != Display.DEFAULT_DISPLAY)) return tasks;
        Map<Integer, ActivityManager.RunningTaskInfo> selected = new HashMap<>();
        synchronized (this) {
            // Select tasks by recency, but keep visiting the older activities of
            // selected tasks after the result cap is reached. Otherwise asking for
            // one task incorrectly makes its top activity its base and sole activity.
            for (int i = mActivities.size() - 1; i >= 0; i--) {
                ActivityRecord r = mActivities.get(i);
                if (r.state == State.DESTROYED) continue;
                ActivityManager.RunningTaskInfo t = selected.get(r.taskId);
                if (t != null) {
                    t.numActivities++;
                    t.baseActivity = r.component();
                    t.baseIntent = keepIntentExtra ? new Intent(r.intent)
                            : new Intent(r.intent).replaceExtras((Bundle) null);
                    continue;
                }
                if (tasks.size() >= maxNum) continue;
                t = new ActivityManager.RunningTaskInfo();
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
                selected.put(r.taskId, t);
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
        return startActivityForVerifiedCreator(packageName, intent, resolvedType, lifetime, false);
    }

    int startActivityForVerifiedCreator(String packageName, Intent intent, String resolvedType,
            java.util.function.BooleanSupplier lifetime, boolean shareIdentity) {
        return startActivityForVerifiedCreator(packageName, intent, resolvedType, lifetime,
                null, null, -1, activityOptions(null, shareIdentity));
    }

    private int startActivityForVerifiedCreator(String packageName, Intent intent, String resolvedType,
            java.util.function.BooleanSupplier lifetime, IBinder resultTo, String resultWho,
            int requestCode, Bundle options) {
        return startActivityForVerifiedCreator(packageName, intent, resolvedType, lifetime,
                resultTo, resultWho, requestCode, options, null);
    }

    private int startActivityForVerifiedCreator(String packageName, Intent intent, String resolvedType,
            java.util.function.BooleanSupplier lifetime, IBinder resultTo, String resultWho,
            int requestCode, Bundle options, java.util.function.BooleanSupplier publication) {
        return withVerifiedCreator(packageName, lifetime, publication,
                () -> start(null, intent, resolvedType, packageName, resultTo, resultWho, requestCode,
                        launchTaskId(options), shareIdentity(options), new ActivityRecord[1]));
    }

    int startActivitiesForVerifiedCreator(String packageName, Intent[] intents, String[] resolvedTypes,
            java.util.function.BooleanSupplier lifetime, Bundle options) {
        return withVerifiedCreator(packageName, lifetime, null, () -> intents.length == 1
                ? start(null, intents[0], resolvedTypes[0], packageName, null, null, -1,
                        launchTaskId(options), shareIdentity(options), new ActivityRecord[1])
                : startActivities(null, packageName, null, intents, resolvedTypes, null, options, 0));
    }

    private int withVerifiedCreator(String packageName, java.util.function.BooleanSupplier lifetime,
            java.util.function.BooleanSupplier publication, java.util.function.IntSupplier launch) {
        if (Binder.getCallingPid() != android.os.Process.myPid()
                || mPm.get(packageName) == null && !"android".equals(packageName)) {
            throw new SecurityException("Only a validated local capability may supply a creator");
        }
        if (lifetime == null || !lifetime.getAsBoolean()) throw new SecurityException("Delegated creator retired");
        if (publication != null && !publication.getAsBoolean()) {
            throw new SecurityException("IntentSender holder retired before acceptance");
        }
        String previous = mVerifiedCreator.get();
        java.util.function.BooleanSupplier oldLifetime = mVerifiedLifetime.get();
        java.util.function.BooleanSupplier oldPublication = mVerifiedPublication.get();
        mVerifiedCreator.set(packageName);
        mVerifiedLifetime.set(lifetime);
        mVerifiedPublication.set(publication);
        try {
            return launch.getAsInt();
        } finally {
            if (previous == null) mVerifiedCreator.remove(); else mVerifiedCreator.set(previous);
            if (oldLifetime == null) mVerifiedLifetime.remove(); else mVerifiedLifetime.set(oldLifetime);
            if (oldPublication == null) mVerifiedPublication.remove(); else mVerifiedPublication.set(oldPublication);
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
        return start(caller, intent, resolvedType, callingPackage, resultToToken, resultWho,
                requestCode, launchTaskId, false, started);
    }

    private int start(IApplicationThread caller, Intent intent, String resolvedType,
            String callingPackage, IBinder resultToToken, String resultWho, int requestCode,
            int launchTaskId, boolean shareIdentity, ActivityRecord[] started) {
        synchronized (mLaunchLock) {
            return startSerialized(caller, intent, resolvedType, callingPackage, resultToToken,
                    resultWho, requestCode, launchTaskId, shareIdentity, started);
        }
    }

    private int startSerialized(IApplicationThread caller, Intent intent, String resolvedType,
            String callingPackage, IBinder resultToToken, String resultWho, int requestCode,
            int launchTaskId, boolean shareIdentity, ActivityRecord[] started) {
        final int originalPid = Binder.getCallingPid();
        final int originalUid = Binder.getCallingUid();
        final ProcessRecord originalProcess = callerProcess(originalPid);
        final boolean internalCreator = mVerifiedCreator.get() != null;
        final String originalName = internalCreator ? mVerifiedCreator.get() : callingPackage;
        final ApkPackage originalCallerCode = mPm.get(originalName);
        final java.util.function.BooleanSupplier delegated = mVerifiedLifetime.get();
        final java.util.function.BooleanSupplier publication = mVerifiedPublication.get();
        if (publication != null && !publication.getAsBoolean()) return ActivityManager.START_CANCELED;
        // The legacy self-launcher attaches its real app thread before its
        // first package has been assigned. Enroll that exact process once, through
        // a launcher request for its own installed UID, not a claimed host identity.
        final boolean legacyBootstrap = !internalCreator && originalCallerCode != null
                && originalCallerCode.uid() == originalUid
                && intent != null && Intent.ACTION_MAIN.equals(intent.getAction())
                && intent.hasCategory(Intent.CATEGORY_LAUNCHER)
                && (intent.getFlags() & Intent.FLAG_ACTIVITY_NEW_TASK) != 0
                && resultToToken == null && resultWho == null && requestCode <= 0 && launchTaskId < 0
                && legacyBootstrapProcess(originalProcess, caller, originalPid, originalUid);
        String verifiedCaller = mVerifiedCreator.get();
        if (verifiedCaller == null && mAm != null && !mAm.callers().isSystem()) {
            if (legacyBootstrap) {
                verifiedCaller = originalCallerCode.packageName;
            } else {
                verifiedCaller = mAm.callers().requirePackage(callingPackage);
                boolean matchingThread = false;
                synchronized (this) {
                    for (ProcessRecord app : mProcesses) {
                        if (app.pid == originalPid && !app.exiting
                                && verifiedCaller.equals(app.packageName) && caller != null
                                && app.thread.asBinder().equals(caller.asBinder())) {
                            matchingThread = true;
                            break;
                        }
                    }
                }
                if (!matchingThread) throw new SecurityException("Unrecognized activity caller thread");
            }
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
                        resultWho, requestCode, launchTaskId, shareIdentity, started);
            }
        }
        // Starts from apps resolve implicit intents to filters with CATEGORY_DEFAULT, as
        // ActivityStarter asks (MATCH_DEFAULT_ONLY); gesher-app's launcher (no caller)
        // resolves MAIN/LAUNCHER in a package, as a launcher does.
        ActivityInfo info;
        ApkPackage targetCode;
        synchronized (mPm) {
            info = resolve(intent, resolvedType, caller != null && !legacyBootstrap);
            targetCode = info == null ? null : mPm.get(info.packageName);
        }
        if (legacyBootstrap && (info == null || targetCode != originalCallerCode
                || !originalCallerCode.packageName.equals(info.packageName))) {
            // An unenrolled process cannot reach external-handler or another
            // package's paths by pretending those are its own initial launch.
            throw new SecurityException("Initial launch does not match the attached process package");
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
                // Other Settings screens (SettingsIntents): an app's details in Gesher's
                // Control Center, a device page in Windows Settings. Without this every
                // "App info" was ActivityNotFoundException ("Application not found").
                SettingsIntents.Target settings;
                synchronized (mPm) {
                    settings = SettingsIntents.resolve(intent, pkg -> mPm.get(pkg) != null);
                }
                if (settings != null) {
                    boolean opened = settings.controlCenter
                            ? ControlCenter.open(settings.packageName)
                            : sOpenSettingsPage.test(settings.windowsUri);
                    SystemServer.log("startActivity: " + action + ": " + settings
                            + (opened ? " opened" : " unavailable"));
                    if (!opened) return ActivityManager.START_INTENT_NOT_RESOLVED;
                    // Settings answers a startActivityForResult when the user leaves it;
                    // Windows' page reports nothing, so the request is not left waiting.
                    // Only to an activity of the calling process: another app's token
                    // must not receive a result it did not ask for.
                    ActivityRecord source = resultToToken != null ? forToken(resultToToken) : null;
                    if (source != null && requestCode >= 0 && source.app != null
                            && source.app.pid == originalPid) {
                        deliverResult(source, resultWho, requestCode,
                                android.app.Activity.RESULT_CANCELED, null);
                    }
                    return ActivityManager.START_SUCCESS;
                }
                if (Intent.ACTION_VIEW.equals(action)) {
                    Uri data = intent.getData();
                    if (data != null) {
                        String scheme = data.getScheme();
                        if ("http".equalsIgnoreCase(scheme) || "https".equalsIgnoreCase(scheme)
                                || "mailto".equalsIgnoreCase(scheme)) {
                            // Also a VIEW for the Windows default browser's package
                            // (BrowserPackageSupport, of which resolve() runs nothing).
                            final ActivityRecord resultSource;
                            final ProcessRecord expectedResultProcess;
                            final String resultTargetWho;
                            final int resultTargetCode;
                            synchronized (this) {
                                ActivityRecord source = forToken(resultToToken);
                                // IntentSender already authenticated its holder before clearing
                                // Binder identity. The creator is not the result recipient.
                                boolean authenticated = internalCreator
                                        ? publication != null && publication.getAsBoolean()
                                        : originalProcess != null && source != null
                                            && source.app == originalProcess;
                                if (authenticated && source != null
                                        && (intent.getFlags() & Intent.FLAG_ACTIVITY_FORWARD_RESULT) != 0) {
                                    if (requestCode >= 0) {
                                        return ActivityManager.START_FORWARD_AND_REQUEST_CONFLICT;
                                    }
                                    // The trusted source owns this route even when its
                                    // recipient belongs to another application process.
                                    ActivityRecord recipient = source.resultTo;
                                    resultSource = recipient != null && !isActivityRetired(recipient)
                                            ? recipient : null;
                                    resultTargetWho = source.resultWho;
                                    resultTargetCode = source.requestCode;
                                    source.resultTo = null;
                                    if (resultSource != null) {
                                        // A finishing trampoline may already have queued a
                                        // result. Remove only its result, not another child's
                                        // result that happens to use the same request code.
                                        resultSource.pendingResults.removeIf(result -> {
                                            if (!(result instanceof PendingActivityResult pending)
                                                    || pending.source != source
                                                    || result.mRequestCode != source.requestCode
                                                    || !java.util.Objects.equals(result.mResultWho,
                                                            source.resultWho)) return false;
                                            resultSource.callerState.remove(result.mCallerToken);
                                            return true;
                                        });
                                    }
                                    // A forwarded result belongs to the original activity
                                    // record, whose saved state can outlive its process.
                                    // Its current record/code lifetime is checked at delivery.
                                    expectedResultProcess = null;
                                } else {
                                    resultSource = authenticated && requestCode >= 0 ? source : null;
                                    resultTargetWho = resultWho;
                                    resultTargetCode = requestCode;
                                    expectedResultProcess = resultSource == null ? null : resultSource.app;
                                }
                            }
                            boolean externalTarget = canDelegateExternalUrl(intent, resolvedType);
                            boolean opened = externalTarget && sOpenExternalUrl.test(data.toString());
                            SystemServer.log("startActivity: Windows external URL "
                                    + (opened ? "opened" : "unavailable") + " (" + scheme + ")");
                            // ActivityStarter cancels a failed launch or detaches an explicit
                            // NEW_TASK result. Shell acceptance alone does not mean an ordinary
                            // result-producing activity finished; do not fabricate that completion.
                            boolean cancelResult = !opened
                                    || (intent.getFlags() & Intent.FLAG_ACTIVITY_NEW_TASK) != 0;
                            if (cancelResult && resultSource != null) {
                                deliverResult(resultSource, new ResultInfo(resultTargetWho, resultTargetCode,
                                        android.app.Activity.RESULT_CANCELED, null, null), null, null,
                                        expectedResultProcess);
                            }
                            return opened ? ActivityManager.START_SUCCESS
                                    : !externalTarget && resolutionIntent(intent).getComponent() != null
                                            ? ActivityManager.START_CLASS_NOT_FOUND
                                    : ActivityManager.START_INTENT_NOT_RESOLVED;
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
        final boolean systemCreator = internalCreator && "android".equals(attributedCaller)
                && originalCallerCode == null;
        final java.util.function.BooleanSupplier sourceLifetime = () ->
                (attributedCaller == null || systemCreator || originalCallerCode != null
                    && mPm.get(attributedCaller) == originalCallerCode && !mPm.isRemovalPending(attributedCaller))
                && (delegated == null || delegated.getAsBoolean());
        // A normal app must still be the authenticated process when its request is
        // accepted. Once accepted, its exit does not cancel a queued target launch;
        // the retained lifetime is the package/delegated capability generation.
        final java.util.function.BooleanSupplier sourceCurrent = () ->
                sourceLifetime.getAsBoolean()
                && (attributedCaller == null || internalCreator || originalProcess != null
                    && callerProcess(originalPid) == originalProcess && !originalProcess.exiting)
                && (!legacyBootstrap
                    || legacyBootstrapProcess(originalProcess, caller, originalPid, originalUid))
                && (publication == null || publication.getAsBoolean());
        Intent launch = new Intent(intent);
        launch.setComponent(new ComponentName(info.packageName, info.name));
        final ActivityCallerState.Origin callerOrigin;
        final ActivityCallerState.Snapshot callerSnapshot;
        synchronized (mPm) {
            if (!sourceCurrent.getAsBoolean()) return ActivityManager.START_CANCELED;
            if (mAm == null) throw new IllegalStateException("Activity caller permission service unavailable");
            callerOrigin = attributedCaller == null || systemCreator
                    ? new ActivityCallerState.Origin(null, systemCreator ? android.os.Process.SYSTEM_UID
                            : Binder.getCallingUid(), systemCreator ? "android" : null, true, shareIdentity)
                    : new ActivityCallerState.Origin(originalCallerCode, originalCallerCode.uid(),
                            attributedCaller, false, shareIdentity);
            callerSnapshot = ActivityCallerState.capture(callerOrigin, launch, mAm.uriGrants());
        }
        final List<UriGrants.Grant> preparedGrants = mAm.uriGrants().prepareIntent(
                callerOrigin.uid, info.packageName, launch);
        int callingPid = Binder.getCallingPid();
        ActivityRecord source = resultToToken != null ? forToken(resultToToken) : null;
        StackLaunch stack = mStackLaunch.get();
        boolean publishHistory = stack != null && stack.intent == intent;
        int launchFlags = publishHistory ? historyLaunchFlags(info, intent, source) : intent.getFlags();
        launch.setFlags(launchFlags);
        // Whether it leaves the activity below it visible (its theme), read before the lock.
        boolean translucent = ActivityTheme.isTranslucentOrFloating(info);
        ActivityRecord r = null;
        ActivityRecord deliverTo = null;
        ActivityRecord reusedTaskTop = null;
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
            if (legacyBootstrap && (processFor(info.packageName,
                    componentProcess(info.packageName, info.processName)) != null
                    || starting(info.packageName, componentProcess(info.packageName, info.processName)))) {
                return ActivityManager.START_CANCELED;
            }
            // startActivityIntentSender authenticated this result activity before
            // clearing Binder identity. Creator authority still controls the target;
            // window visibility follows the live recipient that requested its UI.
            boolean delegatedResult = internalCreator && source != null && source.app != null;
            sourceWindowMode = publishHistory && source != null && source.batchOwner == stack.owner
                    ? source.inheritedWindowMode : sourceWindowMode(delegatedResult ? source.app.thread : caller,
                    delegatedResult ? source.info.packageName : callingPackage, source,
                    delegatedResult ? source.app.pid : callingPid,
                    resultToToken == null, info.packageName,
                    componentProcess(info.packageName, info.processName));
            int taskId = publishHistory ? historyTaskId(info, launch, source, launchTaskId)
                    : launchTaskId >= 0 && topOf(launchTaskId) != null ? launchTaskId
                    : source != null && (launchFlags & Intent.FLAG_ACTIVITY_NEW_TASK) == 0
                    ? source.taskId : mNextTaskId++;
            ActivityRecord top = topOf(taskId);
            boolean clearTask = publishHistory && (launchFlags & Intent.FLAG_ACTIVITY_NEW_TASK) != 0
                    && (launchFlags & Intent.FLAG_ACTIVITY_CLEAR_TASK) != 0;
            if (clearTask) {
                for (int i = mActivities.size() - 1; i >= 0; i--) {
                    ActivityRecord activity = mActivities.get(i);
                    if (activity.taskId == taskId && !activity.finishing) clearTop.add(activity);
                }
                top = null;
            }
            boolean documentIntoExisting = publishHistory
                    && (launchFlags & Intent.FLAG_ACTIVITY_NEW_DOCUMENT) != 0
                    && (launchFlags & Intent.FLAG_ACTIVITY_MULTIPLE_TASK) == 0;
            boolean singleTop = mAccountCompletion.get() == null
                    && (info.launchMode == ActivityInfo.LAUNCH_SINGLE_TOP
                    || publishHistory && (info.launchMode == ActivityInfo.LAUNCH_SINGLE_TASK
                            || info.launchMode == ActivityInfo.LAUNCH_SINGLE_INSTANCE
                            || info.launchMode == ActivityInfo.LAUNCH_SINGLE_INSTANCE_PER_TASK)
                    || documentIntoExisting
                    || (launchFlags & Intent.FLAG_ACTIVITY_SINGLE_TOP) != 0);
            boolean clearToInstance = (launchFlags & Intent.FLAG_ACTIVITY_CLEAR_TOP) != 0
                    || documentIntoExisting
                    || publishHistory && (info.launchMode == ActivityInfo.LAUNCH_SINGLE_TASK
                            || info.launchMode == ActivityInfo.LAUNCH_SINGLE_INSTANCE
                            || info.launchMode == ActivityInfo.LAUNCH_SINGLE_INSTANCE_PER_TASK);
            ActivityRecord below = !clearTask && clearToInstance
                    && (singleTop || publishHistory) ? belowInTask(taskId, launch.getComponent()) : null;
            ActivityRecord reordered = publishHistory && !clearTask && !clearToInstance
                    && (launchFlags & Intent.FLAG_ACTIVITY_REORDER_TO_FRONT) != 0
                    ? belowInTask(taskId, launch.getComponent()) : null;
            ActivityRecord taskRoot = rootOf(taskId);
            if (top != null && activityCodeCurrent(top) && top.component().equals(launch.getComponent())
                    && singleTop) {
                mAm.uriGrants().publish(top.token, preparedGrants);
                deliverTo = top;
            } else if (below != null && singleTop) {
                // Task.performClearTop retains a singleTop/singleTask/document
                // destination, including one whose client has not been created.
                mAm.uriGrants().publish(below.token, preparedGrants);
                deliverTo = below;
                for (int i = mActivities.size() - 1; i >= 0; i--) {
                    ActivityRecord a = mActivities.get(i);
                    if (a == below) break;
                    if (a.taskId == taskId && !a.finishing && a.state != State.DESTROYED) {
                        clearTop.add(a);
                    }
                }
            } else if (reordered != null) {
                mAm.uriGrants().publish(reordered.token, preparedGrants);
                mActivities.remove(reordered);
                mActivities.add(reordered);
                deliverTo = reordered;
            } else if (publishHistory && top != null && !clearTask && !clearToInstance
                    && (launchFlags & Intent.FLAG_ACTIVITY_REORDER_TO_FRONT) == 0
                    && (launchFlags & Intent.FLAG_ACTIVITY_NEW_TASK) != 0
                    && launchTaskId < 0 && taskRoot != null
                    && taskRoot.intent.filterEquals(launch)) {
                // ActivityStarter.complyActivityFlags: a matching NEW_TASK root
                // brings its existing task forward; it does not duplicate the root.
                reusedTaskTop = top;
            } else {
                if (below != null) {
                    // Standard-mode CLEAR_TOP recreates the matching instance as
                    // well as removing the records above it.
                    for (int i = mActivities.size() - 1; i >= 0; i--) {
                        ActivityRecord activity = mActivities.get(i);
                        if (activity.taskId == taskId && !activity.finishing) clearTop.add(activity);
                        if (activity == below) break;
                    }
                }
                ActivityRecord resultRecord = requestCode >= 0 ? source : null;
                r = new ActivityRecord(info, launch, taskId, callingPid, callingPackage,
                        resultRecord, resultWho, requestCode, targetCode);
                mAm.uriGrants().publish(r.token, preparedGrants);
                r.callerState.add(r.initialCallerInfoAccessToken, callerSnapshot);
                r.sourceLifetime = sourceLifetime;
                r.accountCompletion = mAccountCompletion.get();
                r.inheritedWindowMode = sourceWindowMode;
                r.translucent = translucent;
                r.batchOwner = publishHistory ? stack.owner : null;
                r.deferredHistory = publishHistory;
                r.historyEntry = publishHistory;
                // A start in a task moved to the back brings it to the front.
                mHiddenTasks.remove(taskId);
                // The activity in front in its task is paused first; other tasks' stay
                // resumed (multi-resume: each visible task's top activity is resumed).
                ActivityRecord front = publishHistory ? null : front(r);
                mActivities.add(r);
                updateIdle();
                focusTaskLocked(taskId);
                // Its orientation may turn the task's display (windows3 track).
                if (!publishHistory && updateRotationLocked(taskId)) {
                    for (ActivityRecord a : mActivities) {
                        if (a != r && a.taskId == taskId && !a.finishing && a.app != null) {
                            reconfigure.add(a);
                        }
                    }
                }
                if (!publishHistory) {
                    ProcessRecord app = legacyBootstrap ? originalProcess
                            : processFor(info.packageName, componentProcess(info.packageName, info.processName));
                    if (legacyBootstrap) {
                        assignPackage(app, info.packageName, componentProcess(info.packageName, info.processName));
                        bind = app;
                    }
                    if (app != null) {
                        attach(r, app);
                    } else {
                        needProcess = true;
                        for (PendingProcess pending : mStarting.values()) {
                            if (pendingCurrent(pending) && pending.packageName.equals(info.packageName)
                                    && pending.processName.equals(componentProcess(info.packageName, info.processName))) {
                                r.launchGeneration = pending.exitGeneration; break;
                            }
                        }
                    }
                    if (front != null) {
                        r.waitingFor = front;
                        if (front.state != State.PAUSING) pause = front;
                    } else {
                        launchNow = r.waitingFor == null && app != null && app.ready;
                    }
                }
            }
            if (deliverTo != null || reusedTaskTop != null) {
                mHiddenTasks.remove(taskId);
                focusTaskLocked(taskId);
            }
        }
        if (reusedTaskTop != null) {
            started[0] = reusedTaskTop;
            return ActivityManager.START_TASK_TO_FRONT;
        }
        if (deliverTo != null) {
            started[0] = deliverTo;
            SystemServer.log("START u0 {" + launch.toShortString(false, true, true, false)
                    + "} from " + callerName(callingPackage, callingPid)
                    + (clearTop.isEmpty() ? ": delivered to the top activity " + deliverTo.name()
                            + " (singleTop)" : ": delivered to " + deliverTo.name()
                            + " (clear top: the " + clearTop.size() + " above it finish)"));
            if (!clearTop.isEmpty()) finishTogether(clearTop.get(0), clearTop,
                    "clear-top", android.app.Activity.RESULT_CANCELED, null);
            // The authenticated request was accepted above. CLEAR_TOP may itself
            // finish the sender/IntentSender holder; that must not erase this
            // delivery. Creator code/capability and target generations remain live.
            deliverNewIntent(deliverTo, launch, callingPackage, sourceLifetime, callerSnapshot);
            return ActivityManager.START_DELIVERED_TO_TOP;
        }
        started[0] = r;
        ActivityRecord launchResultTarget = r.resultTo;
        SystemServer.log("START u0 {" + launch.toShortString(false, true, true, false) + "} from "
                + callerName(callingPackage, callingPid) + ": task " + r.taskId
                + (r.app != null ? ", in " + r.app : ", waiting for a process")
                + (r.waitingFor != null ? ", after pausing " + r.waitingFor.name() : "")
                + (r.translucent ? ", translucent or floating (the activity below stays"
                        + " visible)" : "")
                + (launchResultTarget != null ? ", result to " + launchResultTarget.name() + " (request "
                        + r.requestCode + ")" : ""));
        if (!clearTop.isEmpty()) finishTogether(clearTop.get(0), clearTop, "clear-task-history",
                android.app.Activity.RESULT_CANCELED, null);
        if (bind != null) bindApplication(bind);
        if (needProcess) startProcess(info.packageName, componentProcess(info.packageName, info.processName),
                "activity " + r.name(), sourceWindowMode);
        if (pause != null) pause(pause, false);
        if (launchNow) realStartActivity(r);
        for (ActivityRecord a : reconfigure) ensureConfiguration(a);
        if (!r.deferredHistory) updateTopResumed();
        return ActivityManager.START_SUCCESS;
    }

    private static String callerName(String callingPackage, int callingPid) {
        return callingPackage != null ? callingPackage : "pid " + callingPid;
    }

    /** ActivityStarter's manifest/document flags, before selecting a history member's task. */
    private static int historyLaunchFlags(ActivityInfo info, Intent intent, ActivityRecord source) {
        int flags = intent.getFlags();
        boolean single = info.launchMode == ActivityInfo.LAUNCH_SINGLE_TASK
                || info.launchMode == ActivityInfo.LAUNCH_SINGLE_INSTANCE;
        if (single && (flags & Intent.FLAG_ACTIVITY_NEW_DOCUMENT) != 0) {
            flags &= ~(Intent.FLAG_ACTIVITY_NEW_DOCUMENT | Intent.FLAG_ACTIVITY_MULTIPLE_TASK);
        } else if (info.documentLaunchMode == ActivityInfo.DOCUMENT_LAUNCH_INTO_EXISTING
                || info.documentLaunchMode == ActivityInfo.DOCUMENT_LAUNCH_ALWAYS) {
            flags |= Intent.FLAG_ACTIVITY_NEW_DOCUMENT;
        } else if (info.documentLaunchMode == ActivityInfo.DOCUMENT_LAUNCH_NEVER
                && (info.launchMode != ActivityInfo.LAUNCH_SINGLE_INSTANCE_PER_TASK
                    || (flags & Intent.FLAG_ACTIVITY_NEW_DOCUMENT) != 0)) {
            flags &= ~(Intent.FLAG_ACTIVITY_NEW_DOCUMENT | Intent.FLAG_ACTIVITY_MULTIPLE_TASK);
        }
        if (source == null || source.info.launchMode == ActivityInfo.LAUNCH_SINGLE_INSTANCE
                || single || info.launchMode == ActivityInfo.LAUNCH_SINGLE_INSTANCE_PER_TASK
                || (flags & Intent.FLAG_ACTIVITY_NEW_DOCUMENT) != 0) {
            flags |= Intent.FLAG_ACTIVITY_NEW_TASK;
        }
        if ((flags & Intent.FLAG_ACTIVITY_NEW_TASK) != 0
                && info.documentLaunchMode == ActivityInfo.DOCUMENT_LAUNCH_ALWAYS) {
            flags |= Intent.FLAG_ACTIVITY_MULTIPLE_TASK;
        }
        return flags;
    }

    /** Batch members use ActivityStarter's source/NEW_TASK choice, never a forced first task. */
    private int historyTaskId(ActivityInfo info, Intent intent, ActivityRecord source, int requestedTask) {
        if (requestedTask >= 0 && topOf(requestedTask) != null) return requestedTask;
        int flags = intent.getFlags();
        boolean singleTask = info.launchMode == ActivityInfo.LAUNCH_SINGLE_TASK
                || info.launchMode == ActivityInfo.LAUNCH_SINGLE_INSTANCE;
        if ((flags & Intent.FLAG_ACTIVITY_NEW_TASK) == 0 && source != null
                && !source.finishing && mActivities.contains(source)) return source.taskId;
        if ((flags & Intent.FLAG_ACTIVITY_MULTIPLE_TASK) == 0 || singleTask) {
            int affinityTask = -1;
            for (int i = mTaskOrder.size() - 1; i >= 0; i--) {
                int task = mTaskOrder.get(i);
                if (info.launchMode == ActivityInfo.LAUNCH_SINGLE_INSTANCE) {
                    if (belowInTask(task, intent.getComponent()) != null) return task;
                    continue;
                }
                ActivityRecord root = rootOf(task);
                if (root == null) continue;
                boolean component = root.component().equals(intent.getComponent());
                if (info.launchMode == ActivityInfo.LAUNCH_SINGLE_INSTANCE_PER_TASK && !component) continue;
                if ((flags & Intent.FLAG_ACTIVITY_NEW_DOCUMENT) != 0) {
                    if ((root.intent.getFlags() & Intent.FLAG_ACTIVITY_NEW_DOCUMENT) != 0
                            && component && java.util.Objects.equals(root.intent.getData(), intent.getData())) return task;
                } else if ((root.intent.getFlags() & Intent.FLAG_ACTIVITY_NEW_DOCUMENT) == 0) {
                    if (component) return task;
                    if (affinityTask < 0 && info.taskAffinity != null
                            && info.taskAffinity.equals(root.info.taskAffinity)) affinityTask = task;
                }
            }
            if (affinityTask >= 0) return affinityTask;
        }
        return mNextTaskId++;
    }

    private ActivityRecord rootOf(int taskId) {
        for (ActivityRecord activity : mActivities) {
            if (activity.taskId == taskId && !activity.finishing && activity.state != State.DESTROYED) return activity;
        }
        return null;
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
        intent = resolutionIntent(intent);
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

    /** An unresolved app-specific or filter-constrained request must not become a shell launch. */
    private boolean canDelegateExternalUrl(Intent intent, String resolvedType) {
        Intent target = resolutionIntent(intent);
        if ("mailto".equalsIgnoreCase(intent.getScheme())) {
            // No synthetic Android mail package is advertised. Only an implicit
            // mail viewer request may use the Windows protocol handler.
            if (target.getComponent() != null || target.getPackage() != null) return false;
            IntentFilter mail = new IntentFilter(Intent.ACTION_VIEW);
            mail.addCategory(Intent.CATEGORY_DEFAULT);
            mail.addCategory(Intent.CATEGORY_BROWSABLE);
            mail.addDataScheme("mailto");
            return mail.match(target.getAction(), resolvedType != null ? resolvedType : target.getType(),
                    target.getScheme(), target.getData(), target.getCategories(), "gesher") >= 0;
        }
        return mPm.get(BrowserPackageSupport.PACKAGE) == null
                && !BrowserPackageSupport.queryIntentActivities(target, resolvedType,
                        PackageManager.MATCH_DEFAULT_ONLY).isEmpty();
    }

    private static Intent resolutionIntent(Intent intent) {
        return intent.getComponent() == null && intent.getSelector() != null ? intent.getSelector() : intent;
    }

    /**
     * The topmost live history record of {@code component}, including a record
     * whose client has not been created yet; or null. Guarded by this.
     */
    private ActivityRecord belowInTask(int taskId, ComponentName component) {
        for (int i = mActivities.size() - 1; i >= 0; i--) {
            ActivityRecord a = mActivities.get(i);
            if (a.taskId != taskId || a.finishing || a.state == State.DESTROYED) {
                continue;
            }
            if (a.component().equals(component)) return a;
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
            if (packageName.equals(p.packageName) && !p.exiting && !p.dead && !p.pidSuperseded
                    && (p.packageCode == null || p.packageCode == mPm.get(packageName))
                    && (p.enabledProcessLifetime == null || p.enabledProcessLifetime.getAsBoolean())
                    && processName.equals(componentProcess(p.packageName, p.processName))) return p;
        }
        return null;
    }

    /**
     * The package of the attached process that owns {@code pid}, until that process dies.
     * A process told to exit (idle exit, a closed last window, force-stop) still runs
     * app code until it is gone: Application.onTerminate, queued main-looper work,
     * listeners. On Android its calls keep their Binder UID identity until the kernel
     * reaps it, so system services answer them instead of failing with a
     * SecurityException that the app would turn into a crash. Lifecycle admission
     * (new work for an exiting process) is decided elsewhere, through
     * {@link #callerProcess}. Only the latest accepted attachment owns the PID: its
     * exit, death or removal must never restore an older record's package identity.
     */
    synchronized String authenticatedPackageForPid(int pid) {
        ProcessRecord app = attachedProcessByPid(pid);
        return app != null && !app.dead ? app.packageName : null;
    }

    /** One-time enrollment in a mode that already permits real self-attachment. */
    private synchronized boolean legacyBootstrapProcess(ProcessRecord app,
            IApplicationThread thread, int pid, int uid) {
        // Match attachApplication's startSeq==0 admission: a configured starter
        // may coexist with legacy self-launches in the nonprotected mode. The
        // protected spawner-owned device still requires a verified start sequence.
        return (mStarter == null || !GmsSignatureSpoofing.isEnabled())
                && app != null && thread != null && app.thread != null
                && mProcesses.contains(app) && processByPid(pid) == app
                && app.pid == pid && app.uid == uid && !app.exiting && !app.spawned
                && !app.ready && app.startSeq == 0 && app.packageName == null && app.processName == null
                && app.thread.asBinder().equals(thread.asBinder());
    }

    /** Binds an attached process to a package without reusing an older start's exit. Guarded by this. */
    private void assignPackage(ProcessRecord app, String packageName, String processName) {
        app.packageName = packageName;
        app.processName = processName;
        if (app.packageCode == null) app.packageCode = mPm.get(packageName);
        if (app.enabledProcessLifetime == null && app.packageCode != null) {
            app.enabledProcessLifetime = app.packageCode.enabledProcessLifetime();
        }
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
            if (p.packageName != null || p.pidSuperseded || p.exiting || p.dead) continue;
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
            if (pendingCurrent(p) && p.packageName.equals(packageName)
                    && p.processName.equals(processName)) return true;
        }
        return false;
    }

    private boolean pendingCurrent(PendingProcess pending) {
        return !pending.cancelled && pending.owner != null
                && mPm.get(pending.packageName) == pending.owner
                && !mPm.isRemovalPending(pending.packageName)
                && pending.enabledLifetime != null && pending.enabledLifetime.getAsBoolean();
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
        final ApkPackage owner;
        final ApplicationInfo ai;
        final java.util.function.BooleanSupplier enabledLifetime;
        synchronized (mPm) {
            owner = mPm.get(packageName);
            if (owner == null || !owner.matchesEnabledApplication(0) || mPm.isRemovalPending(packageName)) return;
            ai = owner.applicationInfo(0);
            enabledLifetime = owner.enabledProcessLifetime();
        }
        long seq;
        PendingProcess pending;
        synchronized (this) {
            if (mPm.get(packageName) != owner || !owner.matchesEnabledApplication(0)
                    || mPm.isRemovalPending(packageName) || !enabledLifetime.getAsBoolean()) return;
            if (processFor(packageName, processName) != null || starting(packageName, processName)) return;
            seq = mNextStartSeq++;
            mExitStatus.started(processKey(packageName, processName), seq);
            WindowManagerService.WindowPolicy policy =
                    WindowManagerService.resolveWindowPolicy(packageName, sourceMode);
            pending = new PendingProcess(packageName, processName, reason, policy.mode, policy.explicit);
            pending.owner = owner;
            pending.enabledLifetime = enabledLifetime;
            pending.exitGeneration = mExitStatus.capture(processKey(packageName, processName), seq);
            for (ActivityRecord record : mActivities) {
                if (record.app == null && record.launchGeneration == null
                        && record.state == State.INITIALIZING && !record.finishing && !record.deferredHistory
                        && record.info.packageName.equals(packageName)
                        && componentProcess(packageName, record.info.processName).equals(processName)) {
                    record.launchGeneration = pending.exitGeneration;
                }
            }
            for (PendingWork work : mPendingWork) {
                if (work.launchGeneration == null && work.current(mPm)
                        && work.packageName.equals(packageName) && work.processName.equals(processName)) {
                    work.launchGeneration = pending.exitGeneration;
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
            // Spawn retains its original process handle. Capture that handle's identity
            // before publishing pid (attach waits for it) or creating the only exit
            // watcher that can consume the handle. An attach-time PID lookup is too
            // late: an exited child might have been consumed/replaced by then.
            pending.nativeStartTime = Zygote.childStartTime(pid);
            pending.pid = pid;
            mExitStatus.bind(pending.exitGeneration, pid);
            if (!pendingCurrent(pending)) {
                pending.cancelled = true;
                Zygote.killChild(pid, pending.nativeStartTime);
            }
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
        java.util.Set<Integer> affectedTasks = new java.util.LinkedHashSet<>();
        synchronized (this) {
            if (mStarting.get(seq) != pending) return;
            mExitStatus.unavailable(pending.exitGeneration);
            mStarting.remove(seq);
            for (int i = mActivities.size() - 1; i >= 0; i--) {
                ActivityRecord r = mActivities.get(i);
                if (r.app == null && !r.deferredHistory && r.launchGeneration == pending.exitGeneration) {
                    r.launchFailed = true;
                    r.state = State.DESTROYED;
                    accountActivityFinished(r);
                    affectedTasks.add(r.taskId);
                    mActivities.remove(i);
                    if (mAm != null && mAm.uriGrants() != null) mAm.uriGrants().revokeByOwner(r.token);
                }
            }
            mPendingWork.removeIf(w -> w.launchGeneration == pending.exitGeneration);
            updateIdle();
            notifyAll();
        }
        for (int taskId : affectedTasks) realizeTaskHistory(taskId);
    }

    /** Whether some activity exists (launching, shown or stopped). Guarded by this. */
    private boolean hasActivityLocked() {
        if (!mActivities.isEmpty()) return true;
        for (ProcessRecord p : mProcesses) {
            if (!p.activities.isEmpty()) return true;
        }
        return false;
    }

    /** A process is being started or has not attached yet. Guarded by this. */
    private boolean startingLocked() {
        if (!mStarting.isEmpty()) return true;
        for (ProcessRecord p : mProcesses) {
            if (!p.ready) return true;
        }
        return false;
    }

    /**
     * An activity restarts the idle clock; without one it runs (from the activity's end).
     * Whether a background process keeps the device busy is asked by {@link #idleSince}.
     * Guarded by this.
     */
    private void updateIdle() {
        if (hasActivityLocked()) {
            mIdleSince = 0;
        } else if (mIdleSince == 0) {
            mIdleSince = SystemClock.uptimeMillis();
        }
    }

    /**
     * Since when the device is idle (for its idle shutdown); 0 while it is busy.
     *
     * <p>Busy: some activity exists, or an app process (GMS's aside, as before) is perceptible
     * to the user without one: it runs a foreground service
     * ({@link ActivityManagerService#keepsDeviceBusy}). Both restart the idle clock. An app
     * process with neither is cached, as Android ranks a process whose activities are gone:
     * it is kept (for a fast relaunch) but does not hold the device, also when it runs a
     * started background service or is bound by the system or by another cached process
     * (a binding from a busy client keeps the device anyway). A process being started (for a
     * job, an alarm, a broadcast) holds the device until it has attached (0 is returned) but
     * does not restart the clock, so a cached app woken every few seconds by its own
     * background work does not keep the device forever.
     */
    long idleSince() {
        List<ProcessRecord> background = new ArrayList<>();
        synchronized (this) {
            updateIdle();
            if (mIdleSince == 0) return 0;
            for (ProcessRecord p : mProcesses) {
                if (p.ready && !p.exiting) background.add(p);
            }
        }
        // Outside the lock, as maybeExit asks keepsAlive: the services take their own.
        boolean busy = false;
        ActivityManagerService am = mAm;
        if (am != null) {
            for (ProcessRecord p : background) {
                if (am.keepsDeviceBusy(p)) {
                    busy = true;
                    break;
                }
            }
        }
        synchronized (this) {
            if (busy) mIdleSince = 0;
            else updateIdle();
            if (mIdleSince == 0 || startingLocked()) return 0;
            // One that attached meanwhile is asked about at the next call.
            for (ProcessRecord p : mProcesses) {
                if (!p.exiting && !background.contains(p)) return 0;
            }
            return mIdleSince;
        }
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
        // Work in it just ended: the idle reaper counts from now.
        app.markActive(SystemClock.uptimeMillis());
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

    private boolean hasPendingWorkLocked(ProcessRecord app) {
        for (PendingWork w : mPendingWork) {
            if (w.packageName.equals(app.packageName) && w.processName.equals(app.processName)) {
                return true;
            }
        }
        return false;
    }

    /** Final validation while service and broadcast admission are frozen by AM. */
    synchronized boolean commitIdleReap(ProcessRecord app, long now, long idleMs) {
        if (app.exiting || !app.ready || !app.spawned || !mProcesses.contains(app)
                || !app.activities.isEmpty() || hasPendingWorkLocked(app)) return false;
        long active = app.lastActive;
        if (now - active < idleMs) return false;
        return mAm.retireUnusedProviders(app, active);
    }

    /**
     * The idle reaper: what OomAdjuster and the low-memory killer do to cached processes
     * (ADR 0082). {@link #maybeExit} ends a process as soon as nothing keeps it; but a
     * process can be kept by something that is no use to anybody (a service bound only by
     * its own process, a provider whose clients released it, a GMS sibling process). This
     * ends the processes this server started that are cached, with no activity, no work
     * waiting, nothing in use ({@link ActivityManagerService#inUse}) and not persistent
     * ({@link ActivityManagerService#isPersistent}), once nothing used them for
     * {@code idleMs}. Whatever needs one again (a service bind, a provider acquisition, a
     * broadcast) starts a new process.
     */
    void reapIdleProcesses(long idleMs) {
        long now = SystemClock.uptimeMillis();
        List<ProcessRecord> candidates = new ArrayList<>();
        synchronized (this) {
            for (ProcessRecord p : mProcesses) {
                if (p.exiting || !p.ready || !p.spawned || p.packageName == null) continue;
                if (!p.activities.isEmpty() || hasPendingWorkLocked(p)) {
                    p.markActive(now);
                    continue;
                }
                candidates.add(p);
            }
        }
        for (ProcessRecord p : candidates) {
            if (mAm.isPersistent(p)) continue;
            if (mAm.inUse(p)) {
                p.markActive(now);
                continue;
            }
            long idle = now - p.lastActive;
            if (idle < idleMs) continue;
            if (!mAm.commitIdleReap(p, now, idleMs)) continue;
            mAm.processExiting(p);
            SystemServer.log("reaping: " + p + " is cached (no activity, nothing in use) and"
                    + " was idle for " + (idle / 1000) + " s");
            try {
                p.thread.scheduleExit();
            } catch (RemoteException e) {
                SystemServer.log("reaping: " + e);
            }
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
            // Force stop removes transient grants both from and to the package,
            // including ownerless grants and packages with no live process.
            // Do this before teardown callbacks can admit a replacement.
            if (mAm != null && mAm.uriGrants() != null) mAm.uriGrants().clear(packageName);
            mPendingWork.removeIf(work -> packageName.equals(work.packageName));
            // Service teardown can synchronously report process death. Retire
            // captured activities first so saved state cannot turn an explicit
            // force stop into ordinary crash restoration.
            for (ActivityRecord activity : mActivities) {
                if (packageName.equals(activity.info.packageName)) activity.finishing = true;
            }
            for (ActivityRecord activity : new ArrayList<>(mActivities)) {
                if (activity.app != null || !packageName.equals(activity.info.packageName)) continue;
                activity.finishing = true; activity.state = State.DESTROYED;
                mExitStatus.unavailable(activity.launchGeneration);
                accountActivityFinished(activity); mActivities.remove(activity);
                if (mAm != null && mAm.uriGrants() != null) mAm.uriGrants().revokeByOwner(activity.token);
            }
            for (ProcessRecord p : mProcesses) {
                if (packageName.equals(p.packageName)) {
                    p.exiting = true;
                    stop.add(p);
                }
            }
            for (Map.Entry<Long, PendingProcess> entry : new ArrayList<>(mStarting.entrySet())) {
                PendingProcess p = entry.getValue();
                if (!p.packageName.equals(packageName)) continue;
                p.cancelled = true;
                if (p.pid > 0) Zygote.killChild(p.pid, p.nativeStartTime);
                else startFailed(entry.getKey(), p);
            }
            notifyAll();
        }
        // Also cancel sticky-service restarts when the old process has died.
        if (mAm != null) mAm.stopServicesForPackage(packageName);
        if (stop.isEmpty()) return;
        SystemServer.log("force-stopping " + packageName);
        endProcesses(stop, 5000);
    }

    /** Restart existing processes after an enabled-state change, without force-stop state. */
    void restartForEnabledState(ApkPackage expected) {
        restartForEnabledState(expected, () -> true);
    }

    void restartForEnabledState(ApkPackage expected, java.util.function.BooleanSupplier currentChange) {
        List<ProcessRecord> processes = new ArrayList<>();
        java.util.Set<String> restart = new java.util.LinkedHashSet<>();
        synchronized (this) {
            // An event queued before update/uninstall cannot stop its replacement.
            if (mPm.get(expected.packageName) != expected || mPm.isRemovalPending(expected.packageName)
                    || !currentChange.getAsBoolean()) return;
            for (ProcessRecord app : mProcesses) {
                if (expected.packageName.equals(app.packageName) && !app.exiting
                        && !app.dead && !app.pidSuperseded
                        && (app.enabledProcessLifetime == null || !app.enabledProcessLifetime.getAsBoolean())) {
                    processes.add(app);
                }
            }
            for (Map.Entry<Long, PendingProcess> entry : new ArrayList<>(mStarting.entrySet())) {
                PendingProcess pending = entry.getValue();
                if (pending.owner != expected || pending.enabledLifetime == null
                        || pending.enabledLifetime.getAsBoolean()) continue;
                pending.cancelled = true;
                mStarting.remove(entry.getKey());
                mExitStatus.unavailable(pending.exitGeneration);
                if (pending.pid > 0) Zygote.killChild(pending.pid, pending.nativeStartTime);
                for (ActivityRecord activity : mActivities) {
                    if (activity.app == null && activity.launchGeneration == pending.exitGeneration
                            && activityEnabled(activity)) activity.launchGeneration = null;
                }
                for (PendingWork work : mPendingWork) {
                    if (work.launchGeneration == pending.exitGeneration) work.launchGeneration = null;
                }
            }
            notifyAll();
        }
        for (ProcessRecord app : processes) {
            synchronized (this) {
                // A preceding Binder callback can synchronously retire a later record,
                // replace its PID, or update its package. Recheck at each dispatch.
                if (mPm.get(expected.packageName) != expected || mPm.isRemovalPending(expected.packageName)
                        || !currentChange.getAsBoolean()) return;
                if (!mProcesses.contains(app) || app.exiting || app.dead || app.pidSuperseded
                        || attachedProcessByPid(app.pid) != app
                        || app.enabledProcessLifetime != null && app.enabledProcessLifetime.getAsBoolean()) continue;
                // As cached retirement, keep recovery for still-enabled services.
                app.reaped = true;
                app.recoverAfterPidSupersession = false;
                app.exiting = true;
                for (ActivityRecord activity : new ArrayList<>(app.activities)) {
                    if (activity.state == State.INITIALIZING && !activity.finishing && activityEnabled(activity)) {
                        app.activities.remove(activity);
                        activity.app = null;
                        activity.launchGeneration = null;
                    }
                }
                // Native code checks and terminates the same retained child handle
                // under its registry lock. No PID reopen can hit a new OS process;
                // holding ATM here also excludes logical attachment supersession.
                if (app.spawned && Zygote.killChild(app.pid, app.nativeStartTime)) continue;
            }
            // A missing/retired native handle can still have a live application-thread
            // capability (custom starters). Use that exact Binder, never a PID fallback.
            try { app.thread.scheduleExit(); }
            catch (RemoteException ignored) { }
        }
        synchronized (this) {
            if (mPm.get(expected.packageName) != expected || mPm.isRemovalPending(expected.packageName)) return;
            for (PendingWork work : mPendingWork) {
                if (expected.packageName.equals(work.packageName) && work.current(mPm)) restart.add(work.processName);
            }
            for (ActivityRecord activity : mActivities) {
                if (activity.targetCode == expected && activity.app == null && !activity.finishing
                        && activityEnabled(activity)) restart.add(componentProcess(expected.packageName, activity.info.processName));
            }
        }
        for (String process : restart) startProcess(expected.packageName, process, "enabled-state change");
    }

    /** Cancel disabled admission and finish only the affected activities, even with KEEP. */
    void cleanupDisabledPackageComponents(PackageManagerService.EnabledChange change) {
        List<ActivityRecord> finish = new ArrayList<>();
        List<PendingWork> canceled = new ArrayList<>();
        synchronized (this) {
            if (!change.current(mPm)) return;
            mPendingWork.removeIf(work -> {
                boolean disabled = work.component == null
                    ? work.owner == change.pkg && change.disablesApplication(mPm)
                    : change.disables(mPm, work.owner, work.component, work.kind);
                if (disabled) canceled.add(work);
                return disabled;
            });
            for (ActivityRecord activity : mActivities) {
                if (change.disables(mPm, activity.targetCode, activity.info.name, ApkPackage.ComponentKind.ACTIVITY)) finish.add(activity);
            }
            updateIdle();
            notifyAll();
        }
        for (PendingWork work : canceled) work.canceled.run();
        for (ActivityRecord activity : finish) {
            if (change.disables(mPm, activity.targetCode, activity.info.name, ApkPackage.ComponentKind.ACTIVITY)) finishDisabledActivity(activity);
        }
    }

    private void finishDisabledActivity(ActivityRecord activity) {
        synchronized (this) { activity.preserveProcessOnFinish = true; }
        mExitStatus.unavailable(activity.launchGeneration);
        finish(activity, "component disabled", android.app.Activity.RESULT_CANCELED, null, false);
    }

    /** Stable provider dependencies cannot continue using a retired provider capability. */
    void providerDependencyDied(ProcessRecord client, ApkPackage expected) {
        boolean terminated;
        synchronized (this) {
            if (expected == null || mPm.get(expected.packageName) != expected
                    || !mProcesses.contains(client) || client.exiting || client.dead
                    || client.pidSuperseded || attachedProcessByPid(client.pid) != client
                    || client.pid == android.os.Process.myPid()
                    || mAm != null && mAm.isPersistent(client)) return;
            client.reaped = true;
            client.recoverAfterPidSupersession = false;
            client.exiting = true;
            terminated = client.spawned && Zygote.killChild(client.pid, client.nativeStartTime);
        }
        try { processDied(client); }
        finally {
            if (!terminated) {
                try { client.thread.scheduleExit(); }
                catch (RemoteException ignored) { }
            }
        }
    }

    /** Ends every app process (the device shuts down), waiting up to {@code waitMs}. */
    void endAllProcesses(long waitMs) {
        List<ProcessRecord> all;
        synchronized (this) {
            all = new ArrayList<>(mProcesses);
            for (Map.Entry<Long, PendingProcess> entry : new ArrayList<>(mStarting.entrySet())) {
                PendingProcess p = entry.getValue(); p.cancelled = true;
                if (p.pid > 0) Zygote.killChild(p.pid, p.nativeStartTime);
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
                Zygote.killChild(app.pid, app.nativeStartTime);
            } else {
                try { app.thread.scheduleExit(); }
                catch (RemoteException ignored) { }
            }
        }
    }

    private void endProcesses(List<ProcessRecord> processes, long waitMs) {
        for (ProcessRecord p : processes) {
            synchronized (this) {
                p.recoverAfterPidSupersession = false;
                p.exiting = true;
            }
            if (p.spawned) {
                Zygote.killChild(p.pid, p.nativeStartTime);
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

    void attach(ActivityRecord r, ProcessRecord app) {
        r.app = app;
        if (r.launchGeneration == null) r.launchGeneration = app.exitGeneration;
        app.activities.add(r);
        updateIdle();
    }

    // --- processes (from ActivityManagerService) ---------------------------------------

    /** Records an attachment after all start-sequence and UID checks. Guarded by this. */
    private void registerAttachedProcess(ProcessRecord app) {
        for (ProcessRecord previous : mProcesses) {
            if (previous.pid != app.pid) continue;
            if (!previous.pidSuperseded) {
                previous.recoverAfterPidSupersession = !previous.exiting;
            }
            previous.pidSuperseded = true;
            previous.exiting = true;
        }
        // Keep superseded records until their own death callbacks retire their work.
        // The permanent flag prevents their identity from reviving after app is removed.
        mProcesses.add(app);
    }

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
                        && pendingCurrent(started)
                        && SystemClock.uptimeMillis() < deadline) {
                    try { wait(Math.max(1, deadline - SystemClock.uptimeMillis())); }
                    catch (InterruptedException interrupted) {
                        Thread.currentThread().interrupt();
                        throw new SecurityException("Interrupted process authentication");
                    }
                }
                if (started == null || !pendingCurrent(started) || started.pid != pid
                        || mStarting.get(startSeq) != started) {
                    throw new SecurityException("App process does not match its start sequence");
                }
                ApkPackage expected = mPm.get(started.packageName);
                if (expected == null || started.owner != expected || mPm.isRemovalPending(started.packageName)) {
                    throw new SecurityException("App process belongs to a retired package");
                }
                if (GmsSignatureSpoofing.isEnabled()
                        && (expected == null || expected.uid() != app.uid)) {
                    throw new SecurityException("App Binder UID does not match its package");
                }
                mStarting.remove(startSeq);
            } else if (GmsSignatureSpoofing.isEnabled() && mStarter != null) {
                throw new SecurityException("Protected app processes must be started by the server");
            }
            registerAttachedProcess(app);
            updateIdle();
            String waiting = null;
            String waitingProcess = null;
            if (started != null) {
                app.spawned = true;
                app.nativeStartTime = started.nativeStartTime;
                app.startSeq = startSeq;
                app.exitGeneration = started.exitGeneration;
                app.packageCode = started.owner;
                app.enabledProcessLifetime = started.enabledLifetime;
                app.windowMode = started.windowMode;
                app.windowModeExplicit = started.windowModeExplicit;
                waiting = started.packageName;
                waitingProcess = started.processName;
            } else {
                // A start waiting for a process (none being started for its package): the
                // one asked by this process, else the first.
                for (ActivityRecord r : mActivities) {
                    if (r.app != null || r.deferredHistory || r.state != State.INITIALIZING
                            || !activityEnabled(r)
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
                    if (w.current(mPm) && !starting(w.packageName, w.processName)) {
                        waiting = w.packageName;
                        waitingProcess = w.processName;
                    }
                }
            }
            if (waiting != null && processFor(waiting, waitingProcess) == null) {
                assignPackage(app, waiting, waitingProcess);
                for (ActivityRecord r : mActivities) {
                    if (r.app == null && !r.deferredHistory && r.state == State.INITIALIZING
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
        List<PendingWork> work = new ArrayList<>();
        ProcessRecord app;
        synchronized (this) {
            app = processByPid(pid);
            if (app == null) return;
            app.markActive(SystemClock.uptimeMillis());
            app.ready = true;
            for (ActivityRecord r : app.activities) {
                if (r.state == State.INITIALIZING && !r.deferredHistory && r.waitingFor == null
                        && (app.enabledProcessLifetime == null || app.enabledProcessLifetime.getAsBoolean())) launch.add(r);
            }
            for (int i = 0; i < mPendingWork.size(); i++) {
                PendingWork w = mPendingWork.get(i);
                if (w.packageName.equals(app.packageName) && w.processName.equals(app.processName)
                        && (!w.current(mPm) || (app.enabledProcessLifetime == null
                            || app.enabledProcessLifetime.getAsBoolean())
                            && (w.launchGeneration == null || w.launchGeneration == app.exitGeneration))) {
                    work.add(w);
                    mPendingWork.remove(i--);
                }
            }
            updateIdle();
        }
        SystemServer.log("finishAttachApplication: " + app);
        for (ActivityRecord r : launch) realStartActivity(r);
        for (PendingWork w : work) {
            if (!app.exiting && !app.dead && w.current(mPm)
                    && (app.enabledProcessLifetime == null || app.enabledProcessLifetime.getAsBoolean())) w.work.accept(app);
            else w.canceled.run();
        }
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
        withProcess(mPm.get(packageName), null, null, declaredProcess, reason, work);
    }

    void withProcess(ApkPackage owner, String component, ApkPackage.ComponentKind kind,
            String declaredProcess, String reason,
            Consumer<ProcessRecord> work) {
        withProcess(owner, component, kind, declaredProcess, reason, work, () -> { });
    }

    void withProcess(ApkPackage owner, String component, ApkPackage.ComponentKind kind,
            String declaredProcess, String reason, Consumer<ProcessRecord> work, Runnable canceled) {
        if (owner == null) { canceled.run(); return; }
        String packageName = owner.packageName;
        String processName = componentProcess(packageName, declaredProcess);
        PendingWork pending = new PendingWork(owner, component, kind, processName, work, canceled);
        ProcessRecord app = null;
        boolean dispatchNow = false, accepted;
        synchronized (this) {
            accepted = pending.current(mPm);
            if (accepted) {
                app = processFor(packageName, processName);
                dispatchNow = app != null && app.ready;
                if (!dispatchNow) {
                    for (PendingProcess started : mStarting.values()) {
                        if (pendingCurrent(started) && started.packageName.equals(packageName)
                                && started.processName.equals(processName)) {
                            pending.launchGeneration = started.exitGeneration;
                            break;
                        }
                    }
                    mPendingWork.add(pending);
                    updateIdle();
                    SystemServer.log("waiting for a process of " + packageName);
                } else {
                    app.markActive(SystemClock.uptimeMillis());
                }
            }
        }
        if (!accepted) {
            canceled.run();
        } else if (app == null) {
            startProcess(packageName, processName, reason);
        } else if (dispatchNow) {
            if (!app.exiting && !app.dead && pending.current(mPm)
                    && (app.enabledProcessLifetime == null || app.enabledProcessLifetime.getAsBoolean())) work.accept(app);
            else canceled.run();
        }
    }

    /** The process with this pid (a caller), or null. */
    ProcessRecord callerProcess(int pid) {
        synchronized (this) {
            return processByPid(pid);
        }
    }

    private void bindApplication(ProcessRecord app) {
        // DONT_KILL_APP also retains an already admitted child which has not yet
        // attached. Bind that child normally; current provider/component queries
        // exclude disabled work without leaving its application thread unbound.
        ApplicationInfo ai = mPm.getApplicationInfo(app.packageName,
                PackageManager.GET_META_DATA | PackageManager.GET_SHARED_LIBRARY_FILES
                        | PackageManager.MATCH_DISABLED_COMPONENTS, 0);
        if (ai == null || app.packageCode != mPm.get(app.packageName) || mPm.isRemovalPending(app.packageName)
                || app.enabledProcessLifetime != null && !app.enabledProcessLifetime.getAsBoolean()) return;
        ai = new ApplicationInfo(ai);
        ai.processName = app.processName;
        Configuration config = LocaleManagerService.configurationFor(app.packageName);
        synchronized (this) {
            app.config = config;
        }
        try {
            mAm.bindApplication(app, ai, config);
            SystemServer.log("bindApplication: " + app);
        } catch (Throwable e) {
            // Reported to the caller (an exception crosses binder), not fatal to the server.
            SystemServer.log("bindApplication " + app + " failed: " + e);
            e.printStackTrace();
            throw new IllegalStateException("bindApplication failed in the system server: "
                    + e, e);
        }
    }

    void processDied(ProcessRecord app) {
        List<ActivityRecord> released = new ArrayList<>();
        java.util.Set<Integer> affectedTasks = new java.util.LinkedHashSet<>();
        synchronized (this) {
            if (!mProcesses.remove(app)) return;
            app.dead = true;
            for (ActivityRecord r : new ArrayList<>(app.activities)) {
                for (ActivityRecord a : mActivities) {
                    if (a.waitingFor == r) {
                        a.waitingFor = null;
                        if (!a.finishing && a.state == State.INITIALIZING
                                && a.app != null && a.app != app && a.app.ready) released.add(a);
                    }
                }
                if (r.finishing || r.state == State.DESTROYED
                        || r.savedState == null && (r.info.flags & ActivityInfo.FLAG_STATE_NOT_NEEDED) == 0) {
                    r.state = State.DESTROYED;
                    accountActivityFinished(r);
                    mActivities.remove(r);
                    if (mAm != null && mAm.uriGrants() != null) mAm.uriGrants().revokeByOwner(r.token);
                    affectedTasks.add(r.taskId);
                    if (topOf(r.taskId) == null) {
                        mTaskOrder.remove(Integer.valueOf(r.taskId));
                        mTaskRotation.delete(r.taskId);
                    }
                } else {
                    if (!r.idle) r.launchFailed = true;
                    r.app = null;
                    r.launchGeneration = null;
                    r.launchScheduled = false;
                    r.state = State.STOPPED;
                    r.idle = false;
                    r.stopWhenIdle = false;
                    r.waitingFor = null;
                    r.activityWindowGrant = null;
                }
            }
            app.activities.clear();
            if (mTopResumed != null && mTopResumed.app == null) mTopResumed = null;
            updateIdle();
            notifyAll();
        }
        updateTopResumed();
        SystemServer.log(app + " died");
        if (mAm != null) mAm.processDied(app);
        for (ActivityRecord activity : released) realStartActivity(activity);
        for (int taskId : affectedTasks) realizeTaskHistory(taskId);
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

    /** Current PID ownership, independent of lifecycle admission. Guarded by this. */
    private ProcessRecord attachedProcessByPid(int pid) {
        for (int i = mProcesses.size() - 1; i >= 0; i--) {
            ProcessRecord app = mProcesses.get(i);
            if (app.pid == pid && !app.pidSuperseded) return app;
        }
        return null;
    }

    private ProcessRecord processByPid(int pid) {
        ProcessRecord app = attachedProcessByPid(pid);
        return app != null && !app.dead && !app.exiting ? app : null;
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

    /** Realize only visible history, retaining the opaque parents without starting their processes. */
    private void realizeTaskHistory(int taskId) {
        synchronized (this) {
            mHistoryToRealize.add(taskId);
            if (mRealizingHistory || mStackPublications != 0) return;
            mRealizingHistory = true;
        }
        try {
            while (true) {
                int next;
                synchronized (this) {
                    if (mHistoryToRealize.isEmpty() || mStackPublications != 0) {
                        mRealizingHistory = false;
                        return;
                    }
                    next = mHistoryToRealize.iterator().next();
                    mHistoryToRealize.remove(next);
                }
                realizeTaskHistoryOnce(next);
            }
        } catch (RuntimeException | Error failure) {
            synchronized (this) { mRealizingHistory = false; }
            throw failure;
        }
    }

    private void realizeTaskHistoryOnce(int taskId) {
        List<ActivityRecord> visible = new ArrayList<>();
        List<ActivityRecord> reconfigure = new ArrayList<>();
        synchronized (this) {
            if (mStackPublications != 0) { mHistoryToRealize.add(taskId); return; }
            if (topOf(taskId) == null) {
                mTaskOrder.remove(Integer.valueOf(taskId));
                mHiddenTasks.remove(taskId);
                mTaskBounds.remove(taskId);
                mTaskRotation.delete(taskId);
            }
            if (mHiddenTasks.contains(taskId)) return;
            if (updateRotationLocked(taskId)) {
                for (ActivityRecord activity : mActivities) {
                    if (activity.taskId == taskId && !activity.finishing && activity.app != null
                            && activity.state != State.INITIALIZING) reconfigure.add(activity);
                }
            }
            for (int i = mActivities.size() - 1; i >= 0; i--) {
                ActivityRecord activity = mActivities.get(i);
                if (activity.taskId != taskId || activity.finishing || activity.state == State.DESTROYED) continue;
                if (activity.deferredHistory && activity.batchOwner == null) visible.add(activity);
                if (!activity.translucent) break;
            }
        }
        java.util.Collections.reverse(visible);
        for (ActivityRecord activity : visible) realizeHistory(activity);
        for (ActivityRecord activity : reconfigure) ensureConfiguration(activity);
        resumeHistoryTop(taskId);
        updateTopResumed();
    }

    /** A reused/reordered live top follows the same pause handoff as a new instance. */
    private void resumeHistoryTop(int taskId) {
        ActivityRecord resume = null, pause = null;
        synchronized (this) {
            if (mStackPublications != 0 || mHiddenTasks.contains(taskId)) return;
            ActivityRecord top = topOf(taskId);
            if (top == null || top.app == null || !top.app.ready || !activityCodeCurrent(top)) return;
            if (top.state != State.PAUSED && top.state != State.STARTED
                    && top.state != State.STOPPED && top.state != State.STOPPING) return;
            ActivityRecord previous = front(top);
            if (previous == null) resume = top;
            else {
                top.waitingFor = previous;
                top.resumeAfterPause = true;
                if (previous.state != State.PAUSING) pause = previous;
            }
        }
        if (pause != null) pause(pause, false);
        if (resume != null) resume(resume);
    }

    private void realizeHistory(ActivityRecord r) {
        boolean current;
        synchronized (this) {
            current = activityCodeCurrent(r)
                    && (r.sourceLifetime == null || r.sourceLifetime.getAsBoolean());
        }
        if (!current) {
            finish(r, "history-target-unavailable", android.app.Activity.RESULT_CANCELED, null);
            return;
        }
        ProcessRecord bind = null;
        ActivityRecord pause = null;
        boolean needProcess = false, launchNow = false;
        synchronized (this) {
            if (!mActivities.contains(r) || r.finishing || r.state != State.INITIALIZING
                    || !r.deferredHistory || r.batchOwner != null || mStackPublications != 0 || !isVisible(r)) return;
            r.deferredHistory = false;
            ProcessRecord app = r.app;
            if (app == null) app = processFor(r.info.packageName,
                    componentProcess(r.info.packageName, r.info.processName));
            if (app == null) {
                app = unassignedProcess(r.launchedFromPid);
                if (app != null) {
                    assignPackage(app, r.info.packageName,
                            componentProcess(r.info.packageName, r.info.processName));
                    bind = app;
                }
            }
            if (app != null) {
                if (r.app == null) attach(r, app);
            } else {
                needProcess = true;
                for (PendingProcess pending : mStarting.values()) {
                    if (pending.packageName.equals(r.info.packageName)
                            && pending.processName.equals(componentProcess(r.info.packageName, r.info.processName))) {
                        r.launchGeneration = pending.exitGeneration;
                        break;
                    }
                }
            }
            // A previously running foreground activity must acknowledge its pause.
            // Uninstantiated parent history has no lifecycle to wait for.
            ActivityRecord front = topOf(r.taskId) == r ? front(r) : null;
            if (front != null) {
                r.waitingFor = front;
                if (front.state != State.PAUSING) pause = front;
            }
            launchNow = r.waitingFor == null && app != null && app.ready;
        }
        if (bind != null) bindApplication(bind);
        if (needProcess) startProcess(r.info.packageName,
                componentProcess(r.info.packageName, r.info.processName), "activity " + r.name(),
                r.inheritedWindowMode);
        if (pause != null) pause(pause, false);
        if (launchNow) realStartActivity(r);
    }

    /** ActivityTaskSupervisor.realStartActivityLocked: create in the required visible state. */
    private void realStartActivity(ActivityRecord r) {
        if (activityCodeCurrent(r) && !r.targetCode.componentEnabled(r.info.name, ApkPackage.ComponentKind.ACTIVITY)) {
            finishDisabledActivity(r); return;
        }
        boolean current;
        synchronized (this) {
            if (r.state != State.INITIALIZING || r.launchScheduled || r.finishing || r.deferredHistory
                    || r.waitingFor != null || r.app == null || !r.app.ready) return;
            if (r.app.exiting || r.app.dead
                    || r.app.enabledProcessLifetime != null && !r.app.enabledProcessLifetime.getAsBoolean()) return;
            if (r.historyEntry && !isVisible(r)) {
                r.deferredHistory = true;
                return;
            }
            current = mActivities.contains(r) && r.state != State.DESTROYED && activityCodeCurrent(r)
                    && (r.sourceLifetime == null || r.sourceLifetime.getAsBoolean());
            if (current) r.launchScheduled = true;
        }
        if (!current) {
            mExitStatus.unavailable(r.launchGeneration); r.finishing = true; remove(r); return;
        }
        Configuration config = LocaleManagerService.configurationFor(r.info.packageName);
        Rect task;
        int rotation;
        Bundle state;
        PersistableBundle persistentState;
        synchronized (this) {
            task = taskBoundsLocked(r.taskId);
            rotation = mTaskRotation.get(r.taskId, Surface.ROTATION_0);
            r.config = effectiveConfiguration(config, task, rotation);
            r.configPending = false;
            state = r.savedState;
            persistentState = r.persistentState;
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
        List<ReferrerIntent> newIntents;
        boolean andResume;
        synchronized (this) {
            newIntents = r.pendingNewIntents.isEmpty() ? null : new ArrayList<>(r.pendingNewIntents);
            r.pendingNewIntents.clear();
            andResume = !r.historyEntry || topOf(r.taskId) == r;
        }
        ClientTransaction t = new ClientTransaction(r.app.thread);
        IBinder windowGrant = createActivityWindowGrant(r);
        if (windowGrant != null) {
            t.addTransactionItem(new GesherActivityWindowModeItem(r.token, windowGrant));
        }
        t.addTransactionItem(new LaunchActivityItem(r.token, r.intent,
                System.identityHashCode(r), r.info, config, override,
                Context.DEVICE_ID_DEFAULT, r.launchedFromPackage, null /* voiceInteractor */,
                ActivityManager.PROCESS_STATE_TOP, state /* state */, persistentState /* persistentState */,
                results /* pendingResults */, newIntents,
                null /* sceneTransitionInfo */, true /* isForward */, null /* profilerInfo */,
                new Binder() /* assistToken */, mClient.asInterface(),
                new Binder() /* shareableActivityToken */, false /* launchedFromBubble */,
                null /* taskFragmentToken */, r.initialCallerInfoAccessToken,
                windowInfo, Display.DEFAULT_DISPLAY));
        if (andResume) {
            t.addTransactionItem(new ResumeActivityItem(r.token, true /* isForward */,
                    false /* shouldSendCompatFakeFocus */));
        } else {
            t.addTransactionItem(new PauseActivityItem(r.token, false, false, false, false));
        }
        setState(r, State.LAUNCHING);
        schedule(r, t, andResume ? "LaunchActivityItem, ResumeActivityItem"
                : "LaunchActivityItem, PauseActivityItem");
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
        if (!activityEnabled(r)) { finishDisabledActivity(r); return; }
        synchronized (this) {
            if (!mActivities.contains(r) || r.finishing || r.app == null || !r.app.ready
                    || r.app.exiting || r.app.dead
                    || r.app.enabledProcessLifetime != null && !r.app.enabledProcessLifetime.getAsBoolean()
                    || !activityCodeCurrent(r) || r.waitingFor != null
                    || (r.state != State.PAUSED && r.state != State.STARTED
                        && r.state != State.STOPPING && r.state != State.STOPPED)) return;
            r.state = State.RESUMING;
            r.stopWhenIdle = false;
            r.idle = false;
            notifyAll();
        }
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
        schedule(r, t, (config != null ? config + ", " : "")
                + (results != null && !relaunch ? "ActivityResultItem, " : "")
                + "ResumeActivityItem");
    }

    /** ActivityRecord.deliverNewIntentLocked: onNewIntent, around a pause if resumed. */
    private boolean activityCodeCurrent(ActivityRecord r) {
        return r.targetCode != null && mPm.get(r.info.packageName) == r.targetCode
                && !mPm.isRemovalPending(r.info.packageName);
    }

    private boolean activityEnabled(ActivityRecord r) {
        return activityCodeCurrent(r) && r.targetCode.componentEnabled(r.info.name, ApkPackage.ComponentKind.ACTIVITY);
    }

    private void deliverNewIntent(ActivityRecord r, Intent intent, String referrer,
            java.util.function.BooleanSupplier sourceCurrent, ActivityCallerState.Snapshot snapshot) {
        boolean resumed;
        IBinder callerToken = new Binder("New intent caller");
        synchronized (this) {
            if (!mActivities.contains(r) || r.finishing || r.state == State.DESTROYED
                    || !activityEnabled(r) || !sourceCurrent.getAsBoolean()) return;
            r.callerState.add(callerToken, snapshot);
            if (r.app == null || r.state == State.INITIALIZING && !r.launchScheduled) {
                r.pendingNewIntents.add(new ReferrerIntent(new Intent(intent), referrer, callerToken));
                return;
            }
            resumed = r.state == State.RESUMED || r.state == State.LAUNCHING
                    || r.state == State.RESUMING;
        }
        List<ReferrerIntent> intents = new ArrayList<>();
        intents.add(new ReferrerIntent(intent, referrer, callerToken));
        ClientTransaction t = new ClientTransaction(r.app.thread);
        t.addTransactionItem(new NewIntentItem(r.token, intents, resumed));
        schedule(r, t, "NewIntentItem");
    }

    private void deliverResult(ActivityRecord r, String resultWho, int requestCode,
            int resultCode, Intent resultData) {
        if (r == null) return;
        // These results come from server-owned UI flows. Attribute them to the server
        // even when completion runs on a worker with no original Binder caller.
        final ActivityCallerState.Snapshot snapshot;
        synchronized (mPm) {
            snapshot = ActivityCallerState.capture(new ActivityCallerState.Origin(null,
                    android.os.Process.myUid(), "android", true, false), resultData, mAm.uriGrants());
        }
        IBinder callerToken = new Binder("Server activity result caller");
        ResultInfo info = new ResultInfo(resultWho, requestCode, resultCode, resultData, callerToken);
        deliverResult(r, info, callerToken, snapshot, null);
    }

    /** Launch cancellation has no activity caller token; server UI results retain theirs. */
    private void deliverResult(ActivityRecord r, ResultInfo info, IBinder callerToken,
            ActivityCallerState.Snapshot snapshot, ProcessRecord expectedProcess) {
        boolean resumed;
        final ProcessRecord recipient;
        final IApplicationThread recipientThread;
        synchronized (this) {
            if (!mActivities.contains(r) || r.finishing || r.state == State.DESTROYED
                    || !activityCodeCurrent(r)) return;
            if (expectedProcess != null && (r.app != expectedProcess
                    || processByPid(expectedProcess.pid) != expectedProcess)) return;
            if (callerToken != null) r.callerState.add(callerToken, snapshot);
            resumed = (r.state == State.RESUMED || r.state == State.LAUNCHING || r.state == State.RESUMING);
            if (!resumed || r.app == null || r.app.thread == null) {
                r.pendingResults.add(info);
                return;
            }
            recipient = r.app;
            recipientThread = recipient == null ? null : recipient.thread;
        }
        if (recipientThread != null) {
            List<ResultInfo> list = new ArrayList<>();
            list.add(info);
            ClientTransaction t = new ClientTransaction(recipientThread);
            t.addTransactionItem(new ActivityResultItem(r.token, list));
            // A reattachment after admission must not redirect an old process's
            // cancellation to its replacement through schedule()'s mutable r.app.
            SystemServer.log("scheduleTransaction ActivityResultItem to " + r.name() + " in " + recipient);
            try {
                recipientThread.scheduleTransaction(t);
            } catch (RemoteException e) {
                SystemServer.log("scheduleTransaction to " + recipient + " failed: " + e);
                synchronized (this) {
                    // AOSP retains a result when the attached recipient could not
                    // accept it. A saved activity may already be waiting for a
                    // replacement process, but a retired record must not revive.
                    if (mActivities.contains(r) && !r.finishing && r.state != State.DESTROYED
                            && activityCodeCurrent(r)
                            && (expectedProcess == null || r.app == expectedProcess
                                && processByPid(expectedProcess.pid) == expectedProcess)) {
                        r.pendingResults.add(info);
                    }
                }
            }
        }
    }

    /** Pauses an activity (PauseActivityItem): finishing, or for another in front. */
    private void pause(ActivityRecord r, boolean finishing) {
        synchronized (this) {
            if (!finishing) r.stopWhenIdle = true;
        }
        if (r.app == null || r.app.thread == null) return;
        setState(r, State.PAUSING);
        ClientTransaction t = new ClientTransaction(r.app.thread);
        t.addTransactionItem(new PauseActivityItem(r.token, finishing /* finished */,
                !finishing /* userLeaving */, false /* dontReport */,
                false /* autoEnteringPip */));
        schedule(r, t, "PauseActivityItem");
    }

    /** Convert microG's legacy result envelope for the framework-aware recipient. */
    private static Intent normalizeMicrogCredentialResult(String sourcePackage, String sourceActivity,
            String recipientActivity, int resultCode, Intent data) {
        // microG's compatibility producer uses the AndroidX Bundle wire format.
        // Modern AndroidX clients on our API level read the framework Parcelable.
        // Preserve the provider's actual opaque data; never synthesize credentials.
        if (resultCode != android.app.Activity.RESULT_OK || data == null
                || !"com.google.android.gms".equals(sourcePackage)
                || !"org.microg.gms.auth.credentials.identity.IdentityCredentialChooserActivity".equals(sourceActivity)
                || !"androidx.credentials.playservices.controllers.identityauth.HiddenActivity".equals(recipientActivity)) return data;
        final String key = "android.service.credentials.extra.GET_CREDENTIAL_RESPONSE";
        try {
            Bundle extras = data.getExtras();
            Object value = extras == null ? null : extras.get(key);
            if (!(value instanceof Bundle)) return data;
            Bundle legacy = (Bundle) value;
            String type = legacy.getString("androidx.credentials.provider.extra.EXTRA_CREDENTIAL_TYPE");
            Bundle content = legacy.getBundle("androidx.credentials.provider.extra.EXTRA_CREDENTIAL_DATA");
            if (type == null || type.isEmpty() || content == null) return data;
            Bundle converted = new Bundle(content);
            if ("com.google.android.libraries.identity.googleid.TYPE_GOOGLE_ID_TOKEN_CREDENTIAL".equals(type)) {
                // This microG producer also serializes the optional picture Uri
                // as a String. The current googleid consumer reads a Parcelable.
                String pictureKey = "com.google.android.libraries.identity.googleid.BUNDLE_KEY_PROFILE_PICTURE_URI";
                Object picture = converted.get(pictureKey);
                if (picture instanceof String) converted.putParcelable(pictureKey, android.net.Uri.parse((String) picture));
            }
            Intent normalized = new Intent(data);
            normalized.putExtra(key, new android.credentials.GetCredentialResponse(
                    new android.credentials.Credential(type, converted)));
            return normalized;
        } catch (RuntimeException malformed) { return data; }
    }

    /**
     * ActivityRecord.finishIfPossible: a result for the activity that asked for one, then
     * pause, stop and destroy.
     */
    boolean finish(ActivityRecord r, String reason, int resultCode, Intent resultData) {
        return finish(r, reason, resultCode, resultData, false);
    }

    boolean finishFromApp(ActivityRecord r, int resultCode, Intent resultData) {
        return finish(r, "app-request", resultCode, resultData, true);
    }

    boolean finishFromApp(ActivityRecord r, int resultCode, Intent resultData, int finishTask) {
        List<ActivityRecord> task = new ArrayList<>();
        synchronized (this) {
            if (isActivityRetired(r)) return false;
            // Public Activity.finishAndRemoveTask uses WITH_ROOT_ACTIVITY (1).
            // WITH_ACTIVITY (2) removes the task even from a non-root activity.
            boolean wholeTask = finishTask == android.app.Activity.FINISH_TASK_WITH_ACTIVITY
                    || finishTask == android.app.Activity.FINISH_TASK_WITH_ROOT_ACTIVITY
                            && isTaskRoot(r);
            if (wholeTask) {
                for (int i = mActivities.size() - 1; i >= 0; i--) {
                    ActivityRecord candidate = mActivities.get(i);
                    if (candidate.taskId == r.taskId) task.add(candidate);
                }
            }
        }
        return task.isEmpty() ? finishFromApp(r, resultCode, resultData)
                // AOSP task removal ignores the caller's success result/data and
                // cancels outstanding results while removing its activities.
                : finishTogether(r, task, "finish-task", android.app.Activity.RESULT_CANCELED, null);
    }

    boolean finishAffinity(ActivityRecord r) {
        List<ActivityRecord> matching = new ArrayList<>();
        synchronized (this) {
            if (isActivityRetired(r)) return false;
            for (int i = mActivities.indexOf(r); i >= 0; i--) {
                ActivityRecord candidate = mActivities.get(i);
                if (candidate.taskId != r.taskId || candidate.state == State.DESTROYED) continue;
                if (!java.util.Objects.equals(candidate.info.taskAffinity, r.info.taskAffinity)) break;
                matching.add(candidate);
            }
        }
        return finishTogether(r, matching, "request-affinity", android.app.Activity.RESULT_CANCELED, null);
    }

    void finishSubActivities(ActivityRecord parent, String resultWho, int requestCode) {
        List<ActivityRecord> matching = new ArrayList<>();
        synchronized (this) {
            if (isActivityRetired(parent)) return;
            for (int i = mActivities.size() - 1; i >= 0; i--) {
                ActivityRecord candidate = mActivities.get(i);
                if (candidate.resultTo == parent && candidate.requestCode == requestCode
                        && java.util.Objects.equals(candidate.resultWho, resultWho)) matching.add(candidate);
            }
        }
        finishTogether(parent, matching, "request-sub", android.app.Activity.RESULT_CANCELED, null);
    }

    /** State captured before any lifecycle transaction can reenter the service. */
    private static final class PendingFinish {
        final ActivityRecord activity;
        final int resultCode;
        final Intent resultData;
        final ActivityCallerState.Snapshot resultCaller;
        Runnable accountCompletion;

        PendingFinish(ActivityRecord activity, int resultCode, Intent resultData,
                ActivityCallerState.Snapshot resultCaller) {
            this.activity = activity;
            this.resultCode = resultCode;
            this.resultData = resultData;
            this.resultCaller = resultCaller;
        }
    }

    /**
     * Close a selected group as one lifecycle decision. Mark every member before sending
     * transactions: a local or fast remote pause acknowledgement must not resume another
     * member of the group. Result capture still happens outside the lifecycle monitor.
     */
    private boolean finishTogether(ActivityRecord source, List<ActivityRecord> selected,
            String reason, int resultCode, Intent resultData) {
        List<PendingFinish> prepared = new ArrayList<>();
        synchronized (mPm) {
            for (ActivityRecord r : selected) {
                int code = r == source ? resultCode : android.app.Activity.RESULT_CANCELED;
                Intent data = r == source ? resultData : null;
                ActivityRecord resultTarget = r.resultTo;
                if (resultTarget != null) data = normalizeMicrogCredentialResult(r.info.packageName,
                        r.info.name, resultTarget.info.name, code, data);
                ActivityCallerState.Snapshot caller = r.resultTo == null || !activityCodeCurrent(r)
                        ? null : ActivityCallerState.capture(new ActivityCallerState.Origin(r.targetCode,
                                r.info.applicationInfo.uid, r.info.packageName, false, false),
                                data, mAm.uriGrants());
                prepared.add(new PendingFinish(r, code, data, caller));
            }
        }
        List<PendingFinish> accepted = new ArrayList<>();
        Map<ActivityRecord, List<ResultInfo>> immediateResults = new java.util.LinkedHashMap<>();
        synchronized (this) {
            if (isActivityRetired(source)) return false;
            for (PendingFinish pending : prepared) {
                ActivityRecord r = pending.activity;
                if (isActivityRetired(r) || r.finishing) continue;
                r.finishing = true;
                pending.accountCompletion = r.accountCompletion;
                r.accountCompletion = null;
                accepted.add(pending);
            }
            for (PendingFinish pending : accepted) {
                ActivityRecord r = pending.activity;
                updateRotationLocked(r.taskId);
                ActivityRecord recipient = r.resultTo;
                if (pending.resultCaller != null && !isActivityRetired(recipient) && !recipient.finishing) {
                    IBinder callerToken = new Binder("Activity result caller");
                    recipient.callerState.add(callerToken, pending.resultCaller);
                    recipient.pendingResults.add(new PendingActivityResult(r,
                            pending.resultCode, pending.resultData, callerToken));
                    if (recipient.state == State.RESUMED && recipient.app != null) {
                        immediateResults.computeIfAbsent(recipient, ignored -> new ArrayList<>())
                                .addAll(takeResults(recipient));
                    }
                }
            }
        }
        for (PendingFinish pending : accepted) {
            if (pending.accountCompletion != null) {
                try { pending.accountCompletion.run(); }
                catch (RuntimeException failure) {
                    SystemServer.log("Activity finish completion failed: " + failure);
                }
            }
        }
        updateTopResumed();
        for (Map.Entry<ActivityRecord, List<ResultInfo>> entry : immediateResults.entrySet()) {
            ActivityRecord recipient = entry.getKey();
            synchronized (this) {
                if (isActivityRetired(recipient) || recipient.finishing) continue;
                if (recipient.state != State.RESUMED || recipient.app == null) {
                    recipient.pendingResults.addAll(0, entry.getValue());
                    continue;
                }
            }
            ClientTransaction transaction = new ClientTransaction(recipient.app.thread);
            transaction.addTransactionItem(new ActivityResultItem(recipient.token, entry.getValue()));
            schedule(recipient, transaction, "ActivityResultItem");
        }
        for (PendingFinish pending : accepted) {
            SystemServer.log("finishing " + pending.activity.name() + " (" + reason + ")");
            completeFinish(pending.activity);
        }
        return true;
    }

    private boolean finish(ActivityRecord r, String reason, int resultCode, Intent resultData,
            boolean requireCurrentCode) {
        ActivityRecord resultTarget = r.resultTo;
        if (resultTarget != null) resultData = normalizeMicrogCredentialResult(r.info.packageName,
                r.info.name, resultTarget.info.name, resultCode, resultData);
        final ActivityCallerState.Snapshot resultCaller;
        synchronized (mPm) {
            // Compute before taking the lifecycle monitor. URI grants and provider
            // permissions have their own locks; result delivery must not invert them.
            resultCaller = r.resultTo == null || !activityCodeCurrent(r) ? null
                    : ActivityCallerState.capture(new ActivityCallerState.Origin(r.targetCode,
                            r.info.applicationInfo.uid, r.info.packageName, false, false),
                            resultData, mAm.uriGrants());
        }
        synchronized (this) {
            if (!mActivities.contains(r) || r.state == State.DESTROYED
                    || requireCurrentCode && !activityCodeCurrent(r)) return false;
            if (r.finishing) return true;
            r.finishing = true;
            accountActivityFinished(r);
            // The activity below may ask for another orientation (it gets the configuration
            // as it resumes).
            updateRotationLocked(r.taskId);
            if (resultCaller != null && activityCodeCurrent(r) && r.resultTo != null
                    && mActivities.contains(r.resultTo) && activityCodeCurrent(r.resultTo)
                    && r.resultTo.state != State.DESTROYED && !r.resultTo.finishing) {
                IBinder callerToken = new Binder("Activity result caller");
                r.resultTo.callerState.add(callerToken, resultCaller);
                r.resultTo.pendingResults.add(new PendingActivityResult(r,
                        resultCode, resultData, callerToken));
            }
        }
        SystemServer.log("finishing " + r.name() + " (" + reason + ")"
                + (resultTarget != null ? ", result " + resultCode + " to " + resultTarget.name()
                        : ""));
        updateTopResumed();
        completeFinish(r);
        return true;
    }

    private void completeFinish(ActivityRecord r) {
        State state;
        synchronized (this) {
            // An earlier transaction's local callback may have already advanced
            // this record, or a dead process may have removed it entirely. Continue
            // from its current state (e.g. a pending resume may just have completed).
            if (!mActivities.contains(r) || r.state == State.DESTROYED) return;
            state = r.state;
        }
        switch (state) {
            case INITIALIZING:
                remove(r);
                return;
            case LAUNCHING:
            case RESUMING:
            case RESUMED:
                pause(r, true);
                return;
            case PAUSED:
            case STARTED:
                stop(r);
                return;
            case STOPPED:
                destroy(r);
                return;
            default:
                return;  // on its way
        }
    }

    /**
     * ActivityRecord.navigateUpTo, lite (NavUtils.navigateUpFromSameTask): the activities
     * of the caller's task above the up destination finish. When the destination is not in
     * the task, it is started in the task
     * first, as if it had been below, and the caller finishes (exec track, docs/exec.md:
     * Simon Tatham's Puzzles goes up to its chooser on a first launch).
     */
    boolean navigateUpTo(ActivityRecord r, Intent destIntent, String resolvedType,
            int resultCode, Intent resultData) {
        if (isActivityRetired(r)) return false;
        ComponentName dest = destIntent.getComponent();
        if (dest == null) {
            ActivityInfo info = resolve(destIntent, resolvedType, true);
            if (info == null) return false;
            dest = new ComponentName(info.packageName, info.name);
        }
        List<ActivityRecord> above = new ArrayList<>();
        ActivityRecord parent = null;
        synchronized (this) {
            if (isActivityRetired(r)) return false;
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
            java.util.function.BooleanSupplier previous = mVerifiedPublication.get();
            mVerifiedPublication.set(() -> !isActivityRetired(r)
                    && (previous == null || previous.getAsBoolean()));
            final int result;
            try {
                result = start(r.app != null ? r.app.thread : null, up, resolvedType,
                        r.info.packageName, r.token, null, -1, started);
            } catch (SecurityException failure) {
                if (isActivityRetired(r)) return false;
                throw failure;
            } finally {
                if (previous == null) mVerifiedPublication.remove(); else mVerifiedPublication.set(previous);
            }
            if (result != ActivityManager.START_SUCCESS) return false;
        }
        if (isActivityRetired(r)) return false;
        return finishTogether(r, above, "navigateUpTo", resultCode, resultData);
    }

    private void stop(ActivityRecord r) {
        if (r.app == null || r.app.thread == null) {
            synchronized (this) {
                r.state = State.STOPPED;
                if (r.finishing) {
                    remove(r);
                    return;
                }
            }
            return;
        }
        setState(r, State.STOPPING);
        ClientTransaction t = new ClientTransaction(r.app.thread);
        t.addTransactionItem(new StopActivityItem(r.token));
        schedule(r, t, "StopActivityItem");
    }

    private void destroy(ActivityRecord r) {
        if (r.app == null || r.app.thread == null) {
            remove(r);
            return;
        }
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
        if (r.app == null || r.app.thread == null) return;
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
        List<ActivityRecord> released = new ArrayList<>();
        synchronized (this) {
            r.state = State.DESTROYED;
            accountActivityFinished(r);
            mActivities.remove(r);
            for (ActivityRecord a : mActivities) {
                if (a.waitingFor == r) {
                    a.waitingFor = null;
                    if (a.state == State.INITIALIZING && !a.finishing
                            && a.app != null && a.app.ready) released.add(a);
                }
            }
            if (r.app != null) {
                r.app.activities.remove(r);
                // ActivityManagerService kills the process of a removed task; here the
                // process is told to exit, unless something else keeps it.
                if (r.app.activities.isEmpty() && r.finishing && !r.preserveProcessOnFinish) check = r.app;
            }
            updateIdle();
            if (r == mTopResumed) mTopResumed = null;
            if (topOf(r.taskId) == null) {
                mTaskOrder.remove(Integer.valueOf(r.taskId));
                mTaskRotation.delete(r.taskId);
            }
            notifyAll();
        }
        updateTopResumed();
        if (r.token != null && mAm != null && mAm.uriGrants() != null) {
            mAm.uriGrants().revokeByOwner(r.token);
        }
        for (ActivityRecord child : released) realStartActivity(child);
        realizeTaskHistory(r.taskId);
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

    boolean isActivityRetired(ActivityRecord r) {
        synchronized (this) {
            return r == null || !mActivities.contains(r) || r.state == State.DESTROYED
                    || !activityCodeCurrent(r);
        }
    }

    /** Return false for retirement; a live Activity still requires its real owner. */
    boolean requireActivityOwner(ActivityRecord r) {
        if (isActivityRetired(r)) return false;
        Runnable beforeAuthentication = mBeforeActivityOwnerAuthentication;
        if (beforeAuthentication != null) beforeAuthentication.run();
        final boolean internal;
        final String name;
        try {
            // Authenticate outside the lifecycle monitor. A process/package may
            // retire during this step; retirement is not a forged-owner error.
            internal = mAm.callers().isSystem();
            name = internal ? null : mAm.callers().callerPackage();
        } catch (SecurityException failure) {
            if (isActivityRetired(r)) return false;
            throw failure;
        }
        synchronized (this) {
            if (isActivityRetired(r)) return false;
            if (internal) return true;
            ProcessRecord process = callerProcess(Binder.getCallingPid());
            if (r.app == null || r.app != process || process.exiting
                    || !name.equals(r.info.packageName)) {
                throw new SecurityException("Activity does not belong to the calling process");
            }
            return true;
        }
    }

    ActivityCallerState.Snapshot callerSnapshot(IBinder activityToken,
            IBinder callerToken, boolean initial) {
        synchronized (this) {
            ActivityRecord record = forToken(activityToken);
            if (record == null || record.state == State.DESTROYED || !activityCodeCurrent(record)) return null;
            return record.callerState.get(initial ? record.initialCallerInfoAccessToken : callerToken);
        }
    }

    ActivityCallerState.Snapshot visibleCaller(IBinder activityToken, IBinder callerToken,
            boolean initial) {
        boolean internal = mAm.callers().isSystem();
        String name = internal ? null : mAm.callers().callerPackage();
        ActivityCallerState.Snapshot snapshot = callerSnapshot(activityToken, callerToken, initial);
        return snapshot != null && snapshot.visibleTo(name, Binder.getCallingUid(), internal) ? snapshot : null;
    }

    int checkActivityCallerContentUriPermission(IBinder activityToken, IBinder callerToken,
            Uri uri, int modeFlags, int userId) {
        if (Binder.getCallingUid() < 0) throw new IllegalArgumentException("Caller UID must not be negative");
        ActivityCallerState.validateQuery(uri, modeFlags, userId);
        // As UGM's full check: validate arguments and the current querying app's
        // access before looking up either the activity or its historical caller.
        boolean access = mAm.callers().isSystem();
        if (!access) {
            String name = mAm.callers().callerPackage();
            synchronized (mPm) {
                access = mAm.uriGrants().canAccessForCaller(mPm.get(name), uri, modeFlags, userId);
            }
        }
        if (!access) throw new SecurityException("The querying app cannot access this content URI");
        ActivityCallerState.Snapshot snapshot = callerSnapshot(activityToken, callerToken, false);
        return snapshot != null && snapshot.allows(uri, modeFlags, userId)
                ? PackageManager.PERMISSION_GRANTED : PackageManager.PERMISSION_DENIED;
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
            r.savedState = null; // AOSP drops the snapshot only after resumed acknowledgment.

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
            if (r.state != State.STOPPED || r.finishing || r.app == null || r.app.thread == null) return;
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
        boolean history;
        synchronized (this) {
            ActivityRecord activity = topOf(taskId);
            history = activity != null && activity.deferredHistory;
            if (history) { mHiddenTasks.remove(taskId); focusTaskLocked(taskId); }
        }
        if (history) { realizeTaskHistory(taskId); return; }
        ActivityRecord top;
        boolean resumeNow = false;
        boolean launchNow = false;
        boolean needProcess = false;
        List<ActivityRecord> toStart;
        boolean wasHidden;
        boolean refocused;
        synchronized (this) {
            wasHidden = mHiddenTasks.remove(taskId);
            top = topOf(taskId);
            if (top == null) return;
            refocused = mTaskOrder.isEmpty() || mTaskOrder.get(mTaskOrder.size() - 1) != taskId;
            focusTaskLocked(taskId);
            if (top.app == null) {
                ProcessRecord proc = processFor(top.info.packageName,
                        componentProcess(top.info.packageName, top.info.processName));
                if (proc != null && proc.ready) {
                    attach(top, proc);
                    top.state = State.INITIALIZING;
                    launchNow = true;
                } else if (proc != null) {
                    attach(top, proc);
                    top.state = State.INITIALIZING;
                } else {
                    top.state = State.INITIALIZING;
                    needProcess = true;
                }
            } else if (!top.app.ready) {
                return;
            } else {
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
            }
            toStart = starting();
            if (top != null) toStart.remove(top);
        }
        SystemServer.log("moving task " + taskId + " to the front"
                + (top != null ? ": " + (launchNow ? "launching " : "resuming ") + top.name() : ""));
        if (needProcess) {
            startProcess(top.info.packageName,
                    componentProcess(top.info.packageName, top.info.processName),
                    "activity " + top.name(), top.inheritedWindowMode);
        }
        if (launchNow) realStartActivity(top);
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
        realizeTaskHistory(r.taskId);
        return true;
    }

    void activityPaused(ActivityRecord r) {
        List<ActivityRecord> launch = new ArrayList<>();
        List<ActivityRecord> resumeAfter = new ArrayList<>();
        ActivityRecord resume = null;
        boolean finishing;
        boolean stopNow;
        boolean needProcess = false;
        ActivityRecord processTarget = null;
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
                    if (a.state == State.INITIALIZING && !a.finishing && a.app != null && a.app.ready) {
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
                if (resume != null && resume.app == null) {
                    ProcessRecord proc = processFor(resume.info.packageName,
                            componentProcess(resume.info.packageName, resume.info.processName));
                    if (proc != null && proc.ready) {
                        attach(resume, proc);
                        resume.state = State.INITIALIZING;
                        launch.add(resume);
                    } else if (proc != null) {
                        attach(resume, proc);
                        resume.state = State.INITIALIZING;
                    } else {
                        resume.state = State.INITIALIZING;
                        needProcess = true;
                        processTarget = resume;
                    }
                    resume = null;
                }
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
        if (needProcess && processTarget != null) {
            startProcess(processTarget.info.packageName,
                    componentProcess(processTarget.info.packageName, processTarget.info.processName),
                    "activity " + processTarget.name(), processTarget.inheritedWindowMode);
        }
        for (ActivityRecord a : launch) realStartActivity(a);
        if (finishing) realizeTaskHistory(r.taskId);
        for (ActivityRecord a : resumeAfter) resume(a);
        if (resume != null) resume(resume);
        if (finishing || stopNow) stop(r);
    }

    void activityStopped(ActivityRecord r, Bundle savedState) {
        activityStopped(r, savedState, null);
    }

    void activityStopped(ActivityRecord r, Bundle savedState, PersistableBundle persistentState) {
        boolean destroy;
        synchronized (this) {
            // Null reports include stop timeouts, not a newly committed snapshot.
            if (savedState != null) r.savedState = savedState;
            if (persistentState != null) r.persistentState = persistentState;
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
