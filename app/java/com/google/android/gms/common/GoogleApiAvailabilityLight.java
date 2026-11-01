/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.google.android.gms.common;

import android.app.PendingIntent;
import android.content.Context;
import android.content.Intent;

/**
 * Helper for verifying that Google Play Services is available and up to date.
 */
public class GoogleApiAvailabilityLight {
    public static final String GOOGLE_PLAY_SERVICES_PACKAGE = "com.google.android.gms";
    public static final String GOOGLE_PLAY_STORE_PACKAGE = "com.android.vending";
    public static final int GOOGLE_PLAY_SERVICES_VERSION_CODE = 233013000;

    private static final GoogleApiAvailabilityLight INSTANCE = new GoogleApiAvailabilityLight();

    public static GoogleApiAvailabilityLight getInstance() {
        return INSTANCE;
    }

    GoogleApiAvailabilityLight() {
    }

    public int isGooglePlayServicesAvailable(Context context) {
        return isGooglePlayServicesAvailable(context, GOOGLE_PLAY_SERVICES_VERSION_CODE);
    }

    public int isGooglePlayServicesAvailable(Context context, int minApkVersion) {
        // Gesher's compatibility layer provides full GMS availability
        return ConnectionResult.SUCCESS;
    }

    public boolean isUserResolvableError(int errorCode) {
        return errorCode != ConnectionResult.SUCCESS;
    }

    public boolean isPlayServicesPossiblyUpdating(Context context, int errorCode) {
        return false;
    }

    public boolean isPlayStorePossiblyUpdating(Context context, int errorCode) {
        return false;
    }

    public String getErrorString(int errorCode) {
        switch (errorCode) {
            case ConnectionResult.SUCCESS: return "SUCCESS";
            case ConnectionResult.SERVICE_MISSING: return "SERVICE_MISSING";
            case ConnectionResult.SERVICE_VERSION_UPDATE_REQUIRED: return "SERVICE_VERSION_UPDATE_REQUIRED";
            case ConnectionResult.SERVICE_DISABLED: return "SERVICE_DISABLED";
            case ConnectionResult.SIGNATURE_CHECK_FAILED: return "SIGNATURE_CHECK_FAILED";
            case ConnectionResult.SERVICE_MISSING_PERMISSION: return "SERVICE_MISSING_PERMISSION";
            default: return "UNKNOWN_ERROR_CODE(" + errorCode + ")";
        }
    }

    public PendingIntent getErrorResolutionPendingIntent(Context context, int errorCode,
            int requestCode) {
        return null;
    }

    public Intent getErrorResolutionIntent(Context context, int errorCode, String customAction) {
        return null;
    }

    public int getApkVersion(Context context) {
        return GOOGLE_PLAY_SERVICES_VERSION_CODE;
    }

    public int getClientVersion(Context context) {
        return GOOGLE_PLAY_SERVICES_VERSION_CODE;
    }

    public void cancelAvailabilityErrorNotifications(Context context) {
    }
}
