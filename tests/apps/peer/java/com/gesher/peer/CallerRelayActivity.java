/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package com.gesher.peer;

import android.app.Activity;
import android.app.PendingIntent;

/** A separate holder which this capability's CLEAR_TOP necessarily finishes. */
public final class CallerRelayActivity extends Activity {
    private boolean sent;
    @Override protected void onResume() {
        super.onResume();
        if (sent) return;
        sent = true;
        PendingIntent capability = getIntent().getParcelableExtra("capability", PendingIntent.class);
        if (capability == null) throw new AssertionError("Missing creator capability");
        try { startIntentSenderForResult(capability.getIntentSender(), -1, null, 0, 0, 0, null); }
        catch (android.content.IntentSender.SendIntentException failure) {
            throw new AssertionError("Valid CLEAR_TOP capability canceled", failure);
        }
        finish();
    }
}
