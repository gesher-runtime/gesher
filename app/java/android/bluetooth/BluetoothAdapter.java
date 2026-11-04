/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package android.bluetooth;

/**
 * BluetoothAdapter for a device without Bluetooth: there is none, so
 * {@link #getDefaultAdapter()} is null and no instance exists (see
 * {@link BluetoothManager}). The class has to be there for code that names it.
 */
public final class BluetoothAdapter {
    private BluetoothAdapter() {
    }

    /** @deprecated as in the module: use BluetoothManager#getAdapter(). */
    @Deprecated
    public static synchronized BluetoothAdapter getDefaultAdapter() {
        return null;
    }

    public static boolean checkBluetoothAddress(String address) {
        if (address == null || address.length() != 17) return false;
        for (int i = 0; i < 17; i++) {
            char c = address.charAt(i);
            if (i % 3 == 2) {
                if (c != ':') return false;
            } else if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F'))) {
                return false;
            }
        }
        return true;
    }
}
