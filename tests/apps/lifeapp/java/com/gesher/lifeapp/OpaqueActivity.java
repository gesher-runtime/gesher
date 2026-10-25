/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.lifeapp;

import android.graphics.Color;
import android.os.Bundle;
import android.view.Gravity;
import android.widget.TextView;

/**
 * An opaque Activity (the app's theme): the Activity below it stops. It does not handle
 * configuration changes, so resizing its window relaunches it (onDestroy, then onCreate
 * with its saved state).
 */
public class OpaqueActivity extends Reporting {
    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        TextView text = new TextView(this);
        text.setText("An opaque activity");
        text.setTextSize(20);
        text.setGravity(Gravity.CENTER);
        text.setBackgroundColor(Color.GREEN);
        setContentView(text);
    }
}
