/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.winapp;

import android.graphics.Color;
import android.os.Bundle;
import android.view.View;
import android.view.ViewGroup;
import android.widget.EditText;
import android.widget.LinearLayout;

/**
 * adjustPan (windowSoftInputMode): a red View above a focused EditText at the bottom of
 * the window. When the touch keyboard covers the EditText, ViewRootImpl pans the window's
 * content up so it shows (the EditText's place in its window moves up; the content keeps
 * its size). Reports, as each frame is drawn (after the pan), the EditText's top and bottom
 * in its window, the content's height and the window's IME insets.
 */
public class PanActivity extends Reporting {
    private String mDrawn;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        final LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        View red = new View(this);
        red.setBackgroundColor(Color.RED);
        root.addView(red, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, 0,
                1f));
        final EditText edit = new EditText(this);
        edit.setSingleLine(true);
        edit.setText("under the keyboard");
        edit.setBackgroundColor(Color.BLUE);
        edit.setTextColor(Color.WHITE);
        root.addView(edit, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT));
        setContentView(root);
        edit.requestFocus();
        root.getViewTreeObserver().addOnDrawListener(() -> {
            if (root.getWidth() == 0) return;
            int[] at = new int[2];
            edit.getLocationInWindow(at);
            String where = "edit top " + at[1] + ", edit bottom " + (at[1] + edit.getHeight())
                    + ", content " + root.getWidth() + "x" + root.getHeight() + ", "
                    + imeInsets();
            if (!where.equals(mDrawn)) {
                mDrawn = where;
                say("PanActivity drawn: " + where);
            }
        });
    }
}
