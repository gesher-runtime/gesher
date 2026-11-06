/* Copyright (C) 2026 The Gesher Authors. SPDX-License-Identifier: Apache-2.0 */
package com.gesher.windowpromotion.source;

import android.app.Activity;
import android.content.ComponentName;
import android.content.Intent;
import android.os.Bundle;

/** Reproduces an app-context NEW_TASK launch from an Activity's onCreate. */
public final class SourceActivity extends Activity {
    @Override public void onCreate(Bundle state) {
        super.onCreate(state);
        Intent target = new Intent().setComponent(new ComponentName(
                "com.gesher.windowpromotion.target",
                "com.gesher.windowpromotion.target.TargetActivity"))
                .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
        getApplicationContext().startActivity(target);
        System.out.println("promotion: application-context delegation submitted");
        finish();
    }
}
