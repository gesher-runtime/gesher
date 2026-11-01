/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.server;

import android.accounts.Account;
import android.accounts.AccountManager;
import android.accounts.IAccountManager;
import android.accounts.IAccountManagerResponse;
import android.os.Bundle;
import android.os.RemoteException;

import java.util.ArrayList;
import java.util.Collections;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;

/**
 * {@code account}: AccountManagerService stand-in for Google and local accounts in Gesher.
 * Returns dummy/local Google accounts and OAuth tokens to satisfy GMS clients.
 */
public final class AccountManagerService extends IAccountManager.Default {
    private static final String GOOGLE_TYPE = "com.google";
    private static final Account DEFAULT_ACCOUNT = new Account("user@gesher.local", GOOGLE_TYPE);

    private final Map<Account, String> mAccounts = new LinkedHashMap<>();

    public AccountManagerService() {
        mAccounts.put(DEFAULT_ACCOUNT, "dummy_password");
    }

    @Override
    public Account[] getAccountsAsUser(String type, int userId, String opPackageName) {
        return filterAccounts(type);
    }

    @Override
    public Account[] getAccounts(String type, String opPackageName) {
        return filterAccounts(type);
    }

    @Override
    public Account[] getAccountsForPackage(String packageName, int uid, String opPackageName) {
        return filterAccounts(null);
    }

    @Override
    public Account[] getAccountsByTypeForPackage(String type, String packageName,
            String opPackageName) {
        return filterAccounts(type);
    }

    private synchronized Account[] filterAccounts(String type) {
        List<Account> list = new ArrayList<>();
        for (Account a : mAccounts.keySet()) {
            if (type == null || type.equals(a.type)) {
                list.add(a);
            }
        }
        return list.toArray(new Account[0]);
    }

    @Override
    public void getAuthToken(IAccountManagerResponse response, Account account,
            String authTokenType, boolean notifyOnAuthFailure, boolean expectActivityLaunch,
            Bundle options) {
        if (response == null) return;
        Bundle result = new Bundle();
        String name = account != null ? account.name : DEFAULT_ACCOUNT.name;
        String type = account != null ? account.type : DEFAULT_ACCOUNT.type;
        result.putString(AccountManager.KEY_ACCOUNT_NAME, name);
        result.putString(AccountManager.KEY_ACCOUNT_TYPE, type);
        result.putString(AccountManager.KEY_AUTHTOKEN, "dummy_token_gesher_" + name.replace("@", "_at_"));
        try {
            response.onResult(result);
        } catch (RemoteException e) {
            SystemServer.log("AccountManagerService.getAuthToken error: " + e);
        }
    }

    @Override
    public String peekAuthToken(Account account, String authTokenType) {
        String name = account != null ? account.name : DEFAULT_ACCOUNT.name;
        return "dummy_token_gesher_" + name.replace("@", "_at_");
    }

    @Override
    public String getPassword(Account account) {
        synchronized (this) {
            return mAccounts.get(account);
        }
    }

    @Override
    public String getUserData(Account account, String key) {
        return "dummy_user_data";
    }

    @Override
    public boolean addAccountExplicitly(Account account, String password, Bundle extras) {
        if (account == null) return false;
        synchronized (this) {
            mAccounts.put(account, password != null ? password : "");
        }
        return true;
    }

    @Override
    public boolean removeAccountExplicitly(Account account) {
        if (account == null) return false;
        synchronized (this) {
            return mAccounts.remove(account) != null;
        }
    }

    @Override
    public void setAuthToken(Account account, String authTokenType, String authToken) {
    }

    @Override
    public void invalidateAuthToken(String accountType, String authToken) {
    }

    @Override
    public void setPassword(Account account, String password) {
        if (account != null) {
            synchronized (this) {
                mAccounts.put(account, password);
            }
        }
    }

    @Override
    public void clearPassword(Account account) {
        if (account != null) {
            synchronized (this) {
                mAccounts.put(account, "");
            }
        }
    }
}
