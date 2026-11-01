/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.google.android.gms.common;

import android.app.PendingIntent;

import java.util.Objects;

/**
 * Represents the result of an attempt to connect to Google Play Services.
 */
public final class ConnectionResult {
    public static final int SUCCESS = 0;
    public static final int SERVICE_MISSING = 1;
    public static final int SERVICE_VERSION_UPDATE_REQUIRED = 2;
    public static final int SERVICE_DISABLED = 3;
    public static final int SIGNATURE_CHECK_FAILED = 9;
    public static final int SERVICE_INVALID = 9;
    public static final int SERVICE_MISSING_PERMISSION = 19;
    public static final int SERVICE_UPDATING = 18;
    public static final int API_UNAVAILABLE = 16;
    public static final int RESOLUTION_REQUIRED = 6;
    public static final int TIMEOUT = 14;
    public static final int INTERRUPTED = 15;
    public static final int NETWORK_ERROR = 7;
    public static final int INTERNAL_ERROR = 8;
    public static final int CANCELED = 13;

    public static final ConnectionResult RESULT_SUCCESS = new ConnectionResult(SUCCESS);

    private final int mStatusCode;
    private final PendingIntent mPendingIntent;
    private final String mStatusMessage;

    public ConnectionResult(int statusCode) {
        this(statusCode, null, null);
    }

    public ConnectionResult(int statusCode, PendingIntent pendingIntent) {
        this(statusCode, pendingIntent, null);
    }

    public ConnectionResult(int statusCode, PendingIntent pendingIntent, String statusMessage) {
        mStatusCode = statusCode;
        mPendingIntent = pendingIntent;
        mStatusMessage = statusMessage;
    }

    public int getErrorCode() {
        return mStatusCode;
    }

    public boolean isSuccess() {
        return mStatusCode == SUCCESS;
    }

    public boolean hasResolution() {
        return mPendingIntent != null;
    }

    public PendingIntent getResolution() {
        return mPendingIntent;
    }

    public String getErrorMessage() {
        return mStatusMessage;
    }

    @Override
    public boolean equals(Object o) {
        if (this == o) return true;
        if (!(o instanceof ConnectionResult)) return false;
        ConnectionResult that = (ConnectionResult) o;
        return mStatusCode == that.mStatusCode &&
                Objects.equals(mPendingIntent, that.mPendingIntent) &&
                Objects.equals(mStatusMessage, that.mStatusMessage);
    }

    @Override
    public int hashCode() {
        return Objects.hash(mStatusCode, mPendingIntent, mStatusMessage);
    }

    @Override
    public String toString() {
        return "ConnectionResult{statusCode=" + mStatusCode +
                ", resolution=" + mPendingIntent +
                ", message=" + mStatusMessage + "}";
    }
}
