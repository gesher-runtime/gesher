/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package com.gesher.components;

import android.app.Activity;
import android.content.Intent;
import android.net.Uri;

/** Delivers a result after this package's access has been revoked. */
public final class CallerResultActivity extends Activity {
    private boolean sent;
    @Override protected void onResume() {
        super.onResume();
        if (sent) return;
        sent = true;
        setResult(RESULT_OK, getIntent().getBooleanExtra("nullData", false) ? null
                : new Intent().setData(Uri.parse("content://com.gesher.peer.caller/item?version=1#one")));
        finish();
    }
}
