/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.server;

import android.app.ActivityManager;
import android.app.IUiModeManager;
import android.app.IUiModeManagerCallback;
import android.app.UiModeManager;
import android.content.Intent;
import android.content.res.Configuration;
import android.os.IBinder;
import android.os.RemoteException;

import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;

/**
 * Android UiModeManager service contract implementation and Windows host theme synchronization
 * (Track W Item W06).
 *
 * <p>Supports {@link UiModeManager#MODE_NIGHT_NO}, {@link UiModeManager#MODE_NIGHT_YES},
 * {@link UiModeManager#MODE_NIGHT_AUTO}, and {@link UiModeManager#MODE_NIGHT_CUSTOM}.
 * Automatically syncs with Windows host theme when set to auto mode, and notifies registered
 * callbacks and {@link ActivityTaskManagerService} when effective night mode changes.
 */
public class UiModeManagerService extends IUiModeManager.Default implements WindowsHost.Listener {
    private static final String TAG = "UiModeManagerService: ";

    private static volatile UiModeManagerService sInstance;
    private static volatile int sUiModeNight = Configuration.UI_MODE_NIGHT_NO;

    private final ActivityTaskManagerService mAtm;
    private final ActivityManagerService mAm;
    private final Object mLock = new Object();

    private int mNightMode = UiModeManager.MODE_NIGHT_NO;
    private boolean mNightModeActivated = false;
    private final Configuration mConfiguration = new Configuration();
    private final Map<IBinder, CallbackRecord> mCallbacks = new HashMap<>();

    private final class CallbackRecord implements IBinder.DeathRecipient {
        final IUiModeManagerCallback callback;
        final int userId;

        CallbackRecord(IUiModeManagerCallback callback, int userId) {
            this.callback = callback;
            this.userId = userId;
        }

        @Override
        public void binderDied() {
            synchronized (mLock) {
                mCallbacks.remove(callback.asBinder());
            }
        }
    }

    public static int getUiModeNight() {
        return sUiModeNight;
    }

    public static UiModeManagerService getInstance() {
        return sInstance;
    }

    public UiModeManagerService() {
        this(null, null);
    }

    public UiModeManagerService(ActivityTaskManagerService atm) {
        this(atm, null);
    }

    public UiModeManagerService(ActivityTaskManagerService atm, ActivityManagerService am) {
        mAtm = atm;
        mAm = am;
        sInstance = this;
        mConfiguration.uiMode = Configuration.UI_MODE_TYPE_NORMAL | sUiModeNight;
        if (WindowsHost.available()) {
            WindowsHost.addListener(this);
        }
        synchronized (mLock) {
            updateModeLocked(false);
        }
    }

    @Override
    public void onWindowsEvents(int events) {
        if ((events & WindowsHost.EVENT_SETTING_CHANGED) == 0) return;
        synchronized (mLock) {
            if (mNightMode == UiModeManager.MODE_NIGHT_AUTO) {
                updateModeLocked(false);
            }
        }
    }

    public int computeEffectiveNightMode() {
        switch (mNightMode) {
            case UiModeManager.MODE_NIGHT_YES:
                return UiModeManager.MODE_NIGHT_YES;
            case UiModeManager.MODE_NIGHT_NO:
                return UiModeManager.MODE_NIGHT_NO;
            case UiModeManager.MODE_NIGHT_AUTO:
                int hostTheme = WindowsHost.appsUseLightTheme();
                return (hostTheme == 0) ? UiModeManager.MODE_NIGHT_YES : UiModeManager.MODE_NIGHT_NO;
            case UiModeManager.MODE_NIGHT_CUSTOM:
                return mNightModeActivated ? UiModeManager.MODE_NIGHT_YES : UiModeManager.MODE_NIGHT_NO;
            default:
                return UiModeManager.MODE_NIGHT_NO;
        }
    }

    public boolean isNightModeActivated() {
        synchronized (mLock) {
            return computeEffectiveNightMode() == UiModeManager.MODE_NIGHT_YES;
        }
    }

    public int getNightMode() {
        return getNightMode(0);
    }

    @Override
    public int getNightMode(int displayId) {
        synchronized (mLock) {
            return mNightMode;
        }
    }

    @Override
    public void setNightMode(int mode) {
        if (mode != UiModeManager.MODE_NIGHT_AUTO
                && mode != UiModeManager.MODE_NIGHT_NO
                && mode != UiModeManager.MODE_NIGHT_YES
                && mode != UiModeManager.MODE_NIGHT_CUSTOM) {
            return;
        }
        synchronized (mLock) {
            int oldMode = mNightMode;
            mNightMode = mode;
            mNightModeActivated = (mode == UiModeManager.MODE_NIGHT_YES);
            updateModeLocked(oldMode != mode);
        }
    }

