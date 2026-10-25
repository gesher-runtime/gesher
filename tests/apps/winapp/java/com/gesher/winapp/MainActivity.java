/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.winapp;

import android.content.ClipData;
import android.content.ClipboardManager;
import android.content.Intent;
import android.content.pm.ActivityInfo;
import android.graphics.Color;
import android.os.Bundle;
import android.text.Editable;
import android.text.TextWatcher;
import android.view.View;
import android.view.ViewGroup;
import android.widget.Button;
import android.widget.EditText;
import android.widget.LinearLayout;

/**
 * The windows3 test app's launcher Activity (tests/windows/tracks/51-windows3.ps1): buttons
 * that start LandActivity in its task, OtherTaskActivity in a new task, turn its own
 * orientation (setRequestedOrientation, landscape then back to portrait: it handles no
 * configuration change, so it is relaunched), move its task to the back; a green spacer;
 * an EditText at the bottom, focused (adjustResize: the touch keyboard's insets shrink the
 * content above it; Ctrl+C/V/X reach the clipboard); a clipboard listener. Reports where its
 * buttons are and how tall its content is each time that changes, and the EditText's text.
 */
public class MainActivity extends Reporting {
    private String mLaidOut;
    private ClipboardManager mClipboard;
    private final ClipboardManager.OnPrimaryClipChangedListener mClipListener = () -> {
        ClipData clip = mClipboard.getPrimaryClip();
        CharSequence text = clip != null && clip.getItemCount() > 0
                ? clip.getItemAt(0).coerceToText(this) : null;
        say("MainActivity clip changed: " + text);
    };

    private Button button(LinearLayout parent, String text, View.OnClickListener l) {
        Button b = new Button(this);
        b.setText(text);
        b.setAllCaps(false);
        b.setOnClickListener(l);
        parent.addView(b, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT));
        return b;
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        final LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        final Button land = button(root, "Landscape activity", v -> {
            say("MainActivity starting LandActivity");
            startActivity(new Intent(this, LandActivity.class));
        });
        final Button task = button(root, "Activity in another task", v -> {
            say("MainActivity starting OtherTaskActivity in a new task");
            startActivity(new Intent(this, OtherTaskActivity.class)
                    .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_MULTIPLE_TASK));
        });
        final Button turn = button(root, "Turn", v -> {
            int now = getRequestedOrientation();
            int want = now == ActivityInfo.SCREEN_ORIENTATION_LANDSCAPE
                    ? ActivityInfo.SCREEN_ORIENTATION_PORTRAIT
                    : ActivityInfo.SCREEN_ORIENTATION_LANDSCAPE;
            say("MainActivity setRequestedOrientation " + (want
                    == ActivityInfo.SCREEN_ORIENTATION_LANDSCAPE ? "landscape" : "portrait")
                    + " (was " + now + ")");
            setRequestedOrientation(want);
        });
        final Button back = button(root, "Move task to back", v -> {
            boolean moved = moveTaskToBack(true);
            say("MainActivity moveTaskToBack: " + moved);
        });
        View spacer = new View(this);
        spacer.setBackgroundColor(Color.GREEN);
        root.addView(spacer, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT,
                0, 1f));
        final EditText edit = new EditText(this);
        edit.setSingleLine(true);
        edit.setText("gesher clip 51");
        edit.addTextChangedListener(new TextWatcher() {
            @Override
            public void beforeTextChanged(CharSequence s, int start, int count, int after) {
            }

            @Override
            public void onTextChanged(CharSequence s, int start, int before, int count) {
            }

            @Override
            public void afterTextChanged(Editable s) {
                say("MainActivity edit: " + s);
            }
        });
        root.addView(edit, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT));
        setContentView(root);
        // Edge to edge (target SDK 36): the app keeps its content above the IME itself, as
        // apps do with adjustResize since Android 15 enforces edge to edge.
        root.setOnApplyWindowInsetsListener((v, insets) -> {
            int bottom = insets.getInsets(android.view.WindowInsets.Type.ime()).bottom;
            if (v.getPaddingBottom() != bottom) v.setPadding(0, 0, 0, bottom);
            return insets;
        });
        edit.requestFocus();
        root.getViewTreeObserver().addOnGlobalLayoutListener(() -> {
            if (root.getWidth() == 0) return;
            int[] at = new int[2];
            edit.getLocationInWindow(at);
            String where = "content " + root.getWidth() + "x" + root.getHeight() + ", land at "
                    + center(land) + ", task at " + center(task) + ", turn at " + center(turn)
                    + ", back at " + center(back) + ", edit at " + center(edit)
                    + ", edit bottom " + (at[1] + edit.getHeight()) + ", " + imeInsets()
                    + " (edit top in parent " + edit.getTop() + ", padding "
                    + root.getPaddingBottom() + ")";
            if (!where.equals(mLaidOut)) {
                mLaidOut = where;
                say("MainActivity laid out: " + where);
            }
        });
        mClipboard = getSystemService(ClipboardManager.class);
        mClipboard.addPrimaryClipChangedListener(mClipListener);
    }

    @Override
    protected void onDestroy() {
        if (mClipboard != null) mClipboard.removePrimaryClipChangedListener(mClipListener);
        super.onDestroy();
    }
}
