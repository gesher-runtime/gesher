/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.google.android.gms.location;

/**
 * A data object that contains quality of service parameters for requests to the
 * FusedLocationProviderClient.
 */
public final class LocationRequest {
    public static final int PRIORITY_HIGH_ACCURACY = 100;
    public static final int PRIORITY_BALANCED_POWER_ACCURACY = 102;
    public static final int PRIORITY_LOW_POWER = 104;
    public static final int PRIORITY_NO_POWER = 105;

    private int mPriority = PRIORITY_BALANCED_POWER_ACCURACY;
    private long mInterval = 3600000L;
    private long mFastestInterval = 600000L;
    private float mSmallestDisplacement = 0.0f;
    private long mExpirationDuration = Long.MAX_VALUE;
    private int mNumUpdates = Integer.MAX_VALUE;

    public static LocationRequest create() {
        return new LocationRequest();
    }

    public LocationRequest() {
    }

    public int getPriority() {
        return mPriority;
    }

    public LocationRequest setPriority(int priority) {
        mPriority = priority;
        return this;
    }

    public long getInterval() {
        return mInterval;
    }

    public LocationRequest setInterval(long millis) {
        mInterval = millis;
        if (mFastestInterval > mInterval) {
            mFastestInterval = mInterval;
        }
        return this;
    }

    public long getFastestInterval() {
        return mFastestInterval;
    }

    public LocationRequest setFastestInterval(long millis) {
        mFastestInterval = millis;
        return this;
    }

    public float getSmallestDisplacement() {
        return mSmallestDisplacement;
    }

    public LocationRequest setSmallestDisplacement(float smallestDisplacementMeters) {
        mSmallestDisplacement = smallestDisplacementMeters;
        return this;
    }

    public long getExpirationDuration() {
        return mExpirationDuration;
    }

    public LocationRequest setExpirationDuration(long millis) {
        mExpirationDuration = millis;
        return this;
    }

    public int getNumUpdates() {
        return mNumUpdates;
    }

    public LocationRequest setNumUpdates(int numUpdates) {
        mNumUpdates = numUpdates;
        return this;
    }

    @Override
    public String toString() {
        return "LocationRequest{priority=" + mPriority + ", interval=" + mInterval +
                "ms, fastestInterval=" + mFastestInterval + "ms, minDisplacement=" +
                mSmallestDisplacement + "m}";
    }
}
