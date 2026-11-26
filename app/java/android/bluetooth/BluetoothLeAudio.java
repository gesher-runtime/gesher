/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package android.bluetooth;

import android.content.Context;
import java.util.ArrayList;
import java.util.List;

/**
 * Public LE Audio type for application linkage. The adapter exposes no proxy until
 * a host LE Audio service is available.
 */
public final class BluetoothLeAudio implements BluetoothProfile {
    public static final String ACTION_LE_AUDIO_CONNECTION_STATE_CHANGED =
            "android.bluetooth.action.LE_AUDIO_CONNECTION_STATE_CHANGED";
    /** @hide */
    public static final String ACTION_LE_AUDIO_ACTIVE_DEVICE_CHANGED =
            "android.bluetooth.action.LE_AUDIO_ACTIVE_DEVICE_CHANGED";
    public static final int GROUP_ID_INVALID = -1;

    private final Context mContext;

    private BluetoothLeAudio(Context context) {
        mContext = context;
    }

    @Override
    public List<BluetoothDevice> getConnectedDevices() {
        BluetoothHost.enforceConnect(mContext);
        return new ArrayList<>();
    }

    @Override
    public List<BluetoothDevice> getDevicesMatchingConnectionStates(int[] states) {
        BluetoothHost.enforceConnect(mContext);
        return new ArrayList<>();
    }

    @Override
    public int getConnectionState(BluetoothDevice device) {
        BluetoothHost.enforceConnect(mContext);
        return STATE_DISCONNECTED;
    }

    public int getGroupId(BluetoothDevice device) {
        BluetoothHost.enforceConnect(mContext);
        return GROUP_ID_INVALID;
    }

    /** @hide */
    public BluetoothDevice getConnectedGroupLeadDevice(int groupId) {
        BluetoothHost.enforceConnect(mContext);
        return null;
    }
}
