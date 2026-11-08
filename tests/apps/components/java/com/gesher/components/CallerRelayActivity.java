/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package com.gesher.components;

import android.app.Activity;
import android.app.ActivityOptions;
import android.content.Intent;
import android.net.Uri;

/** Sends an ordinary new Intent after its package acquires exact read access. */
public final class CallerRelayActivity extends Activity {
    private boolean sent;
    @Override protected void onResume() {
        super.onResume();
        if (sent) return;
        sent = true;
        Intent next = new Intent().setClassName("com.gesher.peer", "com.gesher.peer.CallerContractActivity")
                .setData(Uri.parse("content://com.gesher.peer.caller/item?version=1#one"))
                .putExtra("phase", "ordinary")
                .addFlags(Intent.FLAG_ACTIVITY_CLEAR_TOP | Intent.FLAG_ACTIVITY_SINGLE_TOP);
        startActivity(next, ActivityOptions.makeBasic().setShareIdentityEnabled(true).toBundle());
        finish();
    }
}
