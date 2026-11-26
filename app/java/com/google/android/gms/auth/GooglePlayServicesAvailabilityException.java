/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.google.android.gms.auth;

import android.content.Intent;

/** Play services is not available to answer the request (its ConnectionResult code). */
public class GooglePlayServicesAvailabilityException extends UserRecoverableAuthException {
    private final int mConnectionStatusCode;

    public GooglePlayServicesAvailabilityException(int connectionStatusCode, String message,
            Intent intent) {
        super(message, intent);
        mConnectionStatusCode = connectionStatusCode;
    }

    public int getConnectionStatusCode() {
        return mConnectionStatusCode;
    }
}
