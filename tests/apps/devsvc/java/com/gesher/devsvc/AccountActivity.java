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

    private void run(AccountManager am) throws Exception {
        if (getIntent().getBooleanExtra("prepare-probe", false)) {
            Account hidden = new Account("retained-account@example.test", TestAuthenticator.TYPE);
            Bundle data = new Bundle(); data.putString("server", "imap.example.org");
            am.addAccountExplicitly(hidden, "secret", data);
            am.setUserData(hidden, "sync", "on");
            am.setAuthToken(hidden, "persisted", "fake-persisted-token");
            Account visible = new Account("visible-ungranted@example.test", TestAuthenticator.TYPE);
            am.addAccountExplicitly(visible, "visible-fixture-secret", null);
            am.setAccountVisibility(visible, "com.gesher.microgprobe", AccountManager.VISIBILITY_NOT_VISIBLE);
            am.setAccountVisibility(visible, "com.gesher.microgprobe", AccountManager.VISIBILITY_VISIBLE);
            am.setAuthToken(visible, "probe-ungranted", "fake-visible-ungranted-token");
            getSharedPreferences("account-token-fixture", 0).edit()
                    .putInt("before-probe", TestAuthenticator.tokenRequests(this)).commit();
            android.app.PendingIntent.getBroadcast(this, 93217,
                    new android.content.Intent("com.gesher.devsvc.ACCOUNT_PROBE_PENDING")
                            .setPackage(getPackageName()).putExtra("original", "owner-fixture-value"),
                    android.app.PendingIntent.FLAG_UPDATE_CURRENT | android.app.PendingIntent.FLAG_IMMUTABLE);
            Report.line("account: prepared hidden and visible-but-ungranted attached-probe fixtures");
            return;
        }
        if (getIntent().getBooleanExtra("verify-probe", false)) {
            Account hidden = new Account("retained-account@example.test", TestAuthenticator.TYPE);
            Account visible = new Account("visible-ungranted@example.test", TestAuthenticator.TYPE);
            boolean unchanged = TestAuthenticator.tokenRequests(this)
                    == getSharedPreferences("account-token-fixture", 0).getInt("before-probe", -1)
                    && "secret".equals(am.getPassword(hidden))
                    && "imap.example.org".equals(am.getUserData(hidden, "server"))
                    && "fake-persisted-token".equals(am.peekAuthToken(hidden, "persisted"))
                    && "fake-visible-ungranted-token".equals(am.peekAuthToken(visible, "probe-ungranted"));
            android.app.PendingIntent original = android.app.PendingIntent.getBroadcast(this, 93217,
                    new android.content.Intent("com.gesher.devsvc.ACCOUNT_PROBE_PENDING").setPackage(getPackageName()),
                    android.app.PendingIntent.FLAG_NO_CREATE | android.app.PendingIntent.FLAG_IMMUTABLE);
            boolean capabilityUnchanged = original != null && original.getIntent() != null
                    && "owner-fixture-value".equals(original.getIntent().getStringExtra("original"));
            Report.line("account: attached probe made no authenticator request or owner secret mutation " + unchanged);
            Report.line("account: owner PendingIntent capability and extras preserved " + capabilityUnchanged);
            return;
        }
        String type = TestAuthenticator.TYPE;
        boolean retain = getIntent().getBooleanExtra("retain-account", false);
        boolean verifyRetained = getIntent().getBooleanExtra("verify-retained-account", false);
        String name = retain || verifyRetained ? "retained-account@example.test"
                : "user" + System.currentTimeMillis() + "@example.org";
        if (verifyRetained) {
            Account stored = new Account(name, type);
            boolean persisted = "secret".equals(am.getPassword(stored))
                    && "on".equals(am.getUserData(stored, "sync"))
                    && "fake-persisted-token".equals(am.peekAuthToken(stored, "persisted"));
            Report.line("account: protected storage survived restart " + persisted);
            Report.line("account: retained account removed " + am.removeAccountExplicitly(stored));
            Account visible = new Account("visible-ungranted@example.test", type);
            for (Account candidate : am.getAccountsByType(type)) {
                if (candidate.equals(visible)) am.removeAccountExplicitly(visible);
            }
            return;
        }
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
        if (added.containsKey(AccountManager.KEY_AUTHTOKEN)) throw new AssertionError("addAccount leaked a token");
        Report.line("account: addAccount strips unexpected credentials");

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

        String first = am.getAuthToken(account, "standard", null, false, null, null)
                .getResult(90, TimeUnit.SECONDS).getString(AccountManager.KEY_AUTHTOKEN);
        String cached = am.getAuthToken(account, "standard", null, false, null, null)
                .getResult(90, TimeUnit.SECONDS).getString(AccountManager.KEY_AUTHTOKEN);
        Report.line("account: real token delegation and cache " + (first != null && first.equals(cached)));
        am.invalidateAuthToken(type, first);
        String renewed = am.getAuthToken(account, "standard", null, false, null, null)
                .getResult(90, TimeUnit.SECONDS).getString(AccountManager.KEY_AUTHTOKEN);
        Report.line("account: token invalidation " + (renewed != null && !renewed.equals(first)));
        Bundle interactive = am.getAuthToken(account, "interactive", null, this, null, null)
                .getResult(90, TimeUnit.SECONDS);
        Report.line("account: interactive authenticator continuation "
                + "interactive-fixture-token".equals(interactive.getString(AccountManager.KEY_AUTHTOKEN)));
        Account custom = new Account(name, "com.gesher.devsvc.custom");
        am.addAccountExplicitly(custom, "fixture-custom-password", null);
        String customFirst = am.getAuthToken(custom, "custom", null, false, null, null)
                .getResult(90, TimeUnit.SECONDS).getString(AccountManager.KEY_AUTHTOKEN);
        String customCached = am.getAuthToken(custom, "custom", null, false, null, null)
                .getResult(90, TimeUnit.SECONDS).getString(AccountManager.KEY_AUTHTOKEN);
        Thread.sleep(3500);
        String customExpired = am.getAuthToken(custom, "custom", null, false, null, null)
                .getResult(90, TimeUnit.SECONDS).getString(AccountManager.KEY_AUTHTOKEN);
        Report.line("account: custom token client cache and expiry "
                + (customFirst != null && customFirst.equals(customCached) && !customFirst.equals(customExpired)));
        am.removeAccountExplicitly(custom);
        Bundle sessionOptions = new Bundle();
        String sessionName = "session-" + name;
        sessionOptions.putString("name", sessionName);
        sessionOptions.putString("password", "session-fixture-secret");
        Bundle started = am.startAddAccountSession(type, null, null, sessionOptions, null, null, null)
                .getResult(90, TimeUnit.SECONDS);
        Bundle opaque = started.getBundle(AccountManager.KEY_ACCOUNT_SESSION_BUNDLE);
        boolean privateSession = opaque != null && opaque.size() == 1
                && !opaque.containsKey(AccountManager.KEY_PASSWORD) && !opaque.containsKey("options");
        Bundle finishedSession = am.finishSession(opaque, this, null, null).getResult(90, TimeUnit.SECONDS);
        Account sessionAccount = new Account(sessionName, type);
        boolean realSession = sessionName.equals(finishedSession.getString(AccountManager.KEY_ACCOUNT_NAME))
                && "session-fixture-secret".equals(am.getPassword(sessionAccount))
                && !finishedSession.containsKey(AccountManager.KEY_AUTHTOKEN);
        boolean replayDenied = false;
        try { am.finishSession(opaque, null, null, null).getResult(90, TimeUnit.SECONDS); }
        catch (SecurityException expected) { replayDenied = true; }
        Report.line("account: opaque real authenticator session and replay protection "
                + (privateSession && realSession && replayDenied));
        Bundle updateStart = am.startUpdateCredentialsSession(sessionAccount, null, null, null, null, null)
                .getResult(90, TimeUnit.SECONDS);
        Bundle updateResult = am.finishSession(updateStart.getBundle(AccountManager.KEY_ACCOUNT_SESSION_BUNDLE), this, null, null)
                .getResult(90, TimeUnit.SECONDS);
        Report.line("account: real update-credentials session "
                + ("updated-fixture-secret".equals(am.getPassword(sessionAccount))
                        && !updateResult.containsKey(AccountManager.KEY_AUTHTOKEN)));
        Bundle directUpdate = am.updateCredentials(account, "standard", null, this, null, null)
                .getResult(90, TimeUnit.SECONDS);
        if (directUpdate.containsKey(AccountManager.KEY_AUTHTOKEN)) throw new AssertionError("updateCredentials leaked a token");
        am.setPassword(account, "secret");
        Report.line("account: non-token APIs strip authenticator credentials");
        am.removeAccountExplicitly(sessionAccount);
        am.setAuthToken(account, "persisted", "fake-persisted-token");
        if (retain) { Report.line("account: retained account for restart verification"); return; }

        boolean removed = am.removeAccountExplicitly(account);
        Report.line("account: removed " + removed + ", left " + am.getAccountsByType(type).length);
    }
}
