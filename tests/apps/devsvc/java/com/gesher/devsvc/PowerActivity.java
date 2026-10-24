/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.devsvc;

import android.app.Activity;
import android.content.Intent;
import android.content.IntentFilter;
import android.os.BatteryManager;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.os.PowerManager;

/** The battery (sticky ACTION_BATTERY_CHANGED, BatteryManager) and a wake lock held 10 s. */
public class PowerActivity extends Activity {
    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        Intent battery = registerReceiver(null, new IntentFilter(Intent.ACTION_BATTERY_CHANGED));
        if (battery == null) {
            Report.line("power: no sticky ACTION_BATTERY_CHANGED");
        } else {
            Report.line("power: battery present=" + battery.getBooleanExtra(
                    BatteryManager.EXTRA_PRESENT, false) + " level="
                    + battery.getIntExtra(BatteryManager.EXTRA_LEVEL, -1) + "/"
                    + battery.getIntExtra(BatteryManager.EXTRA_SCALE, -1) + " plugged="
                    + battery.getIntExtra(BatteryManager.EXTRA_PLUGGED, -1) + " status="
                    + battery.getIntExtra(BatteryManager.EXTRA_STATUS, -1));
        }
        BatteryManager bm = getSystemService(BatteryManager.class);
        Report.line("power: BatteryManager " + (bm == null ? "missing" : "capacity="
                + bm.getIntProperty(BatteryManager.BATTERY_PROPERTY_CAPACITY) + " charging="
                + bm.isCharging()));
        PowerManager pm = getSystemService(PowerManager.class);
        Report.line("power: interactive=" + pm.isInteractive() + " powerSave="
                + pm.isPowerSaveMode());
        PowerManager.WakeLock lock = pm.newWakeLock(PowerManager.PARTIAL_WAKE_LOCK,
                "devsvc:test");
        lock.acquire();
        Report.line("power: wake lock held");
        new Handler(Looper.getMainLooper()).postDelayed(() -> {
            lock.release();
            Report.line("power: wake lock released");
            new Handler(Looper.getMainLooper()).postDelayed(this::finish, 4000);
        }, 10000);
    }
}
