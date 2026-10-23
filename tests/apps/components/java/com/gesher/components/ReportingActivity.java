/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.components;

import android.app.Activity;
import android.os.Bundle;

/** An activity that reports its lifecycle callbacks under {@link #name()}. */
public abstract class ReportingActivity extends Activity {
    protected int mResumes;

    protected String name() {
        return getClass().getSimpleName();
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        Report.line(name() + " onCreate");
    }

    @Override
    protected void onStart() {
        super.onStart();
        Report.line(name() + " onStart");
    }

    @Override
    protected void onRestart() {
        super.onRestart();
        Report.line(name() + " onRestart");
    }

    @Override
    protected void onResume() {
        super.onResume();
        mResumes++;
        Report.line(name() + " onResume #" + mResumes);
    }

    @Override
    protected void onPause() {
        super.onPause();
        Report.line(name() + " onPause");
    }

    @Override
    protected void onStop() {
        super.onStop();
        Report.line(name() + " onStop");
    }

    @Override
    protected void onDestroy() {
        super.onDestroy();
        Report.line(name() + " onDestroy");
    }
}
