/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.server;

import android.os.IBinder;
import android.os.IPowerManager;
import android.os.IWakeLockCallback;
import android.os.PowerManager;
import android.os.PowerSaveState;
import android.os.RemoteException;
import android.os.WorkSource;

import java.util.LinkedHashMap;
import java.util.Map;

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
        final int level;
        final String tag;
        final String packageName;
        final PowerManagerService owner;

        WakeLock(PowerManagerService owner, IBinder token, int level, String tag,
                String packageName) {
            this.owner = owner;
            this.token = token;
            this.level = level;
            this.tag = tag;
            this.packageName = packageName;
        }

        boolean display() {
            return level == PowerManager.SCREEN_DIM_WAKE_LOCK
                    || level == PowerManager.SCREEN_BRIGHT_WAKE_LOCK
                    || level == PowerManager.FULL_WAKE_LOCK;
        }

        @Override
        public void binderDied() {
            owner.release(token, "its process died");
        }
    }

    private final Map<IBinder, WakeLock> mLocks = new LinkedHashMap<>();  // guarded by this
    private String mApplied = "";  // guarded by this: the request's reason, "" for none
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
        acquire(lock, flags, tag, packageName);
    }

    @Override
    public void acquireWakeLockWithUid(IBinder lock, int flags, String tag, String packageName,
            int uid, int displayId, IWakeLockCallback callback) {
        acquire(lock, flags, tag, packageName);
    }

    @Override
    public void acquireWakeLockAsync(IBinder lock, int flags, String tag, String packageName,
            WorkSource ws, String historyTag) {
        acquire(lock, flags, tag, packageName);
    }

    @Override
    public void releaseWakeLock(IBinder lock, int flags) {
        release(lock, "released");
    }

    @Override
    public void releaseWakeLockAsync(IBinder lock, int flags) {
        release(lock, "released");
    }

    private void acquire(IBinder lock, int flags, String tag, String packageName) {
        int level = flags & PowerManager.WAKE_LOCK_LEVEL_MASK;
        WakeLock w = new WakeLock(this, lock, level, tag, packageName);
        boolean added;
        synchronized (this) {
            WakeLock old = mLocks.put(lock, w);
            added = old == null;
            if (old != null) lock.unlinkToDeath(old, 0);
        }
        try {
            lock.linkToDeath(w, 0);
        } catch (RemoteException e) {
            release(lock, "its process died");
            return;
        }
        if (added) {
            SystemServer.log("power: wake lock " + tag + " of " + packageName + " acquired ("
                    + (w.display() ? "screen" : "partial") + ")");
        }
        apply();
    }

    private void release(IBinder lock, String why) {
        WakeLock w;
        synchronized (this) {
            w = mLocks.remove(lock);
        }
        if (w == null) return;
        try {
            lock.unlinkToDeath(w, 0);
        } catch (Throwable t) {
            // gone
        }
        SystemServer.log("power: wake lock " + w.tag + " of " + w.packageName + " " + why);
        apply();
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
            if (reason.equals(mApplied)) return;
            mApplied = reason;
            // Under the lock: requests are replaced in the order they were computed.
            boolean ok = WindowsHost.keepAwake(reason, system, display);
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
