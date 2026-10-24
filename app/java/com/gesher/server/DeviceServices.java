/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.server;

import android.app.AppOpsManager;
import android.app.ISearchManager;
import android.app.SyncNotedAppOp;
import android.content.ComponentName;
import android.content.pm.ParceledListSlice;
import android.os.Bundle;
import android.os.IBinder;
import android.os.IThermalService;

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

    /** {@code thermalservice}, which PowerManager needs too: no thermal status (Default's 0, none). */
    static final class ThermalService extends IThermalService.Default {
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
     * {@code search}: no searchable activities and no global search (Activity's
     * getSystemService(SEARCH_SERVICE) throws without the service; NoNonsense Notes).
     */
    static final class SearchService extends ISearchManager.Default {
        @Override
        public java.util.List<android.app.SearchableInfo> getSearchablesInGlobalSearch() {
            return new java.util.ArrayList<>();
        }

        @Override
        public java.util.List<android.content.pm.ResolveInfo> getGlobalSearchActivities() {
            return new java.util.ArrayList<>();
        }
    }
}