    @Override
    public boolean setNightModeActivated(boolean active) {
        synchronized (mLock) {
            mNightModeActivated = active;
            if (mNightMode == UiModeManager.MODE_NIGHT_NO && active) {
                mNightMode = UiModeManager.MODE_NIGHT_YES;
            } else if (mNightMode == UiModeManager.MODE_NIGHT_YES && !active) {
                mNightMode = UiModeManager.MODE_NIGHT_NO;
            }
            updateModeLocked(true);
            return true;
        }
    }

    @Override
    public void setApplicationNightMode(int mode) {
        setNightMode(mode);
    }

    @Override
    public boolean setNightModeActivatedForCustomMode(int modeCustomType, boolean active) {
        return setNightModeActivated(active);
    }

    private void updateModeLocked(boolean modeChanged) {
        int effective = computeEffectiveNightMode();
        int nightMask = (effective == UiModeManager.MODE_NIGHT_YES)
                ? Configuration.UI_MODE_NIGHT_YES
                : Configuration.UI_MODE_NIGHT_NO;
        boolean configChanged = (sUiModeNight != nightMask);
        sUiModeNight = nightMask;
        mConfiguration.uiMode = (mConfiguration.uiMode & ~Configuration.UI_MODE_NIGHT_MASK) | nightMask;

        if (modeChanged || configChanged) {
            notifyCallbacksLocked(effective);
        }

        if (configChanged) {
            if (mAtm != null) {
                mAtm.dispatchConfiguration(null);
            }
            if (mAm != null) {
                try {
                    Intent intent = new Intent(Intent.ACTION_CONFIGURATION_CHANGED);
                    intent.addFlags(Intent.FLAG_RECEIVER_REGISTERED_ONLY | Intent.FLAG_RECEIVER_FOREGROUND);
                    mAm.sendBroadcastFor("android", intent, null);
                } catch (Throwable ignored) {
                }
            }
        }
    }

    private void notifyCallbacksLocked(int effectiveNightMode) {
        List<CallbackRecord> records = new ArrayList<>(mCallbacks.values());
        for (CallbackRecord r : records) {
            try {
                r.callback.notifyForceInvertStateChanged(effectiveNightMode);
                r.callback.notifyForceInvertOverrideStateChanged();
            } catch (RemoteException e) {
                mCallbacks.remove(r.callback.asBinder());
            } catch (Throwable ignored) {
            }
        }
    }

    @Override
    public void addCallback(IUiModeManagerCallback callback, int userId) {
        if (callback == null) return;
        IBinder binder = callback.asBinder();
        if (binder == null) return;
        synchronized (mLock) {
            if (mCallbacks.containsKey(binder)) return;
            CallbackRecord record = new CallbackRecord(callback, userId);
            try {
                binder.linkToDeath(record, 0);
            } catch (Throwable ignored) {
                // Local binder in tests might not support linkToDeath
            }
            mCallbacks.put(binder, record);
        }
    }

    @Override
    public void removeCallback(IUiModeManagerCallback callback, int userId) {
        if (callback == null) return;
        IBinder binder = callback.asBinder();
        if (binder == null) return;
        synchronized (mLock) {
            CallbackRecord record = mCallbacks.remove(binder);
            if (record != null) {
                try {
                    binder.unlinkToDeath(record, 0);
                } catch (Throwable ignored) {
                }
            }
        }
    }

    @Override
    public int getCurrentModeType(int displayId) {
        return Configuration.UI_MODE_TYPE_NORMAL;
    }

    @Override
    public float getContrast(int userId) {
        return 0f;
    }

    @Override
    public int getForceInvertState(int userId) {
        return isNightModeActivated() ? UiModeManager.FORCE_INVERT_TYPE_DARK : UiModeManager.FORCE_INVERT_TYPE_OFF;
    }

    @Override
    public boolean isUiModeLocked() {
        return false;
    }

    @Override
    public boolean isNightModeLocked() {
        return false;
    }

    public Configuration getConfiguration() {
        synchronized (mLock) {
            return new Configuration(mConfiguration);
        }
    }

    public static void resetForTests() {
        sUiModeNight = Configuration.UI_MODE_NIGHT_NO;
        if (sInstance != null) {
            synchronized (sInstance.mLock) {
                sInstance.mNightMode = UiModeManager.MODE_NIGHT_NO;
                sInstance.mNightModeActivated = false;
                sInstance.mCallbacks.clear();
                sInstance.updateModeLocked(false);
            }
        }
    }
}
