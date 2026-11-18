/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.server;

import static android.app.UiModeManager.*;
import static android.content.res.Configuration.UI_MODE_NIGHT_NO;
import static android.content.res.Configuration.UI_MODE_NIGHT_YES;

import android.app.IUiModeManager;
import android.app.IUiModeManagerCallback;
import android.content.pm.PackageManager;
import android.content.res.Configuration;
import android.os.Binder;
import android.os.Process;
import android.os.RemoteCallbackList;
import android.os.UserHandle;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.StandardCopyOption;
import java.time.Instant;
import java.time.LocalDate;
import java.time.LocalTime;
import java.time.ZoneId;
import java.time.ZonedDateTime;
import java.util.HashMap;
import java.util.Map;
import java.util.Properties;
import java.util.concurrent.ScheduledFuture;
import java.util.concurrent.ScheduledThreadPoolExecutor;
import java.util.concurrent.TimeUnit;
import java.util.function.Consumer;

/**
 * Android 16 UiModeManager policy for the normal, single-user Windows device.
 *
 * <p>Global policy, per-package overrides, and the applied configuration are different
 * states. Automatic transitions wait for the observed display-off event; explicit
 * activation releases that wait, while attention preserves it. MODE_NIGHT_AUTO uses
 * astronomical twilight, never the desktop theme. See docs/ui-mode.md for the pinned
 * AOSP contract and boundaries.
 */
public final class UiModeManagerService extends IUiModeManager.Default implements AutoCloseable {
    private static final String MODIFY = android.Manifest.permission.MODIFY_DAY_NIGHT_MODE;
    private static final long DAY_MICROS = 86_400_000_000L;
    private static final long REFRESH_MS = 3_600_000L;
    private static volatile UiModeManagerService sInstance;

    static {
        // AOSP saves AUTO's last computed result at shutdown. Resource updates
        // must not depend on that cache being writable while the device runs.
        Runtime.getRuntime().addShutdownHook(new Thread(() -> {
            UiModeManagerService service = sInstance;
            if (service != null) service.close();
        }, "ui-mode-shutdown"));
    }

    /** Explicit, instance-owned host boundary; ordinary execution always uses Windows. */
    interface Source extends AutoCloseable {
        long now();
        ZoneId zone();
        Environment read(boolean twilight);
        default int takeEvents() { return 0; }
        void listen(Runnable changed);
        @Override void close();
    }
    record Twilight(boolean night, long nextChange) { }
    record Environment(int theme, int display, Boolean powerSave, Twilight twilight) { }
    record AppMode(String owner, int night) { }

    private static final class State {
        boolean configured;
        int mode = MODE_NIGHT_NO;
        int custom = MODE_NIGHT_CUSTOM_TYPE_UNKNOWN;
        long start = 22 * 3_600_000_000L;
        long end = 6 * 3_600_000_000L;
        boolean computed, overrideOn, overrideOff, waitForInactive;
        boolean baseNight; // Last sampled input, separate from computed configuration policy.
        // Transient requests participate in publication, but are not saved policy.
        int attention = MODE_ATTENTION_THEME_OVERLAY_OFF;
        boolean bedtimeRequested;
        final Map<String, AppMode> apps = new HashMap<>();
        State() { }
        State(State before) {
            configured = before.configured; mode = before.mode; custom = before.custom;
            start = before.start; end = before.end; computed = before.computed;
            baseNight = before.baseNight; waitForInactive = before.waitForInactive;
            overrideOn = before.overrideOn; overrideOff = before.overrideOff;
            attention = before.attention; bedtimeRequested = before.bedtimeRequested;
            apps.putAll(before.apps);
        }
    }

