/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.server;

import android.os.CombinedVibration;
import android.os.IBinder;
import android.os.IVibratorManagerService;
import android.os.IVibratorStateListener;
import android.os.VibrationAttributes;
import android.os.VibratorInfo;

/**
 * {@code vibrator_manager}: VibratorManagerService of a device without a vibrator (hwsvc2
 * track, ADR 0051), as Android's is on tablets and TVs that have none: no vibrator ids, no
 * capabilities, so {@code Vibrator.hasVibrator()} and {@code hasAmplitudeControl()} are
 * false, {@code VibratorManager.getVibratorIds()} is empty and {@code getVibrator(id)} the
 * null vibrator; vibrate, cancel and haptic feedback are accepted and do nothing. A PC has
 * no vibrator (a gamepad's rumble is an input device's, which Android reaches through
 * {@code InputDevice.getVibratorManager()}, not this service).
 */
final class VibratorManagerService extends IVibratorManagerService.Default {
    private static final int[] NO_VIBRATORS = new int[0];

    @Override
    public int[] getVibratorIds() {
        return NO_VIBRATORS;
    }

    @Override
    public int getCapabilities() {
        return 0;
    }

    @Override
    public VibratorInfo getVibratorInfo(int vibratorId) {
        return null;  // no such vibrator: the app gets the null vibrator
    }

    @Override
    public boolean isVibrating(int vibratorId) {
        return false;
    }

    @Override
    public boolean registerVibratorStateListener(int vibratorId, IVibratorStateListener listener) {
        return false;
    }

    @Override
    public boolean unregisterVibratorStateListener(int vibratorId,
            IVibratorStateListener listener) {
        return false;
    }

    @Override
    public boolean setAlwaysOnEffect(int uid, String opPkg, int alwaysOnId,
            CombinedVibration vibration, VibrationAttributes attributes) {
        return false;
    }

    @Override
    public void vibrate(int uid, int deviceId, String opPkg, CombinedVibration vibration,
            VibrationAttributes attributes, String reason, IBinder token) {
        // No vibrator to run it on, as VibratorManagerService ignores a vibration for
        // vibrators the device does not have.
    }

    @Override
    public void cancelVibrate(int usageFilter, IBinder token) {
    }

    @Override
    public void performHapticFeedback(int uid, int deviceId, String opPkg, int constant,
            int usage, String reason, int flags, int privFlags) {
    }
}
