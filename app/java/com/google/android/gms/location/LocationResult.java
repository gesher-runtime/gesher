/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.google.android.gms.location;

import android.content.Intent;
import android.location.Location;
import android.location.LocationManager;

import java.util.ArrayList;
import java.util.Collections;
import java.util.List;

public final class LocationResult {
    private static final String EXTRA_LOCATION = "com.google.android.gms.location.EXTRA_LOCATION_RESULT";
    private final List<Location> mLocations;

    private LocationResult(List<Location> locations) {
        mLocations = locations != null ? new ArrayList<>(locations) : Collections.emptyList();
    }

    public static LocationResult create(List<Location> locations) {
        return new LocationResult(locations);
    }

    public Location getLastLocation() {
        return mLocations.isEmpty() ? null : mLocations.get(mLocations.size() - 1);
    }

    public List<Location> getLocations() {
        return Collections.unmodifiableList(mLocations);
    }

    public static boolean hasResult(Intent intent) {
        if (intent == null) return false;
        return intent.hasExtra(EXTRA_LOCATION) || intent.hasExtra(LocationManager.KEY_LOCATION_CHANGED);
    }

    public static LocationResult extractResult(Intent intent) {
        if (intent == null) return null;
        if (intent.hasExtra(EXTRA_LOCATION)) {
            Object obj = intent.getParcelableExtra(EXTRA_LOCATION);
            if (obj instanceof LocationResult) {
                return (LocationResult) obj;
            }
        }
        if (intent.hasExtra(LocationManager.KEY_LOCATION_CHANGED)) {
            Location loc = intent.getParcelableExtra(LocationManager.KEY_LOCATION_CHANGED);
            if (loc != null) {
                return create(Collections.singletonList(loc));
            }
        }
        return null;
    }

    @Override
    public String toString() {
        return "LocationResult{locations=" + mLocations + "}";
    }
}
