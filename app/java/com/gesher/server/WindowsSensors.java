/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.server;

/**
 * The types of the sensors SensorManager lists on this device that describe a sensor
 * (compat/devices/sensors.cc: Windows.Devices.Sensors' default sensors, plus
 * GESHER_SENSOR_TEST's when that test switch is set, docs/devices.md), for the system
 * features that describe them. Gesher's stationary accelerometer, which SensorManager
 * lists on a PC without motion sensors (GESHER_SENSOR_FALLBACK), is not hardware and is
 * left out here by the native list's origin marker, so it declares no feature.
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
