/* Copyright (C) 2026 The Gesher Authors. SPDX-License-Identifier: Apache-2.0 */
package com.gesher.windowpromotion.source;

import android.app.Activity;
import android.content.ComponentName;
import android.content.Intent;
import android.os.Bundle;

/** An installed caller starts the exported target service without any target activity. */
public final class BootstrapActivity extends Activity {
    @Override public void onCreate(Bundle state) {
        super.onCreate(state);
        ComponentName started = getApplicationContext().startService(new Intent().setComponent(new ComponentName(
                "com.gesher.windowpromotion.target", "com.gesher.windowpromotion.target.KeepAliveService")));
        if (started == null) throw new AssertionError("Synthetic service was not started");
        System.out.println("promotion: installed caller started target service");
        finish();
    }
}
