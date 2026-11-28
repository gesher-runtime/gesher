/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package com.gesher.microgprobe;

import android.app.Activity;
import android.content.pm.IPackageInstaller;
import android.content.pm.IPackageManager;
import android.content.pm.PackageInstaller;
import android.os.Binder;
import android.os.Bundle;
import android.os.IBinder;
import android.os.Process;
import android.os.ServiceManager;
import android.util.Log;

/** Real installed ordinary app: installer ownership denials over Binder. No commit. */
public final class InstallerDenialActivity extends Activity {
    private interface Operation { void run() throws Exception; }
    private static void require(boolean value, String reason) {
        if (!value) throw new AssertionError(reason);
    }
    private static void denied(Operation operation, String expected) throws Exception {
        try { operation.run(); }
        catch (SecurityException refusal) {
            require(refusal.getMessage() != null && refusal.getMessage().contains(expected),
                    "Wrong service guard: " + refusal);
            return;
        }
        throw new AssertionError("Unauthorized operation accepted: " + expected);
    }
    @Override public void onCreate(Bundle state) {
        super.onCreate(state);
        try {
            int uid = Process.myUid();
            require(Process.isApplicationUid(uid) && getApplicationInfo().uid == uid,
                    "Fixture did not run under its actual installed application UID");
            IBinder binder = ServiceManager.getService("package");
            require(binder != null && !(binder instanceof Binder), "Package service is not remote");
            IPackageManager pm = IPackageManager.Stub.asInterface(binder);
            require(pm.getPackageUid(getPackageName(), 0, 0) == uid,
                    "Remote package manager disagrees with enrolled application UID");
            IPackageInstaller installer = pm.getPackageInstaller();
            require(installer != null && !(installer.asBinder() instanceof Binder), "Installer is not remote");
            PackageInstaller.SessionParams params = new PackageInstaller.SessionParams(
                    PackageInstaller.SessionParams.MODE_FULL_INSTALL);
            params.sizeBytes = 100;
            for (String claim : new String[]{"android", "com.android.vending", "com.gesher.fixture"}) {
                denied(() -> installer.createSession(params, claim, null, 0), "Unknown or foreign installer");
            }
            // A legitimate owner can create and abandon an empty session. No APK is written or committed.
            int session = installer.createSession(params, getPackageName(), null, 0);
            installer.abandonSession(session);
            Log.i("installer-denial", "PackageInstaller enrolled app ownership denials passed: uid="
                    + uid + " pid=" + Process.myPid() + " package=" + getPackageName());
        } catch (Throwable failure) {
            Log.e("installer-denial", "Enrolled installer service authorization failed", failure);
        } finally { finish(); }
    }
}
