/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.devsvc;

import android.app.Service;
import android.content.Intent;
import android.os.IBinder;

/** A service an alarm's PendingIntent starts. */
public class AlarmService extends Service {
    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        Report.line("alarms: AlarmService onStartCommand (alarm count "
                + intent.getIntExtra(Intent.EXTRA_ALARM_COUNT, 0) + ")" + Report.pid());
        stopSelf(startId);
        return START_NOT_STICKY;
    }

    @Override
    public IBinder onBind(Intent intent) {
        return null;
    }
}
