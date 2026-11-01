/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.google.android.gms.auth;

import android.accounts.Account;
import android.content.Context;
import android.os.Bundle;

import java.io.IOException;

/**
 * Utility for authenticating with Google accounts and obtaining OAuth2 tokens in Gesher.
 */
public final class GoogleAuthUtil {
    public static final String GOOGLE_ACCOUNT_TYPE = "com.google";
    public static final String KEY_REQUEST_ACTIONS = "request_visible_actions";
    public static final String KEY_REQUEST_VISIBLE_ACTIVITIES = "request_visible_actions";
    public static final String KEY_SUPPRESS_PROGRESS_SCREEN = "suppressProgressScreen";

    private GoogleAuthUtil() {
    }

    public static String getToken(Context context, Account account, String scope)
            throws IOException, GoogleAuthException {
        return getToken(context, account, scope, new Bundle());
    }

    public static String getToken(Context context, Account account, String scope, Bundle extras)
            throws IOException, GoogleAuthException {
        String name = account != null ? account.name : "user@gesher.local";
        return "dummy_oauth2_token_gesher_" + name.replace("@", "_at_") + "_" + Math.abs(scope.hashCode());
    }

    public static String getToken(Context context, String accountName, String scope)
            throws IOException, GoogleAuthException {
        return getToken(context, new Account(accountName, GOOGLE_ACCOUNT_TYPE), scope);
    }

    public static String getToken(Context context, String accountName, String scope, Bundle extras)
            throws IOException, GoogleAuthException {
        return getToken(context, new Account(accountName, GOOGLE_ACCOUNT_TYPE), scope, extras);
    }

    public static void invalidateToken(Context context, String token) {
    }

    public static void clearToken(Context context, String token) throws GoogleAuthException, IOException {
    }
}
