/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.devsvc;

import android.app.AlarmManager;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;

/** The alarms' manifest receiver. */
public class AlarmReceiver extends BroadcastReceiver {
    @Override
    public void onReceive(Context context, Intent intent) {
        String action = intent.getAction();
        int count = intent.getIntExtra(Intent.EXTRA_ALARM_COUNT, 0);
        if (AlarmActivity.ACTION_REPEAT.equals(action)) {
            int n = Report.count(context, "repeat");
            Report.line("alarms: repeating alarm " + n + " (alarm count " + count + ")"
                    + Report.pid());
            if (n >= 2) {
                context.getSystemService(AlarmManager.class).cancel(
                        AlarmActivity.broadcast(context, AlarmActivity.ACTION_REPEAT));
                Report.line("alarms: repeating alarm cancelled");
            }
            return;
        }
        if (SleepActivity.ACTION_SLEEP_ALARM.equals(action)) {
            Report.line("sleep: the RTC alarm fired" + Report.pid());
            return;
        }
        Report.line("alarms: AlarmReceiver onReceive " + action + " (alarm count " + count + ")"
                + Report.pid());
    }
}
