/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.uiapp;

import android.app.Activity;
import android.os.Bundle;
import android.text.Editable;
import android.text.TextWatcher;
import android.util.Log;
import android.view.View;
import android.view.ViewTreeObserver;
import android.widget.EditText;
import android.widget.ScrollView;
import android.widget.TextView;

/**
 * gesher-app's window test app: a layout of colored Views and a TextView, a Button whose
 * click turns the red View cyan, an EditText (focused) echoed into a TextView, and a
 * ScrollView. Reports its layout once the window has laid it out and whenever its size
 * changes, the clicks, the echoed text (other than ASCII as backslash-u escapes), the scroll position and
 * each lifecycle step, on stdout (tests/windows/tracks/34-windows.ps1 reads them).
 */
public class UiActivity extends Activity {
    private static final String TAG = "UiActivity";

    private static void say(String message) {
        System.out.println(message);
        System.out.flush();
        Log.i(TAG, message);
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(R.layout.main);
        final View root = findViewById(android.R.id.content);
        final TextView text = findViewById(R.id.text);
        root.getViewTreeObserver().addOnGlobalLayoutListener(
                new ViewTreeObserver.OnGlobalLayoutListener() {
                    @Override
                    public void onGlobalLayout() {
                        root.getViewTreeObserver().removeOnGlobalLayoutListener(this);
                        View red = findViewById(R.id.red);
                        int[] at = new int[2];
                        text.getLocationInWindow(at);
                        say("UiActivity laid out: content " + root.getWidth() + "x"
                                + root.getHeight() + ", red " + red.getWidth() + "x"
                                + red.getHeight() + ", text at " + at[0] + "," + at[1]
                                + " " + text.getWidth() + "x" + text.getHeight()
                                + ", text width " + text.getPaint().measureText(
                                        text.getText().toString())
                                + ", hardware accelerated " + root.isHardwareAccelerated());
                    }
                });
        root.addOnLayoutChangeListener((v, l, t, r, b, ol, ot, or, ob) -> {
            if (r - l != or - ol || b - t != ob - ot) {
                say("UiActivity content " + (r - l) + "x" + (b - t));
            }
        });
        final View red = findViewById(R.id.red);
        findViewById(R.id.button).setOnClickListener(v -> {
            red.setBackgroundColor(0xFF00FFFF);
            say("UiActivity onClick: button");
        });
        final EditText edit = findViewById(R.id.edit);
        final TextView echo = findViewById(R.id.echo);
        edit.addTextChangedListener(new TextWatcher() {
            @Override
            public void beforeTextChanged(CharSequence s, int start, int count, int after) {
            }

            @Override
            public void onTextChanged(CharSequence s, int start, int before, int count) {
            }

            @Override
            public void afterTextChanged(Editable s) {
                echo.setText(s.toString());
                say("UiActivity echo: " + escape(s.toString()));
            }
        });
        edit.requestFocus();
        ScrollView scroll = findViewById(R.id.scroll);
        scroll.setOnScrollChangeListener((v, x, y, oldX, oldY) ->
                say("UiActivity scrolled to " + y));
        say("UiActivity onCreate");
    }

    /** {@code s} in ASCII: other characters as backslash-u and four hex digits. */
    private static String escape(String s) {
        StringBuilder out = new StringBuilder();
        for (int i = 0; i < s.length(); i++) {
            char c = s.charAt(i);
            if (c >= 0x20 && c < 0x7f) {
                out.append(c);
            } else {
                out.append(String.format("\\u%04x", (int) c));
            }
        }
        return out.toString();
    }

    @Override
    protected void onResume() {
        super.onResume();
        say("UiActivity onResume");
    }

    @Override
    protected void onPause() {
        super.onPause();
        say("UiActivity onPause");
    }

    @Override
    protected void onStop() {
        super.onStop();
        say("UiActivity onStop");
    }

    @Override
    protected void onDestroy() {
        super.onDestroy();
        say("UiActivity onDestroy");
    }
}
