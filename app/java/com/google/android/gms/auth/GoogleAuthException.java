/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.google.android.gms.auth;

public class GoogleAuthException extends Exception {
    public GoogleAuthException() {
        super();
    }

    public GoogleAuthException(String message) {
        super(message);
    }

    public GoogleAuthException(String message, Throwable cause) {
        super(message, cause);
    }
}
