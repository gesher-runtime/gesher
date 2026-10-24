/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.devsvc;

import android.app.Activity;
import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.content.Intent;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;

/**
 * Notifications: posts two (one with progress and an action), then 10 s later cancels the
 * first, 8 s later cancels all, and finishes (the test looks at the toast history of
 * Windows in between).
 */
public class NotifyActivity extends Activity {
    static final String CHANNEL = "updates";

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        NotificationManager nm = getSystemService(NotificationManager.class);
        nm.createNotificationChannel(new NotificationChannel(CHANNEL, "Updates",
                NotificationManager.IMPORTANCE_DEFAULT));
        PendingIntent open = PendingIntent.getActivity(this, 0,
                new Intent(this, MainActivity.class), PendingIntent.FLAG_IMMUTABLE);
        nm.notify(7, new Notification.Builder(this, CHANNEL)
                .setSmallIcon(android.R.drawable.ic_dialog_info)
                .setContentTitle("Gesher notification")
                .setContentText("from devsvc")
                .setProgress(100, 40, false)
                .setContentIntent(open)
                .addAction(new Notification.Action.Builder(null, "Open", open).build())
                .build());
        nm.notify("second", 8, new Notification.Builder(this, CHANNEL)
                .setSmallIcon(android.R.drawable.ic_dialog_info)
                .setContentTitle("Second notification")
                .setContentText("tagged")
                .build());
        Report.line("notifications: posted 7 and second/8, active "
                + nm.getActiveNotifications().length);
        Handler h = new Handler(Looper.getMainLooper());
        h.postDelayed(() -> {
            nm.cancel(7);
            Report.line("notifications: cancelled 7, active " + nm.getActiveNotifications().length);
            h.postDelayed(() -> {
                nm.cancelAll();
                Report.line("notifications: cancelled all");
                h.postDelayed(this::finish, 3000);
            }, 8000);
        }, 10000);
    }
}
