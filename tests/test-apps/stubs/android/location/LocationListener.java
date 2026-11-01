/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package android.location;

import android.os.Bundle;

public interface LocationListener {
    void onLocationChanged(Location location);
    default void onProviderEnabled(String provider) {}
    default void onProviderDisabled(String provider) {}
    default void onStatusChanged(String provider, int status, Bundle extras) {}
}
