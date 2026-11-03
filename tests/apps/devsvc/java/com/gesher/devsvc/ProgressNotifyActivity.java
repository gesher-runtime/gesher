/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.devsvc;

import android.app.Activity;
import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;

/**
 * A feed refresh's notification, as AntennaPod posts it: an ongoing, low-importance
 * progress notification with a Hebrew title, updated several times in quick succession
 * and cancelled right away; then another one posted and cancelled, which needs the
 * system server to have survived the first cancel (a cancel once crashed it: the
 * cancel native was entered with the wrong ABI).
 */
public class ProgressNotifyActivity extends Activity {
    static final String CHANNEL = "refreshing";
    static final int ID = 9;
    // "ההסכתים מתרעננים" (the podcasts are refreshing)
    static final String TITLE = "ההסכתים "
            + "מתרעננים";

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        NotificationManager nm = getSystemService(NotificationManager.class);
        nm.createNotificationChannel(new NotificationChannel(CHANNEL, "Refreshing",
                NotificationManager.IMPORTANCE_LOW));
        nm.notify(ID, progress(0, 0, true));
        for (int i = 1; i <= 5; i++) {
            nm.notify(ID, progress(5, i, false));
        }
        Report.line("progress: posted " + ID + " and updated it 5 times");
        nm.cancel(ID);
        Report.line("progress: cancelled " + ID);
        Handler h = new Handler(Looper.getMainLooper());
        h.postDelayed(() -> {
            // The system server is still there: it answers, and takes another one.
            nm.notify(ID + 1, progress(0, 0, true));
            int active = nm.getActiveNotifications().length;
            nm.cancel(ID + 1);
            Report.line("progress: after the cancel, posted and cancelled " + (ID + 1)
                    + " (active " + active + ", then "
                    + nm.getActiveNotifications().length + ")");
            h.postDelayed(this::finish, 2000);
        }, 1000);
    }

    private Notification progress(int max, int value, boolean indeterminate) {
        return new Notification.Builder(this, CHANNEL)
                .setSmallIcon(android.R.drawable.stat_notify_sync)
                .setContentTitle(TITLE)
                .setContentText(value + "/" + max)
                .setOngoing(true)
                .setOnlyAlertOnce(true)
                .setProgress(max, value, indeterminate)
                .build();
    }
}
