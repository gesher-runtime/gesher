/* Copyright (C) 2026 The Gesher Authors. SPDX-License-Identifier: Apache-2.0 */
package com.gesher.windowpromotion.target;

import android.app.Service;
import android.content.Intent;
import android.os.Handler;
import android.os.IBinder;
import android.os.Process;

/** Keeps the original default-headless app process alive across the foreground launch. */
public final class KeepAliveService extends Service {
    static volatile KeepAliveService instance;
    private final Handler handler = new Handler();
    private boolean backgroundRequested;
    private final Runnable heartbeat = new Runnable() {
        public void run() {
            if (instance == KeepAliveService.this) {
                System.out.println("promotion: heartbeat pid=" + Process.myPid());
                handler.postDelayed(this, 1000);
            }
        }
    };
    @Override public void onCreate() {
        super.onCreate();
        instance = this;
        System.out.println("promotion: service-created pid=" + Process.myPid());
        handler.post(heartbeat);
    }
    @Override public int onStartCommand(Intent intent, int flags, int id) { return START_NOT_STICKY; }
    @Override public IBinder onBind(Intent intent) { return null; }
    void beginUnapprovedPhase() {
        if (backgroundRequested || TargetActivity.primary == null) throw new AssertionError("Invalid phase transition");
        backgroundRequested = true;
        TargetActivity.primary.finish();
        handler.postDelayed(new Runnable() {
            int attempts;
            public void run() {
                if (TargetActivity.primary != null) {
                    if (++attempts > 100) throw new AssertionError("Foreground Activity did not die");
                    handler.postDelayed(this, 50);
                    return;
                }
                // No Activity or foreground caller remains when this service initiates the launch.
                handler.postDelayed(() -> {
                    System.out.println("promotion: service-background-launch pid=" + Process.myPid());
                    getApplicationContext().startActivity(new Intent(KeepAliveService.this, BackgroundActivity.class)
                            .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK));
                }, 500);
            }
        }, 50);
    }
    @Override public void onDestroy() {
        instance = null;
        handler.removeCallbacks(heartbeat);
        System.out.println("promotion: service-destroyed pid=" + Process.myPid());
        super.onDestroy();
    }
}
