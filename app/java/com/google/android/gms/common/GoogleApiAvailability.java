/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.google.android.gms.common;

import android.app.Activity;
import android.app.AlertDialog;
import android.app.Dialog;
import android.app.PendingIntent;
import android.content.Context;
import android.content.DialogInterface;

import com.google.android.gms.tasks.Task;
import com.google.android.gms.tasks.Tasks;

/**
 * Standard GoogleApiAvailability entry point for Google Play Services availability checks.
 */
public class GoogleApiAvailability extends GoogleApiAvailabilityLight {
    public static final String GOOGLE_PLAY_SERVICES_PACKAGE = GoogleApiAvailabilityLight.GOOGLE_PLAY_SERVICES_PACKAGE;
    public static final int GOOGLE_PLAY_SERVICES_VERSION_CODE = GoogleApiAvailabilityLight.GOOGLE_PLAY_SERVICES_VERSION_CODE;

    private static final GoogleApiAvailability INSTANCE = new GoogleApiAvailability();

    public static GoogleApiAvailability getInstance() {
        return INSTANCE;
    }

    GoogleApiAvailability() {
    }

    public Dialog getErrorDialog(Activity activity, int errorCode, int requestCode) {
        return getErrorDialog(activity, errorCode, requestCode, null);
    }

    public Dialog getErrorDialog(Activity activity, int errorCode, int requestCode,
            DialogInterface.OnCancelListener cancelListener) {
        if (errorCode == ConnectionResult.SUCCESS) {
            return null;
        }
        AlertDialog.Builder builder = new AlertDialog.Builder(activity)
                .setTitle("Google Play services")
                .setMessage("Google Play services availability code: " + errorCode)
                .setPositiveButton(android.R.string.ok, (dialog, which) -> dialog.dismiss());
        if (cancelListener != null) {
            builder.setOnCancelListener(cancelListener);
        }
        return builder.create();
    }

    public Task<Void> makeGooglePlayServicesAvailable(Activity activity) {
        return Tasks.forResult(null);
    }

    public void showErrorNotification(Context context, int errorCode) {
    }

    public void showErrorNotification(Context context, ConnectionResult connectionResult) {
    }

    public boolean showErrorDialogFragment(Activity activity, int errorCode, int requestCode) {
        return showErrorDialogFragment(activity, errorCode, requestCode, null);
    }

    public boolean showErrorDialogFragment(Activity activity, int errorCode, int requestCode,
            DialogInterface.OnCancelListener cancelListener) {
        Dialog dialog = getErrorDialog(activity, errorCode, requestCode, cancelListener);
        if (dialog == null) {
            return false;
        }
        dialog.show();
        return true;
    }

    public PendingIntent getErrorResolutionPendingIntent(Context context,
            ConnectionResult connectionResult) {
        return connectionResult != null ? connectionResult.getResolution() : null;
    }
}
