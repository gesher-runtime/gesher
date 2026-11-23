/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.winapp;

import android.content.Intent;
import android.graphics.Color;
import android.os.Bundle;
import android.os.Process;
import android.view.View;

/** A singleTask target whose existing, minimized window must follow an Android relaunch. */
public final class ResumeTaskActivity extends Reporting {
    private final String mInstance = java.util.UUID.randomUUID().toString();
    private int mNewIntents;

    private String identity() {
        return "task " + getTaskId() + " pid " + Process.myPid() + " instance " + mInstance;
    }

    @Override protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        View content = new View(this);
        content.setBackgroundColor(Color.BLUE);
        setContentView(content);
        say("ResumeTaskActivity created " + identity());
    }

    @Override protected void onNewIntent(Intent intent) {
        super.onNewIntent(intent);
        setIntent(intent);
        say("ResumeTaskActivity onNewIntent " + (++mNewIntents) + " " + identity());
    }

    @Override public void onWindowFocusChanged(boolean focused) {
        super.onWindowFocusChanged(focused);
        say("ResumeTaskActivity window focus " + focused + " " + identity());
    }
}
