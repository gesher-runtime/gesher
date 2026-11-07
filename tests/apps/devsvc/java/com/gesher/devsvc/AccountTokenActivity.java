/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.devsvc;

import android.accounts.Account;
import android.accounts.AccountAuthenticatorResponse;
import android.accounts.AccountManager;
import android.app.Activity;
import android.os.Bundle;

/** Completes a genuine authenticator response after AccountManager launches its Intent. */
public final class AccountTokenActivity extends Activity {
    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        AccountAuthenticatorResponse response = getIntent().getParcelableExtra(
                AccountManager.KEY_ACCOUNT_AUTHENTICATOR_RESPONSE);
        Account account = getIntent().getParcelableExtra("account");
        Bundle result = new Bundle();
        result.putString(AccountManager.KEY_ACCOUNT_NAME, account.name);
        result.putString(AccountManager.KEY_ACCOUNT_TYPE, account.type);
        result.putString(AccountManager.KEY_AUTHTOKEN, "interactive-fixture-token");
        response.onResult(result);
        finish();
    }
}
