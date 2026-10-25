/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.localeapp;

import android.app.Activity;
import android.content.Intent;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;

/**
 * Waits for configuration changes it does not handle: each one relaunches it ("WaitActivity
 * onCreate #N", N counted through its saved state). The first instance starts
 * {@link HandlesActivity} in front of it, which handles the first change itself and
 * finishes; this one, stopped meanwhile, is relaunched as it comes back (#2). It finishes
 * at #3, or after four minutes.
 */
public final class WaitActivity extends Activity {
    private int mCreation;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        mCreation = (savedInstanceState != null ? savedInstanceState.getInt("creation") : 0) + 1;
        Report.line("WaitActivity onCreate #" + mCreation + " (saved state "
                + (savedInstanceState != null) + ") " + Report.state(this, null));
        Handler main = new Handler(Looper.getMainLooper());
        if (mCreation == 1) {
            main.post(() -> startActivity(new Intent(this, HandlesActivity.class)
                    .putExtra("finishAfter", 1)));
        } else if (mCreation >= 3) {
            main.post(this::finish);
        }
        main.postDelayed(() -> {
            if (!isFinishing() && !isDestroyed()) {
                Report.line("WaitActivity #" + mCreation + " timed out");
                finish();
            }
        }, 240_000);
    }

    @Override
    protected void onResume() {
        super.onResume();
        Report.line("WaitActivity onResume #" + mCreation);
    }

    @Override
    public void onConfigurationChanged(android.content.res.Configuration newConfig) {
        super.onConfigurationChanged(newConfig);
        Report.line("WaitActivity onConfigurationChanged (unexpected: it does not handle it)");
    }

    @Override
    protected void onSaveInstanceState(Bundle outState) {
        super.onSaveInstanceState(outState);
        outState.putInt("creation", mCreation);
    }

    @Override
    protected void onDestroy() {
        super.onDestroy();
        Report.line("WaitActivity onDestroy #" + mCreation + " (changing configurations "
                + isChangingConfigurations() + ")");
    }
}
