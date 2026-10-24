/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.server;

import android.app.AlarmManager;
import android.app.IAlarmListener;
import android.app.IAlarmManager;
import android.app.PendingIntent;
import android.content.ClipData;
import android.content.ClipDescription;
import android.content.IClipboard;
import android.content.IOnPrimaryClipChangedListener;
import android.os.IPowerManager;
import android.os.IThermalService;
import android.os.RemoteCallbackList;
import android.os.RemoteException;
import android.os.WorkSource;

/**
 * System services real apps look up as soon as they start (docs/e2e.md), which the
 * framework's {@code SystemServiceRegistry} fetches with {@code getServiceOrThrow}: without
 * one, {@code getSystemService} answers null and the app dies on it. Each answers like a
 * device that has the service but nothing behind it; what they do not override answers
 * {@code Default}'s (see {@link StandIn}).
 */
final class DeviceServices {
    private DeviceServices() {
    }

    /**
     * {@code power}: the screen is on (interactive), no battery saver, no device idle; wake
     * locks are accepted and change nothing (a PC does not sleep under a running app).
     */
    static final class PowerService extends IPowerManager.Default {
        @Override
        public boolean isInteractive() {
            return true;
        }

        @Override
        public boolean isDisplayInteractive(int displayId) {
            return true;
        }

        @Override
        public boolean isPowerSaveMode() {
            return false;
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

    /** {@code thermalservice}, which PowerManager needs too: no thermal status (Default's 0, none). */
    static final class ThermalService extends IThermalService.Default {
    }

    /**
     * {@code alarm}: alarms are accepted and never go off (a known gap, docs/e2e.md); exact
     * alarms are allowed, as for an app holding SCHEDULE_EXACT_ALARM.
     */
    static final class AlarmService extends IAlarmManager.Default {
        @Override
        public void set(String callingPackage, int type, long triggerAtTime, long windowLength,
                long interval, int flags, PendingIntent operation, IAlarmListener listener,
                String listenerTag, WorkSource workSource, AlarmManager.AlarmClockInfo alarmClock) {
        }

        @Override
        public boolean canScheduleExactAlarms(String packageName) {
            return true;
        }

        @Override
        public boolean hasScheduleExactAlarm(String packageName, int userId) {
            return true;
        }
    }

    /**
     * {@code clipboard}: one primary clip for the device, shared by its apps, with
     * change listeners (not yet Windows' clipboard).
     */
    static final class ClipboardService extends IClipboard.Default {
        private final RemoteCallbackList<IOnPrimaryClipChangedListener> mListeners =
                new RemoteCallbackList<>();
        private ClipData mClip;

        @Override
        public void setPrimaryClip(ClipData clip, String callingPackage, String attributionTag,
                int userId, int deviceId) {
            synchronized (this) {
                mClip = clip;
            }
            int n = mListeners.beginBroadcast();
            try {
                for (int i = 0; i < n; i++) {
                    try {
                        mListeners.getBroadcastItem(i).dispatchPrimaryClipChanged();
                    } catch (RemoteException e) {
                        // gone
                    }
                }
            } finally {
                mListeners.finishBroadcast();
            }
        }

        @Override
        public void clearPrimaryClip(String callingPackage, String attributionTag, int userId,
                int deviceId) {
            setPrimaryClip(null, callingPackage, attributionTag, userId, deviceId);
        }

        @Override
        public synchronized ClipData getPrimaryClip(String pkg, String attributionTag, int userId,
                int deviceId) {
            return mClip;
        }

        @Override
        public synchronized ClipDescription getPrimaryClipDescription(String callingPackage,
                String attributionTag, int userId, int deviceId) {
            return mClip != null ? mClip.getDescription() : null;
        }

        @Override
        public synchronized boolean hasPrimaryClip(String callingPackage, String attributionTag,
                int userId, int deviceId) {
            return mClip != null;
        }

        @Override
        public synchronized boolean hasClipboardText(String callingPackage, String attributionTag,
                int userId, int deviceId) {
            if (mClip == null || mClip.getItemCount() == 0) return false;
            CharSequence text = mClip.getItemAt(0).getText();
            return text != null && text.length() > 0;
        }

        @Override
        public void addPrimaryClipChangedListener(IOnPrimaryClipChangedListener listener,
                String callingPackage, String attributionTag, int userId, int deviceId) {
            mListeners.register(listener);
        }

        @Override
        public void removePrimaryClipChangedListener(IOnPrimaryClipChangedListener listener,
                String callingPackage, String attributionTag, int userId, int deviceId) {
            mListeners.unregister(listener);
        }
    }
}
