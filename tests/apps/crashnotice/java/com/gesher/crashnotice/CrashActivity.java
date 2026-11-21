/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package com.gesher.crashnotice;

import android.app.Activity;
import android.bluetooth.BluetoothAdapter;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.widget.TextView;

/** Deliberately fails on a machine without a radio, through the real Android API. */
public final class CrashActivity extends Activity {
    @Override public void onCreate(Bundle saved) {
        super.onCreate(saved);
        TextView view = new TextView(this);
        view.setText("Isolated Bluetooth crash diagnostic fixture");
        setContentView(view);
        new Handler(Looper.getMainLooper()).postDelayed(() -> {
            if (BluetoothAdapter.getDefaultAdapter() != null) {
                throw new IllegalStateException("fixture requires a machine without a Bluetooth radio");
            }
            BluetoothAdapter.getDefaultAdapter().getBluetoothLeScanner();
        }, 1500);
    }
}
