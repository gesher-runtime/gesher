/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.devsvc;

import android.accounts.Account;
import android.accounts.AccountManager;
import android.accounts.AuthenticatorDescription;
import android.app.Activity;
import android.os.Bundle;

import java.util.concurrent.TimeUnit;

/**
 * Accounts: addAccount reaches the app's authenticator (TestAuthenticator), which adds the
 * account; the account, its password and user data read back; hasFeatures and
 * getAccountsByTypeAndFeatures ask the authenticator; the account is removed. Mail apps
 * (Gmail's IMAP setup) add their accounts this way.
 */
public class AccountActivity extends Activity {
    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        AccountManager am = AccountManager.get(this);
        new Thread(() -> {
            try {
                run(am);
            } catch (Throwable t) {
                Report.line("account: failed: " + t);
            }
            runOnUiThread(this::finish);
        }).start();
    }

    private static void run(AccountManager am) throws Exception {
        String type = TestAuthenticator.TYPE;
        String name = "user" + System.currentTimeMillis() + "@example.org";
        boolean described = false;
        for (AuthenticatorDescription d : am.getAuthenticatorTypes()) {
            described |= d.type.equals(type) && d.packageName.equals("com.gesher.devsvc");
        }
        Report.line("account: authenticator listed " + described);

        Bundle options = new Bundle();
        options.putString("name", name);
        options.putString("password", "secret");
        Bundle added = am.addAccount(type, null, null, options, null, null, null)
                .getResult(90, TimeUnit.SECONDS);
        Account account = new Account(name, type);
        Report.line("account: addAccount answered " + name.equals(
                added.getString(AccountManager.KEY_ACCOUNT_NAME)));

        boolean listed = false;
        for (Account a : am.getAccountsByType(type)) listed |= a.equals(account);
        Report.line("account: listed " + listed + ", password " + "secret".equals(am.getPassword(account))
                + ", user data " + "imap.example.org".equals(am.getUserData(account, "server")));
        am.setUserData(account, "sync", "on");
        Report.line("account: set user data " + "on".equals(am.getUserData(account, "sync")));

        boolean hasA = am.hasFeatures(account, new String[] {"feature-a"}, null, null)
                .getResult(90, TimeUnit.SECONDS);
        boolean hasB = am.hasFeatures(account, new String[] {"feature-b"}, null, null)
                .getResult(90, TimeUnit.SECONDS);
        Account[] withA = am.getAccountsByTypeAndFeatures(type, new String[] {"feature-a"}, null, null)
                .getResult(90, TimeUnit.SECONDS);
        Report.line("account: features " + hasA + " " + hasB + ", by features " + withA.length);

        boolean removed = am.removeAccountExplicitly(account);
        Report.line("account: removed " + removed + ", left " + am.getAccountsByType(type).length);
    }
}
