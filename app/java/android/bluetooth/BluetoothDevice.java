/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package android.bluetooth;

/**
 * BluetoothDevice for a device without Bluetooth: devices come from an adapter
 * or a profile, so no instance is ever made. The class and its constants have to
 * be there for code that names them.
 */
public final class BluetoothDevice {
    public static final String ACTION_FOUND = "android.bluetooth.device.action.FOUND";
    public static final String ACTION_ACL_CONNECTED = "android.bluetooth.device.action.ACL_CONNECTED";
    public static final String ACTION_ACL_DISCONNECTED =
            "android.bluetooth.device.action.ACL_DISCONNECTED";
    public static final String ACTION_BOND_STATE_CHANGED =
            "android.bluetooth.device.action.BOND_STATE_CHANGED";
    public static final String EXTRA_DEVICE = "android.bluetooth.device.extra.DEVICE";
    public static final int BOND_NONE = 10;
    public static final int BOND_BONDING = 11;
    public static final int BOND_BONDED = 12;
    public static final int DEVICE_TYPE_UNKNOWN = 0;
    public static final int DEVICE_TYPE_CLASSIC = 1;
    public static final int DEVICE_TYPE_LE = 2;
    public static final int DEVICE_TYPE_DUAL = 3;

    private BluetoothDevice() {
    }

    public String getAddress() {
        return "02:00:00:00:00:00";
    }

    public String getName() {
        return null;
    }

    public int getType() {
        return DEVICE_TYPE_UNKNOWN;
    }

    public int getBondState() {
        return BOND_NONE;
    }
}
