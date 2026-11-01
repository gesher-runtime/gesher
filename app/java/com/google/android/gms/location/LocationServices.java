/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.google.android.gms.location;

import android.app.Activity;
import android.content.Context;

/**
 * Entry point for Google Play Services location APIs.
 */
public class LocationServices {
    private LocationServices() {
    }

    public static FusedLocationProviderClient getFusedLocationProviderClient(Context context) {
        return new FusedLocationProviderClient(context);
    }

    public static FusedLocationProviderClient getFusedLocationProviderClient(Activity activity) {
        return new FusedLocationProviderClient(activity);
    }

    public static SettingsClient getSettingsClient(Context context) {
        return new SettingsClient(context);
    }

    public static SettingsClient getSettingsClient(Activity activity) {
        return new SettingsClient(activity);
    }
}
