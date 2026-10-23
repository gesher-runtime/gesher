/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.components;

import android.content.ComponentName;
import android.content.Intent;
import android.os.Bundle;

/**
 * A standard activity: level 1 is started for a result by MainActivity and starts level 2
 * (a new instance of the same class); level 2 starts SingleTopActivity through an implicit
 * intent. Back on top, level 2 finishes, then level 1 sets the result and finishes.
 */
public class SecondActivity extends ReportingActivity {
    private int mLevel;

    @Override
    protected String name() {
        return "SecondActivity#" + mLevel;
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        mLevel = getIntent().getIntExtra("level", 0);
        super.onCreate(savedInstanceState);
        ComponentName caller = getCallingActivity();
        Report.line(name() + " started by " + (caller != null ? caller.getShortClassName()
                : "startActivity (no result)") + ", in MainActivity's task: "
                + (getTaskId() == MainActivity.sTaskId));
    }

    @Override
    protected void onResume() {
        super.onResume();
        if (mResumes == 1) {
            if (mLevel == 1) {
                Report.line(name() + " starts SecondActivity (standard: a new instance)");
                startActivity(new Intent(this, SecondActivity.class).putExtra("level", 2));
            } else {
                Report.line(name() + " starts " + SingleTopActivity.ACTION + " (implicit)");
                startActivity(new Intent(SingleTopActivity.ACTION).putExtra("n", 1));
            }
        } else if (mResumes == 2) {
            if (mLevel == 1) {
                setResult(RESULT_OK, new Intent().putExtra("answer", 42));
                Report.line(name() + " setResult(RESULT_OK, answer=42), finish()");
            } else {
                Report.line(name() + " finish()");
            }
            finish();
        }
    }
}
