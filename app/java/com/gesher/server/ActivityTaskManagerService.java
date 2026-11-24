/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.server;

import android.app.ActivityManager;
import android.app.ActivityTaskManager;
import android.app.IActivityClientController;
import android.app.IActivityTaskManager;
import android.app.IAppTask;
import android.app.IApplicationThread;
import android.app.ITaskStackListener;
import android.app.ProfilerInfo;
import android.app.ResultInfo;
import android.app.TaskMoveRequestHandler;
import android.app.TaskWindowingLayerRequestHandler;
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
import android.content.pm.ParceledListSlice;
import android.content.pm.ResolveInfo;
import android.content.res.Configuration;
import android.graphics.Rect;
import android.net.Uri;
import android.os.Binder;
import android.os.Handler;
import android.os.Looper;
import android.os.Bundle;
import android.os.IBinder;
import android.os.IRemoteCallback;
import android.os.Parcel;
import android.os.PersistableBundle;
import android.os.RemoteCallbackList;
import android.os.RemoteException;
import android.os.SystemClock;
import android.os.UserHandle;
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
import java.util.HashSet;
import java.util.List;
import java.util.Map;
import java.util.Objects;
import java.util.Set;
import java.util.concurrent.Executor;
import java.util.concurrent.Executors;
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

    /**
     * How long startActivityAndWait waits for the activity to draw its first frame or be
     * idle. Longer than Android's idle timeout (ActivityTaskSupervisor.IDLE_TIMEOUT, 10 s
     * after the activity resumed): it also covers starting the process, and an app's first
     * start on Windows (dexopt, an interpreted APK) can take minutes.
     */
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
        /** Same immutable app-task presentation as the package's other live processes. */
        boolean separateWindows;
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
        /** Actual native generation: a retained child handle or trusted local startup. */
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
        /** Actual manual-driver lifecycle; null for a server-driven application. */
        State inProcessActivityState;
        InProcessActivity inProcessActivity;
        /** Actual activity-start history for BIND_ALLOW_OOM_MANAGEMENT; owned by this process. */
        boolean shortcutHasShownUi;
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
        int chooserCoverCount;
        boolean resumeAfterChooser;
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
        /**
         * The first frame of one of its windows was drawn in its process (ActivityRecord
         * .onWindowsDrawn; ActivityClientController#windowsDrawn). Unlike idle it stays
         * through a resume or relaunch (a preserved window is not drawn "first" again);
         * a new process must draw anew.
         */
        boolean drawn;
        boolean finishing;
        boolean preserveProcessOnFinish;
        int exactAlarmSettings;
        boolean resumeAfterExactAlarmSettings;
        int nativeUiCount;
        boolean resumeAfterNativeUi;
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
        boolean launchingPaused;
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
    private final ExactAlarmAccessRequests mExactAlarmRequests;
    /** Accepted system Settings requests, including queued/headless windows. Guarded by this. */
    private final java.util.Set<ExactAlarmSettingsSession> mExactAlarmSettingsSessions = new java.util.HashSet<>();
    private BluetoothRequests mBluetoothRequests; // server-local backend injection for tests
    private final Set<BluetoothActivity> mBluetoothActivities = new HashSet<>();
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
    private final Map<Object, DocumentSession> mDocumentPickers = new HashMap<>();
    private DocumentPicker.Backend mDocumentDialog = DocumentPicker.WINDOWS;
    private final Map<Object, IntentChoice> mIntentChoosers = new HashMap<>();
    private IntentChooser.Dialog mIntentChooserDialog = IntentChooser.HOST; // server-local UI fixture only
    private int mNextTaskId = 1;
    private long mNextActivityOrder = 1;
    /** Tasks moved to the back (their window minimized): none of their activities is visible. */
    private final java.util.Set<Integer> mHiddenTasks = new java.util.HashSet<>();
    /**
     * One window per app (ADR 0088): tasks moved to the back because another task of their
     * package came to the front (also in mHiddenTasks), the package's task in front, and the
     * hidden tasks whose activities are still to be paused and stopped (updateTopResumed).
     */
    private final java.util.Set<Integer> mAppHiddenTasks = new java.util.HashSet<>();
    private final Map<String, Integer> mAppFront = new HashMap<>();
    /** When each app's window last changed the task it shows (uptimeMillis). */
    private final Map<String, Long> mAppFrontSince = new HashMap<>();
    /**
     * How long after a task covers its app's others a window activation cannot bring one of
     * them back: activation events of the covered task's window (the window still shown, a
     * focus change already queued, the launcher bringing the app's window forward) arrive
     * after the task in front was started.
     */
    static final long APP_FRONT_SETTLE_MS = 1000;
    private final List<Integer> mAppHidePending = new ArrayList<>();
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
    /** Stable task identity and base intent, independent of its current activities or process. */
    private final Map<Integer, AppTaskRecord> mAppTasks = new HashMap<>();
    private final RemoteCallbackList<ITaskStackListener> mTaskListeners = new RemoteCallbackList<>();
    private final Executor mTaskCallbacks = Executors.newSingleThreadExecutor(work -> {
        Thread thread = new Thread(work, "task-stack-callbacks");
        thread.setDaemon(true);
        return thread;
    });
    private boolean mTaskCallbackPending; // Guarded by this.
    /** The activity told it is the top resumed one (onTopResumedActivityChanged), or null. */
    private ActivityRecord mTopResumed;

    private volatile Runnable mTopResumedPackageListener;
    private String mLastTopResumedPackage; // Guarded by this.

    /** Current top application's UID for foreground-only service APIs. */
    synchronized int topResumedUid() {
        return mTopResumed != null && mTopResumed.app != null ? mTopResumed.app.uid : -1;
    }

    /** Current top application for scheduler policy, including with legacy shared UIDs. */
    synchronized String topResumedPackage() {
        return mTopResumed != null && mTopResumed.app != null ? mTopResumed.app.packageName : null;
    }

    void setTopResumedPackageListener(Runnable listener) {
        mTopResumedPackageListener = listener;
    }

    /** Processes started for a package and not attached yet, by start sequence number. */
    private final Map<Long, PendingProcess> mStarting = new HashMap<>();
    private long mNextStartSeq = 1;
    /** How app processes are started; null: they start by themselves and attach. */
    private volatile ProcessStarter mStarter;
    /** The OS exit code of the latest start, unknown until that process actually exits. */
    private final ProcessExitStatus mExitStatus = new ProcessExitStatus();
    /** Since when (uptimeMillis) no app process runs or starts; 0 while some do. */
    private long mIdleSince = SystemClock.uptimeMillis();
    private boolean mDexoptBusy;

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
        final boolean separateWindows;
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
            this(packageName, processName, reason, windowMode, windowModeExplicit,
                    com.gesher.app.window.AppWindows.separate(packageName));
        }
        PendingProcess(String packageName, String processName, String reason, String windowMode,
                boolean windowModeExplicit, boolean separateWindows) {
            this.packageName = packageName;
            this.processName = processName;
            this.reason = reason;
            this.windowMode = windowMode;
            this.windowModeExplicit = windowModeExplicit;
            this.separateWindows = separateWindows;
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
        mExactAlarmRequests = pm.exactAlarmAccessRequests();
        mBluetoothRequests = new BluetoothRequests(pm);
    }

    void setActivityManager(ActivityManagerService am) {
        mAm = am;
    }

    void attachShortcuts(DesktopShortcutService shortcuts) {
        if (mAm != null) mAm.attachShortcuts(shortcuts);
    }

    synchronized boolean shortcutForeground(int uid) {
        if (mAm == null) return false;
        return mAm.shortcutState(uid, shortcutRoots(), mProcesses, this::shortcutCodeCurrent);
    }

    /** Record real visits even when the process returns to background between API calls. */
    synchronized void noteShortcutForeground(ProcessRecord visited, int state) {
        if (mAm == null) return;
        java.util.Map<ProcessRecord, Integer> roots = shortcutRoots();
        if (visited != null && shortcutProcessCurrent(visited)) {
            roots = new java.util.HashMap<>(roots);
            roots.merge(visited, state, Math::min);
        }
        mAm.shortcutState(-1, roots, mProcesses, this::shortcutCodeCurrent);
    }

    private boolean shortcutProcessCurrent(ProcessRecord app) {
        return app != null && mProcesses.contains(app) && shortcutCodeCurrent(app);
    }

    private boolean shortcutCodeCurrent(ProcessRecord app) {
        return app.ready && !app.dead && !app.exiting
                && !app.pidSuperseded && app.packageCode != null && app.packageCode.uid() == app.uid
                && mPm.get(app.packageName) == app.packageCode && !mPm.isRemovalPending(app.packageName);
    }

    private java.util.Map<ProcessRecord, Integer> shortcutRoots() {
        java.util.Map<ProcessRecord, Integer> roots = null;
        for (ProcessRecord app : mProcesses) {
            if (!app.ready || app.dead || app.exiting || app.pidSuperseded || app.packageCode == null
                    || app.packageCode.uid() != app.uid || mPm.get(app.packageName) != app.packageCode
                    || mPm.isRemovalPending(app.packageName)) continue;
            // OomAdjuster: a process with a visible activity, or one pausing or paused
            // (an activity Gesher keeps paused under a translucent one), is TOP.
            State own = app.inProcessActivityState;
            boolean top = own == State.RESUMED || own == State.RESUMING
                    || own == State.PAUSING || own == State.PAUSED;
            for (ActivityRecord activity : app.activities) {
                if (top) break;
                if (activity.finishing || !mActivities.contains(activity)
                        || !activityCodeCurrent(activity)) continue;
                State state = activity.state;
                boolean visibleState = state == State.RESUMED || state == State.RESUMING
                        || state == State.STARTED
                        || state == State.LAUNCHING && !activity.launchingPaused;
                top = state == State.PAUSING || state == State.PAUSED
                        || visibleState && isVisible(activity);
            }
            if (top) {
                if (roots == null) roots = new java.util.HashMap<>();
                roots.put(app, ActivityManager.PROCESS_STATE_TOP);
            }
        }
        return roots == null ? java.util.Map.of() : roots;
    }

    /** Snapshot actual process/activity state without nesting a service-controller monitor. */
    synchronized java.util.Map<Integer, AppOpsService.UidState> appOpsUidStates() {
        java.util.Map<Integer, AppOpsService.UidState> result = new java.util.HashMap<>();
        for (ProcessRecord app : mProcesses) {
            if (app.dead || app.exiting || app.pidSuperseded || !app.ready
                    || app.packageCode == null || mPm.get(app.packageName) != app.packageCode
                    || mPm.isRemovalPending(app.packageName)) continue;
            int state = android.app.AppOpsManager.UID_STATE_CACHED;
            int capabilities = 0;
            if (app.inProcessActivityState == State.RESUMING
                    || app.inProcessActivityState == State.RESUMED) {
                state = android.app.AppOpsManager.UID_STATE_TOP;
                capabilities = android.app.ActivityManager.PROCESS_CAPABILITY_FOREGROUND_LOCATION
                        | android.app.ActivityManager.PROCESS_CAPABILITY_FOREGROUND_CAMERA
                        | android.app.ActivityManager.PROCESS_CAPABILITY_FOREGROUND_MICROPHONE
                        | android.app.ActivityManager.PROCESS_CAPABILITY_FOREGROUND_AUDIO_CONTROL;
            } else if (app.inProcessActivityState == State.STARTED
                    || app.inProcessActivityState == State.PAUSED) {
                state = android.app.AppOpsManager.UID_STATE_FOREGROUND;
            }
            for (ActivityRecord activity : app.activities) {
                if (activity.finishing || mHiddenTasks.contains(activity.taskId)) continue;
                if (activity.state == State.RESUMED || activity.state == State.RESUMING) {
                    state = android.app.AppOpsManager.UID_STATE_TOP;
                    capabilities = android.app.ActivityManager.PROCESS_CAPABILITY_FOREGROUND_LOCATION
                            | android.app.ActivityManager.PROCESS_CAPABILITY_FOREGROUND_CAMERA
                            | android.app.ActivityManager.PROCESS_CAPABILITY_FOREGROUND_MICROPHONE
                            | android.app.ActivityManager.PROCESS_CAPABILITY_FOREGROUND_AUDIO_CONTROL;
                    break;
                }
                if (activity.state == State.STARTED || activity.state == State.PAUSING
                        || activity.state == State.PAUSED) state = android.app.AppOpsManager.UID_STATE_FOREGROUND;
            }
            AppOpsService.UidState previous = result.get(app.packageCode.uid());
            if (previous != null) {
                state = Math.min(state, previous.state()); capabilities |= previous.capabilities();
            }
            result.put(app.packageCode.uid(), new AppOpsService.UidState(state, capabilities));
        }
        return result;
    }

    UriGrants uriGrants() {
        return mAm != null ? mAm.uriGrants() : null;
    }

    /** Resolves a native foreground observation to a current, authenticated application. */
    synchronized boolean ownsForegroundProcess(int uid, long[] foreground) {
        if (foreground == null || foreground.length != 2 || foreground[0] <= 0
                || foreground[0] > Integer.MAX_VALUE || foreground[1] <= 0) return false;
        ProcessRecord app = attachedProcessByPid((int) foreground[0]);
        return app != null && app.ready && !app.dead && !app.exiting && !app.pidSuperseded
                && app.uid == uid && app.nativeStartTime == foreground[1]
                && app.packageCode != null && app.packageCode.uid() == uid
                && mPm.get(app.packageName) == app.packageCode
                && !mPm.isRemovalPending(app.packageName);
    }

    /**
     * Runtime-only admission for the manual ActivityThread driver. No caller package claim
     * or synthetic focus is accepted: the exact admitted APK, native UID/PID and live
     * Windows process generation must agree before any APK classes are loaded.
     */
    synchronized ProcessRecord attachInProcessApplication(ApkPackage owner,
            IApplicationThread thread) {
        int pid = android.os.Process.myPid();
        int uid = android.os.Process.myUid();
        if (mStarter != null || owner == null || owner != mPm.get(owner.packageName)
                || owner.uidPending() || owner.uid() != uid
                || mPm.isRemovalPending(owner.packageName)
                || Binder.getCallingPid() != pid || Binder.getCallingUid() != uid
                || thread == null || attachedProcessByPid(pid) != null) {
            throw new SecurityException("Standalone process does not match its admitted APK");
        }
        long startTime = Zygote.startTime(pid);
        if (startTime <= 0) {
            throw new IllegalStateException("Standalone native process identity is unavailable");
        }
        ProcessRecord app = new ProcessRecord(pid, thread);
        app.packageCode = owner;
        app.nativeStartTime = startTime;
        app.inProcessActivityState = State.STOPPED;
        assignPackage(app, owner.packageName, componentProcess(owner.packageName, null));
        app.ready = true;
        registerAttachedProcess(app);
        updateIdle();
        return app;
    }

    private record InProcessActivity(Binder token, Handler handler,
            java.util.function.BooleanSupplier active, InProcessApplication.ActivityResult result) {}

    private boolean isCurrentInProcessApplication(ProcessRecord app) {
        return app != null && app.inProcessActivityState != null && app.ready
                && !app.dead && !app.exiting && !app.pidSuperseded
                && attachedProcessByPid(app.pid) == app
                && app.packageCode != null && mPm.get(app.packageName) == app.packageCode
                && !mPm.isRemovalPending(app.packageName)
                && app.packageCode.matchesEnabledApplication(0)
                && (app.enabledProcessLifetime == null || app.enabledProcessLifetime.getAsBoolean());
    }

    synchronized Binder registerInProcessActivity(ProcessRecord app,
            java.util.function.BooleanSupplier active, InProcessApplication.ActivityResult result) {
        if (!isCurrentInProcessApplication(app) || app.inProcessActivity != null
                || app.pid != Binder.getCallingPid() || app.uid != Binder.getCallingUid()) {
            throw new SecurityException("Standalone activity requires its admitted process");
        }
        Binder token = new Binder("Standalone activity");
        app.inProcessActivity = new InProcessActivity(token, new Handler(Looper.getMainLooper()),
                Objects.requireNonNull(active), Objects.requireNonNull(result));
        return token;
    }

    /** Manual-driver lifecycle is used for AppOps state, never as proof of Windows focus. */
    synchronized void inProcessActivityState(ProcessRecord app, State state) {
        if (app.inProcessActivityState == null || app.dead || app.exiting
                || !mProcesses.contains(app) || app.packageCode != mPm.get(app.packageName)
                || mPm.isRemovalPending(app.packageName)) return;
        app.inProcessActivityState = state;
        if (app.ready && !app.pidSuperseded && (state == State.RESUMED || state == State.RESUMING)) {
            app.shortcutHasShownUi = true;
        }
        updateIdle();
    }

    void retireInProcessApplication(ProcessRecord app) {
        synchronized (this) {
            if (app.inProcessActivityState == null) {
                throw new IllegalArgumentException("Not a standalone process admission");
            }
            app.ready = false;
            app.exiting = true;
            app.inProcessActivityState = State.STOPPED;
            app.inProcessActivity = null;
        }
        processDied(app);
    }

    void setAccountManager(AccountManagerService accounts) { mAccounts = accounts; }

    private DesktopUsageStatsService mUsageStats;

    void setUsageStats(DesktopUsageStatsService usageStats) { mUsageStats = usageStats; }

    void flushUsageStats() {
        if (mUsageStats != null) mUsageStats.flush();
    }

    private void recordUsage(ActivityRecord record, ActivityUsageHistory.State state) {
        if (state == ActivityUsageHistory.State.RESUMED && record.app != null
                && record.app.ready && !record.app.dead && !record.app.exiting && !record.app.pidSuperseded
                && mProcesses.contains(record.app) && mActivities.contains(record)
                && record.app.activities.contains(record) && activityCodeCurrent(record)
                && record.app.uid == record.targetCode.uid()) {
            record.app.shortcutHasShownUi = true;
            noteShortcutForeground(record.app, ActivityManager.PROCESS_STATE_TOP);
        }
        if (mUsageStats != null) {
            ActivityRecord root = rootOf(record.taskId);
            mUsageStats.transition(record.token, record.info.packageName, record.info.name,
                    root == null ? record.info.packageName : root.info.packageName, state);
        }
    }

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
            updateIdle();
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
                    updateIdle();
                    ActivityTaskManagerService.this.notifyAll();
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
            synchronized (this) {
                mAccountChoosers.remove(token, reservation);
                updateIdle();
                notifyAll();
            }
            throw e;
        }
    }

    /** Transient system UI; launch and URI-grant authority stay with the original caller. */
    private final class IntentChoice {
        final Object key;
        final IntentChooser request;
        final String creator;
        final ApkPackage creatorCode;
        final ProcessRecord owner;
        final ActivityRecord origin, covered;
        final ProcessRecord originProcess, coveredProcess;
        final java.util.function.BooleanSupplier delegated, publication;
        final String resultWho;
        final int requestCode, taskId;
        final AppTaskRecord requiredTask;
        final boolean shareIdentity, newTask;
        boolean completed, cancelDelivered;

        IntentChoice(Object key, IntentChooser request, String creator, ApkPackage creatorCode,
                ProcessRecord owner, ActivityRecord origin, ActivityRecord covered,
                java.util.function.BooleanSupplier delegated,
                java.util.function.BooleanSupplier publication, String resultWho, int requestCode,
                int taskId, boolean shareIdentity, boolean newTask, AppTaskRecord requiredTask) {
            this.key = key; this.request = request; this.creator = creator; this.creatorCode = creatorCode;
            this.owner = owner; this.origin = origin; this.covered = covered;
            originProcess = origin == null ? null : origin.app;
            coveredProcess = covered == null ? null : covered.app;
            this.delegated = delegated; this.publication = publication; this.resultWho = resultWho;
            this.requestCode = requestCode; this.taskId = taskId; this.shareIdentity = shareIdentity;
            this.newTask = newTask;
            this.requiredTask = requiredTask;
        }
        boolean creatorCurrent() {
            return (creatorCode == null ? "android".equals(creator)
                    : mPm.get(creator) == creatorCode && !mPm.isRemovalPending(creator))
                    && (delegated == null || delegated.getAsBoolean());
        }
        boolean ownerCurrent() {
            synchronized (ActivityTaskManagerService.this) {
                return !completed && mIntentChoosers.get(key) == this && creatorCurrent()
                        && (requiredTask == null || hasAppTaskLocked(requiredTask))
                        && (publication == null || publication.getAsBoolean())
                        && (owner == null || !owner.exiting && !owner.dead && !owner.pidSuperseded
                            && mProcesses.contains(owner))
                        && (origin == null || origin.app == originProcess && originProcess != null
                            && !originProcess.exiting && !originProcess.dead && !originProcess.pidSuperseded
                            && mProcesses.contains(originProcess) && !origin.finishing
                            && mActivities.contains(origin) && activityEnabled(origin));
            }
        }
        boolean coveredCurrent() {
            return covered == null || covered.app == coveredProcess && coveredProcess != null
                    && !coveredProcess.exiting && !coveredProcess.dead && !coveredProcess.pidSuperseded
                    && mProcesses.contains(coveredProcess) && !covered.finishing
                    && mActivities.contains(covered) && activityEnabled(covered)
                    && topOf(covered.taskId) == covered;
        }
        boolean active() {
            synchronized (ActivityTaskManagerService.this) {
                return ownerCurrent() && coveredCurrent()
                        && (covered == null || !mHiddenTasks.contains(covered.taskId));
            }
        }
    }

    private int chooseIntent(Intent incoming, String verifiedCaller, ProcessRecord process,
            boolean internalCreator, java.util.function.BooleanSupplier delegated,
            java.util.function.BooleanSupplier publication, IBinder resultTo, String resultWho,
            int requestCode, int launchTaskId, boolean shareIdentity, ActivityRecord[] started,
            AppTaskRecord requiredTask) {
        String creator = verifiedCaller == null ? "android" : verifiedCaller;
        final ApkPackage creatorCode;
        final android.content.pm.ApplicationInfo creatorInfo;
        synchronized (mPm) {
            creatorCode = mPm.get(creator);
            creatorInfo = creatorCode != null ? creatorCode.applicationInfo(0) : mPm.getApplicationInfo(creator, 0, 0);
        }
        if (creatorInfo == null) return ActivityManager.START_CANCELED;
        DocumentPicker.Backend documents = mDocumentDialog;
        IntentChooser request = new IntentChooser(mPm, verifiedCaller, incoming,
                mAm == null ? null : mAm.mimeResolver(creatorInfo, process),
                documents != null && documents.available());
        // A document target's system picker is one of the options (IntentChooser.Option
        // documentPicker); as the only one it auto-launches like any single choice.
        if (request.unsupported || request.options.isEmpty()) return ActivityManager.START_INTENT_NOT_RESOLVED;
        IntentChooser.Dialog dialog = mIntentChooserDialog;
        // Check the actual JNI backend before accepting a chooser that needs UI.
        if (!request.autoLaunch && (dialog == null || !dialog.available())) {
            return ActivityManager.START_INTENT_NOT_RESOLVED;
        }
        final IntentChoice choice;
        boolean pauseCovered;
        synchronized (this) {
            if (requiredTask != null && !hasAppTaskLocked(requiredTask)) return ActivityManager.START_CANCELED;
            ActivityRecord origin = resultTo == null ? null : forToken(resultTo);
            if (resultTo != null && (origin == null || origin.app == null || origin.finishing
                    || !activityEnabled(origin) || !mProcesses.contains(origin.app)
                    || origin.app.exiting || origin.app.dead
                    || !internalCreator && origin.app != process)) {
                throw new SecurityException("Chooser source must belong to its live caller activity");
            }
            boolean newTask = requiredTask == null && origin == null
                    || (incoming.getFlags() & Intent.FLAG_ACTIVITY_NEW_TASK) != 0
                    || origin != null && origin.info.launchMode == ActivityInfo.LAUNCH_SINGLE_INSTANCE
                    || requestCode < 0 && (incoming.getFlags() & Intent.FLAG_ACTIVITY_NEW_DOCUMENT) != 0;
            int taskId = launchTaskId >= 0 && topOf(launchTaskId) != null ? launchTaskId
                    : !newTask ? origin.taskId : -1;
            ActivityRecord covered = taskId >= 0 ? topOf(taskId) : focusedTaskTopLocked();
            // Native UI can already cover the focused task without removing it.
            if (taskId < 0 && covered != null && covered.app == null) covered = null;
            Object key = origin != null ? origin.token : process != null ? process.thread.asBinder() : new Object();
            if (mIntentChoosers.size() >= 16 || mIntentChoosers.containsKey(key)) return ActivityManager.START_CANCELED;
            choice = new IntentChoice(key, request, creator, creatorCode,
                    internalCreator ? null : process, origin, covered, delegated, publication,
                    resultWho, requestCode, taskId, shareIdentity, newTask, requiredTask);
            mIntentChoosers.put(key, choice);
            if (!choice.ownerCurrent() || !choice.coveredCurrent()) {
                mIntentChoosers.remove(key); return ActivityManager.START_CANCELED;
            }
            pauseCovered = covered != null && (covered.state == State.RESUMED
                    || covered.state == State.RESUMING || covered.state == State.LAUNCHING);
            if (covered != null) {
                covered.chooserCoverCount++;
                covered.resumeAfterChooser = true;
                if (taskId >= 0) { mHiddenTasks.remove(taskId); focusTaskLocked(taskId); }
            }
            // A chooser can be the only visible UI (NEW_TASK without an ActivityRecord).
            // Its real reservation owns device use through the native worker's cleanup.
            updateIdle();
        }
        try {
            if (choice.newTask && choice.origin != null && requestCode >= 0) {
                choice.cancelDelivered = true;
                deliverResult(choice.origin, resultWho, requestCode, android.app.Activity.RESULT_CANCELED, null);
            }
            if (pauseCovered) pause(choice.covered, false,
                    (incoming.getFlags() & Intent.FLAG_ACTIVITY_NO_USER_ACTION) == 0);
            if (request.autoLaunch) {
                int result = launchIntentChoice(choice, 0, started);
                finishIntentChoice(choice, ActivityManager.isStartResultSuccessful(result));
                return result;
            }
            updateTopResumed();
            Thread worker = new Thread(() -> {
                boolean launched = false;
                try {
                    // A modal host dialog must not block the caller's Binder/UI thread.
                    // Wait for onPause, bounded like Android's pause timeout.
                    synchronized (ActivityTaskManagerService.this) {
                        long until = SystemClock.uptimeMillis() + 500;
                        while (choice.active() && choice.covered != null && choice.covered.state == State.PAUSING) {
                            long left = until - SystemClock.uptimeMillis();
                            if (left <= 0) break;
                            try { ActivityTaskManagerService.this.wait(left); }
                            catch (InterruptedException interrupted) { Thread.currentThread().interrupt(); return; }
                        }
                    }
                    if (!choice.active()) return;
                    String[] labels = new String[request.options.size()];
                    boolean[] enabled = new boolean[labels.length];
                    for (int i = 0; i < labels.length; ++i) {
                        labels[i] = request.options.get(i).label;
                        enabled[i] = !request.options.get(i).suspended();
                    }
                    int index = dialog.choose(creator, request.title, labels, enabled, choice::active);
                    if (choice.active() && index >= 0 && index < labels.length) {
                        launched = ActivityManager.isStartResultSuccessful(
                                launchIntentChoice(choice, index, new ActivityRecord[1]));
                    }
                } catch (RuntimeException | LinkageError failure) {
                    SystemServer.log("intent chooser unavailable: " + failure.getClass().getSimpleName());
                } finally { finishIntentChoice(choice, launched); }
            }, "intent-chooser");
            worker.setDaemon(true);
            worker.start();
            return ActivityManager.START_SUCCESS;
        } catch (RuntimeException | Error failure) {
            finishIntentChoice(choice, false);
            throw failure;
        }
    }

    private int launchIntentChoice(IntentChoice choice, int index, ActivityRecord[] started) {
        IntentChooser.Option option = choice.request.options.get(index);
        if (option.documentPicker) return launchDocumentChoice(choice, option);
        if (!choice.active() || !option.current(mPm, choice.creator)) return ActivityManager.START_CANCELED;
        Intent launch = IntentChooser.snapshot(option.intent);
        if (choice.newTask) launch.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
        boolean newTask = (launch.getFlags() & Intent.FLAG_ACTIVITY_NEW_TASK) != 0;
        long identity = Binder.clearCallingIdentity();
        try {
            int result = withVerifiedCreator(choice.creator, choice::creatorCurrent,
                    () -> choice.active() && mPm.get(option.component.getPackageName()) == option.code
                            && !mPm.isRemovalPending(option.component.getPackageName())
                            && option.code.componentEnabled(option.component.getClassName(), ApkPackage.ComponentKind.ACTIVITY),
                    () -> start(null, launch, launch.getType(), choice.creator,
                            choice.origin == null ? null : choice.origin.token, choice.resultWho,
                            newTask ? -1 : choice.requestCode, choice.taskId, choice.shareIdentity,
                            started, choice.requiredTask));
            if (ActivityManager.isStartResultSuccessful(result)) {
                if (newTask && choice.origin != null && choice.requestCode >= 0 && !choice.cancelDelivered) {
                    choice.cancelDelivered = true;
                    deliverResult(choice.origin, choice.resultWho, choice.requestCode, android.app.Activity.RESULT_CANCELED, null);
                }
                if (choice.request.callback != null) {
                    Intent notification = new Intent().putExtra(Intent.EXTRA_CHOSEN_COMPONENT, option.component)
                            .putExtra(Intent.EXTRA_CHOOSER_RESULT, new android.service.chooser.ChooserResult(
                                    android.service.chooser.ChooserResult.CHOOSER_RESULT_SELECTED_COMPONENT,
                                    option.component, false));
                    // The callback is a caller-supplied PendingIntent capability. It runs only
                    // after normal activity admission and URI-grant publication succeeded.
                    try { mAm.sendIntentSender(null, choice.request.callback.getTarget(),
                            choice.request.callback.getWhitelistToken(), android.app.Activity.RESULT_OK,
                            notification, null, null, null, null); }
                    catch (RuntimeException ignored) { SystemServer.log("chosen component callback unavailable"); }
                }
            }
            return result;
        } finally { Binder.restoreCallingIdentity(identity); }
    }

    /**
     * The chooser's system-picker entry: the original document request goes to the
     * host picker with the chooser caller's result route, creator and task. The
     * chooser's NEW_TASK, NO_USER_ACTION and NEW_DOCUMENT flags carry over; a
     * new-task chooser already returned the caller's RESULT_CANCELED, so the picker
     * then has no result recipient, as for a chosen activity. Called without ATM,
     * launch or package locks: the picker initializes its provider transport and
     * takes only this service's monitor for its own reservation.
     */
    private int launchDocumentChoice(IntentChoice choice, IntentChooser.Option option) {
        if (!choice.active()) return ActivityManager.START_CANCELED;
        Intent document = IntentChooser.snapshot(option.intent);
        int forward = choice.request.chooser.getFlags() & (Intent.FLAG_ACTIVITY_NEW_TASK
                | Intent.FLAG_ACTIVITY_NO_USER_ACTION | Intent.FLAG_ACTIVITY_NEW_DOCUMENT);
        if (choice.newTask) forward |= Intent.FLAG_ACTIVITY_NEW_TASK;
        document.addFlags(forward);
        boolean newTask = (document.getFlags() & Intent.FLAG_ACTIVITY_NEW_TASK) != 0;
        long identity = Binder.clearCallingIdentity();
        try {
            int result = withVerifiedCreator(choice.creator, choice::creatorCurrent, choice::active,
                    () -> startDocumentPicker(null, choice.creator, document, null,
                            choice.origin == null ? null : choice.origin.token, choice.resultWho,
                            newTask ? -1 : choice.requestCode, choice.taskId, choice.requiredTask));
            if (ActivityManager.isStartResultSuccessful(result) && newTask && choice.origin != null
                    && choice.requestCode >= 0 && !choice.cancelDelivered) {
                choice.cancelDelivered = true;
                deliverResult(choice.origin, choice.resultWho, choice.requestCode,
                        android.app.Activity.RESULT_CANCELED, null);
            }
            return result;
        } catch (IllegalArgumentException invalid) {
            // IntentChooser offered only a validated request; a late failure cancels
            // this choice (RESULT_CANCELED through finishIntentChoice), never the start.
            SystemServer.log("intent chooser: document picker request rejected: " + invalid.getMessage());
            return ActivityManager.START_CANCELED;
        } finally { Binder.restoreCallingIdentity(identity); }
    }

    private void finishIntentChoice(IntentChoice choice, boolean launched) {
        boolean deliver, resumeCovered;
        synchronized (this) {
            if (choice.completed) return;
            deliver = choice.ownerCurrent() && !launched && !choice.cancelDelivered && choice.requestCode >= 0;
            choice.completed = true;
            mIntentChoosers.remove(choice.key, choice);
            ActivityRecord covered = choice.covered;
            resumeCovered = false;
            if (covered != null) {
                covered.chooserCoverCount--;
                if (covered.chooserCoverCount == 0 && covered.state != State.PAUSING) {
                    covered.resumeAfterChooser = false;
                    resumeCovered = covered.app == choice.coveredProcess && !covered.finishing
                            && !choice.coveredProcess.exiting && !choice.coveredProcess.dead
                            && activityEnabled(covered) && topOf(covered.taskId) == covered
                            && !mHiddenTasks.contains(covered.taskId);
                }
            }
            updateIdle();
            notifyAll();
        }
        if (deliver) deliverResult(choice.origin, choice.resultWho, choice.requestCode,
                android.app.Activity.RESULT_CANCELED, null);
        if (resumeCovered) { realStartActivity(choice.covered); resume(choice.covered); }
        updateTopResumed();
    }

    private static void accountActivityFinished(ActivityRecord record) {
        Runnable completion = record.accountCompletion;
        record.accountCompletion = null;
        if (completion != null) completion.run();
    }

    private final class DocumentSession {
        final Object key;
        final ActivityRecord origin, recipient, covered;
        final ProcessRecord process, originProcess, coveredProcess;
        final ApkPackage creator;
        final java.util.function.BooleanSupplier delegated;
        final String resultWho;
        final int requestCode;
        final AppTaskRecord requiredTask;

        DocumentSession(Object key, ActivityRecord origin, ActivityRecord recipient,
                ActivityRecord covered, ProcessRecord process, ApkPackage creator,
                java.util.function.BooleanSupplier delegated, String resultWho, int requestCode,
                AppTaskRecord requiredTask) {
            this.key = key; this.origin = origin; this.recipient = recipient; this.covered = covered;
            this.process = process; this.creator = creator; this.delegated = delegated;
            this.resultWho = resultWho; this.requestCode = requestCode;
            this.requiredTask = requiredTask;
            originProcess = origin == null ? null : origin.app;
            coveredProcess = covered == null ? null : covered.app;
        }

        boolean active() {
            synchronized (ActivityTaskManagerService.this) {
                return mDocumentPickers.get(key) == this
                        && (requiredTask == null || hasAppTaskLocked(requiredTask))
                        && (process == null || liveProcess(process))
                        && (creator == null || mPm.get(creator.packageName) == creator
                            && !mPm.isRemovalPending(creator.packageName) && creator.matchesEnabledApplication(0))
                        && (delegated == null || delegated.getAsBoolean())
                        && (origin == null || origin.app == originProcess && originProcess != null
                            && liveProcess(originProcess)
                            && !origin.finishing && origin.state != State.DESTROYED
                            && mActivities.contains(origin) && activityEnabled(origin));
            }
        }

        private boolean liveProcess(ProcessRecord record) {
            return !record.exiting && !record.dead && !record.pidSuperseded
                    && mProcesses.contains(record)
                    && (record.enabledProcessLifetime == null || record.enabledProcessLifetime.getAsBoolean());
        }

        void awaitPause() {
            if (covered == null) return;
            synchronized (ActivityTaskManagerService.this) {
                long until = SystemClock.uptimeMillis() + 500;
                while (active() && covered.app == coveredProcess && covered.state == State.PAUSING) {
                    long left = until - SystemClock.uptimeMillis();
                    if (left <= 0) break;
                    try { ActivityTaskManagerService.this.wait(left); }
                    catch (InterruptedException interrupted) { Thread.currentThread().interrupt(); return; }
                }
            }
        }
    }

    private int startDocumentPicker(IApplicationThread caller, String creatorName, Intent intent,
            String resolvedType, IBinder originToken, String resultWho, int requestCode, int launchTaskId,
            AppTaskRecord requiredTask) {
        final DocumentPicker.Backend backend = mDocumentDialog;
        if (!DocumentPicker.resolves(intent, resolvedType) || !backend.available()) {
            return ActivityManager.START_INTENT_NOT_RESOLVED;
        }
        final ApkPackage creator = mPm.get(creatorName);
        final DocumentPicker.Request request = new DocumentPicker.Request(intent, resolvedType,
                creator == null ? 36 : creator.parsed.getTargetSdkVersion());
        final DocumentsStorageProvider provider = mAm.documents();
        provider.holder(); // Initialize the real framework transport before exposing any result URI.
        final DocumentSession session;
        final boolean cancelResult;
        boolean pauseCovered = false;
        synchronized (this) {
            if (requiredTask != null && !hasAppTaskLocked(requiredTask)) return ActivityManager.START_CANCELED;
            ProcessRecord process = callerProcess(Binder.getCallingPid());
            ActivityRecord origin = originToken == null ? null : forToken(originToken);
            boolean internal = mVerifiedCreator.get() != null;
            if (originToken != null && (origin == null || origin.app == null
                    || !internal && (process == null || origin.app != process || caller == null
                        || !caller.asBinder().equals(process.thread.asBinder()))
                    || origin.finishing || origin.state == State.DESTROYED || !activityEnabled(origin))) {
                throw new SecurityException("Document result must belong to its live caller activity");
            }
            if (requestCode >= 0 && origin == null) {
                throw new SecurityException("Document result requires a live activity");
            }
            if (!internal && !mAm.callers().isSystem() && (process == null || creator == null
                    || !creator.packageName.equals(process.packageName))) {
                throw new SecurityException("Document picker requires its authenticated process");
            }
            Object key = origin == null ? (process == null ? new Object() : process) : origin.token;
            if (mDocumentPickers.containsKey(key) || mDocumentPickers.size() >= 16) {
                return ActivityManager.START_CANCELED;
            }
            boolean newTask = requiredTask == null && origin == null
                    || (intent.getFlags() & Intent.FLAG_ACTIVITY_NEW_TASK) != 0
                    || origin != null && origin.info.launchMode == ActivityInfo.LAUNCH_SINGLE_INSTANCE
                    || requestCode < 0 && (intent.getFlags() & Intent.FLAG_ACTIVITY_NEW_DOCUMENT) != 0;
            cancelResult = origin != null && requestCode >= 0 && newTask;
            ActivityRecord recipient = origin != null && requestCode >= 0 && !newTask ? origin : null;
            int task = launchTaskId >= 0 && topOf(launchTaskId) != null ? launchTaskId
                    : !newTask ? origin.taskId : -1;
            ActivityRecord covered = task >= 0 ? topOf(task) : focusedTaskTopLocked();
            if (task < 0 && covered != null && covered.app == null) covered = null;
            session = new DocumentSession(key, origin, recipient, covered, process, creator,
                    mVerifiedLifetime.get(), resultWho, requestCode, requiredTask);
            mDocumentPickers.put(key, session);
            if (task >= 0) { mHiddenTasks.remove(task); focusTaskLocked(task); }
            if (covered != null) {
                covered.nativeUiCount++;
                covered.resumeAfterNativeUi = true;
                pauseCovered = covered.state == State.RESUMED || covered.state == State.RESUMING
                        || covered.state == State.LAUNCHING;
            }
            // A picker may be the only live UI in a headless caller. Keep its
            // reservation until the native window and selection worker return.
            updateIdle();
        }
        try {
            if (cancelResult) deliverResult(session.origin, resultWho, requestCode,
                    android.app.Activity.RESULT_CANCELED, null);
            if (pauseCovered) pause(session.covered, false,
                    (intent.getFlags() & Intent.FLAG_ACTIVITY_NO_USER_ACTION) == 0);
            updateTopResumed();
            Thread worker = new Thread(() -> {
                long identity = Binder.clearCallingIdentity();
                DocumentPicker.Result result = null;
                boolean accepted = false;
                try {
                    session.awaitPause();
                    if (!session.active()) return;
                    int ownerPid = session.coveredProcess != null ? session.coveredProcess.pid
                            : session.originProcess != null ? session.originProcess.pid
                            : session.process != null ? session.process.pid : 0;
                    if (request.rejection != null) {
                        // Accepted like Android's picker activity, which then cancels.
                        SystemServer.log("document-picker: request cancelled: " + request.rejection);
                    }
                    String[] selected = request.rejection != null ? null
                            : backend.pick(request, ownerPid, session::active);
                    if (!session.active()) return;
                    result = DocumentPicker.select(request, selected, provider, backend);
                    synchronized (ActivityTaskManagerService.this) {
                        if (!session.active()) return;
                        if (result != null && session.recipient != null) {
                            List<UriGrants.Grant> prepared = result.prepare(mAm.uriGrants(), provider,
                                    session.recipient.info.packageName);
                            // Publish the whole selection, tied to the receiving Activity.
                            // Every grant participant is rechecked by the F11 foundation.
                            mAm.uriGrants().publish(session.recipient.token, prepared);
                        }
                        accepted = true;
                    }
                } catch (java.io.IOException | RuntimeException | LinkageError failure) {
                    SystemServer.log("document-picker: request failed: " + failure);
                } finally {
                    try {
                        if (!accepted && result != null) result.abandon();
                    } finally {
                        try { finishDocumentPicker(session, accepted && result != null ? result.intent : null); }
                        finally { Binder.restoreCallingIdentity(identity); }
                    }
                }
            }, "document-picker");
            worker.setDaemon(true);
            worker.start();
            return ActivityManager.START_SUCCESS;
        } catch (RuntimeException | Error failure) {
            finishDocumentPicker(session, null);
            throw failure;
        }
    }

    private void finishDocumentPicker(DocumentSession session, Intent result) {
        boolean deliver, resumeCovered = false;
        synchronized (this) {
            deliver = session.active();
            if (!mDocumentPickers.remove(session.key, session)) return;
            ActivityRecord covered = session.covered;
            if (covered != null && --covered.nativeUiCount == 0 && covered.resumeAfterNativeUi
                    && covered.state != State.PAUSING) {
                covered.resumeAfterNativeUi = false;
                resumeCovered = !covered.finishing && covered.state != State.DESTROYED
                        && mActivities.contains(covered) && activityEnabled(covered)
                        && covered.app == session.coveredProcess && covered.app != null
                        && session.liveProcess(covered.app)
                        && topOf(covered.taskId) == covered && !mHiddenTasks.contains(covered.taskId);
            }
            updateIdle();
            notifyAll();
        }
        if (deliver && session.recipient != null) deliverResult(session.recipient, session.resultWho,
                session.requestCode, result == null ? android.app.Activity.RESULT_CANCELED
                        : android.app.Activity.RESULT_OK, result);
        if (resumeCovered) { realStartActivity(session.covered); resume(session.covered); }
        updateTopResumed();
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
        if (userId != 0) throw new SecurityException("Only user 0 is available");
        if (mVerifiedCreator.get() == null && mAm != null) {
            boolean system = mAm.callers().isSystem();
            checkLaunchTaskOptions(options, mPm,
                    system ? callingPackage : mAm.callers().requirePackage(callingPackage), system);
        }
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
                if (!tasks.isEmpty()) notifyTaskStackChangedLocked();
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
            // complete, or it went away (cleanupActivity). Android completes it when the
            // activity's windows are drawn (ActivityRecord.onWindowsDrawn ->
            // reportActivityLaunched), not when its main thread is idle: an app that keeps
            // its looper busy (a map that posts frame after frame) is never idle, yet has
            // launched. Here: its first frame was drawn in its process, or else it is idle
            // (no window drawn: a headless process). A trampoline (a launcher activity that
            // starts the app's real one and stays below it, stopped, never drawn or idle)
            // completes the launch with that activity: one started after it in its task
            // that has launched (Android's launch transition ends when the task's windows
            // are drawn).
            while (!launched(r) && r.state != State.DESTROYED && launchedAbove(r) == null) {
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
            // A rejected native start or unexpected process death before the activity
            // drew or was idle is different from an activity deliberately finishing in
            // onCreate.
            if (r.launchFailed && !launched(r) && launchedAbove(r) == null) {
                result.result = ActivityManager.START_CANCELED;
            }
        }
        result.totalTime = SystemClock.uptimeMillis() - r.startTime;
        return result;
    }

    /** Its launch is complete: a window drawn, or idle. Guarded by this. */
    private static boolean launched(ActivityRecord r) {
        return r.drawn || r.idle;
    }

    /**
     * An activity started after {@code r} in its task that has launched ({@link #launched}),
     * or null. Guarded by this.
     */
    private ActivityRecord launchedAbove(ActivityRecord r) {
        for (int i = mActivities.size() - 1; i >= 0; i--) {
            ActivityRecord a = mActivities.get(i);
            if (a != r && a.taskId == r.taskId && launched(a) && a.state != State.DESTROYED
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
        return finishTogether(toFinish.get(0), toFinish, "remove-task",
                android.app.Activity.RESULT_CANCELED, null);
    }

    /** Metadata retained for a task, rather than whichever activity happens to be at its base. */
    private static final class AppTaskRecord {
        final int id;
        final ApkPackage owner;
        final WindowContainerToken token;
        Intent baseIntent;
        ComponentName baseComponent;
        IBinder handle;
        long lastActiveTime;

        AppTaskRecord(ActivityRecord root) {
            id = root.taskId;
            owner = root.targetCode;
            token = root.taskToken;
            updateBase(root);
        }

        void updateBase(ActivityRecord root) {
            baseIntent = new Intent(root.intent);
            baseIntent.setSourceBounds(null);
            baseIntent.setSelector(null);
            if ((root.info.flags & ActivityInfo.FLAG_EXCLUDE_FROM_RECENTS) != 0) {
                baseIntent.addFlags(Intent.FLAG_ACTIVITY_EXCLUDE_FROM_RECENTS);
            }
            baseComponent = root.component();
        }

        boolean available(PackageManagerService packages) {
            return owner == packages.get(owner.packageName)
                    && !packages.isRemovalPending(owner.packageName);
        }
    }

    @Override
    public List<IBinder> getAppTasks(String callingPackage) {
        Objects.requireNonNull(callingPackage, "callingPackage");
        String owner = mAm.callers().requirePackage(callingPackage);
        List<IBinder> result = new ArrayList<>();
        synchronized (this) {
            Set<Integer> live = new HashSet<>();
            for (ActivityRecord activity : mActivities) {
                if (isLiveTaskActivity(activity)) live.add(activity.taskId);
            }
            for (int i = mTaskOrder.size() - 1; i >= 0; i--) {
                AppTaskRecord task = mAppTasks.get(mTaskOrder.get(i));
                if (task != null && owner.equals(task.owner.packageName)
                        && live.contains(task.id) && task.available(mPm)) {
                    if (task.handle == null) task.handle = new OwnedAppTask(task);
                    result.add(task.handle);
                }
            }
        }
        return result;
    }

    @Override
    public ParceledListSlice<ActivityManager.RecentTaskInfo> getRecentTasks(int maxNum, int flags,
            int userId) {
        boolean system = mAm.callers().isSystem();
        String owner = system ? null : mAm.callers().callerPackage();
        if (userId != UserHandle.USER_SYSTEM && userId != UserHandle.USER_CURRENT
                && userId != UserHandle.USER_CURRENT_OR_SELF) {
            if (!system) throw new SecurityException("Recents are only available for the calling user");
            return ParceledListSlice.emptyList();
        }
        if (maxNum <= 0) return ParceledListSlice.emptyList();
        List<ActivityManager.RecentTaskInfo> result = new ArrayList<>();
        synchronized (this) {
            Map<Integer, TaskActivities> activities = new HashMap<>();
            for (ActivityRecord activity : mActivities) {
                if (isLiveTaskActivity(activity)) {
                    activities.computeIfAbsent(activity.taskId, ignored -> new TaskActivities()).add(activity);
                }
            }
            for (int i = mTaskOrder.size() - 1; i >= 0 && result.size() < maxNum; i--) {
                AppTaskRecord task = mAppTasks.get(mTaskOrder.get(i));
                if (task == null || !task.available(mPm)
                        || !system && !owner.equals(task.owner.packageName)) continue;
                TaskActivities members = activities.get(task.id);
                if (members == null) continue;
                if ((flags & ActivityManager.RECENT_WITH_EXCLUDED) == 0
                        && (task.baseIntent.getFlags() & Intent.FLAG_ACTIVITY_EXCLUDE_FROM_RECENTS) != 0
                        && !taskVisibleLocked(members.top)) continue;
                result.add(recentTaskInfoLocked(task, members, true, system));
            }
        }
        return new ParceledListSlice<>(result);
    }

    private static boolean isLiveTaskActivity(ActivityRecord activity) {
        return !activity.finishing && activity.state != State.DESTROYED;
    }

    private static final class TaskActivities {
        ActivityRecord base;
        ActivityRecord top;
        int count;

        void add(ActivityRecord record) {
            if (base == null) base = record;
            top = record;
            count++;
        }
    }

    /** Guarded by this. App callers only receive component details from their own package. */
    private ActivityManager.RecentTaskInfo recentTaskInfoLocked(AppTaskRecord task,
            TaskActivities members, boolean stripExtras, boolean showForeignActivities) {
        ActivityRecord base = members.base;
        ActivityRecord top = members.top;
        ActivityManager.RecentTaskInfo info = new ActivityManager.RecentTaskInfo();
        info.taskId = info.id = info.persistentId = task.id;
        info.userId = UserHandle.USER_SYSTEM;
        info.effectiveUid = task.owner.uid();
        info.displayId = Display.DEFAULT_DISPLAY;
        info.isRunning = true;
        info.numActivities = members.count;
        info.baseIntent = stripExtras ? task.baseIntent.cloneFilter() : new Intent(task.baseIntent);
        info.baseIntent.setFlags(task.baseIntent.getFlags());
        info.realActivity = task.baseComponent;
        info.baseActivity = showForeignActivities || task.owner == base.targetCode
                ? base.component() : new ComponentName("", "");
        boolean canSeeTop = showForeignActivities || task.owner == top.targetCode;
        info.topActivity = canSeeTop ? top.component() : new ComponentName("", "");
        if (canSeeTop) {
            info.topActivityInfo = new ActivityInfo(top.info);
            if (top.info.applicationInfo != null) {
                info.topActivityInfo.applicationInfo = new ApplicationInfo(top.info.applicationInfo);
            }
        }
        info.isFocused = mTopResumed != null && mTopResumed.taskId == task.id
                && !mHiddenTasks.contains(task.id);
        info.isVisible = taskVisibleLocked(top);
        info.isVisibleRequested = !mHiddenTasks.contains(task.id);
        info.lastActiveTime = task.lastActiveTime;
        info.token = task.token;
        info.taskDescription = new ActivityManager.TaskDescription();
        int rotation = mTaskRotation.get(task.id, Surface.ROTATION_0);
        info.configuration.setTo(effectiveConfiguration(LocaleManagerService.configurationFor(
                task.owner.packageName), taskBoundsLocked(task.id), rotation));
        return info;
    }

    private boolean taskVisibleLocked(ActivityRecord top) {
        return top.app != null && !mHiddenTasks.contains(top.taskId) && switch (top.state) {
            case LAUNCHING, RESUMING, RESUMED, PAUSING, PAUSED, STARTED, STOPPING -> true;
            default -> false;
        };
    }

    private final class OwnedAppTask extends IAppTask.Stub
            implements com.gesher.app.window.AppWindowTasks.Local {
        private final AppTaskRecord mTask;

        OwnedAppTask(AppTaskRecord task) { mTask = task; }

        @Override public android.os.IInterface queryLocalInterface(String descriptor) {
            if (com.gesher.app.window.AppWindowTasks.DESCRIPTOR.equals(descriptor)) return this;
            return super.queryLocalInterface(descriptor);
        }

        @Override public boolean onTransact(int code, Parcel data, Parcel reply, int flags)
                throws RemoteException {
            if (code != com.gesher.app.window.AppWindowTasks.FINISH_NON_MINIMIZED) {
                return super.onTransact(code, data, reply, flags);
            }
            data.enforceInterface(com.gesher.app.window.AppWindowTasks.DESCRIPTOR);
            data.enforceNoDataAvail();
            if (reply == null || (flags & IBinder.FLAG_ONEWAY) != 0) {
                throw new IllegalArgumentException("App-window close requires a reply");
            }
            boolean closed = finishFromAppWindow();
            reply.writeNoException();
            reply.writeBoolean(closed);
            return true;
        }

        private boolean checkCaller() {
            boolean system = mAm.callers().isSystem();
            if (!system) mAm.callers().requirePackage(mTask.owner.packageName);
            return system;
        }

        private void requireLiveTaskLocked() {
            if (!hasAppTaskLocked(mTask)) {
                throw new IllegalArgumentException("Unable to find task ID " + mTask.id);
            }
        }

        @Override public ActivityManager.RecentTaskInfo getTaskInfo() {
            boolean system = checkCaller();
            synchronized (ActivityTaskManagerService.this) {
                if (mAppTasks.get(mTask.id) != mTask || !mTask.available(mPm)) return null;
                TaskActivities members = new TaskActivities();
                for (ActivityRecord activity : mActivities) {
                    if (activity.taskId == mTask.id && isLiveTaskActivity(activity)) members.add(activity);
                }
                return members.top == null ? null : recentTaskInfoLocked(mTask, members, false, system);
            }
        }

        @Override public void setExcludeFromRecents(boolean exclude) {
            checkCaller();
            synchronized (ActivityTaskManagerService.this) {
                requireLiveTaskLocked();
                int original = mTask.baseIntent.getFlags();
                int updated = exclude ? original | Intent.FLAG_ACTIVITY_EXCLUDE_FROM_RECENTS
                        : original & ~Intent.FLAG_ACTIVITY_EXCLUDE_FROM_RECENTS;
                if (original != updated) {
                    mTask.baseIntent.setFlags(updated);
                    notifyTaskStackChangedLocked();
                }
            }
        }

        @Override public void finishAndRemoveTask() {
            checkCaller();
            synchronized (mLaunchLock) {
                synchronized (ActivityTaskManagerService.this) { requireLiveTaskLocked(); }
                if (!removeTask(mTask.id)) {
                    throw new IllegalArgumentException("Unable to find task ID " + mTask.id);
                }
            }
        }

        /** X closes other app tasks, but the user may have minimized one in another process. */
        @Override public boolean finishFromAppWindow() {
            checkCaller();
            synchronized (mLaunchLock) {
                List<ActivityRecord> selected = new ArrayList<>();
                synchronized (ActivityTaskManagerService.this) {
                    requireLiveTaskLocked();
                    if (userMinimized()) return false;
                    for (ActivityRecord activity : mActivities) {
                        if (activity.taskId == mTask.id && !activity.finishing) selected.add(activity);
                    }
                }
                if (selected.isEmpty()) return false;
                java.util.Collections.reverse(selected);
                // Recheck at the same lifecycle decision that marks every selected
                // Activity finishing. A minimize during result preparation wins.
                return finishTogether(selected.get(0), selected, "app-window-close",
                        android.app.Activity.RESULT_CANCELED, null,
                        () -> hasAppTaskLocked(mTask) && !userMinimized());
            }
        }

        /** Guarded by ActivityTaskManagerService.this; app-covered is not user-minimized. */
        private boolean userMinimized() {
            return mHiddenTasks.contains(mTask.id) && !mAppHiddenTasks.contains(mTask.id);
        }

        @Override public void moveToFront(IApplicationThread appThread, String callingPackage) {
            boolean system = checkCaller();
            mAm.callers().requirePackage(callingPackage);
            if (!system && appThread != null) {
                ProcessRecord caller = callerProcess(Binder.getCallingPid());
                if (caller == null || caller.exiting || !caller.thread.asBinder().equals(appThread.asBinder())) {
                    throw new SecurityException("Unrecognized task caller thread");
                }
            }
            moveTaskToFront(mTask.id, mTask);
        }

        @Override public int startActivity(IBinder thread, String callingPackage, String callingFeatureId,
                Intent intent, String resolvedType, Bundle options) {
            boolean system = checkCaller();
            mAm.callers().requirePackage(callingPackage);
            IApplicationThread caller = IApplicationThread.Stub.asInterface(thread);
            if (caller == null) throw new IllegalArgumentException("Missing app thread");
            if (intent != null && intent.hasFileDescriptors()) {
                throw new IllegalArgumentException("File descriptors passed in Intent");
            }
            checkLaunchTaskOptions(options, mPm, callingPackage, system);
            return start(caller, intent, resolvedType, callingPackage, null, null,
                    -1, mTask.id, shareIdentity(options), new ActivityRecord[1], mTask);
        }

        @Override public void moveTaskTo(int displayId, Rect bounds, IRemoteCallback callback) {
            checkCaller();
            if (callback == null) return;
            int result;
            if (mAm.checkPermission("android.permission.REPOSITION_SELF_WINDOWS", Binder.getCallingPid(),
                    Binder.getCallingUid()) != PackageManager.PERMISSION_GRANTED) {
                result = TaskMoveRequestHandler.RESULT_FAILED_NO_PERMISSIONS;
            } else if (displayId != Display.DEFAULT_DISPLAY && displayId != Display.INVALID_DISPLAY) {
                result = TaskMoveRequestHandler.RESULT_FAILED_NONEXISTENT_DISPLAY;
            } else {
                // There is no Shell task-placement transition in the desktop backend.
                result = TaskMoveRequestHandler.RESULT_FAILED_BAD_STATE;
            }
            sendTaskResult(callback, TaskMoveRequestHandler.REMOTE_CALLBACK_RESULT_KEY, result);
        }

        @Override public void requestWindowingLayer(int layer, IRemoteCallback callback) {
            checkCaller();
            Objects.requireNonNull(callback, "callback");
            sendTaskResult(callback, TaskWindowingLayerRequestHandler.REMOTE_CALLBACK_RESULT_KEY,
                    TaskWindowingLayerRequestHandler.RESULT_FAILED_BAD_STATE);
        }
    }

    private boolean hasAppTaskLocked(AppTaskRecord task) {
        return mAppTasks.get(task.id) == task && task.available(mPm) && topOf(task.id) != null;
    }

    private static void sendTaskResult(IRemoteCallback callback, String key, int result) {
        Bundle data = new Bundle();
        data.putInt(key, result);
        try { callback.sendResult(data); } catch (RemoteException ignored) { }
    }

    @Override public void registerTaskStackListener(ITaskStackListener listener) {
        enforceTaskListenerPermission();
        mTaskListeners.register(Objects.requireNonNull(listener, "listener"));
    }

    @Override public void unregisterTaskStackListener(ITaskStackListener listener) {
        enforceTaskListenerPermission();
        mTaskListeners.unregister(Objects.requireNonNull(listener, "listener"));
    }

    private void enforceTaskListenerPermission() {
        if (!mAm.callers().isSystem() && mAm.checkPermission("android.permission.MANAGE_ACTIVITY_TASKS",
                Binder.getCallingPid(), Binder.getCallingUid()) != PackageManager.PERMISSION_GRANTED) {
            throw new SecurityException("Task stack listeners require MANAGE_ACTIVITY_TASKS");
        }
    }

    /** Coalesce invalidations and deliver outside the lifecycle monitor. Guarded by this. */
    private void notifyTaskStackChangedLocked() {
        if (mTaskCallbackPending || mStackPublications != 0
                || mTaskListeners.getRegisteredCallbackCount() == 0) return;
        mTaskCallbackPending = true;
        mTaskCallbacks.execute(() -> {
            synchronized (this) { mTaskCallbackPending = false; }
            int count = mTaskListeners.beginBroadcast();
            try {
                for (int i = 0; i < count; i++) {
                    try { mTaskListeners.getBroadcastItem(i).onTaskStackChanged(); }
                    catch (RemoteException | RuntimeException e) {
                        SystemServer.log("Task stack listener failed: " + e);
                    }
                }
            } finally {
                mTaskListeners.finishBroadcast();
            }
        });
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
        final InProcessActivity standalone;
        final ApkPackage captured;
        synchronized (this) {
            source = forToken(resultToToken);
            if (source == null) {
                process = attachedProcessByPid(Binder.getCallingPid());
                if (!isCurrentInProcessApplication(process) || process.inProcessActivity == null
                        || process.inProcessActivity.token() != resultToToken || caller == null
                        || Binder.getCallingUid() != process.uid
                        || !caller.asBinder().equals(process.thread.asBinder())
                        || !process.packageName.equals(callingPackage)
                        || !process.inProcessActivity.active().getAsBoolean()) {
                    throw new SecurityException("Permission request must come from its own activity");
                }
                standalone = process.inProcessActivity;
                captured = process.packageCode;
            } else {
                if (source.app == null || source.finishing || source.state == State.DESTROYED
                        || caller == null || source.app.pid != Binder.getCallingPid()
                        || !caller.asBinder().equals(source.app.thread.asBinder())
                        || !source.info.packageName.equals(callingPackage)) {
                    throw new SecurityException("Permission request must come from its own activity");
                }
                process = source.app;
                standalone = null;
                captured = mPm.get(callingPackage);
            }
        }
        if (intent.getIntExtra(PackageManager.EXTRA_REQUEST_PERMISSIONS_DEVICE_ID, 0) != 0) {
            throw new SecurityException("Only the default permission device is supported");
        }
        mPermissionRequests.request(captured,
                intent.getStringArrayExtra(PackageManager.EXTRA_REQUEST_PERMISSIONS_NAMES), data -> {
                    if (standalone != null) {
                        standalone.handler().post(() -> {
                            synchronized (ActivityTaskManagerService.this) {
                                if (!isCurrentInProcessApplication(process)
                                        || process.inProcessActivity != standalone
                                        || process.packageCode != captured) return;
                            }
                            standalone.result().dispatch(standalone.token(), resultWho, requestCode, data);
                        });
                        return;
                    }
                    synchronized (ActivityTaskManagerService.this) {
                        if (source.app != process || source.finishing
                                || source.state == State.DESTROYED || !mActivities.contains(source)
                                || mPm.get(callingPackage) != captured) return;
                    }
                    deliverResult(source, resultWho, requestCode, android.app.Activity.RESULT_OK, data);
                });
        return ActivityManager.START_SUCCESS;
    }

    private record ActivityResultRoute(ActivityRecord recipient, String who, int requestCode) {}

    /** ActivityStarter's forward-result transfer. Caller holds the lifecycle monitor. */
    private ActivityResultRoute takeForwardResultLocked(ActivityRecord source) {
        ActivityRecord recipient = source.resultTo;
        if (recipient != null && isActivityRetired(recipient)) recipient = null;
        ActivityResultRoute route = new ActivityResultRoute(recipient, source.resultWho, source.requestCode);
        source.resultTo = null;
        if (recipient != null) {
            // A finishing trampoline may already have queued its result. Preserve
            // results from other children, even with identical who/request codes.
            recipient.pendingResults.removeIf(result -> result instanceof PendingActivityResult pending
                    && pending.source == source && result.mRequestCode == source.requestCode
                    && java.util.Objects.equals(result.mResultWho, source.resultWho));
        }
        return route;
    }

    private void cancelActivityResult(ActivityResultRoute route) {
        if (route.recipient != null) deliverResult(route.recipient,
                new ResultInfo(route.who, route.requestCode, android.app.Activity.RESULT_CANCELED, null, null),
                null, null, null);
    }

    /**
     * The Settings window has its own lifetime, like an activity in the Settings
     * process. Retiring the caller or its activity must not release an open window's
     * device use. The coordinator releases this reservation after the host call exits.
     */
    private static final class ExactAlarmSettingsSession {
        final ActivityRecord covered, resultRecipient;
        final ProcessRecord coveredProcess;
        final AppTaskRecord task;
        final String resultWho;
        final int requestCode, taskId;

        ExactAlarmSettingsSession(ActivityRecord covered, AppTaskRecord task, ActivityResultRoute route) {
            this.covered = covered;
            coveredProcess = covered == null ? null : covered.app;
            this.task = task;
            this.resultRecipient = route == null ? null : route.recipient;
            this.resultWho = route == null ? null : route.who;
            this.requestCode = route == null ? -1 : route.requestCode;
            taskId = task == null ? -1 : task.id;
        }
    }

    /** Settings is opaque even when the covered task's top activity is translucent. Guarded by this. */
    private boolean exactAlarmSettingsCoversTask(int taskId) {
        AppTaskRecord task = mAppTasks.get(taskId);
        if (task == null || !hasAppTaskLocked(task)) return false;
        for (ExactAlarmSettingsSession session : mExactAlarmSettingsSessions) {
            if (session.task == task) return true;
        }
        return false;
    }

    private void finishExactAlarmSettings(ExactAlarmSettingsSession session, int activityResult) {
        synchronized (this) {
            // Completion, rejected submission and a synchronous executor failure may
            // meet here. Only the accepted reservation owns the release.
            if (!mExactAlarmSettingsSessions.remove(session)) return;
            ActivityRecord covered = session.covered;
            if (covered != null) {
                covered.exactAlarmSettings--;
                if (covered.exactAlarmSettings == 0 && covered.state != State.PAUSING) {
                    covered.resumeAfterExactAlarmSettings = false;
                }
            }
            updateIdle();
            notifyAll();
        }
        // A stopped recipient keeps its result for its own eventual resume. If its
        // process died, ordinary saved history/result rules still apply.
        try {
            if (session.resultRecipient != null) deliverResult(session.resultRecipient,
                    session.resultWho, session.requestCode, activityResult, null);
        } finally {
            try { restoreExactAlarmTask(session); }
            finally { updateTopResumed(); }
        }
    }

    private void restoreExactAlarmTask(ExactAlarmSettingsSession session) {
        // Result delivery can reenter and remove the task. Serialize restoration
        // with new launches and recheck its actual generation after that delivery.
        synchronized (mLaunchLock) {
            ActivityRecord restore = null;
            boolean restoreProcess = false;
            List<ActivityRecord> toStart;
            synchronized (this) {
                if (session.task == null || !hasAppTaskLocked(session.task)) return;
                if (!exactAlarmSettingsCoversTask(session.taskId) && !mHiddenTasks.contains(session.taskId)) {
                    ActivityRecord top = topOf(session.taskId);
                    if (top != null && top.state != State.PAUSING && activityEnabled(top)) {
                        if (top.app != null && top.chooserCoverCount == 0 && top.nativeUiCount == 0) restore = top;
                        else if (top.app == null && !mTaskOrder.isEmpty()
                                && mTaskOrder.get(mTaskOrder.size() - 1) == session.taskId) {
                            restore = top;
                            restoreProcess = true;
                        }
                    }
                }
                toStart = starting();
                toStart.removeIf(activity -> activity.taskId != session.taskId);
                toStart.remove(restore);
            }
            if (restoreProcess) moveTaskToFront(session.taskId, session.task);
            else if (restore != null) { realStartActivity(restore); resume(restore); }
            for (ActivityRecord activity : toStart) makeVisible(activity);
            synchronized (this) {
                if (!hasAppTaskLocked(session.task)) return;
            }
            realizeTaskHistory(session.taskId);
        }
    }

    private int requestExactAlarmAccess(Intent intent, String callingPackage,
            IBinder resultToToken, String resultWho, int requestCode, int launchTaskId,
            AppTaskRecord requiredTask) {
        String target = ExactAlarmAccessRequests.target(intent);
        if (!mExactAlarmRequests.available()) return ActivityManager.START_INTENT_NOT_RESOLVED;
        final ActivityRecord origin;
        final ActivityRecord covered;
        final ExactAlarmSettingsSession session;
        final boolean cancelResult;
        final ActivityResultRoute resultRoute;
        List<ActivityRecord> toStop = new ArrayList<>();
        boolean pauseCovered = false;
        synchronized (this) {
            if (requiredTask != null && !hasAppTaskLocked(requiredTask)) return ActivityManager.START_CANCELED;
            ProcessRecord callerProcess = callerProcess(Binder.getCallingPid());
            origin = resultToToken == null ? null : forToken(resultToToken);
            boolean delegatedResult = mVerifiedCreator.get() != null;
            boolean forward = origin != null && (intent.getFlags() & Intent.FLAG_ACTIVITY_FORWARD_RESULT) != 0;
            if (resultToToken != null && (origin == null || origin.app == null
                    || !delegatedResult && (origin.app != callerProcess || callerProcess == null)
                    || origin.finishing && !forward || origin.state == State.DESTROYED
                    || !activityCodeCurrent(origin))) {
                throw new SecurityException("Settings result must belong to its live caller activity");
            }
            if (forward && requestCode >= 0) return ActivityManager.START_FORWARD_AND_REQUEST_CONFLICT;
            if (mExactAlarmSettingsSessions.size() >= 16) return ActivityManager.START_CANCELED;
            resultRoute = forward ? takeForwardResultLocked(origin)
                    : new ActivityResultRoute(origin != null && requestCode >= 0 ? origin : null, resultWho, requestCode);
            boolean newTask = requiredTask == null && origin == null
                    || (intent.getFlags() & Intent.FLAG_ACTIVITY_NEW_TASK) != 0
                    || origin != null && origin.info.launchMode == ActivityInfo.LAUNCH_SINGLE_INSTANCE
                    || resultRoute.recipient == null && (intent.getFlags() & Intent.FLAG_ACTIVITY_NEW_DOCUMENT) != 0;
            cancelResult = resultRoute.recipient != null && newTask;
            int taskId = launchTaskId >= 0 && topOf(launchTaskId) != null ? launchTaskId
                    : !newTask ? origin.taskId : -1;
            // ActivityStarter selects the source task; TaskFragment pauses that
            // task's resumed activity. The result recipient can be stopped below it.
            covered = taskId >= 0 ? topOf(taskId) : focusedTaskTopLocked();
            AppTaskRecord coveredTask = covered == null ? null : mAppTasks.get(covered.taskId);
            if (covered != null && (coveredTask == null || !hasAppTaskLocked(coveredTask))) {
                return ActivityManager.START_CANCELED;
            }
            session = new ExactAlarmSettingsSession(covered, coveredTask, newTask ? null : resultRoute);
            if (taskId >= 0) {
                mHiddenTasks.remove(taskId);
                focusTaskLocked(taskId);
            }
            if (covered != null) {
                covered.exactAlarmSettings++;
                pauseCovered = covered.state == State.RESUMED || covered.state == State.RESUMING
                        || covered.state == State.LAUNCHING;
                covered.resumeAfterExactAlarmSettings = true;
                for (ActivityRecord activity : mActivities) {
                    if (activity.taskId == covered.taskId && !activity.finishing && activity.app != null
                            && (activity.state == State.PAUSED || activity.state == State.STARTED)) {
                        toStop.add(activity);
                    }
                }
            }
            mExactAlarmSettingsSessions.add(session);
            updateIdle();
        }
        java.util.function.IntConsumer complete = result -> finishExactAlarmSettings(session, result);
        try {
            // A new-task launch has no result dependency on its originator (AOSP
            // ActivityStarter.sendNewTaskResultRequestIfNeeded).
            if (cancelResult) cancelActivityResult(resultRoute);
            if (pauseCovered) pause(covered, false,
                    (intent.getFlags() & Intent.FLAG_ACTIVITY_NO_USER_ACTION) == 0);
            for (ActivityRecord activity : toStop) stop(activity);
            updateTopResumed();
            boolean queued = mExactAlarmRequests.request(target, () -> {
                // Preserve pause-before-Settings ordering without holding a Binder
                // transaction open. Android also bounds a missing pause acknowledgment.
                if (covered == null) return;
                synchronized (ActivityTaskManagerService.this) {
                    long until = SystemClock.uptimeMillis() + 500;
                    while (covered.app == session.coveredProcess && session.coveredProcess != null
                            && !session.coveredProcess.exiting && covered.state == State.PAUSING) {
                        long left = until - SystemClock.uptimeMillis();
                        if (left <= 0) break;
                        try { ActivityTaskManagerService.this.wait(left); }
                        catch (InterruptedException interrupted) { Thread.currentThread().interrupt(); break; }
                    }
                }
            }, complete);
            if (!queued) { complete.accept(android.app.Activity.RESULT_CANCELED); return ActivityManager.START_CANCELED; }
            return ActivityManager.START_SUCCESS;
        } catch (RuntimeException | Error failure) {
            complete.accept(android.app.Activity.RESULT_CANCELED);
            throw failure;
        }
    }

    /** A native consent activity covers the app and keeps a headless device alive. */
    private final class BluetoothActivity {
        final ActivityRecord covered;
        final ProcessRecord coveredProcess;
        final boolean needsPause;

        // Constructed under the activity monitor; only completion releases the reservation.
        BluetoothActivity(ActivityRecord source) {
            ActivityRecord top = source == null ? focusedTaskTopLocked() : topOf(source.taskId);
            covered = top != null && top.app != null ? top : null;
            coveredProcess = covered == null ? null : covered.app;
            needsPause = covered != null && (covered.state == State.RESUMED
                    || covered.state == State.RESUMING || covered.state == State.LAUNCHING);
            if (covered != null) {
                covered.nativeUiCount++;
                covered.resumeAfterNativeUi = true;
            }
            mBluetoothActivities.add(this);
            updateIdle();
        }

        void pauseCovered(boolean userLeaving) {
            final IApplicationThread thread;
            final ClientTransaction transaction;
            synchronized (ActivityTaskManagerService.this) {
                // A detached result callback may already have finished its caller.
                // Admit the pause atomically and retain this process's thread.
                if (!needsPause || !mBluetoothActivities.contains(this) || covered.finishing
                        || !mActivities.contains(covered) || !activityEnabled(covered)
                        || covered.app != coveredProcess || coveredProcess == null
                        || processByPid(coveredProcess.pid) != coveredProcess
                        || coveredProcess.thread == null
                        || (covered.state != State.RESUMED && covered.state != State.RESUMING
                            && covered.state != State.LAUNCHING)) return;
                thread = coveredProcess.thread;
                covered.stopWhenIdle = true;
                setState(covered, State.PAUSING);
                transaction = new ClientTransaction(thread);
                transaction.addTransactionItem(new PauseActivityItem(covered.token, false,
                        userLeaving, false, false));
            }
            try { thread.scheduleTransaction(transaction); }
            catch (RemoteException failure) {
                SystemServer.log("Bluetooth consent pause failed: " + failure);
            }
        }

        void awaitPause() {
            if (covered == null) return;
            synchronized (ActivityTaskManagerService.this) {
                long until = SystemClock.uptimeMillis() + 500;
                while (mBluetoothActivities.contains(this) && covered.app == coveredProcess
                        && covered.state == State.PAUSING) {
                    long left = until - SystemClock.uptimeMillis();
                    if (left <= 0) break;
                    try { ActivityTaskManagerService.this.wait(left); }
                    catch (InterruptedException interrupted) {
                        Thread.currentThread().interrupt();
                        return;
                    }
                }
            }
        }

        void finish(Runnable deliver) {
            boolean resumeCovered = false;
            synchronized (ActivityTaskManagerService.this) {
                if (!mBluetoothActivities.remove(this)) return;
                if (covered != null && --covered.nativeUiCount == 0 && covered.resumeAfterNativeUi
                        && covered.state != State.PAUSING) {
                    covered.resumeAfterNativeUi = false;
                    resumeCovered = !covered.finishing && covered.state != State.DESTROYED
                            && mActivities.contains(covered) && activityEnabled(covered)
                            && covered.app == coveredProcess && coveredProcess != null
                            && processByPid(coveredProcess.pid) == coveredProcess
                            && topOf(covered.taskId) == covered && !mHiddenTasks.contains(covered.taskId);
                }
                updateIdle();
                ActivityTaskManagerService.this.notifyAll();
            }
            try { deliver.run(); }
            finally {
                if (resumeCovered) { realStartActivity(covered); resume(covered); }
                updateTopResumed();
            }
        }
    }

    private int requestBluetoothState(Intent intent, String verifiedCaller,
            ProcessRecord callingProcess, boolean internalCreator,
            java.util.function.BooleanSupplier delegated,
            java.util.function.BooleanSupplier publication,
            IBinder resultToToken, String resultWho, int requestCode) {
        // A selector selects the activity; the launched activity receives the outer
        // intent and performs its action (RequestPermissionActivity.parseIntent).
        final String action = intent.getAction();
        final String pkg = verifiedCaller == null ? "android" : verifiedCaller;
        final ApkPackage captured = mPm.get(pkg);
        final BluetoothRequests requests = mBluetoothRequests;
        // Validate before consuming a forwarded result route or scheduling host work.
        requests.validate(action, pkg, captured);
        final ActivityRecord source, recipient;
        final ProcessRecord sourceProcess, expectedProcess;
        final String targetWho;
        final int targetCode;
        final boolean forwarded, cancelResult;
        final BluetoothActivity activity;
        synchronized (this) {
            source = resultToToken == null ? null : forToken(resultToToken);
            if (resultToToken != null && (source == null || source.app == null
                    || source.finishing || source.state == State.DESTROYED || !activityEnabled(source)
                    || (internalCreator ? publication == null || !publication.getAsBoolean()
                            : callingProcess == null || source.app != callingProcess))) {
                throw new SecurityException("Bluetooth result must belong to its live caller activity");
            }
            if (requestCode >= 0 && source == null) {
                throw new SecurityException("Bluetooth result requires a live activity");
            }
            sourceProcess = source == null ? null : source.app;
            forwarded = source != null && (intent.getFlags() & Intent.FLAG_ACTIVITY_FORWARD_RESULT) != 0;
            if (forwarded && requestCode >= 0) return ActivityManager.START_FORWARD_AND_REQUEST_CONFLICT;
            if (forwarded) {
                ActivityRecord target = source.resultTo;
                recipient = target != null && !isActivityRetired(target) ? target : null;
                targetWho = source.resultWho;
                targetCode = source.requestCode;
                source.resultTo = null;
                if (recipient != null) {
                    recipient.pendingResults.removeIf(result -> {
                        if (!(result instanceof PendingActivityResult pending) || pending.source != source
                                || result.mRequestCode != source.requestCode
                                || !Objects.equals(result.mResultWho, source.resultWho)) return false;
                        recipient.callerState.remove(result.mCallerToken);
                        return true;
                    });
                }
                // A forwarded route belongs to the saved activity, not its old process.
                expectedProcess = null;
            } else {
                recipient = requestCode >= 0 ? source : null;
                targetWho = resultWho;
                targetCode = requestCode;
                expectedProcess = sourceProcess;
            }
            cancelResult = recipient != null && ((intent.getFlags() & Intent.FLAG_ACTIVITY_NEW_TASK) != 0
                    || source.info.launchMode == ActivityInfo.LAUNCH_SINGLE_INSTANCE);
            activity = new BluetoothActivity(source);
        }
        // After a result is forwarded/detached (or none was requested), the system UI
        // owns its work. A trampoline may finish or die without canceling that request.
        final boolean detached = forwarded || cancelResult || source == null || requestCode < 0;
        java.util.function.BooleanSupplier lifetime = () -> {
            synchronized (ActivityTaskManagerService.this) {
                return (delegated == null || delegated.getAsBoolean())
                        && (detached || internalCreator || callingProcess == null
                            || !callingProcess.exiting && !callingProcess.dead && !callingProcess.pidSuperseded
                                && processByPid(callingProcess.pid) == callingProcess
                                && (callingProcess.enabledProcessLifetime == null
                                    || callingProcess.enabledProcessLifetime.getAsBoolean()))
                        && (detached
                            || source.app == sourceProcess && !source.finishing
                                && source.state != State.DESTROYED && mActivities.contains(source)
                                && activityEnabled(source) && sourceProcess != null
                                && !sourceProcess.exiting && !sourceProcess.dead
                                && processByPid(sourceProcess.pid) == sourceProcess);
            }
        };
        java.util.function.IntConsumer completion = result -> activity.finish(() -> {
            if (recipient != null && !cancelResult && lifetime.getAsBoolean()) {
                deliverResult(recipient, targetWho, targetCode, result, null, expectedProcess);
            }
        });
        try {
            if (cancelResult) {
                deliverResult(recipient, new ResultInfo(targetWho, targetCode,
                        android.app.Activity.RESULT_CANCELED, null, null), null, null, expectedProcess);
            }
            activity.pauseCovered((intent.getFlags() & Intent.FLAG_ACTIVITY_NO_USER_ACTION) == 0);
            updateTopResumed();
            requests.request(action, pkg, captured, lifetime, activity::awaitPause, completion);
        } catch (SecurityException revoked) {
            // Permission may be revoked after admission consumed a forwarded route.
            // That accepted launch must finish with cancellation, not lose its result.
            completion.accept(android.app.Activity.RESULT_CANCELED);
        } catch (RuntimeException | Error failure) {
            completion.accept(android.app.Activity.RESULT_CANCELED);
            throw failure;
        }
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
        return start(caller, intent, resolvedType, callingPackage, resultToToken, resultWho,
                requestCode, launchTaskId, shareIdentity, started, null);
    }

    private int start(IApplicationThread caller, Intent intent, String resolvedType,
            String callingPackage, IBinder resultToToken, String resultWho, int requestCode,
            int launchTaskId, boolean shareIdentity, ActivityRecord[] started, AppTaskRecord requiredTask) {
        if (IntentChooser.handles(intent)) {
            synchronized (this) {
                if (requiredTask != null && !hasAppTaskLocked(requiredTask)) {
                    throw new IllegalArgumentException("Unable to find task ID " + launchTaskId);
                }
            }
            // MIME resolution can call a provider. Keep it outside mLaunchLock;
            // the chooser and eventual target retain this exact task generation.
            return startSerialized(caller, intent, resolvedType, callingPackage, resultToToken,
                    resultWho, requestCode, launchTaskId, shareIdentity, started, requiredTask);
        }
        synchronized (mLaunchLock) {
            if (requiredTask != null) {
                synchronized (this) {
                    if (!hasAppTaskLocked(requiredTask)) {
                        throw new IllegalArgumentException("Unable to find task ID " + launchTaskId);
                    }
                }
            }
            return startSerialized(caller, intent, resolvedType, callingPackage, resultToToken,
                    resultWho, requestCode, launchTaskId, shareIdentity, started, requiredTask);
        }
    }

    private int startSerialized(IApplicationThread caller, Intent intent, String resolvedType,
            String callingPackage, IBinder resultToToken, String resultWho, int requestCode,
            int launchTaskId, boolean shareIdentity, ActivityRecord[] started, AppTaskRecord requiredTask) {
        if (started == null) started = new ActivityRecord[1];
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
        if (ExactAlarmAccessRequests.handles(intent, resolvedType)
                && mPm.get(ExactAlarmAccessRequests.PACKAGE) == null) {
            ResolveInfo settings = mPm.resolveIntent(intent, resolvedType, PackageManager.MATCH_DEFAULT_ONLY, 0);
            if (settings != null && ExactAlarmAccessRequests.isActivity(settings.activityInfo)) {
                return requestExactAlarmAccess(intent, callingPackage, resultToToken, resultWho,
                        requestCode, launchTaskId, requiredTask);
            }
        }
        if (AccountChooser.handles(intent)) {
            return chooseAccount(caller, callingPackage, intent, resultToToken, resultWho, requestCode);
        }
        if (IntentChooser.handles(intent)) {
            return chooseIntent(intent, verifiedCaller, originalProcess, internalCreator,
                    delegated, publication, resultToToken, resultWho, requestCode,
                    launchTaskId, shareIdentity, started, requiredTask);
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
            if (requiredTask == null && BluetoothRequests.handles(intent, resolvedType)) {
                // Standalone services share the APK's PID and UID. Local Binder calls
                // are normally trusted control-plane work, but must not turn that
                // admitted APK (or a retired admission) into the Android system caller.
                // A verified PendingIntent creator already carries its own authority.
                if (!internalCreator && originalPid == android.os.Process.myPid()
                        && originalUid >= android.os.Process.FIRST_APPLICATION_UID) {
                    synchronized (this) {
                        ApkPackage owner = originalProcess == null ? null : originalProcess.packageCode;
                        if (originalProcess == null || originalProcess.inProcessActivityState == null
                                || !originalProcess.ready || processByPid(originalPid) != originalProcess
                                || originalProcess.uid != originalUid || owner == null
                                || owner.uid() != originalUid || mPm.get(owner.packageName) != owner
                                || mPm.isRemovalPending(owner.packageName)
                                || !owner.matchesEnabledApplication(0)
                                || originalProcess.enabledProcessLifetime != null
                                    && !originalProcess.enabledProcessLifetime.getAsBoolean()
                                || caller == null || originalProcess.thread == null
                                || !originalProcess.thread.asBinder().equals(caller.asBinder())) {
                            throw new SecurityException("Bluetooth request requires its live standalone admission");
                        }
                        ApkPackage claimed = mPm.get(callingPackage);
                        if (!owner.packageName.equals(callingPackage)
                                && (claimed == null || claimed.uid() != owner.uid())) {
                            throw new SecurityException("Bluetooth caller cannot act for another package");
                        }
                        verifiedCaller = owner.packageName;
                    }
                }
                return requestBluetoothState(intent, verifiedCaller, originalProcess,
                        internalCreator, delegated, publication, resultToToken, resultWho, requestCode);
            }
            if (DocumentPicker.handles(intent)) {
                return startDocumentPicker(caller, originalName, intent, resolvedType,
                        resultToToken, resultWho, requestCode, launchTaskId, requiredTask);
            }
            if (requiredTask != null) return ActivityManager.START_INTENT_NOT_RESOLVED;
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
                }
            }
            SystemServer.log("startActivity: nothing handles " + intent);
            return ActivityManager.START_INTENT_NOT_RESOLVED;
        }
        int componentCallerUid = verifiedCaller == null ? android.os.Process.SYSTEM_UID
                : mPm.getPackageUid(verifiedCaller, 0, 0);
        if (info.applicationInfo == null || mPm.permissions().checkComponentPermission(info.permission,
                componentCallerUid, info.applicationInfo.uid, info.exported) != PackageManager.PERMISSION_GRANTED) {
            throw new SecurityException(info.exported ? "Activity permission denied" : "Activity is not exported");
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
        boolean inAppTask = requiredTask != null;
        // Single launches and batches share Android's task/manifest semantics;
        // only a batch defers the creation of its activity history.
        int launchFlags = taskLaunchFlags(info, intent, source, inAppTask,
                requestCode >= 0 && source != null);
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
        boolean cancelResult = false;
        ProcessRecord resultProcess = null;
        String sourceWindowMode = null;
        List<ActivityRecord> reconfigure = new ArrayList<>();
        Runnable beforePublication = mBeforeActivityPublication;
        if (beforePublication != null) beforePublication.run();
        synchronized (this) {
            if (requiredTask != null && !hasAppTaskLocked(requiredTask)) {
                return ActivityManager.START_CANCELED;
            }
            if (targetCode == null || mPm.get(info.packageName) != targetCode
                    || mPm.isRemovalPending(info.packageName) || !sourceCurrent.getAsBoolean()
                    || info.applicationInfo == null || !java.util.Objects.equals(info.applicationInfo.sourceDir, targetCode.apkPath)) {
                return ActivityManager.START_CANCELED;
            }
            if (attributedCaller != null && !internalCreator && (originalProcess == null
                    || callerProcess(originalPid) != originalProcess || originalProcess.exiting)) {
                throw new SecurityException("Activity caller retired before publication");
            }
            // ActivityStarter.computeLaunchingTaskFlags rejects singleTask and
            // singleInstance in a nonempty explicit task, including a matching root.
            if (inAppTask && (info.launchMode == ActivityInfo.LAUNCH_SINGLE_TASK
                    || info.launchMode == ActivityInfo.LAUNCH_SINGLE_INSTANCE)) {
                throw new IllegalArgumentException("Cannot launch singleTask/singleInstance into nonempty task "
                        + requiredTask.id);
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
            int taskId = findLaunchTaskId(info, launch, source, launchTaskId);
            ActivityRecord top = topOf(taskId);
            boolean clearTask = (launchFlags & Intent.FLAG_ACTIVITY_NEW_TASK) != 0
                    && (launchFlags & Intent.FLAG_ACTIVITY_CLEAR_TASK) != 0;
            if (clearTask) {
                for (int i = mActivities.size() - 1; i >= 0; i--) {
                    ActivityRecord activity = mActivities.get(i);
                    if (activity.taskId == taskId && !activity.finishing) clearTop.add(activity);
                }
                top = null;
            }
            boolean documentIntoExisting = (launchFlags & Intent.FLAG_ACTIVITY_NEW_DOCUMENT) != 0
                    && (launchFlags & Intent.FLAG_ACTIVITY_MULTIPLE_TASK) == 0;
            boolean singleTop = mAccountCompletion.get() == null
                    && (info.launchMode == ActivityInfo.LAUNCH_SINGLE_TOP
                    || info.launchMode == ActivityInfo.LAUNCH_SINGLE_TASK
                    || info.launchMode == ActivityInfo.LAUNCH_SINGLE_INSTANCE
                    || info.launchMode == ActivityInfo.LAUNCH_SINGLE_INSTANCE_PER_TASK
                    || documentIntoExisting
                    || (launchFlags & Intent.FLAG_ACTIVITY_SINGLE_TOP) != 0);
            boolean clearToInstance = (launchFlags & Intent.FLAG_ACTIVITY_CLEAR_TOP) != 0
                    || documentIntoExisting
                    || mAccountCompletion.get() == null && (info.launchMode == ActivityInfo.LAUNCH_SINGLE_TASK
                            || info.launchMode == ActivityInfo.LAUNCH_SINGLE_INSTANCE
                            || info.launchMode == ActivityInfo.LAUNCH_SINGLE_INSTANCE_PER_TASK);
            ActivityRecord below = !clearTask && clearToInstance
                    ? belowInTask(taskId, launch.getComponent()) : null;
            ActivityRecord reordered = mAccountCompletion.get() == null && (!inAppTask
                    || (launchFlags & Intent.FLAG_ACTIVITY_NEW_TASK) != 0) && !clearTask && !clearToInstance
                    && (launchFlags & Intent.FLAG_ACTIVITY_REORDER_TO_FRONT) != 0
                    ? belowInTask(taskId, launch.getComponent()) : null;
            ActivityRecord taskRoot = rootOf(taskId);
            if (inAppTask && top != null && !clearTask && !clearToInstance
                    && (launchFlags & Intent.FLAG_ACTIVITY_REORDER_TO_FRONT) == 0
                    && (launchFlags & Intent.FLAG_ACTIVITY_NEW_TASK) != 0
                    && requiredTask.baseComponent.equals(launch.getComponent())) {
                // ActivityStarter.complyActivityFlags uses the explicit task's
                // original component here, not Intent.filterEquals or its current
                // bottom activity. Clear/document/reorder flags take precedence.
                reusedTaskTop = top;
            } else if (top != null && activityCodeCurrent(top) && top.component().equals(launch.getComponent())
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
            } else if (!inAppTask && mAccountCompletion.get() == null
                    && top != null && !clearTask && !clearToInstance
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
                ActivityRecord resultRecord = requestCode >= 0
                        && (launchFlags & Intent.FLAG_ACTIVITY_NEW_TASK) == 0 ? source : null;
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
                // Replacing the activities of an explicit AppTask must not
                // transfer its retained owner/capability to the new target.
                recordAppTaskLocked(r, clearTask && !inAppTask);
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
                        // Clearing the task already schedules this
                        // activity's finishing pause through finishTogether.
                        if (front.state != State.PAUSING && !clearTop.contains(front)) {
                            pause = front;
                        }
                    } else {
                        launchNow = r.waitingFor == null && app != null && app.ready;
                    }
                }
            }
            if (publishHistory && (deliverTo != null || reusedTaskTop != null)) {
                mHiddenTasks.remove(taskId);
                focusTaskLocked(taskId);
            }
            // A new task cannot return a result to its launching Activity. Reusing
            // an existing instance also completes this launch's result with cancellation.
            // Capture the admitted recipient process before invoking any lifecycle work.
            cancelResult = source != null && requestCode >= 0
                    && ((launchFlags & Intent.FLAG_ACTIVITY_NEW_TASK) != 0
                            || deliverTo != null || reusedTaskTop != null);
            if (cancelResult) resultProcess = source.app;
        }
        if (cancelResult) {
            deliverResult(source, new ResultInfo(resultWho, requestCode,
                    android.app.Activity.RESULT_CANCELED, null, null), null, null, resultProcess);
        }
        if (reusedTaskTop != null) {
            started[0] = reusedTaskTop;
            if (!publishHistory) moveTaskToFront(reusedTaskTop.taskId, requiredTask, false, true);
            applyAppWindows();
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
            if (!publishHistory) moveTaskToFront(deliverTo.taskId, requiredTask, false, true);
            applyAppWindows();
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

    /** ActivityStarter's manifest/document flags for ordinary, batch and AppTask launches. */
    private static int taskLaunchFlags(ActivityInfo info, Intent intent, ActivityRecord source,
            boolean inAppTask, boolean forResult) {
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
        // AppTask supplies a task without a source activity. That does not, by
        // itself, make the request NEW_TASK; document/per-task modes still do.
        if (!inAppTask && (source == null || source.info.launchMode == ActivityInfo.LAUNCH_SINGLE_INSTANCE
                    || single)
                || info.launchMode == ActivityInfo.LAUNCH_SINGLE_INSTANCE_PER_TASK
                || (flags & Intent.FLAG_ACTIVITY_NEW_DOCUMENT) != 0 && !forResult) {
            flags |= Intent.FLAG_ACTIVITY_NEW_TASK;
        }
        if ((flags & Intent.FLAG_ACTIVITY_NEW_TASK) != 0
                && info.documentLaunchMode == ActivityInfo.DOCUMENT_LAUNCH_ALWAYS) {
            flags |= Intent.FLAG_ACTIVITY_MULTIPLE_TASK;
        }
        return flags;
    }

    /** ActivityStarter's source/NEW_TASK choice, shared by single launches and batches. */
    private int findLaunchTaskId(ActivityInfo info, Intent intent, ActivityRecord source, int requestedTask) {
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
     * {@link #callerProcess}. Only the latest accepted attachment owns[Truncated]
