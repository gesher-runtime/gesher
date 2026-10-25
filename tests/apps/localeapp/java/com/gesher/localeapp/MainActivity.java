/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.localeapp;

import android.app.Activity;
import android.os.Bundle;

/** Reports what the app sees of the locale ("MainActivity state ..."), then finishes. */
public final class MainActivity extends Activity {
    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        Report.line("MainActivity state " + Report.state(this, null));
    }

    @Override
    protected void onResume() {
        super.onResume();
        Report.line("MainActivity onResume");
        new android.os.Handler(android.os.Looper.getMainLooper()).post(this::finish);
    }
}
