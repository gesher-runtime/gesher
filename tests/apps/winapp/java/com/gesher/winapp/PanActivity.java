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

    /** ViewRootImpl's pan and insets, for the test's log: "scroll 0/0, focus true, ...". */
    private String panState(View edit) {
        StringBuilder s = new StringBuilder("focus " + edit.isFocused() + ", mode 0x"
                + Integer.toHexString(getWindow().getAttributes().softInputMode));
        try {
            Object root = edit.getViewRootImpl();
            for (String f : new String[] {"mScrollY", "mCurScrollY"}) {
                java.lang.reflect.Field field = root.getClass().getDeclaredField(f);
                field.setAccessible(true);
                s.append(", ").append(f).append(' ').append(field.get(root));
            }
            java.lang.reflect.Field ai = View.class.getDeclaredField("mAttachInfo");
            ai.setAccessible(true);
            Object info = ai.get(edit);
            for (String f : new String[] {"mVisibleInsets", "mContentInsets"}) {
                java.lang.reflect.Field field = info.getClass().getDeclaredField(f);
                field.setAccessible(true);
                s.append(", ").append(f).append(' ').append(field.get(info));
            }
        } catch (ReflectiveOperationException | RuntimeException e) {
            s.append(", ").append(e);
        }
        return s.toString();
    }

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
        getWindow().setSoftInputMode(android.view.WindowManager.LayoutParams.SOFT_INPUT_ADJUST_PAN);
        edit.requestFocus();
        root.getViewTreeObserver().addOnDrawListener(() -> {
            if (root.getWidth() == 0) return;
            int[] at = new int[2];
            edit.getLocationInWindow(at);
            String where = "edit top " + at[1] + ", edit bottom " + (at[1] + edit.getHeight())
                    + ", content " + root.getWidth() + "x" + root.getHeight() + ", "
                    + imeInsets() + " (" + panState(edit) + ")";
            if (!where.equals(mDrawn)) {
                mDrawn = where;
                say("PanActivity drawn: " + where);
            }
        });
    }
}
