/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.server;

import android.os.IBinder;
import android.os.Binder;
import android.os.IPowerManager;
import android.os.IWakeLockCallback;
import android.os.PowerManager;
import android.os.PowerSaveState;
import android.os.RemoteException;
import android.os.WorkSource;

import java.util.LinkedHashMap;
import java.util.Map;
import java.util.Objects;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;

/**
 * {@code power}: PowerManagerService on Windows (devsvc track, ADR 0038). Wake locks are a
 * power request of the system server (PowerCreateRequest/PowerSetRequest, which
 * {@code powercfg /requests} lists with the holders' packages and tags): a partial wake
 * lock keeps Windows from sleeping, a screen or full one keeps the display on too, until
 * released or their process dies. Battery saver is Windows' (GetSystemPowerStatus's
 * SystemStatusFlag). The screen is on (interactive) while Windows is awake: when Windows goes
 * to sleep (WM_POWERBROADCAST PBT_APMSUSPEND) the device is not interactive and registered
 * receivers get ACTION_SCREEN_OFF, on resume (PBT_APMRESUMEAUTOMATIC/RESUMESUSPEND)
 * ACTION_SCREEN_ON (hwsvc2, ADR 0051). There is no device idle (doze).
 */
final class PowerManagerService extends IPowerManager.Default implements WindowsHost.Listener {
    /** A held wake lock. */
    private static final class WakeLock implements IBinder.DeathRecipient {
        final IBinder token;
        int flags;
        String tag;
        final String packageName;
        final PowerManagerService owner;
        WorkSource workSource;
        IWakeLockCallback callback;
        boolean dead; // guarded by owner, including death during registration

        WakeLock(PowerManagerService owner, IBinder token, int flags, String tag,
                String packageName, WorkSource workSource, IWakeLockCallback callback) {
            this.owner = owner;
            this.token = token;
            this.flags = flags;
            this.tag = tag;
            this.packageName = packageName;
            this.workSource = workSource;
            this.callback = callback;
        }

        boolean display() {
            int level = flags & PowerManager.WAKE_LOCK_LEVEL_MASK;
            return level == PowerManager.SCREEN_DIM_WAKE_LOCK
                    || level == PowerManager.SCREEN_BRIGHT_WAKE_LOCK
                    || level == PowerManager.FULL_WAKE_LOCK;
        }

        @Override
        public void binderDied() {
            owner.died(this);
        }
    }

    private final Map<IBinder, WakeLock> mLocks = new LinkedHashMap<>();  // guarded by this
    private String mApplied = "";  // guarded by this: the request's reason, "" for none
    private boolean mApplyPending; // a failed request must be retried, including release
    // Notifier's ordered asynchronous delivery: never invoke an app on the
    // service's mutation thread or while that callback holds the service lock.
    private final ExecutorService mCallbacks = Executors.newSingleThreadExecutor(task -> {
        Thread thread = new Thread(task, "gesher-wake-lock-callbacks");
        thread.setDaemon(true);
        return thread;
    });
    private volatile boolean mInteractive = true;

    PowerManagerService() {
        // Sleep and resume come from the system server's event window.
        if (WindowsHost.inSystemServer()) WindowsHost.addListener(this);
    }

    @Override
    public void onWindowsEvents(int events) {
        // A resume after a suspend in the same batch: the device is awake again.
        if ((events & WindowsHost.EVENT_SUSPENDING) != 0) setInteractive(false);
        if ((events & WindowsHost.EVENT_RESUMED) != 0) setInteractive(true);
    }

    private void setInteractive(boolean interactive) {
        if (mInteractive == interactive) return;
        mInteractive = interactive;
        SystemServer.log("power: Windows " + (interactive ? "resumed: the screen is on"
                : "goes to sleep: the screen is off"));
        Services.Created c = Services.sLast;
        if (c == null) return;
        // As PowerManagerService's Notifier: registered receivers only, in the foreground.
        android.content.Intent i = new android.content.Intent(interactive
                ? android.content.Intent.ACTION_SCREEN_ON : android.content.Intent.ACTION_SCREEN_OFF);
        i.addFlags(android.content.Intent.FLAG_RECEIVER_REGISTERED_ONLY
                | android.content.Intent.FLAG_RECEIVER_FOREGROUND);
        try {
            c.am.sendBroadcastFor("android", i, null);
        } catch (Throwable t) {
            SystemServer.log("power: " + i.getAction() + " failed: " + t);
        }
    }

