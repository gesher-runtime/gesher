/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.net;

import android.app.ActivityThread;
import android.content.Context;
import android.content.pm.PackageManager;

/** Context capture and early client checks; the service enforces caller authority. @hide */
public final class WifiAccess {
    private static final String ACCESS = "android.permission.ACCESS_WIFI_STATE";
    private static final String CHANGE = "android.permission.CHANGE_WIFI_STATE";
    private WifiAccess() { }

    /** Resolve once when a manager is constructed, never from a later callback thread. */
    public static Context context(Context given) {
        if (given != null) return given;
        try {
            return ActivityThread.currentApplication();
        } catch (RuntimeException | LinkageError e) {
            return null;
        }
    }

    private static boolean granted(Context context, String permission) {
        if (context == null) return false;
        try {
            return context.checkSelfPermission(permission) == PackageManager.PERMISSION_GRANTED;
        } catch (RuntimeException e) {
            return false;
        }
    }

    private static void enforce(Context context, String permission) {
        if (!granted(context, permission)) {
            throw new SecurityException("Wi-Fi requires " + permission);
        }
    }

    public static void enforceAccess(Context context) { enforce(context, ACCESS); }
    public static void enforceChange(Context context) { enforce(context, CHANGE); }

}
