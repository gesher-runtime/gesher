/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.lifeapp;

import android.os.Bundle;
import android.view.Gravity;
import android.widget.TextView;

/**
 * An Activity with a dialog theme (Theme.Material.Light.Dialog: windowIsFloating): the
 * Activity below it stays visible, paused, not stopped.
 */
public class DialogThemeActivity extends Reporting {
    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        TextView text = new TextView(this);
        text.setText("A dialog-themed activity");
        text.setTextSize(20);
        text.setGravity(Gravity.CENTER);
        text.setPadding(32, 32, 32, 32);
        setContentView(text);
    }
}