    @Override
    public void acquireWakeLock(IBinder lock, int flags, String tag, String packageName,
            WorkSource ws, String historyTag, int displayId, IWakeLockCallback callback) {
        acquire(lock, flags, tag, packageName, ws, callback);
    }

    @Override
    public void acquireWakeLockWithUid(IBinder lock, int flags, String tag, String packageName,
            int uid, int displayId, IWakeLockCallback callback) {
        acquire(lock, flags, tag, packageName,
                new WorkSource(uid < 0 ? Binder.getCallingUid() : uid), callback);
    }

    @Override
    public void acquireWakeLockAsync(IBinder lock, int flags, String tag, String packageName,
            WorkSource ws, String historyTag) {
        acquire(lock, flags, tag, packageName, ws, null);
    }

    @Override
    public void releaseWakeLock(IBinder lock, int flags) {
        release(lock, "released");
    }

    @Override
    public void releaseWakeLockAsync(IBinder lock, int flags) {
        release(lock, "released");
    }

    private static void requireToken(IBinder token) {
        if (token == null) throw new IllegalArgumentException("lock must not be null");
    }

    private static boolean sameCallback(IWakeLockCallback first, IWakeLockCallback second) {
        return first == second || first != null && second != null
                && first.asBinder() == second.asBinder();
    }

    private static boolean monitored(int flags) {
        int level = flags & PowerManager.WAKE_LOCK_LEVEL_MASK;
        return level == PowerManager.PARTIAL_WAKE_LOCK
                || level == PowerManager.PARTIAL_SLEEP_WAKE_LOCK
                || level == PowerManager.SCREEN_DIM_WAKE_LOCK
                || level == PowerManager.SCREEN_BRIGHT_WAKE_LOCK
                || level == PowerManager.FULL_WAKE_LOCK
                || level == PowerManager.PROXIMITY_SCREEN_OFF_WAKE_LOCK
                || level == PowerManager.DRAW_WAKE_LOCK;
    }

    /** Called while holding this, so queued transitions keep lifecycle order. */
    private void notifyCallback(IWakeLockCallback callback, boolean enabled) {
        if (callback == null) return;
        mCallbacks.execute(() -> {
            try {
                callback.onStateChanged(enabled);
            } catch (RemoteException | RuntimeException error) {
                // A dead/rejected listener is not the death of its lock token.
                SystemServer.log("power: wake lock listener failed: " + error);
            }
        });
    }

    private void changingCallbacks(int oldFlags, WorkSource oldSource, IWakeLockCallback oldCallback,
            int flags, WorkSource source, IWakeLockCallback callback) {
        // The pinned Notifier's two-attributed-source path updates accounting
        // without listener transitions. Attribution itself is not power state.
        if (oldSource != null && source != null && monitored(oldFlags) && monitored(flags)) return;
        if (oldCallback != null && !sameCallback(oldCallback, callback)) notifyCallback(oldCallback, false);
        notifyCallback(callback, true);
    }

    @Override
    public void updateWakeLockCallback(IBinder lock, IWakeLockCallback callback) {
        requireToken(lock);
        synchronized (this) {
            WakeLock w = mLocks.get(lock);
            if (w == null) throw new IllegalArgumentException("Wake lock not active");
            if (sameCallback(w.callback, callback)) return;
            IWakeLockCallback old = w.callback;
            w.callback = callback;
            changingCallbacks(w.flags, w.workSource, old, w.flags, w.workSource, callback);
        }
    }

