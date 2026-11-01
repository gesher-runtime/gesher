/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.gmsapp;

import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.os.Bundle;

public class GmsTestReceiver extends BroadcastReceiver {
    private static volatile Intent sLastReceivedIntent;

    public static Intent getLastReceivedIntent() {
        return sLastReceivedIntent;
    }

    @Override
    public void onReceive(Context context, Intent intent) {
        if (intent == null) return;
        sLastReceivedIntent = intent;
        String action = intent.getAction();
        Bundle extras = intent.getExtras();
        String payload = extras != null ? extras.getString("message") : null;
        String from = extras != null ? extras.getString("from") : null;
        String regId = extras != null ? extras.getString("registration_id") : null;
        Report.line("receiver: action=" + action + " from=" + from + " msg=" + payload + " regId=" + regId);
    }
}
