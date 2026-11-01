/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package android.location;

import android.app.PendingIntent;
import android.os.Looper;

public class LocationManager {
    public static final String FUSED_PROVIDER = "fused";
    public static final String GPS_PROVIDER = "gps";
    public static final String NETWORK_PROVIDER = "network";
    public static final String PASSIVE_PROVIDER = "passive";
    public static final String KEY_LOCATION_CHANGED = "location";

    private Location mLastLocation;

    public Location getLastKnownLocation(String provider) {
        return mLastLocation;
    }

    public void setLastKnownLocationForTest(Location location) {
        mLastLocation = location;
    }

    public void requestLocationUpdates(String provider, long minTimeMs, float minDistanceM,
            LocationListener listener, Looper looper) {
    }

    public void requestLocationUpdates(String provider, long minTimeMs, float minDistanceM,
            PendingIntent pendingIntent) {
    }

    public void removeUpdates(LocationListener listener) {
    }

    public void removeUpdates(PendingIntent pendingIntent) {
    }

    public void setTestProviderLocation(String provider, Location loc) {
        mLastLocation = loc;
    }
}
