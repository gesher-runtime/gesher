/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.devsvc;

import android.service.notification.NotificationListenerService;
import android.service.notification.StatusBarNotification;

/** A notification listener: what the system server tells it. */
public class TestListener extends NotificationListenerService {
    @Override
    public void onListenerConnected() {
        Report.line("listener: connected, active " + getActiveNotifications().length);
    }

    @Override
    public void onNotificationPosted(StatusBarNotification sbn) {
        Report.line("listener: posted " + sbn.getPackageName() + " " + sbn.getId() + " \""
                + sbn.getNotification().extras.getCharSequence("android.title") + "\"");
    }

    @Override
    public void onNotificationRemoved(StatusBarNotification sbn) {
        Report.line("listener: removed " + sbn.getPackageName() + " " + sbn.getId());
    }
}
