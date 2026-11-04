/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.devsvc;

import android.accounts.AbstractAccountAuthenticator;
import android.accounts.Account;
import android.accounts.AccountAuthenticatorResponse;
import android.accounts.AccountManager;
import android.app.Service;
import android.content.Context;
import android.content.Intent;
import android.os.Bundle;
import android.os.IBinder;
import android.os.Process;

/**
 * The authenticator of {@link #TYPE}, bound by the account manager: addAccount adds the
 * account the options name with addAccountExplicitly (as mail apps' authenticators do),
 * hasFeatures answers for "feature-a".
 */
public class TestAuthenticator extends Service {
    static final String TYPE = "com.gesher.devsvc.account";

    private Authenticator mAuthenticator;

    @Override
    public void onCreate() {
        super.onCreate();
        mAuthenticator = new Authenticator(this);
    }

    @Override
    public IBinder onBind(Intent intent) {
        return mAuthenticator.getIBinder();
    }

    private static final class Authenticator extends AbstractAccountAuthenticator {
        private final Context mContext;

        Authenticator(Context context) {
            super(context);
            mContext = context;
        }

        @Override
        public Bundle addAccount(AccountAuthenticatorResponse response, String accountType,
                String authTokenType, String[] requiredFeatures, Bundle options) {
            String name = options.getString("name");
            Account account = new Account(name, accountType);
            Bundle data = new Bundle();
            data.putString("server", "imap.example.org");
            boolean added = AccountManager.get(mContext).addAccountExplicitly(account,
                    options.getString("password"), data);
            Report.line("account: authenticator adds " + name + ": " + added
                    + ", caller uid " + (options.getInt(AccountManager.KEY_CALLER_UID, -1) >= 0)
                    + Report.pid());
            Bundle result = new Bundle();
            result.putString(AccountManager.KEY_ACCOUNT_NAME, name);
            result.putString(AccountManager.KEY_ACCOUNT_TYPE, accountType);
            return result;
        }

        @Override
        public Bundle hasFeatures(AccountAuthenticatorResponse response, Account account,
                String[] features) {
            boolean has = true;
            for (String f : features) has &= f.equals("feature-a");
            Bundle result = new Bundle();
            result.putBoolean(AccountManager.KEY_BOOLEAN_RESULT, has);
            return result;
        }

        @Override
        public Bundle editProperties(AccountAuthenticatorResponse response, String accountType) {
            return null;
        }

        @Override
        public Bundle confirmCredentials(AccountAuthenticatorResponse response, Account account,
                Bundle options) {
            return null;
        }

        @Override
        public Bundle getAuthToken(AccountAuthenticatorResponse response, Account account,
                String authTokenType, Bundle options) {
            return null;
        }

        @Override
        public String getAuthTokenLabel(String authTokenType) {
            return null;
        }

        @Override
        public Bundle updateCredentials(AccountAuthenticatorResponse response, Account account,
                String authTokenType, Bundle options) {
            return null;
        }
    }
}
