/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.uiapp;

import android.app.Activity;
import android.os.Bundle;
import android.util.Log;
import android.view.View;
import android.view.ViewTreeObserver;
import android.widget.TextView;

/**
 * gesher-app's window test app: a layout of colored Views and a TextView. Reports its
 * layout once the window has laid it out, and each lifecycle step, on stdout.
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
        say("UiActivity onCreate");
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
