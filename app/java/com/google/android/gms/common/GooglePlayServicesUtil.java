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

public final class GooglePlayServicesUtil extends GooglePlayServicesUtilLight {
    private GooglePlayServicesUtil() {
    }

    public static Dialog getErrorDialog(int errorCode, Activity activity, int requestCode) {
        return GoogleApiAvailability.getInstance().getErrorDialog(activity, errorCode, requestCode);
    }

    public static Dialog getErrorDialog(int errorCode, Activity activity, int requestCode,
            DialogInterface.OnCancelListener cancelListener) {
        return GoogleApiAvailability.getInstance().getErrorDialog(activity, errorCode, requestCode, cancelListener);
    }

    public static boolean showErrorDialogFragment(int errorCode, Activity activity, int requestCode) {
        return GoogleApiAvailability.getInstance().showErrorDialogFragment(activity, errorCode, requestCode);
    }

    public static boolean showErrorDialogFragment(int errorCode, Activity activity, int requestCode,
            DialogInterface.OnCancelListener cancelListener) {
        return GoogleApiAvailability.getInstance().showErrorDialogFragment(activity, errorCode, requestCode, cancelListener);
    }

    public static void showErrorNotification(int errorCode, Context context) {
        GoogleApiAvailability.getInstance().showErrorNotification(context, errorCode);
    }
}
