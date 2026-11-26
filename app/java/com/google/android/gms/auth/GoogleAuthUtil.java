/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.google.android.gms.auth;

import android.accounts.Account;
import android.accounts.AccountManager;
import android.accounts.AuthenticatorException;
import android.accounts.OperationCanceledException;
import android.content.Context;
import android.content.Intent;
import android.os.Bundle;

import com.google.android.gms.common.ConnectionResult;
import com.google.android.gms.common.GoogleApiAvailabilityLight;

import java.io.IOException;

/**
 * Google OAuth2 tokens for the gmsapp test fixture (app/build.sh keeps these client-library
 * classes out of gesher-app.jar). It reports Play services' absence as the client library
 * does, and otherwise asks the account's authenticator through AccountManager: it never
 * answers a token that no authenticator issued (issue #335).
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
        int available = GoogleApiAvailabilityLight.getInstance().isGooglePlayServicesAvailable(context);
        if (available != ConnectionResult.SUCCESS) {
            throw new GooglePlayServicesAvailabilityException(available,
                    GoogleApiAvailabilityLight.getInstance().getErrorString(available), null);
        }
        Bundle result;
        try {
            result = AccountManager.get(context).getAuthToken(account, scope, extras,
                    false /* notifyAuthFailure */, null, null).getResult();
        } catch (AuthenticatorException | OperationCanceledException e) {
            throw new GoogleAuthException(e.getMessage(), e);
        }
        String token = result.getString(AccountManager.KEY_AUTHTOKEN);
        if (token != null) return token;
        Intent intent = result.getParcelable(AccountManager.KEY_INTENT, Intent.class);
        if (intent != null) throw new UserRecoverableAuthException("NeedPermission", intent);
        throw new GoogleAuthException("no token from the authenticator");
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
        AccountManager.get(context).invalidateAuthToken(GOOGLE_ACCOUNT_TYPE, token);
    }

    public static void clearToken(Context context, String token) throws GoogleAuthException, IOException {
        invalidateToken(context, token);
    }
}
