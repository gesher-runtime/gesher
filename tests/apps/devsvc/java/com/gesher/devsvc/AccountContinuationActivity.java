/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.devsvc;

import android.accounts.AccountAuthenticatorResponse;
import android.accounts.AccountManager;
import android.app.Activity;
import android.os.Bundle;

/** Exported, credential-free continuation for the cross-app AccountManager fixture. */
public class AccountContinuationActivity extends Activity {
    public static final class Private extends AccountContinuationActivity {}

    @Override protected void onCreate(Bundle state) {
        super.onCreate(state);
        AccountAuthenticatorResponse response = getIntent().getParcelableExtra(
                AccountManager.KEY_ACCOUNT_AUTHENTICATOR_RESPONSE, AccountAuthenticatorResponse.class);
        if (response != null) {
            Bundle result = new Bundle();
            result.putBoolean(AccountManager.KEY_BOOLEAN_RESULT, true);
            response.onResult(result);
        }
        finish();
    }
}
