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

/** Clock changes: waits (up to 60 s) for TIME_SET in a registered receiver. */
public class TimeActivity extends Activity {
    private boolean mDone;

    private final BroadcastReceiver mReceiver = new BroadcastReceiver() {
        @Override
        public void onReceive(Context context, Intent intent) {
            Report.line("time: registered receiver got " + intent.getAction());
            if (Intent.ACTION_TIME_CHANGED.equals(intent.getAction()) && !mDone) {
                mDone = true;
                new Handler(Looper.getMainLooper()).postDelayed(TimeActivity.this::finish, 3000);
            }
        }
    };

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        IntentFilter filter = new IntentFilter(Intent.ACTION_TIME_CHANGED);
        filter.addAction(Intent.ACTION_TIMEZONE_CHANGED);
        registerReceiver(mReceiver, filter, Context.RECEIVER_EXPORTED);
        Report.line("time: receiver registered" + Report.pid());
        new Handler(Looper.getMainLooper()).postDelayed(() -> {
            if (!mDone) {
                mDone = true;
                Report.line("time: no TIME_SET came");
                finish();
            }
        }, 60_000);
    }

    @Override
    protected void onDestroy() {
        unregisterReceiver(mReceiver);
        super.onDestroy();
    }
}
