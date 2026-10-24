/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.server;

import android.content.Intent;
import android.os.BatteryManager;
import android.os.BatteryProperty;
import android.os.IBatteryPropertiesRegistrar;

import com.android.internal.app.IBatteryStats;

/**
 * The battery, from Windows' power status (GetSystemPowerStatus; devsvc track, ADR 0038):
 * {@code batteryproperties} (BatteryManager.getIntProperty: capacity and status),
 * {@code batterystats} (isCharging and the time estimates BatteryManager asks for), and
 * the sticky ACTION_BATTERY_CHANGED (level, scale, status, plugged, present), sent again
 * when Windows reports a power status change. A PC without a battery is a device on AC
 * power whose battery is not present, full.
 */
final class BatteryService implements WindowsHost.Listener {
    private final ActivityManagerService mAm;
    private boolean mStarted;  // guarded by this
    private String mLast;      // guarded by this: the last state sent

    BatteryService(ActivityManagerService am) {
        mAm = am;
        // ACTION_BATTERY_CHANGED is sticky from the first registerReceiver on; in the system
        // server from its start.
        StickyBroadcasts.addStarter(this::start);
        if (WindowsHost.inSystemServer()) start();
    }

    /** {@code batteryproperties}. */
    Properties properties() {
        return new Properties();
    }

    /** Windows' state: {present, percent, plugged (BATTERY_PLUGGED_*), status, low}. */
    static int[] state() {
        int[] s = WindowsHost.powerStatus();
        if (s == null) {
            return new int[] {0, 100, BatteryManager.BATTERY_PLUGGED_AC,
                    BatteryManager.BATTERY_STATUS_FULL, 0};
        }
        boolean present = s[1] != 255 && (s[1] & 128) == 0;
        int percent = s[2] >= 0 && s[2] <= 100 ? s[2] : 100;
        int plugged = s[0] == 1 || !present ? BatteryManager.BATTERY_PLUGGED_AC : 0;
        int status;
        if (!present || percent >= 100 && plugged != 0) {
            status = BatteryManager.BATTERY_STATUS_FULL;
        } else if ((s[1] & 8) != 0) {
            status = BatteryManager.BATTERY_STATUS_CHARGING;
        } else if (plugged != 0) {
            status = BatteryManager.BATTERY_STATUS_NOT_CHARGING;
        } else {
            status = BatteryManager.BATTERY_STATUS_DISCHARGING;
        }
        int low = present && (s[1] & (2 | 4)) != 0 ? 1 : 0;
        return new int[] {present ? 1 : 0, percent, plugged, status, low};
    }

    /** Makes ACTION_BATTERY_CHANGED sticky and listens to Windows' power status. */
    private void start() {
        synchronized (this) {
            if (mStarted) return;
            mStarted = true;
        }
        update(false);
        WindowsHost.addListener(this);
    }

    @Override
    public void onWindowsEvents(int events) {
        if ((events & (WindowsHost.EVENT_POWER_STATUS | WindowsHost.EVENT_RESUMED)) != 0) {
            update(true);
        }
    }

    private void update(boolean broadcast) {
        int[] s = state();
        Intent i = new Intent(Intent.ACTION_BATTERY_CHANGED);
        i.addFlags(Intent.FLAG_RECEIVER_REGISTERED_ONLY | Intent.FLAG_RECEIVER_REPLACE_PENDING);
        i.putExtra(BatteryManager.EXTRA_PRESENT, s[0] == 1);
        i.putExtra(BatteryManager.EXTRA_LEVEL, s[1]);
        i.putExtra(BatteryManager.EXTRA_SCALE, 100);
        i.putExtra(BatteryManager.EXTRA_PLUGGED, s[2]);
        i.putExtra(BatteryManager.EXTRA_STATUS, s[3]);
        i.putExtra(BatteryManager.EXTRA_BATTERY_LOW, s[4] == 1);
        i.putExtra(BatteryManager.EXTRA_HEALTH, BatteryManager.BATTERY_HEALTH_GOOD);
        i.putExtra(BatteryManager.EXTRA_TECHNOLOGY, s[0] == 1 ? "Li-ion" : "");
        i.putExtra(BatteryManager.EXTRA_VOLTAGE, 0);
        i.putExtra(BatteryManager.EXTRA_TEMPERATURE, 250);
        String text = "present " + (s[0] == 1) + ", level " + s[1] + "%, plugged " + s[2]
                + ", status " + s[3];
        synchronized (this) {
            if (text.equals(mLast)) return;
            mLast = text;
        }
        StickyBroadcasts.put(i);
        SystemServer.log("battery: " + text);
        if (broadcast) {
            try {
                mAm.sendBroadcastFor("android", i, null);
            } catch (Throwable t) {
                SystemServer.log("battery: ACTION_BATTERY_CHANGED failed: " + t);
            }
            // Charging changed: power connected or disconnected (exempt broadcasts).
        }
    }

    /** {@code batteryproperties}: BatteryManager's properties. */
    static final class Properties extends IBatteryPropertiesRegistrar.Default {
        @Override
        public int getProperty(int id, BatteryProperty prop) {
            int[] s = state();
            switch (id) {
                case BatteryManager.BATTERY_PROPERTY_CAPACITY:
                    prop.setLong(s[1]);
                    return 0;
                case BatteryManager.BATTERY_PROPERTY_STATUS:
                    prop.setLong(s[3]);
                    return 0;
                default:
                    return -1;  // not supported (BatteryManager answers MIN_VALUE)
            }
        }
    }

    /** {@code batterystats}: what BatteryManager asks of it. */
    static final class Stats extends IBatteryStats.Default {
        @Override
        public boolean isCharging() {
            int status = state()[3];
            return status == BatteryManager.BATTERY_STATUS_CHARGING
                    || status == BatteryManager.BATTERY_STATUS_FULL;
        }

        @Override
        public long computeBatteryTimeRemaining() {
            return -1;  // unknown
        }

        @Override
        public long computeChargeTimeRemaining() {
            return -1;
        }
    }
}