    private void acquire(IBinder lock, int flags, String tag, String packageName,
            WorkSource workSource, IWakeLockCallback callback) {
        requireToken(lock);
        if (packageName == null) throw new IllegalArgumentException("packageName must not be null");
        PowerManager.validateWakeLockParameters(flags, tag);
        WorkSource source = workSource == null || workSource.isEmpty() ? null : new WorkSource(workSource);
        synchronized (this) {
            WakeLock w = mLocks.get(lock);
            if (w != null) {
                if (!w.packageName.equals(packageName)) {
                    throw new IllegalStateException("Existing wake lock package name changed");
                }
                // Android's hasSameProperties does not consider the callback:
                // updating only that listener uses updateWakeLockCallback.
                boolean changed = w.flags != flags || !w.tag.equals(tag)
                        || !Objects.equals(w.workSource, source);
                int oldFlags = w.flags;
                WorkSource oldSource = w.workSource;
                IWakeLockCallback oldCallback = w.callback;
                if (changed) {
                    w.flags = flags;
                    w.tag = tag;
                    w.workSource = source;
                    w.callback = callback;
                }
                apply();
                if (changed) changingCallbacks(oldFlags, oldSource, oldCallback, flags, source, callback);
                return;
            }
            w = new WakeLock(this, lock, flags, tag, packageName, source, callback);
            // Serialize registration with release. Publish only a successfully
            // linked token, and never let an old recipient retire a new record.
            try {
                lock.linkToDeath(w, 0);
            } catch (RemoteException error) {
                throw new IllegalArgumentException("Wake lock token is already dead", error);
            }
            if (w.dead) {
                unlink(w);
                throw new IllegalArgumentException("Wake lock token died during registration");
            }
            mLocks.put(lock, w);
            SystemServer.log("power: wake lock " + tag + " of " + packageName + " acquired ("
                    + (w.display() ? "screen" : "partial") + ")");
            apply();
            notifyCallback(callback, true);
        }
    }

    private void release(IBinder lock, String why) {
        requireToken(lock);
        synchronized (this) {
            WakeLock w = mLocks.get(lock);
            if (w != null) releaseLocked(w, why);
        }
    }

    private synchronized void died(WakeLock w) {
        w.dead = true;
        if (mLocks.get(w.token) == w) releaseLocked(w, "its process died");
    }

    private static void unlink(WakeLock w) {
        try {
            w.token.unlinkToDeath(w, 0);
        } catch (RuntimeException error) {
            // gone
        }
    }

    private void releaseLocked(WakeLock w, String why) {
        mLocks.remove(w.token);
        unlink(w);
        SystemServer.log("power: wake lock " + w.tag + " of " + w.packageName + " " + why);
        apply();
        notifyCallback(w.callback, false);
    }

    /** Sets the power request from the held wake locks. */
    private void apply() {
        boolean system;
        boolean display;
        String reason;
        synchronized (this) {
            system = !mLocks.isEmpty();
            display = false;
            StringBuilder b = new StringBuilder();
            for (WakeLock w : mLocks.values()) {
                display |= w.display();
                if (b.length() > 0) b.append(", ");
                b.append(w.packageName).append(" (").append(w.tag).append(')');
            }
            reason = system ? "Android wake lock: " + b + (display ? " [screen]" : "") : "";
            if (!mApplyPending && reason.equals(mApplied)) return;
            // Under the lock: requests are replaced in the order they were computed.
            boolean ok = WindowsHost.keepAwake(reason, system, display);
            mApplyPending = !ok;
            if (ok) mApplied = reason;
            if (system || ok) {
                SystemServer.log("power: " + (system ? "Windows power request \"" + reason
                        + "\"" + (ok ? "" : " not made") : "no power request"));
            }
        }
    }

    @Override
    public boolean isWakeLockLevelSupported(int level) {
        return level == PowerManager.PARTIAL_WAKE_LOCK
                || level == PowerManager.SCREEN_DIM_WAKE_LOCK
                || level == PowerManager.SCREEN_BRIGHT_WAKE_LOCK
                || level == PowerManager.FULL_WAKE_LOCK;
    }

    @Override
    public boolean isWakeLockLevelSupportedWithDisplayId(int level, int displayId) {
        return isWakeLockLevelSupported(level);
    }

    @Override
    public boolean isInteractive() {
        return mInteractive;
    }

    @Override
    public boolean isDisplayInteractive(int displayId) {
        return mInteractive;
    }

    private static boolean batterySaver() {
        int[] s = WindowsHost.powerStatus();
        return s != null && s[3] == 1;
    }

    @Override
    public boolean isPowerSaveMode() {
        return batterySaver();
    }

    @Override
    public PowerSaveState getPowerSaveState(int serviceType) {
        boolean on = batterySaver();
        return new PowerSaveState.Builder().setBatterySaverEnabled(on)
                .setGlobalBatterySaverEnabled(on).setBrightnessFactor(1f).build();
    }

    @Override
    public boolean isBatterySaverSupported() {
        return true;
    }

    @Override
    public boolean isDeviceIdleMode() {
        return false;
    }

    @Override
    public boolean isLightDeviceIdleMode() {
        return false;
    }
}
