/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.server;

/**
 * The sensor types SensorManager lists on this device (compat/devices/sensors.cc:
 * Windows.Devices.Sensors' default sensors, plus GESHER_SENSOR_TEST's when that test
 * switch is set, docs/devices.md), for the system features that describe them.
 */
public final class WindowsSensors {
    private static volatile int[] sTypes;

    private WindowsSensors() {
    }

    /** The android.hardware.Sensor TYPE_* values (found once per process); empty without natives. */
    public static int[] types() {
        int[] types = sTypes;
        if (types != null) return types.clone();
        try {
            types = nativeTypes();
        } catch (UnsatisfiedLinkError e) {
            types = null; // Not Gesher's runtime: no sensors.
        }
        if (types == null) types = new int[0];
        sTypes = types;
        return types.clone();
    }

    private static native int[] nativeTypes();
}