    private final PackageManagerService mPm;
    private final CallerIdentity mCallers;
    private final String mStandalonePackage;
    private final Source mSource;
    private final File mFile;
    private final Consumer<String> mConfigurationChanged;
    private final Object mLock = new Object();
    private final Consumer<String> mRemovalListener = this::removePackage;
    private final RemoteCallbackList<IUiModeManagerCallback> mCallbacks = new RemoteCallbackList<>();
    private final ScheduledThreadPoolExecutor mWorker;
    private volatile State mState;
    private volatile int mAppliedNight = UI_MODE_NIGHT_NO;
    private Environment mEnvironment;
    private ScheduledFuture<?> mAlarm;
    private boolean mClosed, mRefreshQueued, mPolicyDirty;

    UiModeManagerService(PackageManagerService pm, ActivityTaskManagerService atm,
            boolean persistent) {
        this(pm, atm, new WindowsNightModeSource(), settingsFile(pm, persistent),
                !persistent && pm.packages().size() == 1
                        ? pm.packages().iterator().next().packageName : null,
                LocaleManagerService::onUiModeChanged);
    }

    private static File settingsFile(PackageManagerService pm, boolean persistent) {
        if (persistent) return new File(Device.dataDir(), "system/ui-mode.properties");
        if (pm.packages().size() != 1) return null;
        // The isolated driver must not mutate a concurrently running device's settings.
        ApkPackage pkg = pm.packages().iterator().next();
        return new File(pkg.dataDir(), "no_backup/gesher-ui-mode.properties");
    }

    UiModeManagerService(PackageManagerService pm, ActivityTaskManagerService atm, Source source,
            File file, String standalonePackage, Consumer<String> configurationChanged) {
        mPm = pm;
        mCallers = new CallerIdentity(pm, atm);
        mSource = source;
        mFile = file;
        mStandalonePackage = standalonePackage;
        mConfigurationChanged = configurationChanged;
        UiModeManagerService previous = sInstance;
        // Flush the previous AUTO cache before reading it during replacement,
        // and release its worker/listeners before creating the new service.
        if (previous != null) previous.close();
        mWorker = new ScheduledThreadPoolExecutor(1, task -> {
            Thread thread = new Thread(task, "ui-mode");
            thread.setDaemon(true);
            return thread;
        });
        mWorker.setRemoveOnCancelPolicy(true);
        mState = read();
        mEnvironment = source.read(mState.mode == MODE_NIGHT_AUTO);
        synchronized (mLock) {
            State initial = new State(mState);
            adoptHostPreference(initial);
            compute(initial);
            mState = initial;
            mAppliedNight = effectiveNight(initial);
            scheduleLocked();
        }
        pm.onPackageRemoval(mRemovalListener);
        source.listen(this::queueRefresh);
        synchronized (UiModeManagerService.class) {
            sInstance = this;
        }
    }

    public static int getUiModeNight() {
        UiModeManagerService service = sInstance;
        return service == null ? UI_MODE_NIGHT_NO : service.mAppliedNight;
    }

    static void applyConfiguration(String packageName, Configuration configuration) {
        UiModeManagerService service = sInstance;
        if (service == null || packageName == null) return;
        ApkPackage pkg = service.mPm.get(packageName);
        AppMode mode = service.mState.apps.get(packageName);
        if (pkg != null && !service.mPm.isRemovalPending(packageName) && mode != null
                && mode.owner().equals(service.applicationOwner(pkg))) {
            configuration.uiMode = (configuration.uiMode & ~Configuration.UI_MODE_NIGHT_MASK)
                    | mode.night();
        }
    }

    @Override public int getCurrentModeType(int displayId) {
        return Configuration.UI_MODE_TYPE_NORMAL;
    }
    // There is no car/dock mode on this device. AOSP ignores car-mode requests when locked.
    @Override public boolean isUiModeLocked() { return true; }
    @Override public boolean isNightModeLocked() { return false; }
    @Override public int getNightMode(int displayId) { return mState.mode; }

    @Override public void setNightMode(int mode) {
        setMode(mode, mode == MODE_NIGHT_CUSTOM
                ? MODE_NIGHT_CUSTOM_TYPE_SCHEDULE : MODE_NIGHT_CUSTOM_TYPE_UNKNOWN);
    }

