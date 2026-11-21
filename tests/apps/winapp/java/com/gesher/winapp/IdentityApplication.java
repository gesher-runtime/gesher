/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.winapp;

import android.app.ActivityManager;
import android.app.Application;
import android.content.Context;
import android.os.Binder;
import android.os.Process;

/** Ordinary APK startup assertions: no Binder identity or server process record is injected. */
public final class IdentityApplication extends Application {
    @Override protected void attachBaseContext(Context context) {
        super.attachBaseContext(context);
        int uid = Process.myUid();
        try {
            int installed = getPackageManager().getApplicationInfo(getPackageName(), 0).uid;
            if (uid != getApplicationInfo().uid || uid != installed
                    || uid != android.system.Os.getuid() || uid != android.system.Os.geteuid()
                    || uid != Binder.getCallingUid() || Process.myPid() != Binder.getCallingPid()) {
                throw new AssertionError("Application context, PM, native and Binder identities differ");
            }
            boolean registered = false;
            for (ActivityManager.RunningAppProcessInfo app :
                    getSystemService(ActivityManager.class).getRunningAppProcesses()) {
                if (app.pid == Process.myPid() && app.uid == uid
                        && java.util.Arrays.asList(app.pkgList).contains(getPackageName())) registered = true;
            }
            if (!registered) throw new AssertionError("Actual application process is not registered");
            Reporting.say("winapp identity uid " + uid + " pid " + Process.myPid()
                    + " canonical native Binder PM registered before Application.onCreate");
        } catch (android.content.pm.PackageManager.NameNotFoundException missing) {
            throw new AssertionError(missing);
        }
    }

    @Override public void onCreate() {
        super.onCreate();
        try {
            android.content.IClipboard clipboard = android.content.IClipboard.Stub.asInterface(
                    android.os.ServiceManager.getService(Context.CLIPBOARD_SERVICE));
            try {
                clipboard.hasPrimaryClip("android", null, 0, 0);
                throw new AssertionError("A foreign clipboard package claim was accepted");
            } catch (SecurityException expected) {
                Reporting.say("winapp identity foreign clipboard package rejected");
            }
        } catch (android.os.RemoteException failed) {
            throw new AssertionError(failed);
        }
    }
}
