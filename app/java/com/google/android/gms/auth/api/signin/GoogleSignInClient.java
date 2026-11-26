/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.google.android.gms.auth.api.signin;

import android.content.Context;
import android.content.Intent;

import com.google.android.gms.common.ConnectionResult;
import com.google.android.gms.common.GoogleApiAvailabilityLight;
import com.google.android.gms.common.api.ApiException;
import com.google.android.gms.common.api.CommonStatusCodes;
import com.google.android.gms.common.api.Status;
import com.google.android.gms.tasks.Task;
import com.google.android.gms.tasks.Tasks;

public class GoogleSignInClient {
    private final Context mContext;
    private final GoogleSignInOptions mOptions;

    public GoogleSignInClient(Context context, GoogleSignInOptions options) {
        mContext = context != null ? context.getApplicationContext() : null;
        mOptions = options;
    }

    public Intent getSignInIntent() {
        Intent intent = new Intent("com.google.android.gms.auth.NO_IMPL");
        intent.setPackage("com.google.android.gms");
        intent.putExtra("googleSignInOptions", "options");
        return intent;
    }

    /**
     * Without Play services the request fails with its availability code, as a Google API
     * call does; with it, this fixture still has no signed-in user (SIGN_IN_REQUIRED).
     */
    public Task<GoogleSignInAccount> silentSignIn() {
        int available = GoogleApiAvailabilityLight.getInstance().isGooglePlayServicesAvailable(mContext);
        int code = available != ConnectionResult.SUCCESS ? available : CommonStatusCodes.SIGN_IN_REQUIRED;
        return Tasks.forException(new ApiException(new Status(code)));
    }

    public Task<Void> signOut() {
        return Tasks.forResult(null);
    }

    public Task<Void> revokeAccess() {
        return Tasks.forResult(null);
    }
}
