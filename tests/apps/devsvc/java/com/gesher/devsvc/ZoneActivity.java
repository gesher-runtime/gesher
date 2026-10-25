/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.devsvc;

import android.app.Activity;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;

import java.util.TimeZone;

/**
 * A real change of Windows' time zone (hwsvc2, ADR 0051): waits (up to 150 s) for
 * TIMEZONE_CHANGED in a registered receiver and reports its zone and the process's default
 * zone after it (ActivityThread.updateTimeZone reset it).
 */
public class ZoneActivity extends Activity {
    private boolean mDone;

    private final BroadcastReceiver mReceiver = new BroadcastReceiver() {
        @Override
        public void onReceive(Context context, Intent intent) {
            Report.line("zone: got " + intent.getAction() + " time-zone="
                    + intent.getStringExtra(Intent.EXTRA_TIMEZONE) + " default="
                    + TimeZone.getDefault().getID());
            if (!mDone) {
                mDone = true;
                new Handler(Looper.getMainLooper()).postDelayed(ZoneActivity.this::finish, 3000);
            }
        }
    };

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        registerReceiver(mReceiver, new IntentFilter(Intent.ACTION_TIMEZONE_CHANGED),
                Context.RECEIVER_EXPORTED);
        Report.line("zone: receiver registered, default=" + TimeZone.getDefault().getID()
                + Report.pid());
        new Handler(Looper.getMainLooper()).postDelayed(() -> {
            if (!mDone) {
                mDone = true;
                Report.line("zone: no TIMEZONE_CHANGED came");
                finish();
            }
        }, 150_000);
    }

    @Override
    protected void onDestroy() {
        unregisterReceiver(mReceiver);
        super.onDestroy();
    }
}
