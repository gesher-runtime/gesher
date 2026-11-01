/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.google.android.gms.auth.api.signin;

import android.accounts.Account;
import android.net.Uri;

import com.google.android.gms.common.api.Scope;

import java.util.Collections;
import java.util.HashSet;
import java.util.Set;

/**
 * Information about the signed-in Google account.
 */
public class GoogleSignInAccount {
    private final String mId;
    private final String mEmail;
    private final String mDisplayName;
    private final String mGivenName;
    private final String mFamilyName;
    private final Uri mPhotoUrl;
    private final String mIdToken;
    private final String mServerAuthCode;
    private final long mExpirationTimeSecs;
    private final Set<Scope> mGrantedScopes;

    public static GoogleSignInAccount createDefault() {
        Set<Scope> scopes = new HashSet<>();
        scopes.add(new Scope("email"));
        scopes.add(new Scope("profile"));
        scopes.add(new Scope("openid"));
        return new GoogleSignInAccount(
                "100000000000000000001",
                "user@gesher.local",
                "Gesher User",
                "Gesher",
                "User",
                Uri.parse("https://lh3.googleusercontent.com/a/default-user"),
                "dummy_id_token_gesher_local_jwt_header.payload.signature",
                "dummy_auth_code_gesher_server_token_12345",
                System.currentTimeMillis() / 1000 + 86400 * 30,
                scopes
        );
    }

    public GoogleSignInAccount(String id, String email, String displayName, String givenName,
            String familyName, Uri photoUrl, String idToken, String serverAuthCode,
            long expirationTimeSecs, Set<Scope> grantedScopes) {
        mId = id;
        mEmail = email;
        mDisplayName = displayName;
        mGivenName = givenName;
        mFamilyName = familyName;
        mPhotoUrl = photoUrl;
        mIdToken = idToken;
        mServerAuthCode = serverAuthCode;
        mExpirationTimeSecs = expirationTimeSecs;
        mGrantedScopes = grantedScopes != null ? new HashSet<>(grantedScopes) : Collections.emptySet();
    }

    public String getId() {
        return mId;
    }

    public String getEmail() {
        return mEmail;
    }

    public String getDisplayName() {
        return mDisplayName;
    }

    public String getGivenName() {
        return mGivenName;
    }

    public String getFamilyName() {
        return mFamilyName;
    }

    public Uri getPhotoUrl() {
        return mPhotoUrl;
    }

    public String getIdToken() {
        return mIdToken;
    }

    public String getServerAuthCode() {
        return mServerAuthCode;
    }

    public Account getAccount() {
        return mEmail != null ? new Account(mEmail, "com.google") : null;
    }

    public Set<Scope> getRequestedScopes() {
        return Collections.unmodifiableSet(mGrantedScopes);
    }

    public Set<Scope> getGrantedScopes() {
        return Collections.unmodifiableSet(mGrantedScopes);
    }

    public boolean isExpired() {
        return (System.currentTimeMillis() / 1000) >= (mExpirationTimeSecs - 300);
    }

    @Override
    public boolean equals(Object o) {
        if (this == o) return true;
        if (!(o instanceof GoogleSignInAccount)) return false;
        GoogleSignInAccount that = (GoogleSignInAccount) o;
        return mId != null && mId.equals(that.mId);
    }

    @Override
    public int hashCode() {
        return mId != null ? mId.hashCode() : 0;
    }

    @Override
    public String toString() {
        return "GoogleSignInAccount{id=" + mId + ", email=" + mEmail + ", displayName=" + mDisplayName + "}";
    }
}
