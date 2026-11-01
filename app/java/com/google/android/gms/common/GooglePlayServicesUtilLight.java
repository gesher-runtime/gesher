/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.google.android.gms.common;

import android.app.Activity;
import android.app.Dialog;
import android.app.PendingIntent;
import android.content.Context;
import android.content.DialogInterface;

public class GooglePlayServicesUtilLight {
    public static final String GOOGLE_PLAY_SERVICES_PACKAGE = GoogleApiAvailabilityLight.GOOGLE_PLAY_SERVICES_PACKAGE;
    public static final String GOOGLE_PLAY_STORE_PACKAGE = GoogleApiAvailabilityLight.GOOGLE_PLAY_STORE_PACKAGE;
    public static final int GOOGLE_PLAY_SERVICES_VERSION_CODE = GoogleApiAvailabilityLight.GOOGLE_PLAY_SERVICES_VERSION_CODE;

    GooglePlayServicesUtilLight() {
    }

    public static int isGooglePlayServicesAvailable(Context context) {
        return GoogleApiAvailabilityLight.getInstance().isGooglePlayServicesAvailable(context);
    }

    public static int isGooglePlayServicesAvailable(Context context, int minApkVersion) {
        return GoogleApiAvailabilityLight.getInstance().isGooglePlayServicesAvailable(context, minApkVersion);
    }

    public static boolean isUserRecoverableError(int errorCode) {
        return GoogleApiAvailabilityLight.getInstance().isUserResolvableError(errorCode);
    }

    public static String getErrorString(int errorCode) {
        return GoogleApiAvailabilityLight.getInstance().getErrorString(errorCode);
    }

    public static PendingIntent getErrorPendingIntent(int errorCode, Context context, int requestCode) {
        return GoogleApiAvailabilityLight.getInstance().getErrorResolutionPendingIntent(context, errorCode, requestCode);
    }
}
