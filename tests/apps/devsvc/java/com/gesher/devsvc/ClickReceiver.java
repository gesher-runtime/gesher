/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.devsvc;

import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;

/** Notification 21's content intent and action (ClickActivity). */
public class ClickReceiver extends BroadcastReceiver {
    @Override
    public void onReceive(Context context, Intent intent) {
        Report.line("click: receiver got " + intent.getAction() + " from "
                + intent.getStringExtra("from") + Report.pid());
    }
}
