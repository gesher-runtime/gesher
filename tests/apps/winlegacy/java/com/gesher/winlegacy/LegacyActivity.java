/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.winlegacy;

import android.app.Activity;
import android.graphics.Color;
import android.os.Bundle;
import android.util.Log;
import android.view.View;
import android.view.ViewGroup;
import android.view.WindowInsets;
import android.widget.EditText;
import android.widget.LinearLayout;

/** A target-28 app whose content is resized by the framework, without manual IME padding. */
public final class LegacyActivity extends Activity {
    private String previous;
    private static void say(String text) {
        System.out.println(text); System.out.flush(); Log.i("winlegacy", text);
    }
    @Override protected void onCreate(Bundle saved) {
        super.onCreate(saved);
        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        View spacer = new View(this);
        spacer.setBackgroundColor(Color.rgb(64, 128, 224));
        root.addView(spacer, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, 0, 1f));
        EditText edit = new EditText(this);
        edit.setSingleLine(true);
        edit.setText("legacy resize");
        root.addView(edit, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT));
        setContentView(root);
        edit.requestFocus();
        root.getViewTreeObserver().addOnGlobalLayoutListener(() -> {
            if (root.getWidth() == 0) return;
            WindowInsets insets = getWindow().getDecorView().getRootWindowInsets();
            int[] position = new int[2]; edit.getLocationInWindow(position);
            String value = "W4 legacy target " + getApplicationInfo().targetSdkVersion
                    + " content " + root.getWidth() + "x" + root.getHeight()
                    + " edit bottom " + (position[1] + edit.getHeight()) + " padding " + root.getPaddingBottom()
                    + " ime visible " + (insets != null && insets.isVisible(WindowInsets.Type.ime()))
                    + " bottom " + (insets == null ? 0 : insets.getInsets(WindowInsets.Type.ime()).bottom);
            if (!value.equals(previous)) { previous = value; say(value); }
        });
    }
    @Override protected void onDestroy() { say("LegacyActivity onDestroy"); super.onDestroy(); }
}