    private void setMode(int mode, int custom) {
        validateMode(mode);
        if (mode == MODE_NIGHT_CUSTOM && custom != MODE_NIGHT_CUSTOM_TYPE_SCHEDULE
                && custom != MODE_NIGHT_CUSTOM_TYPE_BEDTIME) {
            throw new IllegalArgumentException("Unknown custom night mode: " + custom);
        }
        boolean changed;
        synchronized (mLock) {
            ensureOpen();
            State next = new State(mState);
            if (next.configured && next.mode == mode && next.custom == custom) return;
            next.configured = true; next.mode = mode; next.custom = custom;
            next.overrideOn = next.overrideOff = false;
            next.waitForInactive = false;
            changed = mode == MODE_NIGHT_AUTO || mode == MODE_NIGHT_CUSTOM
                    ? publishAutomaticLocked(next) : publishLocked(next);
        }
        android.app.UiModeManager.invalidateNightModeCache();
        dispatch(changed, null);
        // Location is obtained off the Binder thread and only while AUTO needs it.
        if (mode == MODE_NIGHT_AUTO) queueRefresh();
    }

    @Override public void setNightModeCustomType(int custom) {
        enforce(MODIFY);
        setMode(MODE_NIGHT_CUSTOM, custom);
    }
    @Override public int getNightModeCustomType() {
        enforce(MODIFY);
        return mState.custom;
    }

    @Override public void setApplicationNightMode(int mode) {
        validateMode(mode);
        ApkPackage caller = applicationCaller();
        // AOSP's package updater has no effect without a registered application PID.
        if (caller == null) return;
        int night = mode == MODE_NIGHT_YES ? UI_MODE_NIGHT_YES
                : mode == MODE_NIGHT_NO ? UI_MODE_NIGHT_NO : Configuration.UI_MODE_NIGHT_UNDEFINED;
        synchronized (mLock) {
            ensureOpen();
            synchronized (mPm) {
                if (mPm.get(caller.packageName) != caller || mPm.isRemovalPending(caller.packageName)) {
                    throw new SecurityException("Application installation changed");
                }
                State next = new State(mState);
                AppMode old = next.apps.get(caller.packageName);
                AppMode value = night == Configuration.UI_MODE_NIGHT_UNDEFINED ? null
                        : new AppMode(applicationOwner(caller), night);
                if (java.util.Objects.equals(value, old)) return;
                if (value == null) next.apps.remove(caller.packageName);
                else next.apps.put(caller.packageName, value);
                persist(next);
                mPolicyDirty = false;
                mState = next;
            }
        }
        dispatch(true, caller.packageName);
    }

    @Override public boolean setNightModeActivated(boolean active) {
        return activate(mState.custom, active, true);
    }
    @Override public boolean setNightModeActivatedForCustomMode(int custom, boolean active) {
        return activate(custom, active, false);
    }
    private boolean activate(int custom, boolean active, boolean interaction) {
        if (!hasPermission(MODIFY)) return false;
        if (UserHandle.getUserId(Binder.getCallingUid()) != 0
                && !hasPermission(android.Manifest.permission.INTERACT_ACROSS_USERS)) return false;
        boolean changed;
        synchronized (mLock) {
            ensureOpen();
            State next = new State(mState);
            if (custom == MODE_NIGHT_CUSTOM_TYPE_BEDTIME) next.bedtimeRequested = active;
            if (custom != next.custom) {
                // AOSP remembers an authorized bedtime request even when its
                // subtype is not selected. This is not a durable policy change.
                mState = next;
                return false;
            }
            next.configured = true;
            if (next.mode == MODE_NIGHT_AUTO || next.mode == MODE_NIGHT_CUSTOM) {
                next.overrideOn = active; next.overrideOff = !active;
            } else {
                next.mode = active ? MODE_NIGHT_YES : MODE_NIGHT_NO;
            }
            if (interaction) next.attention = MODE_ATTENTION_THEME_OVERLAY_OFF;
            next.waitForInactive = false;
            changed = publishLocked(next);
        }
        android.app.UiModeManager.invalidateNightModeCache();
        dispatch(changed, null);
        return true;
    }

