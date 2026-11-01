/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.google.android.gms.location;

import android.app.PendingIntent;
import android.content.Context;
import android.location.Location;
import android.location.LocationListener;
import android.location.LocationManager;
import android.os.Bundle;
import android.os.Looper;

import com.google.android.gms.tasks.CancellationToken;
import com.google.android.gms.tasks.Task;
import com.google.android.gms.tasks.Tasks;

import java.util.Collections;
import java.util.HashMap;
import java.util.Map;

/**
 * The main entry point for interacting with the fused location provider in Gesher.
 * Bridges GMS location requests directly to Gesher's LocationManagerService.
 */
public class FusedLocationProviderClient {
    public static final String KEY_VERTICAL_ACCURACY = "verticalAccuracy";

    private final Context mContext;
    private final LocationManager mLocationManager;
    private final Map<LocationCallback, LocationListener> mListeners = new HashMap<>();

    public FusedLocationProviderClient(Context context) {
        mContext = context != null ? context.getApplicationContext() : null;
        mLocationManager = mContext != null
                ? (LocationManager) mContext.getSystemService(Context.LOCATION_SERVICE)
                : null;
    }

    public Task<Location> getLastLocation() {
        if (mLocationManager == null) {
            return Tasks.forResult(null);
        }
        Location loc = null;
        try {
            loc = mLocationManager.getLastKnownLocation(LocationManager.FUSED_PROVIDER);
            if (loc == null) {
                loc = mLocationManager.getLastKnownLocation(LocationManager.GPS_PROVIDER);
            }
            if (loc == null) {
                loc = mLocationManager.getLastKnownLocation(LocationManager.NETWORK_PROVIDER);
            }
            if (loc == null) {
                loc = mLocationManager.getLastKnownLocation(LocationManager.PASSIVE_PROVIDER);
            }
        } catch (SecurityException ignored) {
        }
        return Tasks.forResult(loc);
    }

    public Task<Location> getCurrentLocation(int priority, CancellationToken token) {
        // If cancellation requested, return canceled task
        if (token != null && token.isCancellationRequested()) {
            return Tasks.forCanceled();
        }
        return getLastLocation();
    }

    public Task<Void> requestLocationUpdates(LocationRequest request, LocationCallback callback,
            Looper looper) {
        if (mLocationManager == null || callback == null) {
            return Tasks.forResult(null);
        }
        Looper targetLooper = looper != null ? looper : Looper.getMainLooper();
        LocationListener listener = new LocationListener() {
            @Override
            public void onLocationChanged(Location location) {
                if (location != null) {
                    callback.onLocationResult(LocationResult.create(Collections.singletonList(location)));
                }
            }

            @Override
            public void onProviderEnabled(String provider) {
                callback.onLocationAvailability(LocationAvailability.create(true));
            }

            @Override
            public void onProviderDisabled(String provider) {
                callback.onLocationAvailability(LocationAvailability.create(false));
            }

            @Override
            public void onStatusChanged(String provider, int status, Bundle extras) {
            }
        };

        synchronized (mListeners) {
            LocationListener old = mListeners.put(callback, listener);
            if (old != null) {
                try {
                    mLocationManager.removeUpdates(old);
                } catch (Exception ignored) {
                }
            }
        }

        try {
            long interval = request != null ? request.getInterval() : 1000L;
            float minDistance = request != null ? request.getSmallestDisplacement() : 0.0f;
            mLocationManager.requestLocationUpdates(LocationManager.FUSED_PROVIDER, interval,
                    minDistance, listener, targetLooper);
        } catch (SecurityException ignored) {
        }
        return Tasks.forResult(null);
    }

    public Task<Void> requestLocationUpdates(LocationRequest request,
            PendingIntent pendingIntent) {
        if (mLocationManager != null && pendingIntent != null) {
            try {
                long interval = request != null ? request.getInterval() : 1000L;
                float minDistance = request != null ? request.getSmallestDisplacement() : 0.0f;
                mLocationManager.requestLocationUpdates(LocationManager.FUSED_PROVIDER, interval,
                        minDistance, pendingIntent);
            } catch (SecurityException ignored) {
            }
        }
        return Tasks.forResult(null);
    }

    public Task<Void> removeLocationUpdates(LocationCallback callback) {
        if (mLocationManager != null && callback != null) {
            LocationListener listener;
            synchronized (mListeners) {
                listener = mListeners.remove(callback);
            }
            if (listener != null) {
                try {
                    mLocationManager.removeUpdates(listener);
                } catch (SecurityException ignored) {
                }
            }
        }
        return Tasks.forResult(null);
    }

    public Task<Void> removeLocationUpdates(PendingIntent pendingIntent) {
        if (mLocationManager != null && pendingIntent != null) {
            try {
                mLocationManager.removeUpdates(pendingIntent);
            } catch (SecurityException ignored) {
            }
        }
        return Tasks.forResult(null);
    }

    public Task<Void> setMockMode(boolean isMockMode) {
        return Tasks.forResult(null);
    }

    public Task<Void> setMockLocation(Location location) {
        if (mLocationManager != null && location != null) {
            try {
                mLocationManager.setTestProviderLocation(LocationManager.FUSED_PROVIDER, location);
            } catch (Exception ignored) {
            }
        }
        return Tasks.forResult(null);
    }

    public Task<Void> flushLocations() {
        return Tasks.forResult(null);
    }

    public Task<LocationAvailability> getLocationAvailability() {
        return Tasks.forResult(LocationAvailability.create(true));
    }
}
