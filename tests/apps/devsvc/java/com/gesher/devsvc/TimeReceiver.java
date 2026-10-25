/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.devsvc;

import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;

/** The manifest receiver of TIME_SET (an implicit broadcast apps still get in the manifest). */
public class TimeReceiver extends BroadcastReceiver {
    @Override
    public void onReceive(Context context, Intent intent) {
        Report.line("time: manifest receiver got " + intent.getAction() + Report.pid());
    }
}
