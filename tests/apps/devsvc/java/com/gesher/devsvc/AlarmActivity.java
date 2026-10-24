/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.devsvc;

import android.app.Activity;
import android.app.AlarmManager;
import android.app.PendingIntent;
import android.content.Intent;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.os.SystemClock;

/**
 * Alarms: a listener alarm (this process, while the activity runs), then, once it fired,
 * alarms for after the activity finished and its process ended: an exact RTC_WAKEUP alarm to
 * a manifest receiver, an ELAPSED_REALTIME alarm to a service, a repeating alarm (its
 * receiver cancels it after two), an alarm clock; one alarm set and cancelled.
 */
public class AlarmActivity extends Activity {
    static final String ACTION_ALARM = "com.gesher.devsvc.ALARM";
    static final String ACTION_REPEAT = "com.gesher.devsvc.REPEAT";
    static final String ACTION_CLOCK = "com.gesher.devsvc.CLOCK";
    static final String ACTION_CANCELLED = "com.gesher.devsvc.CANCELLED";

    static PendingIntent broadcast(android.content.Context c, String action) {
        return PendingIntent.getBroadcast(c, 0, new Intent(action).setClass(c, AlarmReceiver.class),
                PendingIntent.FLAG_IMMUTABLE | PendingIntent.FLAG_UPDATE_CURRENT);
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        AlarmManager am = getSystemService(AlarmManager.class);
        Report.line("alarms: canScheduleExactAlarms " + am.canScheduleExactAlarms());
        Handler main = new Handler(Looper.getMainLooper());
        long start = SystemClock.elapsedRealtime();
        am.setExact(AlarmManager.ELAPSED_REALTIME, start + 1000, "devsvc-listener", () -> {
            Report.line("alarms: OnAlarmListener onAlarm after "
                    + (SystemClock.elapsedRealtime() - start) + " ms" + Report.pid());
            scheduleTheRest(am);
            finish();
        }, main);
        Report.line("alarms: listener alarm set" + Report.pid());
    }

    private void scheduleTheRest(AlarmManager am) {
        long now = System.currentTimeMillis();
        long elapsed = SystemClock.elapsedRealtime();
        // Cancelled before it fires: never delivered.
        PendingIntent cancelled = broadcast(this, ACTION_CANCELLED);
        am.setExact(AlarmManager.RTC, now + 6000, cancelled);
        am.cancel(cancelled);
        // After this process ended: the system server starts another for each.
        am.setExactAndAllowWhileIdle(AlarmManager.RTC_WAKEUP, now + 12000,
                broadcast(this, ACTION_ALARM));
        am.setWindow(AlarmManager.ELAPSED_REALTIME, elapsed + 14000, 1000,
                PendingIntent.getService(this, 0, new Intent(this, AlarmService.class),
                        PendingIntent.FLAG_IMMUTABLE));
        am.setRepeating(AlarmManager.ELAPSED_REALTIME_WAKEUP, elapsed + 16000, 4000,
                broadcast(this, ACTION_REPEAT));
        am.setAlarmClock(new AlarmManager.AlarmClockInfo(now + 20000,
                PendingIntent.getActivity(this, 0, new Intent(this, MainActivity.class),
                        PendingIntent.FLAG_IMMUTABLE)), broadcast(this, ACTION_CLOCK));
        AlarmManager.AlarmClockInfo next = am.getNextAlarmClock();
        Report.line("alarms: set for after this process; next alarm clock in "
                + (next != null ? (next.getTriggerTime() - now) / 1000 + " s" : "none"));
    }
}
