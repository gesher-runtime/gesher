/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package com.gesher.winapp;

import android.content.Intent;
import android.os.Bundle;
import android.view.ViewGroup;
import android.widget.Button;
import android.widget.LinearLayout;

/** Real lifecycle/window fixture: a floating-themed root is replaced or opens another task. */
public final class FloatingSplashActivity extends Reporting {
    private String mLayout;

    @Override protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        LinearLayout content = new LinearLayout(this);
        content.setOrientation(LinearLayout.VERTICAL);
        Button same = new Button(this);
        same.setText("Replace this splash");
        same.setOnClickListener(v -> {
            startActivity(new Intent(this, FloatingRootActivity.class));
            finish();
            // Both calls finish on this UI callback before the successor can attach its view.
            say("FloatingSplashActivity replacement requested, finishing " + isFinishing());
        });
        content.addView(same);
        Button other = new Button(this);
        other.setText("Open a floating root in another task");
        other.setOnClickListener(v -> startActivity(new Intent(this, FloatingRootActivity.class)
                .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_MULTIPLE_TASK)));
        content.addView(other);
        setContentView(content);
        getWindow().setLayout(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT);
        content.getViewTreeObserver().addOnGlobalLayoutListener(() -> {
            if (content.getWidth() == 0 || content.getHeight() == 0) return;
            String layout = "task " + getTaskId() + ", same at " + center(same)
                    + ", other at " + center(other);
            if (!layout.equals(mLayout)) {
                mLayout = layout;
                say("FloatingSplashActivity ready " + layout);
            }
        });
    }
}
