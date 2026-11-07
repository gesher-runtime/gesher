/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package com.gesher.microgprobe;

import android.Manifest;
import android.app.Activity;
import android.app.NotificationManager;
import android.content.pm.PackageManager;
import android.os.Bundle;
import android.os.SystemClock;
import android.util.Log;

/** Parent host grants and revokes POST while this same app process remains alive. */
public final class PermissionCacheActivity extends Activity {
    @Override protected void onCreate(Bundle state) {
        super.onCreate(state);
        new Thread(()->{
            try {
                NotificationManager notifications=getSystemService(NotificationManager.class);
                int pid=android.os.Process.myPid();
                if (checkSelfPermission(Manifest.permission.POST_NOTIFICATIONS)!=PackageManager.PERMISSION_DENIED
                        || notifications.areNotificationsEnabled()) throw new AssertionError("Initial POST must be denied");
                Log.i("permission-cache-probe","READY_INITIAL_DENIED pid="+pid);
                int phase=0;
                long deadline=SystemClock.elapsedRealtime()+60000;
                while (SystemClock.elapsedRealtime()<deadline) {
                    boolean permission=checkSelfPermission(Manifest.permission.POST_NOTIFICATIONS)==PackageManager.PERMISSION_GRANTED;
                    boolean enabled=notifications.areNotificationsEnabled();
                    if (phase==0 && permission && enabled) {
                        phase=1; Log.i("permission-cache-probe","GRANT_OBSERVED pid="+pid);
                    } else if (phase==1 && !permission && !enabled) {
                        Log.i("permission-cache-probe","PASS deny-grant-deny in same process pid="+pid);
                        return;
                    }
                    SystemClock.sleep(100);
                }
                throw new AssertionError("Timed out waiting for host grant/revoke, phase="+phase);
            } catch (Throwable failure) {
                Log.e("permission-cache-probe","FAIL "+failure.getClass().getSimpleName()+": "+failure.getMessage());
            } finally { runOnUiThread(this::finish); }
        },"permission-cache-probe").start();
    }
}
