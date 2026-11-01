/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.google.android.gms.location;

public final class LocationAvailability {
    private final boolean mAvailable;

    LocationAvailability(boolean available) {
        mAvailable = available;
    }

    public static LocationAvailability create(boolean available) {
        return new LocationAvailability(available);
    }

    public boolean isLocationAvailable() {
        return mAvailable;
    }

    @Override
    public String toString() {
        return "LocationAvailability[isLocationAvailable: " + mAvailable + "]";
    }
}
