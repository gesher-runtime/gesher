/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package com.gesher.winapp;

import android.graphics.Color;
import android.os.Bundle;
import android.view.Gravity;
import android.view.ViewGroup;
import android.widget.TextView;

/** A floating-themed activity that must retain its real task and an independent root HWND. */
public final class FloatingRootActivity extends Reporting {
    private String mLayout;

    @Override protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        TextView content = new TextView(this);
        content.setText("Floating root");
        content.setGravity(Gravity.CENTER);
        content.setBackgroundColor(Color.GREEN);
        content.setOnClickListener(v -> say("FloatingRootActivity content clicked"));
        setContentView(content);
        getWindow().setLayout(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT);
        content.getViewTreeObserver().addOnGlobalLayoutListener(() -> {
            if (content.getWidth() == 0 || content.getHeight() == 0) return;
            String layout = "task " + getTaskId() + ", click at " + center(content);
            if (!layout.equals(mLayout)) {
                mLayout = layout;
                say("FloatingRootActivity ready " + layout);
            }
        });
    }
}
