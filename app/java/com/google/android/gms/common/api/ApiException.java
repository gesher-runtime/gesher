/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.google.android.gms.common.api;

public class ApiException extends Exception {
    protected final Status mStatus;

    public ApiException(Status status) {
        super(status.getStatusCode() + ": " + (status.getStatusMessage() != null ? status.getStatusMessage() : ""));
        mStatus = status;
    }

    public Status getStatus() {
        return mStatus;
    }

    public int getStatusCode() {
        return mStatus.getStatusCode();
    }
}
