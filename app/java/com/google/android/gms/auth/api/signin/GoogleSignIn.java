/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.google.android.gms.auth.api.signin;

import android.app.Activity;
import android.content.Context;
import android.content.Intent;

import com.google.android.gms.common.api.ApiException;
import com.google.android.gms.common.api.Scope;
import com.google.android.gms.common.api.Status;
import com.google.android.gms.tasks.Task;
import com.google.android.gms.tasks.Tasks;

import java.util.Collections;
import java.util.HashSet;
import java.util.Set;

/**
 * Google Sign-In for the gmsapp test fixture. Nobody is signed in until a sign-in result
 * says so; this fixture has no sign-in service, so none does (issue #335).
 */
public final class GoogleSignIn {
    private static GoogleSignInAccount sLastAccount;

    private GoogleSignIn() {
    }

    public static GoogleSignInClient getClient(Context context, GoogleSignInOptions options) {
        return new GoogleSignInClient(context, options);
    }

    public static GoogleSignInClient getClient(Activity activity, GoogleSignInOptions options) {
        return new GoogleSignInClient(activity, options);
    }

    public static GoogleSignInAccount getLastSignedInAccount(Context context) {
        return sLastAccount;
    }

    /** No sign-in activity of this fixture returns an account: the result is a failure. */
    public static Task<GoogleSignInAccount> getSignedInAccountFromIntent(Intent data) {
        return Tasks.forException(new ApiException(new Status(GoogleSignInStatusCodes.SIGN_IN_FAILED,
                "no sign-in result")));
    }

    public static boolean hasPermissions(GoogleSignInAccount account, Scope... scopes) {
        if (account == null || scopes == null) return true;
        Set<Scope> granted = account.getGrantedScopes();
        for (Scope s : scopes) {
            if (!granted.contains(s)) {
                return false;
            }
        }
        return true;
    }

    public static void requestPermissions(Activity activity, int requestCode,
            GoogleSignInAccount account, Scope... scopes) {
    }

    public static void setLastSignedInAccountForTest(GoogleSignInAccount account) {
        sLastAccount = account;
    }
}
