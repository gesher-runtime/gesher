/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.server;

import android.app.AppOpsManager;
import android.app.ISearchManager;
import android.app.SyncNotedAppOp;
import android.app.admin.IDevicePolicyManager;
import android.app.trust.ITrustManager;
import android.app.usage.IUsageStatsManager;
import android.content.ComponentName;
import android.content.pm.IShortcutService;
import android.content.pm.ParceledListSlice;
import android.os.Build;
import android.os.Bundle;
import android.os.IBinder;
import android.os.IDeviceIdentifiersPolicyService;
import android.os.IThermalService;
import android.telephony.SubscriptionManager;
import android.view.ISensitiveContentProtectionManager;

import com.android.internal.app.IAppOpsService;
import com.android.internal.appwidget.IAppWidgetHost;
import com.android.internal.appwidget.IAppWidgetService;
import com.android.internal.os.IDropBoxManagerService;
import com.android.internal.telephony.IPhoneSubInfo;
import com.android.internal.telephony.ISub;
import com.android.internal.telephony.ITelephony;
import com.android.internal.telephony.ITelephonyRegistry;

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

    /** No Android thermal HAL: no sensors, forecasts, or thermal listener capability. */
    static final class ThermalService extends IThermalService.Default {
        @Override public android.os.Temperature[] getCurrentTemperatures() {
            return new android.os.Temperature[0];
        }
        @Override public android.os.Temperature[] getCurrentTemperaturesWithType(int type) {
            return getCurrentTemperatures();
        }
        @Override public android.os.CoolingDevice[] getCurrentCoolingDevices() {
            return new android.os.CoolingDevice[0];
        }
        @Override public android.os.CoolingDevice[] getCurrentCoolingDevicesWithType(int type) {
            return getCurrentCoolingDevices();
        }
        @Override public int getCurrentThermalStatus() {
            return android.os.Temperature.THROTTLING_NONE;
        }
        @Override public float getThermalHeadroom(int forecastSeconds) { return Float.NaN; }
        @Override public float[] getThermalHeadroomThresholds() {
            // PowerManager indexes every thermal status, including SHUTDOWN.
            float[] thresholds = new float[android.os.Temperature.THROTTLING_SHUTDOWN + 1];
            java.util.Arrays.fill(thresholds, Float.NaN);
            return thresholds;
        }
        @Override public boolean registerThermalEventListener(android.os.IThermalEventListener l) {
            java.util.Objects.requireNonNull(l, "listener");
            return false;
        }
        @Override public boolean registerThermalEventListenerWithType(
                android.os.IThermalEventListener l, int type) {
            return registerThermalEventListener(l);
        }
        @Override public boolean unregisterThermalEventListener(android.os.IThermalEventListener l) {
            java.util.Objects.requireNonNull(l, "listener");
            return false;
        }
        @Override public boolean registerThermalStatusListener(android.os.IThermalStatusListener l) {
            java.util.Objects.requireNonNull(l, "listener");
            return false;
        }
        @Override public boolean unregisterThermalStatusListener(android.os.IThermalStatusListener l) {
            java.util.Objects.requireNonNull(l, "listener");
            return false;
        }
        @Override public boolean registerThermalHeadroomListener(android.os.IThermalHeadroomListener l) {
            java.util.Objects.requireNonNull(l, "listener");
            return false;
        }
        @Override public boolean unregisterThermalHeadroomListener(android.os.IThermalHeadroomListener l) {
            java.util.Objects.requireNonNull(l, "listener");
            return false;
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

    /**
     * {@code dropbox}: DropBoxManager service stand-in. Returns null for getNextEntry
     * so that crash reporters (ACRA DropBoxCollector) query without error or data leakage.
     */
    static final class DropBoxManagerService extends IDropBoxManagerService.Default {
    }

    /**
     * {@code usagestats}: no collected usage history or Android app standby.
     */
    static final class UsageStatsService extends DesktopUsageStatsService {
        UsageStatsService(PackageManagerService packages) { super(packages); }
    }

    /**
     * {@code device_policy}: no Android administrators; Android encryption is unsupported.
     */
    static final class DevicePolicyService extends UnmanagedDevicePolicyService {
    }

    /**
     * {@code phone}: Telephony service stand-in for ITelephony. Reports no phone / no SIM
     * and returns safe fallbacks for device ID / network info.
     */
    static final class TelephonyService extends ITelephony.Default {
    }

    /**
     * {@code iphonesubinfo}: Phone subscriber info service stand-in. Returns null for
     * device ID, subscriber ID, and phone number queries.
     */
    static final class PhoneSubInfoService extends IPhoneSubInfo.Default {
    }

    /**
     * {@code device_identifiers}: Build.getSerial()'s service. The serial is
     * {@link Build#UNKNOWN}, as an app without the privileged phone permission sees it
     * through device_info_plus (Flutter's RustDesk); without the service getSerial threw
     * NullPointerException and the app's initialization failed.
     */
    static final class DeviceIdentifiersPolicyService extends IDeviceIdentifiersPolicyService.Default {
        @Override
        public String getSerial() {
            return Build.UNKNOWN;
        }

        @Override
        public String getSerialForPackage(String callingPackage, String callingFeatureId) {
            return Build.UNKNOWN;
        }
    }

    /**
     * {@code isub}: SubscriptionManager service stand-in. Reports no active subscriptions
     * and invalid subscription IDs so apps treat the device as having no cellular SIMs.
     */
    static final class SubscriptionService extends ISub.Default {
        // Generated Default answers 0, which is a valid subscription identifier.
        // No SIMs are installed here: expose the same absent identifiers as AOSP's
        // empty subscription database, including the user-specific entry points.
        @Override public int getDefaultSubId() {
            return SubscriptionManager.INVALID_SUBSCRIPTION_ID;
        }
        @Override public int getDefaultSubIdAsUser(int userId) { return getDefaultSubId(); }
        @Override public int getDefaultDataSubId() { return getDefaultSubId(); }
        @Override public int getDefaultVoiceSubId() { return getDefaultSubId(); }
        @Override public int getDefaultVoiceSubIdAsUser(int userId) { return getDefaultSubId(); }
        @Override public int getDefaultSmsSubId() { return getDefaultSubId(); }
        @Override public int getDefaultSmsSubIdAsUser(int userId) { return getDefaultSubId(); }
        @Override public int getActiveDataSubscriptionId() { return getDefaultSubId(); }
        @Override public int getPreferredDataSubscriptionId() {
            // AOSP falls back to the default data subscription without PhoneSwitcher.
            return getDefaultDataSubId();
        }
        @Override public int getSubId(int slotIndex) { return getDefaultSubId(); }
        @Override public int getSlotIndex(int subId) {
            return SubscriptionManager.INVALID_SIM_SLOT_INDEX;
        }
        @Override public int getPhoneId(int subId) {
            if (subId == SubscriptionManager.DEFAULT_SUBSCRIPTION_ID) subId = getDefaultSubId();
            // AOSP retains the default-phone sentinel for a valid but unmapped ID;
            // it is neither an installed modem index nor an active subscription.
            return subId > SubscriptionManager.INVALID_SUBSCRIPTION_ID
                    ? SubscriptionManager.DEFAULT_PHONE_INDEX : SubscriptionManager.INVALID_PHONE_INDEX;
        }
        @Override public int getEnabledSubscriptionId(int slotIndex) {
            if (!SubscriptionManager.isValidSlotIndex(slotIndex)) {
                throw new IllegalArgumentException("Invalid slot index " + slotIndex);
            }
            return getDefaultSubId();
        }
        @Override public int[] getActiveSubIdList(boolean visibleOnly) { return new int[0]; }
    }

    /**
     * {@code telephony.registry}: TelephonyRegistry service stand-in for event callbacks.
     */
    static final class TelephonyRegistryService extends ITelephonyRegistry.Default {
    }

    /**
     * {@code trust}: TrustManager service stand-in for ITrustManager. Reports device as
     * unlocked and not secure so KeyguardManager initializes and isDeviceSecure() returns false.
     */
    static final class TrustService extends ITrustManager.Default {
        @Override
        public boolean isDeviceLocked(int userId, int deviceId) {
            return false;
        }

        @Override
        public boolean isDeviceSecure(int userId, int deviceId) {
            return false;
        }
    }

    /**
     * {@code sensitive_content_protection_service}: SensitiveContentProtectionManager stand-in.
     */
    static final class SensitiveContentProtectionService
            extends ISensitiveContentProtectionManager.Default {
    }

    /**
     * {@code domain_verification}: DomainVerificationManager stand-in. Answers like a device
     * with no domain verification records, preventing NullPointerException on app startup.
     */
    static final class DomainVerificationService
            extends android.content.pm.verify.domain.IDomainVerificationManager.Default {
    }

    /**
     * {@code shortcut}: package-owned dynamic shortcuts with Windows shell integration;
     * Android pin/share-target hosts remain unavailable.
     */
    static final class ShortcutService extends DesktopShortcutService {
        ShortcutService(PackageManagerService packages) { super(packages); }
        ShortcutService(PackageManagerService packages, CallerIdentity callers) { super(packages, callers); }
    }
}
