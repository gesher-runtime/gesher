/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package android.bluetooth;

import android.content.Context;

/**
 * BluetoothManager with the Bluetooth module's public API for a device without
 * Bluetooth: {@link #getAdapter()} is null, which apps check for before they use
 * Bluetooth (BluetoothAdapter.getDefaultAdapter() is null too). Registered as
 * {@code bluetooth} by {@link BluetoothFrameworkInitializer}.
 */
public final class BluetoothManager {
    /** @hide */
    public BluetoothManager(Context context) {
    }

    public BluetoothAdapter getAdapter() {
        return null;
    }
}
