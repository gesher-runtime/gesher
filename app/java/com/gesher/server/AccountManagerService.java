/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.server;

import android.accounts.Account;
import android.accounts.AccountManager;
import android.accounts.AuthenticatorDescription;
import android.accounts.IAccountAuthenticator;
import android.accounts.IAccountAuthenticatorResponse;
import android.accounts.IAccountManager;
import android.accounts.IAccountManagerResponse;
import android.app.IServiceConnection;
import android.content.ComponentName;
import android.content.Context;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.content.pm.ResolveInfo;
import android.content.pm.ServiceInfo;
import android.content.res.AssetManager;
import android.content.res.Configuration;
import android.content.res.Resources;
import android.content.res.XmlResourceParser;
import android.os.Binder;
import android.os.Bundle;
import android.os.IBinder;
import android.os.RemoteException;
import android.util.DisplayMetrics;

import org.xmlpull.v1.XmlPullParser;

import java.io.File;
import java.io.IOException;
import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.Properties;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.TimeUnit;

/**
 * {@code account}: AccountManagerService for Google and app accounts in Gesher.
 *
 * <p>A dummy Google account and OAuth tokens satisfy GMS clients. Apps' own account types
 * work as on Android: {@code addAccount}, {@code hasFeatures} and {@code getAccountsByFeatures}
 * go to the type's authenticator (the app's service for
 * {@link AccountManager#ACTION_AUTHENTICATOR_INTENT}, its type in the
 * {@code account-authenticator} XML of its meta-data), bound through the activity manager;
 * the authenticator adds the account with {@code addAccountExplicitly}. Accounts, their
 * passwords and user data persist in {@code $ANDROID_DATA/system/accounts} (as Android keeps
 * them in its accounts database).
 */
public final class AccountManagerService extends IAccountManager.Default {
    private static final String TAG = "accounts: ";
    private static final String GOOGLE_TYPE = "com.google";
    private static final Account DEFAULT_ACCOUNT = new Account("user@gesher.local", GOOGLE_TYPE);
    private static final String ANDROID_NS = "http://schemas.android.com/apk/res/android";
    /** How long an authenticator has to bind and answer. */
    private static final long AUTHENTICATOR_TIMEOUT_SECONDS = 60;

    /** An account's password and user data. */
    private static final class Entry {
        String password;
        final Map<String, String> data = new LinkedHashMap<>();
    }

    /** An app's authenticator: its account type and its service. */
    private static final class Authenticator {
        final String type;
        final ComponentName service;
        final int label, icon, smallIcon, preferences;
        final boolean customTokens;

        Authenticator(String type, ComponentName service, int label, int icon, int smallIcon,
                int preferences, boolean customTokens) {
            this.type = type;
            this.service = service;
            this.label = label;
            this.icon = icon;
            this.smallIcon = smallIcon;
            this.preferences = preferences;
            this.customTokens = customTokens;
        }
    }

    private final ActivityManagerService mAm;
    private final PackageManagerService mPm;
    private final File mFile = new File(Device.dataDir(), "system/accounts");
    // Guarded by this.
    private final Map<Account, Entry> mAccounts = new LinkedHashMap<>();
    // Calls to authenticators wait on their binding and answer: off the binder threads.
    private final ExecutorService mCalls = Executors.newCachedThreadPool(r -> {
        Thread t = new Thread(r, "accounts");
        t.setDaemon(true);
        return t;
    });

    public AccountManagerService(ActivityManagerService am, PackageManagerService pm) {
        mAm = am;
        mPm = pm;
        Entry dummy = new Entry();
        dummy.password = "dummy_password";
        mAccounts.put(DEFAULT_ACCOUNT, dummy);
        load();
    }

    // --- accounts ----------------------------------------------------------------------

