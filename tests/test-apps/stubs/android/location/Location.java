/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package android.location;

import android.os.Bundle;

public class Location {
    private String mProvider;
    private double mLat;
    private double mLon;
    private float mAccuracy;

    public Location(String provider) { mProvider = provider; }
    public String getProvider() { return mProvider; }
    public double getLatitude() { return mLat; }
    public void setLatitude(double lat) { mLat = lat; }
    public double getLongitude() { return mLon; }
    public void setLongitude(double lon) { mLon = lon; }
    public float getAccuracy() { return mAccuracy; }
    public void setAccuracy(float acc) { mAccuracy = acc; }
}
