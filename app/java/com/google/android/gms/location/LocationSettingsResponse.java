/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.google.android.gms.location;

public class LocationSettingsResponse {
    private final LocationSettingsStates mStates;

    public LocationSettingsResponse() {
        this(new LocationSettingsStates(true, true, true, true, true, true));
    }

    public LocationSettingsResponse(LocationSettingsStates states) {
        mStates = states;
    }

    public LocationSettingsStates getLocationSettingsStates() {
        return mStates;
    }
}