    @Override public long getCustomNightModeStart() { return mState.start; }
    @Override public long getCustomNightModeEnd() { return mState.end; }
    @Override public void setCustomNightModeStart(long time) { setCustomTime(time, true); }
    @Override public void setCustomNightModeEnd(long time) { setCustomTime(time, false); }
    private void setCustomTime(long micros, boolean start) {
        boolean changed;
        synchronized (mLock) {
            ensureOpen();
            State next = new State(mState);
            // AOSP retains the configured endpoint on invalid input, but its
            // DateTimeException path unregisters the inactive-device wait.
            // Do not publish resources or alter durable policy in this path.
            if (micros < 0 || micros >= DAY_MICROS) {
                next.waitForInactive = false;
                mState = next;
                return;
            }
            if (start) next.start = micros; else next.end = micros;
            if (next.mode == MODE_NIGHT_CUSTOM) {
                next.overrideOn = next.overrideOff = false;
                changed = publishAutomaticLocked(next);
            } else {
                changed = commitLocked(next, true, false);
            }
        }
        dispatch(changed, null);
    }

    @Override public void setAttentionModeThemeOverlay(int type) {
        enforce(MODIFY);
        boolean changed;
        synchronized (mLock) {
            ensureOpen();
            State next = new State(mState);
            if (next.attention == type) return;
            next.attention = type;
            changed = publishLocked(next, false);
        }
        dispatch(changed, null);
    }
    @Override public int getAttentionModeThemeOverlay() { enforce(MODIFY); return mState.attention; }

    @Override public void addCallback(IUiModeManagerCallback callback, int userId) {
        userId = incomingUser(userId);
        if (userId == 0 && callback != null) {
            synchronized (mLock) { if (!mClosed) mCallbacks.register(callback); }
        }
    }
    @Override public void removeCallback(IUiModeManagerCallback callback, int userId) {
        if (incomingUser(userId) == 0 && callback != null) mCallbacks.unregister(callback);
    }
    // Contrast and accessibility inversion have their own settings. Changing Android
    // night resources is not an accessibility inversion and must not invoke these callbacks.
    @Override public float getContrast(int userId) { incomingUser(userId); return 0f; }
    @Override public int getForceInvertState(int userId) {
        incomingUser(userId); return FORCE_INVERT_TYPE_OFF;
    }
    @Override public int getForceInvertOverrideState(int userId, String packageName) {
        int uid = Binder.getCallingUid();
        if (packageName == null || mPm.isRemovalPending(packageName)
                || mPm.getPackageUid(packageName, 0, UserHandle.getUserId(uid)) != uid) {
            throw new SecurityException("Package does not belong to caller");
        }
        incomingUser(userId);
        return FORCE_INVERT_PACKAGE_ALLOWED;
    }

    private int incomingUser(int user) {
        if (user == UserHandle.USER_CURRENT || user == UserHandle.USER_CURRENT_OR_SELF) user = 0;
        if (user < 0) throw new IllegalArgumentException("Invalid user " + user);
        if (user != UserHandle.getUserId(Binder.getCallingUid())) {
            enforce(android.Manifest.permission.INTERACT_ACROSS_USERS_FULL);
        }
        return user;
    }
    private ApkPackage applicationCaller() {
        String name = Binder.getCallingPid() == Process.myPid() && mStandalonePackage != null
                ? mStandalonePackage : mCallers.packageForPid(Binder.getCallingPid());
        if (name == null) {
            if (mCallers.isSystem()) return null;
            throw new SecurityException("Unregistered application PID");
        }
        ApkPackage pkg = mPm.get(name);
        if (pkg == null || mPm.isRemovalPending(name) || pkg.uid() != Binder.getCallingUid()) {
            throw new SecurityException("Application Binder identity changed");
        }
        return pkg;
    }
    private boolean hasPermission(String permission) {
        if (mStandalonePackage == null && mCallers.isSystem()) return true;
        ApkPackage pkg;
        try { pkg = applicationCaller(); }
        catch (SecurityException unknownIdentity) { return false; }
        return pkg != null && mPm.checkPermission(permission, pkg.packageName, 0)
                == PackageManager.PERMISSION_GRANTED;
    }
    private void enforce(String permission) {
        if (!hasPermission(permission)) throw new SecurityException("Requires " + permission);
    }
    private static void validateMode(int mode) {
        if (mode != MODE_NIGHT_AUTO && mode != MODE_NIGHT_NO && mode != MODE_NIGHT_YES
                && mode != MODE_NIGHT_CUSTOM) throw new IllegalArgumentException("Unknown mode: " + mode);
    }

