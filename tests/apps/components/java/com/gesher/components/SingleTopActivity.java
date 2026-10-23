/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.components;

import android.content.Intent;
import android.os.Bundle;

/**
 * launchMode singleTop, reached through its intent filter. On top, it starts itself again:
 * the intent comes to this instance (onNewIntent), then it finishes.
 */
public class SingleTopActivity extends ReportingActivity {
    static final String ACTION = "com.gesher.components.SINGLE_TOP";
    private static int sInstances;
    private int mInstance;

    @Override
    protected String name() {
        return "SingleTopActivity#" + mInstance;
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        mInstance = ++sInstances;
        super.onCreate(savedInstanceState);
        Report.line(name() + " intent n=" + getIntent().getIntExtra("n", 0));
    }

    @Override
    protected void onNewIntent(Intent intent) {
        super.onNewIntent(intent);
        setIntent(intent);
        Report.line(name() + " onNewIntent n=" + intent.getIntExtra("n", 0));
    }

    @Override
    protected void onResume() {
        super.onResume();
        if (mResumes == 1) {
            Report.line(name() + " starts " + ACTION + " again");
            startActivity(new Intent(ACTION).putExtra("n", 2));
        } else if (mResumes == 2) {
            Report.line(name() + " finish()");
            finish();
        }
    }
}
