/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.google.android.gms.auth.api.signin;

public final class GoogleSignInStatusCodes {
    public static final int SIGN_IN_SUCCESS = 0;
    public static final int SIGN_IN_FAILED = 12500;
    public static final int SIGN_IN_CANCELLED = 12501;
    public static final int SIGN_IN_CURRENTLY_IN_PROGRESS = 12502;

    private GoogleSignInStatusCodes() {
    }

    public static String getStatusCodeString(int statusCode) {
        switch (statusCode) {
            case SIGN_IN_SUCCESS: return "SIGN_IN_SUCCESS";
            case SIGN_IN_FAILED: return "SIGN_IN_FAILED";
            case SIGN_IN_CANCELLED: return "SIGN_IN_CANCELLED";
            case SIGN_IN_CURRENTLY_IN_PROGRESS: return "SIGN_IN_CURRENTLY_IN_PROGRESS";
            default: return "UNKNOWN_STATUS_CODE(" + statusCode + ")";
        }
    }
}