    @Override
    public Account[] getAccountsAsUser(String type, int userId, String opPackageName) {
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

    @Override
    public Map<String, Integer> getAccountsAndVisibilityForPackage(String packageName,
            String accountType) {
        Map<String, Integer> result = new LinkedHashMap<>();
        for (Account a : filterAccounts(accountType)) {
            result.put(a.name, AccountManager.VISIBILITY_VISIBLE);
        }
        return result;
    }

    @Override
    public int getAccountVisibility(Account account, String packageName) {
        return AccountManager.VISIBILITY_VISIBLE;
    }

    @Override
    public boolean setAccountVisibility(Account account, String packageName, int newVisibility) {
        return exists(account);
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

    private synchronized boolean exists(Account account) {
        return account != null && mAccounts.containsKey(account);
    }

    @Override
    public boolean addAccountExplicitly(Account account, String password, Bundle extras,
            String opPackageName) {
        if (account == null) return false;
        synchronized (this) {
            if (mAccounts.containsKey(account)) return false;
            Entry e = new Entry();
            e.password = password;
            if (extras != null) {
                for (String key : extras.keySet()) {
                    Object value = extras.get(key);
                    if (value != null) e.data.put(key, value.toString());
                }
            }
            mAccounts.put(account, e);
            save();
        }
        SystemServer.log(TAG + "added " + account.type + " account " + account.name);
        changed(account, false);
        return true;
    }

    @Override
    public boolean addAccountExplicitlyWithVisibility(Account account, String password,
            Bundle extras, Map visibility, String opPackageName) {
        return addAccountExplicitly(account, password, extras, opPackageName);
    }

    @Override
    public boolean removeAccountExplicitly(Account account) {
        if (account == null || account.equals(DEFAULT_ACCOUNT)) return false;
        synchronized (this) {
            if (mAccounts.remove(account) == null) return false;
            save();
        }
        SystemServer.log(TAG + "removed " + account.type + " account " + account.name);
        changed(account, true);
        return true;
    }

    @Override
    public void removeAccountAsUser(IAccountManagerResponse response, Account account,
            boolean expectActivityLaunch, int userId) {
        Bundle result = new Bundle();
        result.putBoolean(AccountManager.KEY_BOOLEAN_RESULT, removeAccountExplicitly(account));
        reply(response, result);
    }

    // --- passwords, tokens and user data -------------------------------------------------

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
        reply(response, result);
    }

    @Override
    public String peekAuthToken(Account account, String authTokenType) {
        String name = account != null ? account.name : DEFAULT_ACCOUNT.name;
        return "dummy_token_gesher_" + name.replace("@", "_at_");
    }

    @Override
    public synchronized String getPassword(Account account) {
        Entry e = mAccounts.get(account);
        return e != null ? e.password : null;
    }

    @Override
    public void setPassword(Account account, String password) {
        synchronized (this) {
            Entry e = mAccounts.get(account);
            if (e == null) return;
            e.password = password;
            save();
        }
    }

    @Override
    public void clearPassword(Account account) {
        setPassword(account, null);
    }

    @Override
    public synchronized String getUserData(Account account, String key) {
        Entry e = mAccounts.get(account);
        if (e == null) return null;
        String value = e.data.get(key);
        // GMS clients read the dummy Google account's data.
        return value == null && DEFAULT_ACCOUNT.equals(account) ? "dummy_user_data" : value;
    }

    @Override
    public void setUserData(Account account, String key, String value) {
        if (key == null) return;
        synchronized (this) {
            Entry e = mAccounts.get(account);
            if (e == null) return;
            if (value == null) e.data.remove(key);
            else e.data.put(key, value);
            save();
        }
    }

    @Override
    public void setAuthToken(Account account, String authTokenType, String authToken) {
    }

    @Override
    public void invalidateAuthToken(String accountType, String authToken) {
    }

    // Apps listen for account changes with broadcasts (changed()); nothing to keep.
    @Override
    public void registerAccountListener(String[] accountTypes, String opPackageName) {
    }

    @Override
    public void unregisterAccountListener(String[] accountTypes, String opPackageName) {
    }

    // --- authenticators ----------------------------------------------------------------

    @Override
    public AuthenticatorDescription[] getAuthenticatorTypes(int userId) {
        List<AuthenticatorDescription> result = new ArrayList<>();
        for (Authenticator a : authenticators()) {
            result.add(new AuthenticatorDescription(a.type, a.service.getPackageName(), a.label,
                    a.icon, a.smallIcon, a.preferences, a.customTokens));
        }
        return result.toArray(new AuthenticatorDescription[0]);
    }

    @Override
    public void addAccountAsUser(IAccountManagerResponse response, String accountType,
            String authTokenType, String[] requiredFeatures, boolean expectActivityLaunch,
            Bundle options, int userId) {
        addAccount(response, accountType, authTokenType, requiredFeatures,
                expectActivityLaunch, options);
    }

    @Override
    public void addAccount(IAccountManagerResponse response, String accountType,
            String authTokenType, String[] requiredFeatures, boolean expectActivityLaunch,
            Bundle options) {
        Bundle o = options != null ? new Bundle(options) : new Bundle();
        o.putInt(AccountManager.KEY_CALLER_UID, Binder.getCallingUid());
        o.putInt(AccountManager.KEY_CALLER_PID, Binder.getCallingPid());
        mCalls.execute(() -> {
            Authenticator a = authenticator(accountType);
            if (a == null) {
                error(response, AccountManager.ERROR_CODE_BAD_ARGUMENTS,
                        "no authenticator for " + accountType);
                return;
            }
            Answer answer = call(a, (auth, r) ->
                    auth.addAccount(r, accountType, authTokenType, requiredFeatures, o));
            answer.deliver(response);
        });
    }

    @Override
    public void hasFeatures(IAccountManagerResponse response, Account account,
            String[] features, int userId, String opPackageName) {
        mCalls.execute(() -> {
            Bundle result = new Bundle();
            Boolean has = hasFeatures(account, features);
            if (has == null) {
                error(response, AccountManager.ERROR_CODE_REMOTE_EXCEPTION,
                        "the authenticator of " + (account != null ? account.type : null)
                                + " did not answer");
                return;
            }
            result.putBoolean(AccountManager.KEY_BOOLEAN_RESULT, has);
            reply(response, result);
        });
    }

    @Override
    public void getAccountsByFeatures(IAccountManagerResponse response, String accountType,
            String[] features, String opPackageName) {
        mCalls.execute(() -> {
            Bundle result = new Bundle();
            result.putParcelableArray(AccountManager.KEY_ACCOUNTS,
                    accountsWithFeatures(accountType, features));
            reply(response, result);
        });
    }

    @Override
    public void getAccountByTypeAndFeatures(IAccountManagerResponse response,
            String accountType, String[] features, String opPackageName) {
        mCalls.execute(() -> {
            Account[] accounts = accountsWithFeatures(accountType, features);
            Bundle result = new Bundle();
            if (accounts.length == 1) {
                result.putString(AccountManager.KEY_ACCOUNT_NAME, accounts[0].name);
                result.putString(AccountManager.KEY_ACCOUNT_TYPE, accounts[0].type);
            }
            reply(response, result);
        });
    }

    /** The accounts of a type with every one of {@code features}. */
    private Account[] accountsWithFeatures(String accountType, String[] features) {
        Account[] accounts = filterAccounts(accountType);
        if (features == null || features.length == 0) return accounts;
        List<Account> result = new ArrayList<>();
        for (Account a : accounts) {
            if (Boolean.TRUE.equals(hasFeatures(a, features))) result.add(a);
        }
        return result.toArray(new Account[0]);
    }

    /** Whether {@code account} has {@code features}, as its authenticator says; null: no answer. */
    private Boolean hasFeatures(Account account, String[] features) {
        if (features == null || features.length == 0) return exists(account);
        // The dummy Google account is no authenticator's: it has no features.
        if (!exists(account) || DEFAULT_ACCOUNT.equals(account)) return false;
        Authenticator a = authenticator(account.type);
        if (a == null) return false;
        Answer answer = call(a, (auth, r) -> auth.hasFeatures(r, account, features));
        if (answer.result == null) return null;
        return answer.result.getBoolean(AccountManager.KEY_BOOLEAN_RESULT, false);
    }

    /** The installed authenticators, from each package's manifest and XML. */
    private List<Authenticator> authenticators() {
        List<Authenticator> result = new ArrayList<>();
        Intent intent = new Intent(AccountManager.ACTION_AUTHENTICATOR_INTENT);
        for (ResolveInfo ri : mPm.services(intent, null, PackageManager.GET_META_DATA)) {
            ServiceInfo service = ri.serviceInfo;
            if (service == null || service.metaData == null) continue;
            int xml = service.metaData.getInt(AccountManager.AUTHENTICATOR_META_DATA_NAME);
            if (xml == 0) continue;
            Authenticator a = parse(service, xml);
            if (a != null) result.add(a);
        }
        return result;
    }

    private Authenticator authenticator(String accountType) {
        if (accountType == null) return null;
        for (Authenticator a : authenticators()) {
            if (accountType.equals(a.type)) return a;
        }
        return null;
    }

    /** An {@code account-authenticator} element, read with the app's resources. */
    private static Authenticator parse(ServiceInfo service, int xml) {
        ComponentName component = new ComponentName(service.packageName, service.name);
        try (AssetManager assets = new AssetManager()) {
            if (assets.addAssetPath(service.applicationInfo.sourceDir) == 0) {
                throw new IOException("cannot read APK resources " + service.applicationInfo.sourceDir);
            }
            DisplayMetrics metrics = new DisplayMetrics();
            metrics.setToDefaults();
            Configuration config = new Configuration();
            config.setToDefaults();
            Resources res = new Resources(assets, metrics, config);
            try (XmlResourceParser parser = res.getXml(xml)) {
                int event;
                while ((event = parser.next()) != XmlPullParser.END_DOCUMENT
                        && event != XmlPullParser.START_TAG) {
                }
                if (event != XmlPullParser.START_TAG
                        || !AccountManager.AUTHENTICATOR_ATTRIBUTES_NAME.equals(parser.getName())) {
                    return null;
                }
                String type;
                int typeId = parser.getAttributeResourceValue(ANDROID_NS, "accountType", 0);
                type = typeId != 0 ? res.getString(typeId)
                        : parser.getAttributeValue(ANDROID_NS, "accountType");
                if (type == null || type.isEmpty()) return null;
                return new Authenticator(type, component,
                        parser.getAttributeResourceValue(ANDROID_NS, "label", 0),
                        parser.getAttributeResourceValue(ANDROID_NS, "icon", 0),
                        parser.getAttributeResourceValue(ANDROID_NS, "smallIcon", 0),
                        parser.getAttributeResourceValue(ANDROID_NS, "accountPreferences", 0),
                        parser.getAttributeBooleanValue(ANDROID_NS, "customTokens", false));
            }
        } catch (Exception e) {
            SystemServer.log(TAG + "the authenticator of " + component.flattenToShortString()
                    + " was not read: " + e);
            return null;
        }
    }

    /** A call to an authenticator, given its interface and the response it answers. */
    private interface Call {
        void run(IAccountAuthenticator authenticator, IAccountAuthenticatorResponse response)
                throws RemoteException;
    }

    /** An authenticator's answer: a result, or an error. */
    private static final class Answer extends IAccountAuthenticatorResponse.Stub {
        final CountDownLatch done = new CountDownLatch(1);
        volatile Bundle result;
        volatile int code = AccountManager.ERROR_CODE_REMOTE_EXCEPTION;
        volatile String message = "the authenticator did not answer";

        @Override
        public void onResult(Bundle b) {
            result = b != null ? b : new Bundle();
            done.countDown();
        }

        @Override
        public void onRequestContinued() {
        }

        @Override
        public void onError(int errorCode, String errorMessage) {
            code = errorCode;
            message = errorMessage;
            done.countDown();
        }

        void fail(String why) {
            message = why;
            done.countDown();
        }

        void deliver(IAccountManagerResponse response) {
            if (result != null) reply(response, result);
            else error(response, code, message);
        }
    }

    /** Binds {@code a}'s service, makes the call and waits for its answer (on mCalls). */
    private Answer call(Authenticator a, Call call) {
        Answer answer = new Answer();
        IServiceConnection connection = new IServiceConnection.Stub() {
            @Override
            public void connected(ComponentName name, IBinder binder,
                    android.app.IBinderSession session, boolean dead) {
                if (binder == null) return;
                try {
                    call.run(IAccountAuthenticator.Stub.asInterface(binder), answer);
                } catch (RemoteException | RuntimeException e) {
                    answer.fail("the authenticator failed: " + e);
                }
            }
        };
        int bound;
        long token = Binder.clearCallingIdentity();
        try {
            bound = mAm.bindService(null, null, new Intent(AccountManager.ACTION_AUTHENTICATOR_INTENT)
                    .setComponent(a.service), null, connection, Context.BIND_AUTO_CREATE,
                    "android", 0);
        } catch (Throwable t) {
            SystemServer.log(TAG + "binding " + a.service.flattenToShortString() + " failed: " + t);
            bound = 0;
        } finally {
            Binder.restoreCallingIdentity(token);
        }
        if (bound == 0) {
            answer.fail("the authenticator " + a.service.flattenToShortString()
                    + " could not be bound");
            return answer;
        }
        try {
            if (!answer.done.await(AUTHENTICATOR_TIMEOUT_SECONDS, TimeUnit.SECONDS)) {
                answer.fail("the authenticator " + a.service.flattenToShortString()
                        + " did not answer in " + AUTHENTICATOR_TIMEOUT_SECONDS + " s");
            }
        } catch (InterruptedException e) {
            answer.fail("interrupted");
        } finally {
            try {
                mAm.unbindService(connection);
            } catch (Throwable t) {
                // not bound
            }
        }
        return answer;
    }

    private static void reply(IAccountManagerResponse response, Bundle result) {
        if (response == null) return;
        try {
            response.onResult(result);
        } catch (RemoteException e) {
            SystemServer.log(TAG + "the caller is gone: " + e);
        }
    }

    private static void error(IAccountManagerResponse response, int code, String message) {
        SystemServer.log(TAG + message);
        if (response == null) return;
        try {
            response.onError(code, message);
        } catch (RemoteException e) {
            SystemServer.log(TAG + "the caller is gone: " + e);
        }
    }

    // --- changes and persistence ---------------------------------------------------------

    /** Tells apps, as Android's service does, that the accounts changed. */
    private void changed(Account account, boolean removed) {
        List<Intent> intents = new ArrayList<>();
        intents.add(new Intent(AccountManager.LOGIN_ACCOUNTS_CHANGED_ACTION));
        intents.add(new Intent(AccountManager.ACTION_VISIBLE_ACCOUNTS_CHANGED));
        if (removed) {
            intents.add(new Intent(AccountManager.ACTION_ACCOUNT_REMOVED)
                    .putExtra(AccountManager.KEY_ACCOUNT_NAME, account.name)
                    .putExtra(AccountManager.KEY_ACCOUNT_TYPE, account.type));
        }
        long token = Binder.clearCallingIdentity();
        try {
            for (Intent intent : intents) {
                mAm.broadcastIntentWithFeature(null, null, intent, null, null, 0, null, null,
                        null, null, null, -1, null, false, false, 0);
            }
        } catch (Throwable t) {
            SystemServer.log(TAG + "the account change was not broadcast: " + t);
        } finally {
            Binder.restoreCallingIdentity(token);
        }
    }

    private synchronized void load() {
        Properties p = Device.read(mFile);
        if (p == null) return;
        int count = Integer.parseInt(p.getProperty("count", "0"));
        for (int i = 0; i < count; i++) {
            String name = p.getProperty(i + ".name");
            String type = p.getProperty(i + ".type");
            if (name == null || type == null) continue;
            Entry e = new Entry();
            e.password = p.getProperty(i + ".password");
            String prefix = i + ".data.";
            for (String key : p.stringPropertyNames()) {
                if (key.startsWith(prefix)) {
                    e.data.put(key.substring(prefix.length()), p.getProperty(key));
                }
            }
            mAccounts.put(new Account(name, type), e);
        }
    }

    /** Called with this locked. */
    private void save() {
        Properties p = new Properties();
        int i = 0;
        for (Map.Entry<Account, Entry> a : mAccounts.entrySet()) {
            if (a.getKey().equals(DEFAULT_ACCOUNT)) continue;
            p.setProperty(i + ".name", a.getKey().name);
            p.setProperty(i + ".type", a.getKey().type);
            if (a.getValue().password != null) p.setProperty(i + ".password", a.getValue().password);
            for (Map.Entry<String, String> d : a.getValue().data.entrySet()) {
                p.setProperty(i + ".data." + d.getKey(), d.getValue());
            }
            i++;
        }
        p.setProperty("count", String.valueOf(i));
        try {
            Device.write(mFile, p);
        } catch (IOException e) {
            SystemServer.log(TAG + "the accounts were not saved: " + e);
        }
    }
}
