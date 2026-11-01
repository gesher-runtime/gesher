/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.google.android.gms.location;

public final class LocationSettingsStates {
    private final boolean mGpsUsable;
    private final boolean mGpsPresent;
    private final boolean mNetworkLocationUsable;
    private final boolean mNetworkLocationPresent;
    private final boolean mBleUsable;
    private final boolean mBlePresent;

    public LocationSettingsStates(boolean gpsUsable, boolean gpsPresent, boolean netUsable,
            boolean netPresent, boolean bleUsable, boolean blePresent) {
        mGpsUsable = gpsUsable;
        mGpsPresent = gpsPresent;
        mNetworkLocationUsable = netUsable;
        mNetworkLocationPresent = netPresent;
        mBleUsable = bleUsable;
        mBlePresent = blePresent;
    }

    public boolean isGpsUsable() { return mGpsUsable; }
    public boolean isGpsPresent() { return mGpsPresent; }
    public boolean isNetworkLocationUsable() { return mNetworkLocationUsable; }
    public boolean isNetworkLocationPresent() { return mNetworkLocationPresent; }
    public boolean isBleUsable() { return mBleUsable; }
    public boolean isBlePresent() { return mBlePresent; }
    public boolean isLocationUsable() { return mGpsUsable || mNetworkLocationUsable; }
    public boolean isLocationPresent() { return mGpsPresent || mNetworkLocationPresent; }
}
