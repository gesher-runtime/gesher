/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.google.android.gms.auth;

import android.content.Intent;

public class UserRecoverableAuthException extends GoogleAuthException {
    private final Intent mIntent;

    public UserRecoverableAuthException(String message, Intent intent) {
        super(message);
        mIntent = intent;
    }

    public Intent getIntent() {
        return mIntent != null ? new Intent(mIntent) : null;
    }
}
