/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.popupapp;

import android.app.Activity;
import android.os.Bundle;
import android.view.Gravity;
import android.widget.TextView;

/**
 * An Activity with a dialog theme (Theme.Material.Light.Dialog): its window floats over the
 * Activity below it, which stays on the screen.
 */
public class DialogActivity extends Activity {
    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        TextView text = new TextView(this);
        text.setText("A dialog activity");
        text.setTextSize(24);
        text.setGravity(Gravity.CENTER);
        text.setPadding(32, 32, 32, 32);
        setContentView(text);
        PopupActivity.say("DialogActivity onCreate");
    }

    @Override
    protected void onResume() {
        super.onResume();
        PopupActivity.say("DialogActivity onResume");
    }

    @Override
    protected void onDestroy() {
        super.onDestroy();
        PopupActivity.say("DialogActivity onDestroy");
    }
}
