/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.server;

import android.app.ApplicationThreadConstants;
import android.app.ContentProviderHolder;
import android.app.IActivityManager;
import android.app.IApplicationThread;
import android.content.pm.ApplicationInfo;
import android.content.pm.FeatureInfo;
import android.content.pm.PackageManager;
import android.content.pm.ProviderInfo;
import android.content.pm.ProviderInfoList;
import android.content.pm.SystemFeaturesCache;
import android.content.pm.UserInfo;
import android.content.res.CompatibilityInfo;
import android.os.Binder;
import android.os.Bundle;
import android.os.SystemClock;
import android.system.Os;
import android.util.ArrayMap;

import com.android.internal.os.ApplicationSharedMemory;
import com.gesher.app.DeviceProfile;

import java.io.FileDescriptor;
import java.io.IOException;
import java.util.ArrayList;

/**
 * {@code activity}: ActivityManagerService, lite. Answers what an app's lifecycle asks of
 * it, and attaches app processes ({@link #attachApplication}): the process is handed to
 * {@link ActivityTaskManagerService}, which binds it to a package and starts its
 * activities.
 */
public final class ActivityManagerService extends IActivityManager.Default {
    private final ActivityTaskManagerService mAtm;
    private ApplicationSharedMemory mSharedMemory;  // created on the first bind

    ActivityManagerService(ActivityTaskManagerService atm) {
        mAtm = atm;
        atm.setActivityManager(this);
    }

    @Override
    public ContentProviderHolder getContentProvider(android.app.IApplicationThread caller,
            String callingPackage, String name, int userId, boolean stable) {
        return null;  // No providers yet: Settings and friends fall back to defaults.
    }

    @Override
    public int checkPermission(String permission, int pid, int uid) {
        return PackageManager.PERMISSION_GRANTED;
    }

    @Override
    public int checkPermissionForDevice(String permission, int pid, int uid, int deviceId) {
        return PackageManager.PERMISSION_GRANTED;
    }

    @Override
    public UserInfo getCurrentUser() {
        return Services.UserManager.owner();
    }

    @Override
    public void attachApplication(IApplicationThread app, long startSeq) {
        mAtm.attachApplication(app, Binder.getCallingPid());
    }

    @Override
    public void finishAttachApplication(long startSeq, long timestampApplicationOnCreateNs) {
        mAtm.finishAttachApplication(Binder.getCallingPid());
    }

    /**
     * IApplicationThread.bindApplication, with what ActivityManagerService's
     * attachApplicationLocked sends: the configuration, no providers, no instrumentation,
     * no services map (the app asks servicemanager), no font map (Typeface loads the
     * preinstalled one) and the shared memory region with the system features.
     */
    void bindApplication(IApplicationThread thread, ApplicationInfo ai) throws Exception {
        FileDescriptor sharedMemory = sharedMemory().getReadOnlyFileDescriptor();
        Bundle coreSettings = new Bundle();
        // CoreSettingsObserver's bundle: the default device's settings, by device id.
        coreSettings.putBundle("0", new Bundle());
        thread.bindApplication(ai.processName != null ? ai.processName : ai.packageName, ai,
                null /* sdkSandboxClientAppVolumeUuid */, null /* sdkSandboxClientAppPackage */,
                false /* isSdkInSandbox */,
                ProviderInfoList.fromList(new ArrayList<ProviderInfo>()),
                null /* instrumentationName */, null /* profilerInfo */,
                null /* instrumentationArgs */, null /* instrumentationWatcher */,
                null /* instrumentationUiConnection */, ApplicationThreadConstants.DEBUG_OFF,
                false /* enableBinderTracking */, false /* trackAllocation */,
                false /* isRestrictedBackupMode */, false /* persistent */,
                DeviceProfile.configuration(), CompatibilityInfo.DEFAULT_COMPATIBILITY_INFO,
                null /* services */, coreSettings, "unknown" /* buildSerial */,
                null /* autofillOptions */, null /* contentCaptureOptions */,
                new long[0] /* disabledCompatChanges */, new long[0] /* loggableCompatChanges */,
                false /* logChangeChecksToStatsD */, null /* serializedSystemFontMap */,
                sharedMemory, SystemClock.elapsedRealtime(), SystemClock.uptimeMillis());
        if (!(thread.asBinder() instanceof Binder)) {
            Os.close(sharedMemory);  // the Parcel sent a duplicate
        }
    }

    /**
     * The ApplicationSharedMemory every app maps read-only, as SystemServer creates it:
     * here with no system features (the headless device declares none).
     */
    private synchronized ApplicationSharedMemory sharedMemory() throws IOException {
        if (mSharedMemory == null) {
            ApplicationSharedMemory m = ApplicationSharedMemory.create();
            // SystemServer writes PackageManagerService's features; the headless device
            // declares none.
            m.writeSystemFeaturesCache(new SystemFeaturesCache(
                    new ArrayMap<String, FeatureInfo>()).getSdkFeatureVersions());
            // WindowManagerService keeps the animator duration scale there: 1.
            m.setCurrentAnimatorScale(1f);
            mSharedMemory = m;
        }
        return mSharedMemory;
    }
}
