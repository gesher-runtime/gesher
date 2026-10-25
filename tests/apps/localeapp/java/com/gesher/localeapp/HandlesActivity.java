/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.localeapp;

import android.app.Activity;
import android.content.res.Configuration;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;

/**
 * Handles locale and layout direction changes itself (android:configChanges): each change
 * is onConfigurationChanged, not a relaunch. Finishes after {@code finishAfter} changes
 * (an extra, default 2), or after four minutes.
 */
public final class HandlesActivity extends Activity {
    private int mChanges;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        Report.line("HandlesActivity onCreate (saved state " + (savedInstanceState != null)
                + ") " + Report.state(this, null));
        new Handler(Looper.getMainLooper()).postDelayed(() -> {
            if (!isFinishing() && !isDestroyed()) {
                Report.line("HandlesActivity timed out");
                finish();
            }
        }, 240_000);
    }

    @Override
    protected void onResume() {
        super.onResume();
        Report.line("HandlesActivity onResume");
    }

    @Override
    public void onConfigurationChanged(Configuration newConfig) {
        super.onConfigurationChanged(newConfig);
        mChanges++;
        Report.line("HandlesActivity onConfigurationChanged #" + mChanges + " "
                + Report.state(this, newConfig));
        if (mChanges >= getIntent().getIntExtra("finishAfter", 2)) {
            new Handler(Looper.getMainLooper()).post(this::finish);
        }
    }

    @Override
    protected void onDestroy() {
        super.onDestroy();
        Report.line("HandlesActivity onDestroy");
    }
}
