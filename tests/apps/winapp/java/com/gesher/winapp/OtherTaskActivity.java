/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.winapp;

import android.graphics.Color;
import android.os.Bundle;
import android.view.View;

/**
 * An Activity started in a task of its own (FLAG_ACTIVITY_NEW_TASK): a window of its own on
 * the desktop. With multi-resume MainActivity stays resumed; the top resumed one is the
 * focused task's (onTopResumedActivityChanged).
 */
public class OtherTaskActivity extends Reporting {
    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        View v = new View(this);
        v.setBackgroundColor(Color.rgb(255, 128, 0));
        setContentView(v);
    }
}
