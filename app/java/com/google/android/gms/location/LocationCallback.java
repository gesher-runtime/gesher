/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.google.android.gms.location;

public abstract class LocationCallback {
    public void onLocationResult(LocationResult locationResult) {
    }

    public void onLocationAvailability(LocationAvailability locationAvailability) {
    }
}
