/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.google.android.gms.common.api;

import android.app.PendingIntent;

import java.util.Objects;

public final class Status {
    public static final Status RESULT_SUCCESS = new Status(0);
    public static final Status RESULT_TIMEOUT = new Status(14);
    public static final Status RESULT_CANCELED = new Status(16);
    public static final Status RESULT_INTERNAL_ERROR = new Status(8);

    private final int mStatusCode;
    private final String mStatusMessage;
    private final PendingIntent mPendingIntent;

    public Status(int statusCode) {
        this(statusCode, null, null);
    }

    public Status(int statusCode, String statusMessage) {
        this(statusCode, statusMessage, null);
    }

    public Status(int statusCode, String statusMessage, PendingIntent pendingIntent) {
        mStatusCode = statusCode;
        mStatusMessage = statusMessage;
        mPendingIntent = pendingIntent;
    }

    public int getStatusCode() {
        return mStatusCode;
    }

    public String getStatusMessage() {
        return mStatusMessage;
    }

    public PendingIntent getResolution() {
        return mPendingIntent;
    }

    public boolean hasResolution() {
        return mPendingIntent != null;
    }

    public boolean isSuccess() {
        return mStatusCode <= 0;
    }

    public boolean isCanceled() {
        return mStatusCode == 16;
    }

    @Override
    public boolean equals(Object o) {
        if (this == o) return true;
        if (!(o instanceof Status)) return false;
        Status status = (Status) o;
        return mStatusCode == status.mStatusCode &&
                Objects.equals(mStatusMessage, status.mStatusMessage) &&
                Objects.equals(mPendingIntent, status.mPendingIntent);
    }

    @Override
    public int hashCode() {
        return Objects.hash(mStatusCode, mStatusMessage, mPendingIntent);
    }

    @Override
    public String toString() {
        return "Status{statusCode=" + mStatusCode + ", resolution=" + mPendingIntent + "}";
    }
}
