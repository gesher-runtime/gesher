/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.google.android.gms.auth.api.signin;

import android.accounts.Account;

import com.google.android.gms.common.api.Scope;

import java.util.ArrayList;
import java.util.Collections;
import java.util.HashSet;
import java.util.List;
import java.util.Set;

public final class GoogleSignInOptions {
    public static final Scope SCOPE_EMAIL = new Scope("email");
    public static final Scope SCOPE_OPEN_ID = new Scope("openid");
    public static final Scope SCOPE_GAMES_LITE = new Scope("https://www.googleapis.com/auth/games_lite");

    public static final GoogleSignInOptions DEFAULT_SIGN_IN = new Builder(0).requestProfile().build();
    public static final GoogleSignInOptions DEFAULT_GAMES_SIGN_IN = new Builder(1).requestScopes(SCOPE_GAMES_LITE).build();

    private final Set<Scope> mScopes;
    private final Account mAccount;
    private final boolean mIdTokenRequested;
    private final boolean mServerAuthCodeRequested;
    private final boolean mForceCodeForRefreshToken;
    private final String mServerClientId;

    private GoogleSignInOptions(Set<Scope> scopes, Account account, boolean idTokenRequested,
            boolean serverAuthCodeRequested, boolean forceCodeForRefreshToken, String serverClientId) {
        mScopes = scopes;
        mAccount = account;
        mIdTokenRequested = idTokenRequested;
        mServerAuthCodeRequested = serverAuthCodeRequested;
        mForceCodeForRefreshToken = forceCodeForRefreshToken;
        mServerClientId = serverClientId;
    }

    public ArrayList<Scope> getScopes() {
        return new ArrayList<>(mScopes);
    }

    public Scope[] getScopeArray() {
        return mScopes.toArray(new Scope[0]);
    }

    public Account getAccount() {
        return mAccount;
    }

    public boolean isIdTokenRequested() {
        return mIdTokenRequested;
    }

    public boolean isServerAuthCodeRequested() {
        return mServerAuthCodeRequested;
    }

    public String getServerClientId() {
        return mServerClientId;
    }

    public static final class Builder {
        private final Set<Scope> mScopes = new HashSet<>();
        private Account mAccount;
        private boolean mIdTokenRequested;
        private boolean mServerAuthCodeRequested;
        private boolean mForceCodeForRefreshToken;
        private String mServerClientId;

        public Builder() {
            mScopes.add(SCOPE_OPEN_ID);
        }

        public Builder(int defaultSignIn) {
            mScopes.add(SCOPE_OPEN_ID);
        }

        public Builder(GoogleSignInOptions options) {
            if (options != null) {
                mScopes.addAll(options.mScopes);
                mAccount = options.mAccount;
                mIdTokenRequested = options.mIdTokenRequested;
                mServerAuthCodeRequested = options.mServerAuthCodeRequested;
                mForceCodeForRefreshToken = options.mForceCodeForRefreshToken;
                mServerClientId = options.mServerClientId;
            }
        }

        public Builder requestId() {
            mScopes.add(SCOPE_OPEN_ID);
            return this;
        }

        public Builder requestEmail() {
            mScopes.add(SCOPE_EMAIL);
            return this;
        }

        public Builder requestProfile() {
            mScopes.add(new Scope("profile"));
            return this;
        }

        public Builder requestScopes(Scope scope, Scope... scopes) {
            mScopes.add(scope);
            if (scopes != null) {
                Collections.addAll(mScopes, scopes);
            }
            return this;
        }

        public Builder requestIdToken(String serverClientId) {
            mIdTokenRequested = true;
            mServerClientId = serverClientId;
            return this;
        }

        public Builder requestServerAuthCode(String serverClientId) {
            return requestServerAuthCode(serverClientId, false);
        }

        public Builder requestServerAuthCode(String serverClientId, boolean forceCodeForRefreshToken) {
            mServerAuthCodeRequested = true;
            mServerClientId = serverClientId;
            mForceCodeForRefreshToken = forceCodeForRefreshToken;
            return this;
        }

        public Builder setAccountName(String accountName) {
            mAccount = new Account(accountName, "com.google");
            return this;
        }

        public GoogleSignInOptions build() {
            return new GoogleSignInOptions(mScopes, mAccount, mIdTokenRequested,
                    mServerAuthCodeRequested, mForceCodeForRefreshToken, mServerClientId);
        }
    }
}
