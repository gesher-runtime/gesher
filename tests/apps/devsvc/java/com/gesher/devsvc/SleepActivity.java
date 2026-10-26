/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.devsvc;

import android.app.Activity;
import android.app.AlarmManager;
import android.app.PendingIntent;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.os.PowerManager;

/**
 * Windows' sleep and resume (hwsvc2, ADR 0051): a registered receiver of SCREEN_OFF and
 * SCREEN_ON, and an RTC (not _WAKEUP) alarm 4 s ahead to AlarmReceiver; the test sends the
 * device's event window PBT_APMSUSPEND, waits past the alarm's time, then sends
 * PBT_APMRESUMEAUTOMATIC: the alarm fires only after the resume. Finishes after SCREEN_ON
 * (or 90 s).
 */
public class SleepActivity extends Activity {
    static final String ACTION_SLEEP_ALARM = "com.gesher.devsvc.SLEEP_ALARM";
    private boolean mDone;

    private final BroadcastReceiver mReceiver = new BroadcastReceiver() {
        @Override
        public void onReceive(Context context, Intent intent) {
            PowerManager pm = getSystemService(PowerManager.class);
            Report.line("sleep: got " + intent.getAction() + ", interactive "
                    + pm.isInteractive());
            if (Intent.ACTION_SCREEN_ON.equals(intent.getAction()) && !mDone) {
                mDone = true;
                new Handler(Looper.getMainLooper()).postDelayed(SleepActivity.this::finish, 4000);
            }
        }
    };

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        IntentFilter filter = new IntentFilter(Intent.ACTION_SCREEN_OFF);
        filter.addAction(Intent.ACTION_SCREEN_ON);
        registerReceiver(mReceiver, filter, Context.RECEIVER_EXPORTED);
        AlarmManager am = getSystemService(AlarmManager.class);
        am.setExact(AlarmManager.RTC, System.currentTimeMillis() + 4000,
                PendingIntent.getBroadcast(this, 3, new Intent(ACTION_SLEEP_ALARM)
                        .setClass(this, AlarmReceiver.class), PendingIntent.FLAG_IMMUTABLE));
        Report.line("sleep: ready, interactive "
                + getSystemService(PowerManager.class).isInteractive() + Report.pid());
        new Handler(Looper.getMainLooper()).postDelayed(() -> {
            if (!mDone) {
                mDone = true;
                Report.line("sleep: no SCREEN_ON came");
                finish();
            }
        }, 90_000);
    }

    @Override
    protected void onDestroy() {
        unregisterReceiver(mReceiver);
        super.onDestroy();
    }
}
