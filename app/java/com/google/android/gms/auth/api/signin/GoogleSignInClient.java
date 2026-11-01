/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.google.android.gms.auth.api.signin;

import android.content.Context;
import android.content.Intent;

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

    public Task<GoogleSignInAccount> silentSignIn() {
        // Return default Gesher user account
        return Tasks.forResult(GoogleSignInAccount.createDefault());
    }

    public Task<Void> signOut() {
        return Tasks.forResult(null);
    }

    public Task<Void> revokeAccess() {
        return Tasks.forResult(null);
    }
}