    /** Refresh an observed host snapshot; called by the worker, and the explicit fixture source. */
    void refreshEnvironment() {
        int events = mSource.takeEvents();
        ZoneId previousZone = mSource.zone();
        Environment observed = mSource.read(mState.mode == MODE_NIGHT_AUTO);
        boolean changed, preferenceChanged;
        synchronized (mLock) {
            if (mClosed) return;
            if (observed.powerSave() == null) observed = new Environment(observed.theme(),
                    observed.display(), mEnvironment.powerSave(), observed.twilight());
            boolean powerChanged = !java.util.Objects.equals(mEnvironment.powerSave(), observed.powerSave());
            boolean twilightChanged = !java.util.Objects.equals(mEnvironment.twilight(), observed.twilight());
            mEnvironment = observed;
            State next = new State(mState);
            preferenceChanged = adoptHostPreference(next);
            // Sample the automatic input without changing computed policy or
            // retiring a manual override. AOSP's deferred callbacks only wait
            // and reschedule; configuration-update entrypoints do the computation.
            next.baseNight = baseNight(next);
            boolean automaticChanged = next.mode == MODE_NIGHT_AUTO && twilightChanged
                    || next.mode == MODE_NIGHT_CUSTOM && next.custom == MODE_NIGHT_CUSTOM_TYPE_SCHEDULE
                    && (next.baseNight != mState.baseNight
                            || (events & WindowsHost.EVENT_TIME_CHANGED) != 0
                            || !previousZone.equals(mSource.zone()));
            boolean releasedWait = next.waitForInactive
                    && observed.display() == WindowsHost.DISPLAY_STATE_OFF;
            if (observed.display() == WindowsHost.DISPLAY_STATE_OFF) {
                next.waitForInactive = false;
            } else if (automaticChanged) {
                waitForAutomaticChange(next);
            }
            boolean updateConfiguration = preferenceChanged || powerChanged || releasedWait
                    || automaticChanged && automaticChangeAppliesImmediately(next);
            changed = updateConfiguration ? publishLocked(next, false) : commitLocked(next, false, false);
        }
        if (preferenceChanged) android.app.UiModeManager.invalidateNightModeCache();
        dispatch(changed, null);
    }
    private void queueRefresh() {
        synchronized (mLock) {
            if (mClosed || mRefreshQueued) return;
            mRefreshQueued = true;
            mWorker.execute(() -> {
                synchronized (mLock) { mRefreshQueued = false; }
                refreshFromHost();
            });
        }
    }
    private void refreshFromHost() {
        try { refreshEnvironment(); }
        catch (RuntimeException failure) {
            SystemServer.log("UiModeManagerService: host update failed; retaining committed policy: " + failure);
            synchronized (mLock) { scheduleLocked(); }
        }
    }
    private boolean adoptHostPreference(State state) {
        if (state.configured || (mEnvironment.theme() != 0 && mEnvironment.theme() != 1)) return false;
        int mode = mEnvironment.theme() == 0 ? MODE_NIGHT_YES : MODE_NIGHT_NO;
        boolean changed = mode != state.mode;
        state.mode = mode;
        return changed;
    }
    /** Pure input evaluation, also safe while an automatic update is deferred. */
    private boolean baseNight(State state) {
        switch (state.mode) {
            case MODE_NIGHT_NO: return false;
            case MODE_NIGHT_YES: return true;
            case MODE_NIGHT_AUTO:
                return mEnvironment.twilight() == null ? state.computed : mEnvironment.twilight().night();
            case MODE_NIGHT_CUSTOM:
                return state.custom == MODE_NIGHT_CUSTOM_TYPE_BEDTIME ? state.bedtimeRequested
                        : between(Instant.ofEpochMilli(mSource.now()).atZone(mSource.zone()).toLocalTime(),
                                LocalTime.ofNanoOfDay(state.start * 1000),
                                LocalTime.ofNanoOfDay(state.end * 1000));
            default: throw new AssertionError(state.mode);
        }
    }
    private void compute(State state) {
        boolean base = baseNight(state);
        state.baseNight = state.computed = base;
        if (state.attention != MODE_ATTENTION_THEME_OVERLAY_OFF) {
            state.computed = state.attention == MODE_ATTENTION_THEME_OVERLAY_NIGHT;
        } else if (state.mode == MODE_NIGHT_AUTO || state.mode == MODE_NIGHT_CUSTOM) {
            if (state.overrideOn && !base) state.computed = true;
            else if (state.overrideOff && base) state.computed = false;
            else if (state.mode != MODE_NIGHT_AUTO || mEnvironment.twilight() != null) {
                state.overrideOn = state.overrideOff = false;
            }
        }
    }
    // Android TimeUtils.isTimeBetween: both endpoints inclusive, equal endpoints all day.
    static boolean between(LocalTime now, LocalTime start, LocalTime end) {
        if (start.isBefore(end)) return !now.isBefore(start) && !now.isAfter(end);
        return !now.isBefore(start) || !now.isAfter(end);
    }
    private int effectiveNight(State state) {
        return state.computed || Boolean.TRUE.equals(mEnvironment.powerSave()) ? UI_MODE_NIGHT_YES : UI_MODE_NIGHT_NO;
    }
    private boolean automaticChangeAppliesImmediately(State state) {
        return mEnvironment.display() == WindowsHost.DISPLAY_STATE_OFF
                || state.mode == MODE_NIGHT_CUSTOM && state.custom == MODE_NIGHT_CUSTOM_TYPE_BEDTIME;
    }
    private void waitForAutomaticChange(State state) {
        // AOSP does not register a new inactive-device wait during battery saver.
        // An existing wait is retained until inactivity or an explicit selection.
        if (!automaticChangeAppliesImmediately(state) && !Boolean.TRUE.equals(mEnvironment.powerSave())) {
            state.waitForInactive = true;
        }
    }
    private boolean publishAutomaticLocked(State next) {
        if (automaticChangeAppliesImmediately(next)) {
            next.waitForInactive = false;
            return publishLocked(next);
        }
        next.baseNight = baseNight(next);
        waitForAutomaticChange(next);
        return commitLocked(next, true, false);
    }
    private boolean publishLocked(State next) { return publishLocked(next, true); }
    private boolean publishLocked(State next, boolean requirePersistence) {
        compute(next);
        return commitLocked(next, requirePersistence, true);
    }
    private boolean commitLocked(State next, boolean requirePersistence, boolean updateConfiguration) {
        State old = mState;
        // Computed resources, attention and bedtime are transient. AUTO's last
        // computed cache is saved at shutdown, not before configuration dispatch.
        boolean policyChanged = old.configured != next.configured || old.mode != next.mode || old.custom != next.custom
                || old.start != next.start || old.end != next.end || old.overrideOn != next.overrideOn
                || old.overrideOff != next.overrideOff || !old.apps.equals(next.apps);
        if (policyChanged || mPolicyDirty) {
            if (requirePersistence) {
                persist(next);
                mPolicyDirty = false;
            } else {
                // Host observations and attention updates can reset a saved
                // override. AOSP still publishes resources if that write fails.
                persistDerivedState(next);
            }
        }
        mState = next;
        // Attention updates configuration without cancelling a previously armed
        // wait. Battery saver bypasses this gate, but does not release the wait.
        boolean apply = updateConfiguration
                && (!next.waitForInactive || Boolean.TRUE.equals(mEnvironment.powerSave()));
        int effective = effectiveNight(next);
        boolean changed = apply && effective != mAppliedNight;
        if (apply) mAppliedNight = effective;
        scheduleLocked();
        return changed;
    }
    private void persistDerivedState(State state) {
        try {
            persist(state);
            mPolicyDirty = false;
        } catch (RuntimeException failure) {
            mPolicyDirty = true;
            SystemServer.log("UiModeManagerService: cannot save derived state; retaining active configuration: " + failure);
        }
    }
    private void scheduleLocked() {
        if (mAlarm != null) { mAlarm.cancel(false); mAlarm = null; }
        if (mClosed) return;
        long now = mSource.now();
        long next = now + REFRESH_MS;
        State state = mState;
        if (state.mode == MODE_NIGHT_CUSTOM && state.custom == MODE_NIGHT_CUSTOM_TYPE_SCHEDULE
                && state.start != state.end) {
            next = Math.min(next, Math.min(nextBoundary(now, state.start, false),
                    nextBoundary(now, state.end, true)));
        } else if (state.mode == MODE_NIGHT_AUTO && mEnvironment.twilight() != null
                && mEnvironment.twilight().nextChange() > now) {
            next = Math.min(next, mEnvironment.twilight().nextChange());
        }
        mAlarm = mWorker.schedule(this::refreshFromHost, Math.max(1, next - now), TimeUnit.MILLISECONDS);
    }
    private long nextBoundary(long now, long micros, boolean end) {
        ZoneId zone = mSource.zone();
        LocalDate date = Instant.ofEpochMilli(now).atZone(zone).toLocalDate();
        LocalTime time = LocalTime.ofNanoOfDay(micros * 1000);
        ZonedDateTime boundary = date.atTime(time).atZone(zone);
        long next = boundaryMillis(boundary, end);
        // Compare the actual observable transition, not its unrounded endpoint:
        // at an inclusive end equal to now the transition is still one ms away.
        if (next <= now) next = boundaryMillis(date.plusDays(1).atTime(time).atZone(zone), end);
        return next;
    }
    private static long boundaryMillis(ZonedDateTime boundary, boolean end) {
        long millis = boundary.toInstant().toEpochMilli();
        // Source.now() observes milliseconds. Start is inclusive (ceil), whereas
        // the inclusive end changes state at the first millisecond strictly after it.
        return millis + (end || boundary.getNano() % 1_000_000 != 0 ? 1 : 0);
    }
    private void dispatch(boolean changed, String pkg) {
        if (!changed) return;
        long identity = Binder.clearCallingIdentity();
        try { mConfigurationChanged.accept(pkg); }
        finally { Binder.restoreCallingIdentity(identity); }
    }
    private void removePackage(String name) {
        synchronized (mLock) {
            if (mClosed || !mState.apps.containsKey(name)) return;
            State next = new State(mState);
            next.apps.remove(name);
            persist(next);
            mPolicyDirty = false;
            mState = next;
        }
    }
    private String applicationOwner(ApkPackage pkg) {
        // The standalone file is inside this package's own data lifetime. The device
        // file instead binds every row to PM's durable installation marker.
        return pkg.packageName.equals(mStandalonePackage) ? "standalone" : pkg.enabledStateOwner();
    }
    private void ensureOpen() { if (mClosed) throw new IllegalStateException("UiMode service is closed"); }

