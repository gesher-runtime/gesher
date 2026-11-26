/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.keystore;

import android.app.Activity;
import android.os.Bundle;
import android.widget.TextView;

public final class KeySpecActivity extends Activity {
    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        TextView result = new TextView(this);
        setContentView(result);
        new Thread(() -> {
            String message;
            try {
                KeySpecCases.run(this, System.out::println);
                message = "Keystore spec checks passed";
            } catch (Exception | AssertionError e) {
                e.printStackTrace(System.out);
                message = "Keystore spec checks failed";
            }
            String text = message;
            runOnUiThread(() -> result.setText(text));
        }, "keystore-fixture").start();
    }
}
