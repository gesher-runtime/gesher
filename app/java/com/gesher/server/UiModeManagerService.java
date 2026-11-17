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
 * user changes apply immediately. MODE_NIGHT_AUTO uses astronomical twilight, never
 * the desktop theme. See docs/ui-mode.md for the pinned AOSP contract and boundaries.
 */
public final class UiModeManagerService extends IUiModeManager.Default implements AutoCloseable {
    private static final String MODIFY = android.Manifest.permission.MODIFY_DAY_NIGHT_MODE;
    private static final long DAY_MICROS = 86_400_000_000L;
    private static final long REFRESH_MS = 3_600_000L;
    private static volatile UiModeManagerService sInstance;

    /** Explicit, instance-owned host boundary; ordinary execution always uses Windows. */
    interface Source extends AutoCloseable {
        long now();
        ZoneId zone();
        Environment read(boolean twilight);
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
        boolean computed, overrideOn, overrideOff;
        final Map<String, AppMode> apps = new HashMap<>();
        State() { }
        State(State before) {
            configured = before.configured; mode = before.mode; custom = before.custom;
            start = before.start; end = before.end; computed = before.computed;
            overrideOn = before.overrideOn; overrideOff = before.overrideOff;
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
    private boolean mBedtimeRequested, mClosed, mRefreshQueued;
    private volatile int mAttention = MODE_ATTENTION_THEME_OVERLAY_OFF;

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
        UiModeManagerService previous;
        synchronized (UiModeManagerService.class) {
            previous = sInstance;
            sInstance = this;
        }
        // Services may be rebuilt in an isolated driver. The old singleton must not
        // retain a worker, host listener, callbacks or installation-removal listener.
        if (previous != null) previous.close();
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
            compute(next);
            changed = publishLocked(next, mode != MODE_NIGHT_AUTO && mode != MODE_NIGHT_CUSTOM);
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
            if (custom == MODE_NIGHT_CUSTOM_TYPE_BEDTIME) mBedtimeRequested = active;
            if (custom != mState.custom) return false;
            State next = new State(mState);
            next.configured = true;
            if (next.mode == MODE_NIGHT_AUTO || next.mode == MODE_NIGHT_CUSTOM) {
                next.overrideOn = active; next.overrideOff = !active;
            } else {
                next.mode = active ? MODE_NIGHT_YES : MODE_NIGHT_NO;
            }
            if (interaction) mAttention = MODE_ATTENTION_THEME_OVERLAY_OFF;
            compute(next);
            changed = publishLocked(next, true);
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
        // AOSP ignores times outside LocalTime's range; units are microseconds, not millis.
        if (micros < 0 || micros >= DAY_MICROS) return;
        boolean changed;
        synchronized (mLock) {
            ensureOpen();
            State next = new State(mState);
            if (start) next.start = micros; else next.end = micros;
            if (next.mode == MODE_NIGHT_CUSTOM) next.overrideOn = next.overrideOff = false;
            compute(next);
            changed = publishLocked(next, false);
        }
        dispatch(changed, null);
    }

    @Override public void setAttentionModeThemeOverlay(int type) {
        enforce(MODIFY);
        boolean changed;
        synchronized (mLock) {
            ensureOpen();
            mAttention = type;
            State next = new State(mState);
            compute(next);
            changed = publishLocked(next, true);
        }
        dispatch(changed, null);
    }
    @Override public int getAttentionModeThemeOverlay() { enforce(MODIFY); return mAttention; }

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
        Environment observed = mSource.read(mState.mode == MODE_NIGHT_AUTO);
        boolean changed, preferenceChanged;
        synchronized (mLock) {
            if (mClosed) return;
            if (observed.powerSave() == null) observed = new Environment(observed.theme(),
                    observed.display(), mEnvironment.powerSave(), observed.twilight());
            boolean powerChanged = !java.util.Objects.equals(mEnvironment.powerSave(), observed.powerSave());
            mEnvironment = observed;
            State next = new State(mState);
            preferenceChanged = adoptHostPreference(next);
            compute(next);
            changed = publishLocked(next, preferenceChanged || powerChanged);
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
    private void compute(State state) {
        boolean base;
        switch (state.mode) {
            case MODE_NIGHT_NO: base = false; break;
            case MODE_NIGHT_YES: base = true; break;
            case MODE_NIGHT_AUTO:
                base = mEnvironment.twilight() == null ? state.computed : mEnvironment.twilight().night();
                break;
            case MODE_NIGHT_CUSTOM:
                base = state.custom == MODE_NIGHT_CUSTOM_TYPE_BEDTIME ? mBedtimeRequested
                        : between(Instant.ofEpochMilli(mSource.now()).atZone(mSource.zone()).toLocalTime(),
                                LocalTime.ofNanoOfDay(state.start * 1000),
                                LocalTime.ofNanoOfDay(state.end * 1000));
                break;
            default: throw new AssertionError(state.mode);
        }
        state.computed = base;
        if (mAttention != MODE_ATTENTION_THEME_OVERLAY_OFF) {
            state.computed = mAttention == MODE_ATTENTION_THEME_OVERLAY_NIGHT;
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
    private boolean publishLocked(State next, boolean immediate) {
        State old = mState;
        if (old.configured != next.configured || old.mode != next.mode || old.custom != next.custom
                || old.start != next.start || old.end != next.end || old.overrideOn != next.overrideOn
                || old.overrideOff != next.overrideOff || old.computed != next.computed
                || !old.apps.equals(next.apps)) persist(next);
        mState = next;
        boolean apply = immediate || mEnvironment.display() == WindowsHost.DISPLAY_STATE_OFF
                || next.mode == MODE_NIGHT_CUSTOM && next.custom == MODE_NIGHT_CUSTOM_TYPE_BEDTIME;
        int effective = effectiveNight(next);
        boolean changed = apply && effective != mAppliedNight;
        if (apply) mAppliedNight = effective;
        scheduleLocked();
        return changed;
    }
    private void scheduleLocked() {
        if (mAlarm != null) { mAlarm.cancel(false); mAlarm = null; }
        if (mClosed) return;
        long now = mSource.now();
        long next = now + REFRESH_MS;
        State state = mState;
        if (state.mode == MODE_NIGHT_CUSTOM && state.custom == MODE_NIGHT_CUSTOM_TYPE_SCHEDULE) {
            next = Math.min(next, Math.min(nextBoundary(now, state.start), nextBoundary(now, state.end)));
        } else if (state.mode == MODE_NIGHT_AUTO && mEnvironment.twilight() != null
                && mEnvironment.twilight().nextChange() > now) {
            next = Math.min(next, mEnvironment.twilight().nextChange());
        }
        mAlarm = mWorker.schedule(this::refreshFromHost, Math.max(1, next - now), TimeUnit.MILLISECONDS);
    }
    private long nextBoundary(long now, long micros) {
        ZoneId zone = mSource.zone();
        LocalDate date = Instant.ofEpochMilli(now).atZone(zone).toLocalDate();
        LocalTime time = LocalTime.ofNanoOfDay(micros * 1000);
        ZonedDateTime boundary = date.atTime(time).atZone(zone);
        if (boundary.toInstant().toEpochMilli() <= now) boundary = date.plusDays(1).atTime(time).atZone(zone);
        // The end is inclusive in Android's LocalTime predicate. Reevaluate just after it.
        return boundary.toInstant().toEpochMilli() + 1;
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
            synchronized (UiModeManagerService.class) { if (sInstance == this) sInstance = null; }
        }
        mSource.close();
        mPm.removePackageRemovalListener(mRemovalListener);
    }
}
