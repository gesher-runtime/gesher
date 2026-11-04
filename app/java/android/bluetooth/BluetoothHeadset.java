/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package android.bluetooth;

import java.util.Collections;
import java.util.List;

/**
 * BluetoothHeadset for a device without Bluetooth: the profile proxy an
 * adapter hands a {@link BluetoothProfile.ServiceListener}, so no instance is
 * ever made. The class and its constants have to be there for code that names
 * them (WhatsApp's call audio).
 */
public final class BluetoothHeadset implements BluetoothProfile {
    public static final String ACTION_CONNECTION_STATE_CHANGED =
            "android.bluetooth.headset.profile.action.CONNECTION_STATE_CHANGED";
    public static final String ACTION_AUDIO_STATE_CHANGED =
            "android.bluetooth.headset.profile.action.AUDIO_STATE_CHANGED";
    public static final String ACTION_VENDOR_SPECIFIC_HEADSET_EVENT =
            "android.bluetooth.headset.action.VENDOR_SPECIFIC_HEADSET_EVENT";
    public static final int STATE_AUDIO_DISCONNECTED = 10;
    public static final int STATE_AUDIO_CONNECTING = 11;
    public static final int STATE_AUDIO_CONNECTED = 12;

    private BluetoothHeadset() {
    }

    @Override
    public List<BluetoothDevice> getConnectedDevices() {
        return Collections.emptyList();
    }

    @Override
    public List<BluetoothDevice> getDevicesMatchingConnectionStates(int[] states) {
        return Collections.emptyList();
    }

    @Override
    public int getConnectionState(BluetoothDevice device) {
        return STATE_DISCONNECTED;
    }

    public boolean isAudioConnected(BluetoothDevice device) {
        return false;
    }

    public boolean startVoiceRecognition(BluetoothDevice device) {
        return false;
    }

    public boolean stopVoiceRecognition(BluetoothDevice device) {
        return false;
    }
}
