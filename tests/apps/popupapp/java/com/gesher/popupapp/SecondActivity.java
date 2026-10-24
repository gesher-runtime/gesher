/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.popupapp;

import android.app.Activity;
import android.os.Bundle;
import android.view.View;

/** A second Activity in the task: its window takes the task window's place. */
public class SecondActivity extends Activity {
    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        View green = new View(this);
        green.setBackgroundColor(0xFF00FF00);
        setContentView(green);
        PopupActivity.say("SecondActivity onCreate");
    }

    @Override
    protected void onResume() {
        super.onResume();
        PopupActivity.say("SecondActivity onResume");
    }

    @Override
    protected void onDestroy() {
        super.onDestroy();
        PopupActivity.say("SecondActivity onDestroy");
    }
}
