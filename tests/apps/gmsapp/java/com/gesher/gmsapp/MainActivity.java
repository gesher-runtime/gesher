/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.gmsapp;

import android.app.Activity;
import android.os.Bundle;

import java.io.PrintWriter;
import java.io.StringWriter;

/**
 * Main activity for the GMS & microG compatibility tests.
 * Runs all validation steps in onCreate and reports results to stdout/log.
 */
public class MainActivity extends Activity {
    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        Report.line("MainActivity onCreate");
        try {
            new GmsUnitSteps(this).runAll();
            Report.line("all steps ran");
        } catch (Throwable t) {
            StringWriter sw = new StringWriter();
            t.printStackTrace(new PrintWriter(sw));
            Report.line("FAIL: " + sw);
        }
        finish();
    }
}
