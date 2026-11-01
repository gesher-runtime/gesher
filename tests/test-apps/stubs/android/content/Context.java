/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package android.content;

import android.location.LocationManager;

public class Context {
    public static final String LOCATION_SERVICE = "location";
    public static final String ACCOUNT_SERVICE = "account";

    private final LocationManager mLocationManager = new LocationManager();

    public Context getApplicationContext() {
        return this;
    }

    public Object getSystemService(String name) {
        if (LOCATION_SERVICE.equals(name)) {
            return mLocationManager;
        }
        return null;
    }
}
