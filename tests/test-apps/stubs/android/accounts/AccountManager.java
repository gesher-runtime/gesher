/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package android.accounts;

import android.content.Context;
import android.os.Bundle;
import android.os.Handler;

/**
 * Host fixture: a device without authenticators. A token request fails as Android's
 * AccountManagerService answers one without an authenticator ("bind failure").
 */
public class AccountManager {
    public static final String KEY_AUTHTOKEN = "authtoken";
    public static final String KEY_INTENT = "intent";

    public static AccountManager get(Context context) {
        return new AccountManager();
    }

    public Account[] getAccountsByType(String type) {
        return new Account[0];
    }

    public AccountManagerFuture<Bundle> getAuthToken(Account account, String authTokenType,
            Bundle options, boolean notifyAuthFailure, AccountManagerCallback<Bundle> callback,
            Handler handler) {
        return () -> {
            throw new AuthenticatorException("bind failure");
        };
    }

    public void invalidateAuthToken(String accountType, String authToken) {
    }
}
