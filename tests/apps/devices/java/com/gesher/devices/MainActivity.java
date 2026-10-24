/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.devices;

import android.app.Activity;
import android.os.Bundle;

import java.io.PrintWriter;
import java.io.StringWriter;

/**
 * Runs each device step in onCreate (the single-process mode ends the run once the
 * launch's messages are done, so nothing is left for later), then finishes. Steps are
 * chosen with the intent's boolean extras (gesher-app --ez); all run by default.
 */
public class MainActivity extends Activity {
    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        Report.line("MainActivity onCreate");
        if (getIntent().getBooleanExtra("location", true)) {
            step("location", () -> new LocationSteps(this).run());
        }
        Report.line("all steps ran");
        finish();
    }

    interface Step {
        void run() throws Exception;
    }

    private static void step(String name, Step step) {
        try {
            step.run();
        } catch (Throwable t) {
            StringWriter s = new StringWriter();
            t.printStackTrace(new PrintWriter(s));
            Report.line(name + ": failed: " + s);
        }
    }

    @Override
    protected void onDestroy() {
        super.onDestroy();
        Report.line("MainActivity onDestroy");
    }
}
