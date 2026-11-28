/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.server;

import android.app.AppOpsManager;
import android.app.SyncNotedAppOp;
import android.app.admin.IDevicePolicyManager;
import android.app.trust.ITrustManager;
import android.content.ComponentName;
import android.content.pm.IShortcutService;
import android.content.pm.ParceledListSlice;
import android.content.pm.verify.domain.DomainOwner;
import android.content.pm.verify.domain.DomainSet;
import android.content.pm.verify.domain.DomainVerificationInfo;
import android.content.pm.verify.domain.DomainVerificationManager;
import android.content.pm.verify.domain.DomainVerificationUserState;
import android.os.Build;
import android.os.Bundle;
import android.os.IBinder;
import android.os.IDeviceIdentifiersPolicyService;
import android.os.ServiceSpecificException;
import android.os.IThermalService;
import android.view.ISensitiveContentProtectionManager;

import com.android.internal.app.IAppOpsService;
import com.android.internal.appwidget.IAppWidgetHost;
import com.android.internal.appwidget.IAppWidgetService;
import com.android.internal.os.IDropBoxManagerService;

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
     * No Android thermal HAL: no sensors, forecasts, headroom listeners or thermal events.
     * The thermal status is THROTTLING_NONE, and, as ThermalManagerService does whether or
     * not a HAL is present, a status listener registers (RemoteCallbackList: a repeated
     * registration replaces the earlier one) and is told the current status once
     * registered, on another thread; nothing changes it afterwards. PowerManager.addThermalStatusListener throws when registration
     * answers false (Chromium's PowerMonitor registers one at startup).
     */
    static final class ThermalService extends IThermalService.Default {
        private final android.os.RemoteCallbackList<android.os.IThermalStatusListener>
                mStatusListeners = new android.os.RemoteCallbackList<>();
        private final java.util.concurrent.Executor mStatusCallbacks =
                java.util.concurrent.Executors.newSingleThreadExecutor(work -> {
                    Thread thread = new Thread(work, "thermal-status-callbacks");
                    thread.setDaemon(true);
                    return thread;
                });
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
            if (!mStatusListeners.register(l)) return false;
            final int status = getCurrentThermalStatus();
            mStatusCallbacks.execute(() -> {
                try {
                    l.onStatusChange(status);
                } catch (android.os.RemoteException | RuntimeException e) {
                    // As ThermalManagerService: a failed callback is only logged.
                    System.err.println("thermal: status callback failed: " + e);
                }
            });
            return true;
        }
        @Override public boolean unregisterThermalStatusListener(android.os.IThermalStatusListener l) {
            java.util.Objects.requireNonNull(l, "listener");
            return mStatusListeners.unregister(l);
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
     * {@code dropbox}: DropBoxManager service stand-in. Returns null for getNextEntry
     * so that crash reporters (ACRA DropBoxCollector) query without error or data leakage.
     */
    static final class DropBoxManagerService extends IDropBoxManagerService.Default {
    }

    /**
     * {@code usagestats}: no collected usage history or Android app standby.
     */
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
     * {@code trust}: ITrustManager, what KeyguardManager.isDeviceSecure/isDeviceLocked and
     * BiometricService's PreAuthInfo ({@code credentialAvailable}) ask (ADR 0091).
     *
     * <p>The device is secure when the user has a credential Gesher can confirm
     * ({@link DeviceCredential}: Windows Hello, on Windows build 22000 or later). It is never
     * locked: an app runs in the signed-in user's unlocked Windows session. As
     * TrustManagerService, a virtual device is not secure and another user's state needs
     * INTERACT_ACROSS_USERS_FULL; Gesher has the one user.
     */
    static final class TrustService extends ITrustManager.Default {
        /** Whether pid/uid holds a permission. */
        interface Permissions {
            boolean has(String permission, int pid, int uid);
        }

        private final DeviceCredential mCredential;
        private final Permissions mPermissions;

        TrustService(DeviceCredential credential, Permissions permissions) {
            mCredential = credential;
            mPermissions = permissions;
        }

        @Override
        public boolean isDeviceLocked(int userId, int deviceId) {
            return false;
        }

        @Override
        public boolean isDeviceSecure(int userId, int deviceId) {
            if (deviceId != android.content.Context.DEVICE_ID_DEFAULT) return false;
            if (userId != android.os.UserHandle.getCallingUserId()
                    && !mPermissions.has("android.permission.INTERACT_ACROSS_USERS_FULL",
                            android.os.Binder.getCallingPid(), android.os.Binder.getCallingUid())) {
                throw new SecurityException("isDeviceSecure: user " + userId
                        + " requires android.permission.INTERACT_ACROSS_USERS_FULL");
            }
            return userId == android.os.UserHandle.USER_SYSTEM && mCredential.available();
        }
    }

    /**
     * {@code sensitive_content_protection_service}: SensitiveContentProtectionManager stand-in.
     */
    static final class SensitiveContentProtectionService
            extends ISensitiveContentProtectionManager.Default {
    }

    /**
     * No attached domain-verification records or verifier backend. Preserve the Android 16
     * empty-record contracts, including non-null containers and failed writes to absent IDs.
     */
    static final class DomainVerificationService
            extends android.content.pm.verify.domain.IDomainVerificationManager.Default {
        private final PackageManagerService mPackages;

        DomainVerificationService(PackageManagerService packages) {
            mPackages = java.util.Objects.requireNonNull(packages);
        }

        private static void requireUser(int userId) {
            if (userId != 0) throw new IllegalArgumentException("Only user 0 is supported");
        }

        private static ServiceSpecificException unavailable(String packageName) {
            return new ServiceSpecificException(
                    DomainVerificationManager.INTERNAL_ERROR_NAME_NOT_FOUND, packageName);
        }

        private void requirePackage(String packageName) {
            java.util.Objects.requireNonNull(packageName, "packageName");
            if (!mPackages.isPackageAvailable(packageName, 0)
                    || mPackages.isRemovalPending(packageName)) throw unavailable(packageName);
        }

        @Override public java.util.List<String> queryValidVerificationPackageNames() {
            return new java.util.ArrayList<>();
        }

        @Override public DomainVerificationInfo getDomainVerificationInfo(String packageName) {
            requirePackage(packageName);
            return null;
        }

        @Override public DomainVerificationUserState getDomainVerificationUserState(
                String packageName, int userId) {
            requireUser(userId);
            requirePackage(packageName);
            return null;
        }

        @Override public java.util.List<DomainOwner> getOwnersForDomain(String domain, int userId) {
            java.util.Objects.requireNonNull(domain, "domain");
            requireUser(userId);
            // Invalid host names also have no owners, as the public manager documents.
            return new java.util.ArrayList<>();
        }

        private static void validateDomainSet(String domainSetId, DomainSet domains) {
            // Mirror the AOSP Binder adapter before looking up the absent record. An
            // unknown ID wins over empty-domain validation in its service implementation.
            java.util.UUID.fromString(java.util.Objects.requireNonNull(domainSetId, "domainSetId"));
            java.util.Objects.requireNonNull(domains, "domains");
        }

        @Override public int setDomainVerificationStatus(String domainSetId, DomainSet domains,
                int state) {
            validateDomainSet(domainSetId, domains);
            if (state != DomainVerificationInfo.STATE_SUCCESS
                    && state < DomainVerificationInfo.STATE_FIRST_VERIFIER_DEFINED) {
                throw new IllegalArgumentException("Caller is not allowed to set state code " + state);
            }
            return DomainVerificationManager.ERROR_DOMAIN_SET_ID_INVALID;
        }

        @Override public int setDomainVerificationUserSelection(String domainSetId, DomainSet domains,
                boolean enabled, int userId) {
            validateDomainSet(domainSetId, domains);
            requireUser(userId);
            return DomainVerificationManager.ERROR_DOMAIN_SET_ID_INVALID;
        }

        @Override public void setDomainVerificationLinkHandlingAllowed(String packageName,
                boolean allowed, int userId) {
            java.util.Objects.requireNonNull(packageName, "packageName");
            requireUser(userId);
            // AOSP cannot update user preferences without an attached domain record.
            throw unavailable(packageName);
        }

        @Override public void setUriRelativeFilterGroups(String packageName, Bundle groups) {
            java.util.Objects.requireNonNull(packageName, "packageName");
            java.util.Objects.requireNonNull(groups, "groups");
            // Android treats an empty update as a no-op, even without a record.
            if (!groups.isEmpty()) throw unavailable(packageName);
        }

        @Override public Bundle getUriRelativeFilterGroups(String packageName,
                java.util.List<String> domains) {
            java.util.Objects.requireNonNull(packageName, "packageName");
            java.util.Objects.requireNonNull(domains, "domains");
            return new Bundle();
        }
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
