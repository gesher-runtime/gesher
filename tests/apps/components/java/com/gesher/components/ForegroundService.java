/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.components;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.Service;
import android.content.Intent;
import android.os.IBinder;

/** startForegroundService: a channel, startForeground with a notification, then stop. */
public class ForegroundService extends Service {
    private static final String CHANNEL = "gesher";

    @Override
    public void onCreate() {
        super.onCreate();
        Report.line("ForegroundService onCreate");
    }

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        NotificationManager nm = getSystemService(NotificationManager.class);
        nm.createNotificationChannel(new NotificationChannel(CHANNEL, "Gesher",
                NotificationManager.IMPORTANCE_LOW));
        Notification n = new Notification.Builder(this, CHANNEL)
                .setSmallIcon(android.R.drawable.ic_dialog_info)
                .setContentTitle("Gesher foreground service")
                .build();
        startForeground(7, n);
        Report.line("ForegroundService startForeground(7), channel "
                + nm.getNotificationChannel(CHANNEL).getId());
        stopForeground(STOP_FOREGROUND_REMOVE);
        Report.line("ForegroundService stopForeground, stopSelf");
        stopSelf();
        return START_NOT_STICKY;
    }

    @Override
    public IBinder onBind(Intent intent) {
        return null;
    }

    @Override
    public void onDestroy() {
        super.onDestroy();
        Report.line("ForegroundService onDestroy");
        MainActivity.advance();
    }
}
