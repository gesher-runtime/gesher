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
 * chosen with GESHER_DEVICES_STEPS (a comma-separated list, from the environment the
 * process inherits) or the intent's boolean extras (gesher-app --ez); all run by default.
 */
public class MainActivity extends Activity {
    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        Report.line("MainActivity onCreate");
        if (wanted("location")) step("location", () -> new LocationSteps(this).run());
        if (wanted("sensors")) step("sensors", () -> new SensorSteps(this).run());
        Report.line("all steps ran");
        finish();
    }

    /** A step runs unless an extra turns it off or GESHER_DEVICES_STEPS leaves it out. */
    private boolean wanted(String name) {
        String steps = System.getenv("GESHER_DEVICES_STEPS");
        if (steps != null && !steps.isEmpty()
                && !java.util.Arrays.asList(steps.split(",")).contains(name)) {
            return false;
        }
        return getIntent().getBooleanExtra(name, true);
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
