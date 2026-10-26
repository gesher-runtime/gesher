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
 * Toast clicks after the device stopped (hwsvc2, ADR 0051): posts notification 21, whose
 * content intent and action button are broadcasts to ClickReceiver, and finishes; the test
 * stops the device and activates the toast through Gesher's toast activator.
 */
public class ClickActivity extends Activity {
    static final String ACTION_CLICKED = "com.gesher.devsvc.CLICKED";
    static final String ACTION_ACTED = "com.gesher.devsvc.ACTED";
    static final int ID = 21;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        NotificationManager nm = getSystemService(NotificationManager.class);
        nm.createNotificationChannel(new NotificationChannel(NotifyActivity.CHANNEL, "Updates",
                NotificationManager.IMPORTANCE_DEFAULT));
        PendingIntent content = PendingIntent.getBroadcast(this, 1,
                new Intent(ACTION_CLICKED).setClass(this, ClickReceiver.class)
                        .putExtra("from", "content"), PendingIntent.FLAG_IMMUTABLE);
        PendingIntent act = PendingIntent.getBroadcast(this, 2,
                new Intent(ACTION_ACTED).setClass(this, ClickReceiver.class)
                        .putExtra("from", "button"), PendingIntent.FLAG_IMMUTABLE);
        nm.notify(ID, new Notification.Builder(this, NotifyActivity.CHANNEL)
                .setSmallIcon(android.R.drawable.ic_dialog_info)
                .setContentTitle("Click me")
                .setContentText("after the device stopped")
                .setContentIntent(content)
                .addAction(new Notification.Action.Builder(null, "Act", act).build())
                .build());
        Report.line("click: posted " + ID + Report.pid());
        new Handler(Looper.getMainLooper()).postDelayed(this::finish, 2000);
    }
}
