/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.lifeapp;

import android.graphics.Color;
import android.os.Bundle;
import android.view.Gravity;
import android.view.View;
import android.widget.FrameLayout;

/**
 * An Activity with a translucent theme (Theme.Translucent: windowIsTranslucent, a
 * transparent background): a blue square in the middle of a transparent window. The
 * Activity below it stays visible, paused, not stopped.
 */
public class TranslucentActivity extends Reporting {
    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        FrameLayout root = new FrameLayout(this);
        View blue = new View(this);
        blue.setBackgroundColor(Color.BLUE);
        int size = Math.round(80 * getResources().getDisplayMetrics().density);
        root.addView(blue, new FrameLayout.LayoutParams(size, size, Gravity.CENTER));
        setContentView(root);
        blue.post(() -> say("TranslucentActivity blue at " + center(blue)));
    }
}
