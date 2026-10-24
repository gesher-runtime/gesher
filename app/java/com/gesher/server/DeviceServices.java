/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.server;

import android.app.AlarmManager;
import android.app.AppOpsManager;
import android.app.IAlarmListener;
import android.app.IAlarmManager;
import android.app.PendingIntent;
import android.app.SyncNotedAppOp;
import android.content.ClipData;
import android.content.ComponentName;
import android.content.ClipDescription;
import android.content.IClipboard;
import android.content.IOnPrimaryClipChangedListener;
import android.content.pm.ParceledListSlice;
import android.os.Bundle;
import android.os.IBinder;
import android.os.IPowerManager;
import android.os.IThermalService;
import android.os.RemoteCallbackList;
import android.os.RemoteException;
import android.os.WorkSource;

import com.android.internal.app.IAppOpsService;
import com.android.internal.appwidget.IAppWidgetHost;
import com.android.internal.appwidget.IAppWidgetService;

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
     * {@code appops}: every operation is allowed (Gesher grants an app what it asks for,
     * as the activity manager's permission checks do); noted and started operations
     * answer MODE_ALLOWED. Environment.isExternalStorageManager asks it, among others.
     */
    static final class AppOpsService extends IAppOpsService.Default {
        @Override
        public int checkOperation(int code, int uid, String packageName) {
            return AppOpsManager.MODE_ALLOWED;
        }

        @Override
        public int checkOperationForDevice(int code, int uid, String packageName,
                String attributionTag, int virtualDeviceId) {
            return AppOpsManager.MODE_ALLOWED;
        }

        @Override
        public int checkOperationRaw(int code, int uid, String packageName,
                String attributionTag) {
            return AppOpsManager.MODE_ALLOWED;
        }

        @Override
        public int checkOperationRawForDevice(int code, int uid, String packageName,
                String attributionTag, int virtualDeviceId) {
            return AppOpsManager.MODE_ALLOWED;
        }

        @Override
        public int checkAudioOperation(int code, int usage, int uid, String packageName) {
            return AppOpsManager.MODE_ALLOWED;
        }

        @Override
        public int checkPackage(int uid, String packageName) {
            return AppOpsManager.MODE_ALLOWED;
        }

        @Override
        public int permissionToOpCode(String permission) {
            return AppOpsManager.OP_NONE;
        }

        @Override
        public SyncNotedAppOp noteOperation(int code, int uid, String packageName,
                String attributionTag, boolean shouldCollectAsyncNotedOp, String message,
                boolean shouldCollectMessage) {
            return new SyncNotedAppOp(AppOpsManager.MODE_ALLOWED, code, attributionTag,
                    packageName);
        }

        @Override
        public SyncNotedAppOp noteOperationForDevice(int code, int uid, String packageName,
                String attributionTag, int virtualDeviceId, boolean shouldCollectAsyncNotedOp,
                String message, boolean shouldCollectMessage) {
            return new SyncNotedAppOp(AppOpsManager.MODE_ALLOWED, code, attributionTag,
                    packageName);
        }

        @Override
        public SyncNotedAppOp startOperation(IBinder clientId, int code, int uid,
                String packageName, String attributionTag, boolean startIfModeDefault,
                boolean shouldCollectAsyncNotedOp, String message, boolean shouldCollectMessage,
                int attributionFlags, int attributionChainId) {
            return new SyncNotedAppOp(AppOpsManager.MODE_ALLOWED, code, attributionTag,
                    packageName);
        }

        @Override
        public SyncNotedAppOp startOperationForDevice(IBinder clientId, int code, int uid,
                String packageName, String attributionTag, int virtualDeviceId,
                boolean startIfModeDefault, boolean shouldCollectAsyncNotedOp, String message,
                boolean shouldCollectMessage, int attributionFlags, int attributionChainId) {
            return new SyncNotedAppOp(AppOpsManager.MODE_ALLOWED, code, attributionTag,
                    packageName);
        }
    }

    /**
     * {@code appwidget}: a launcher that hosts no widgets yet. Apps ask which of their
     * widgets exist (none) to update them; pinning is not supported.
     */
    static final class AppWidgetService extends IAppWidgetService.Default {
        @Override
        public int[] getAppWidgetIds(ComponentName providerComponent) {
            return new int[0];
        }

        @Override
        public int[] getAppWidgetIdsForHost(String callingPackage, int hostId) {
            return new int[0];
        }

        @Override
        public ParceledListSlice getInstalledProvidersForProfile(int categoryFilter,
                int profileId, String packageName) {
            return ParceledListSlice.emptyList();
        }

        @Override
        public ParceledListSlice startListening(IAppWidgetHost host, String callingPackage,
                int hostId, int[] appWidgetIds) {
            return ParceledListSlice.emptyList();
        }

        @Override
        public Bundle getAppWidgetOptions(String callingPackage, int appWidgetId) {
            return new Bundle();
        }

        @Override
        public int getMaxBitmapMemory() {
            return 6 * 1080 * 2400;
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