    private State read() {
        State state = new State();
        if (mFile == null || !mFile.exists()) return state;
        Properties values = new Properties();
        try (FileInputStream in = new FileInputStream(mFile)) {
            values.load(in);
            state.configured = Boolean.parseBoolean(values.getProperty("configured", "false"));
            state.mode = Integer.parseInt(values.getProperty("mode", "1"));
            validateMode(state.mode);
            state.custom = Integer.parseInt(values.getProperty("custom", "-1"));
            if (state.mode == MODE_NIGHT_CUSTOM && state.custom != MODE_NIGHT_CUSTOM_TYPE_SCHEDULE
                    && state.custom != MODE_NIGHT_CUSTOM_TYPE_BEDTIME) throw new IllegalArgumentException("custom");
            if (state.mode != MODE_NIGHT_CUSTOM) state.custom = MODE_NIGHT_CUSTOM_TYPE_UNKNOWN;
            state.start = Long.parseLong(values.getProperty("start", Long.toString(state.start)));
            state.end = Long.parseLong(values.getProperty("end", Long.toString(state.end)));
            if (state.start < 0 || state.start >= DAY_MICROS || state.end < 0 || state.end >= DAY_MICROS) {
                throw new IllegalArgumentException("schedule");
            }
            // Secure.UI_NIGHT_MODE_LAST_COMPUTED: AUTO keeps its last known result
            // across a restart until a real twilight location is available again.
            if (state.mode == MODE_NIGHT_AUTO) {
                state.computed = Boolean.parseBoolean(values.getProperty("computed", "false"));
            }
            state.overrideOn = Boolean.parseBoolean(values.getProperty("overrideOn", "false"));
            state.overrideOff = Boolean.parseBoolean(values.getProperty("overrideOff", "false"));
            for (String key : values.stringPropertyNames()) {
                if (!key.startsWith("app.")) continue;
                String name = key.substring(4);
                String[] parts = values.getProperty(key).split(":", -1);
                if (parts.length != 2) throw new IllegalArgumentException("application mode");
                int night = Integer.parseInt(parts[1]);
                ApkPackage pkg = mPm.get(name);
                if (pkg != null && parts[0].equals(applicationOwner(pkg))
                        && (night == UI_MODE_NIGHT_NO || night == UI_MODE_NIGHT_YES)) {
                    state.apps.put(name, new AppMode(parts[0], night));
                }
            }
        } catch (IOException | IllegalArgumentException failure) {
            throw new IllegalStateException("Cannot read UI mode settings", failure);
        }
        return state;
    }
    private void persist(State state) {
        if (mFile == null) return;
        Properties values = new Properties();
        values.setProperty("configured", Boolean.toString(state.configured));
        values.setProperty("mode", Integer.toString(state.mode));
        values.setProperty("custom", Integer.toString(state.custom));
        values.setProperty("start", Long.toString(state.start));
        values.setProperty("end", Long.toString(state.end));
        values.setProperty("computed", Boolean.toString(state.computed));
        values.setProperty("overrideOn", Boolean.toString(state.overrideOn));
        values.setProperty("overrideOff", Boolean.toString(state.overrideOff));
        for (var app : state.apps.entrySet()) {
            values.setProperty("app." + app.getKey(), app.getValue().owner() + ":" + app.getValue().night());
        }
        Path temporary = null;
        try {
            Path directory = mFile.toPath().toAbsolutePath().getParent();
            Files.createDirectories(directory);
            temporary = Files.createTempFile(directory, ".ui-mode-", ".tmp");
            try (FileOutputStream out = new FileOutputStream(temporary.toFile())) {
                values.store(out, "Android UI mode policy"); out.getFD().sync();
            }
            Files.move(temporary, mFile.toPath(), StandardCopyOption.ATOMIC_MOVE,
                    StandardCopyOption.REPLACE_EXISTING);
        } catch (IOException failure) {
            throw new IllegalStateException("Cannot persist UI mode settings", failure);
        } finally {
            if (temporary != null) try { Files.deleteIfExists(temporary); } catch (IOException ignored) { }
        }
    }
    @Override public void close() {
        synchronized (mLock) {
            if (mClosed) return;
            mClosed = true;
            if (mAlarm != null) mAlarm.cancel(false);
            mWorker.shutdownNow();
            mCallbacks.kill();
            if (mState.mode == MODE_NIGHT_AUTO || mPolicyDirty) persistDerivedState(mState);
            synchronized (UiModeManagerService.class) { if (sInstance == this) sInstance = null; }
        }
        mSource.close();
        mPm.removePackageRemovalListener(mRemovalListener);
    }
}
