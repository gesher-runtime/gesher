/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.server;

import android.app.AppOpsManager;
import android.app.trust.ITrustManager;
import android.content.pm.PackageManager;
import android.content.res.Resources;
import android.os.Binder;
import android.os.RemoteException;

import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;

/**
 * {@link AuthService.Host} in the system server (ADR 0091): permissions and app ops from
 * the package and activity managers, the foreground from Windows' foreground process (as
 * the clipboard's), the device credential from {@code trust} (as AOSP's PreAuthInfo), and
 * BiometricService's handler as one daemon thread.
 */
final class AuthServiceHost implements AuthService.Host {
    private final ActivityManagerService mAm;
    private final ActivityTaskManagerService mAtm;
    private final PackageManagerService mPm;
    private final ITrustManager mTrust;
    private final ExecutorService mThread = Executors.newSingleThreadExecutor(r -> {
        Thread thread = new Thread(r, "auth");
        thread.setDaemon(true);
        return thread;
    });

    AuthServiceHost(ActivityManagerService am, ActivityTaskManagerService atm,
            PackageManagerService pm, ITrustManager trust) {
        mAm = am;
        mAtm = atm;
        mPm = pm;
        mTrust = trust;
    }

    @Override
    public boolean hasPermission(String permission, int pid, int uid) {
        return mAm.checkPermission(permission, pid, uid) == PackageManager.PERMISSION_GRANTED;
    }

    @Override
    public boolean noteUseBiometric(int uid, String packageName) {
        if (packageName == null) return false;
        // An internal system-server note for the caller's identity, as the clipboard's.
        long token = Binder.clearCallingIdentity();
        try {
            return mPm.appOps().noteOperation(AppOpsManager.OP_USE_BIOMETRIC, uid, packageName,
                    null, false, null, false).getOpMode() == AppOpsManager.MODE_ALLOWED;
        } catch (SecurityException e) {
            return false;  // the package is not the caller's
        } finally {
            Binder.restoreCallingIdentity(token);
        }
    }

    @Override
    public boolean isForeground(int uid, int pid) {
        // Unknown foreground (no devsvc natives) is not the caller's, as for the clipboard.
        return mAtm.ownsForegroundProcess(uid, WindowsHost.foregroundProcess());
    }

    @Override
    public boolean isDeviceSecure(int userId) {
        try {
            return mTrust.isDeviceSecure(userId, 0 /* default device */);
        } catch (RemoteException e) {
            return false;
        }
    }

    @Override
    public CharSequence string(String name) {
        Resources res = Resources.getSystem();
        switch (name) {
            case "screen_lock_app_setting_name":
                return res.getString(com.android.internal.R.string.screen_lock_app_setting_name);
            case "screen_lock_dialog_default_subtitle":
                return res.getString(
                        com.android.internal.R.string.screen_lock_dialog_default_subtitle);
            default:
                throw new IllegalArgumentException("auth string " + name);
        }
    }

    @Override
    public void post(Runnable work) {
        mThread.execute(work);
    }
}
