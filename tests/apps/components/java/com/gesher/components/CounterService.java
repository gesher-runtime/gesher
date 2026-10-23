/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.components;

import android.app.Service;
import android.content.Intent;
import android.os.IBinder;

/** A started service: reports each start; with the extra "stopSelf" it stops itself. */
public class CounterService extends Service {
    @Override
    public void onCreate() {
        super.onCreate();
        Report.line("CounterService onCreate");
    }

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        Report.line("CounterService onStartCommand start id " + startId + ", n="
                + intent.getIntExtra("n", 0));
        if (intent.getBooleanExtra("stopSelf", false)) {
            Report.line("CounterService stopSelf(" + startId + ")");
            stopSelf(startId);
        }
        return START_NOT_STICKY;
    }

    @Override
    public IBinder onBind(Intent intent) {
        return null;
    }

    @Override
    public void onDestroy() {
        super.onDestroy();
        Report.line("CounterService onDestroy");
        MainActivity.advance();
    }
}
