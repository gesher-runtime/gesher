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
import android.os.SystemClock;
import android.util.DisplayMetrics;

import org.xmlpull.v1.XmlPullParser;

import java.io.File;
import java.io.ByteArrayOutputStream;
import java.io.FileInputStream;
import java.security.MessageDigest;
import java.util.Base64;
import java.util.HashMap;
import java.util.HashSet;
import java.util.Set;
import java.util.WeakHashMap;
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
 * <p>Account secrets belong to their installed authenticator's code identity. Visible
 * clients request real tokens from that authenticator; grants are scoped to client code
 * identity and custom-token expiry. The synthetic Google fallback exists only without
 * an installed GMS package. Apps' own account types work as on Android: {@code addAccount}, {@code hasFeatures} and {@code getAccountsByFeatures}
 * go to the type's authenticator (the app's service for
 * {@link AccountManager#ACTION_AUTHENTICATOR_INTENT}, its type in the
 * {@code account-authenticator} XML of its meta-data), bound through the activity manager;
 * the authenticator adds the account with {@code addAccountExplicitly}. Accounts, their
 * passwords and user data persist in a Windows-DPAPI-protected account store. Existing
 * unowned plaintext records are sealed for recovery and require re-authentication.
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
        String owner, ownerIdentity, password;
        long authenticatedAt;
        final Map<String, String> data = new LinkedHashMap<>();
        final Map<String, String> tokens = new LinkedHashMap<>();
        final Map<String, Integer> visibility = new LinkedHashMap<>();
        final Map<String, Set<String>> tokenGrants = new LinkedHashMap<>();
        final Map<String, CachedToken> customTokens = new LinkedHashMap<>();
    }

    /** An app's authenticator: its account type and its service. */
    private static final class Authenticator {
        final String type;
        final ComponentName service;
        final int label, icon, smallIcon, preferences;
        final boolean customTokens;
        long removalEpoch;

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
    private final CallerIdentity mCallers;
    private final File mLegacyFile = new File(Device.dataDir(), "system/accounts");
    private final AccountSecretStore mStore = new AccountSecretStore(
            new File(Device.dataDir(), "system/accounts.sealed"));
    /** Server-local fixture seam; production always writes through the guarded DPAPI store. */
    private interface StoreWriter { void write(Properties values) throws IOException; }
    private StoreWriter mStoreWriter = mStore::write;
    private String mUnclaimedLegacy;
    private final AccountSessions mSessions = new AccountSessions();
    private final Map<String, Long> mRemovalEpochs = new HashMap<>();
    private final Set<Answer> mAnswers = java.util.Collections.newSetFromMap(new WeakHashMap<>());
    private final Map<ApkPackage, String> mCodeIdentities = new WeakHashMap<>();
    /** Server-local test seam; never exposed through Binder or process settings. */
    private interface VisibilityNotifier {
        void changed(String packageName, String codeIdentity, boolean visibleTransition);
    }
    private VisibilityNotifier mVisibilityNotifier = this::notifyVisibility;

    private static final class CachedToken {
        final String value;
        final long expires;
        CachedToken(String value, long expires) { this.value = value; this.expires = expires; }
    }

    /** Captured on the incoming Binder thread; never trust caller fields in a Bundle. */
    private static final class Caller {
        final String name, code;
        final boolean system;
        final int uid, pid;
        final long removalEpoch;
        Caller(String name, String code, boolean system, int uid, int pid) {
            this(name, code, system, uid, pid, 0);
        }
        Caller(String name, String code, boolean system, int uid, int pid, long removalEpoch) {
            this.name = name; this.code = code; this.system = system; this.uid = uid; this.pid = pid;
            this.removalEpoch = removalEpoch;
        }
        String key() { return name + "\u0000" + code; }
    }
    // Guarded by this.
    private final Map<Account, Entry> mAccounts = new LinkedHashMap<>();
    private State mBeforeMutation;

    private static final class State {
        final Map<Account, Entry> accounts = new LinkedHashMap<>();
        final String legacy;
        State(Map<Account, Entry> source, String legacy) {
            this.legacy = legacy;
            for (Map.Entry<Account, Entry> item : source.entrySet()) {
                Entry old = item.getValue(), copy = new Entry();
                copy.owner = old.owner; copy.ownerIdentity = old.ownerIdentity;
                copy.password = old.password; copy.authenticatedAt = old.authenticatedAt;
                copy.data.putAll(old.data); copy.tokens.putAll(old.tokens);
                copy.visibility.putAll(old.visibility); copy.customTokens.putAll(old.customTokens);
                for (Map.Entry<String, Set<String>> grant : old.tokenGrants.entrySet()) {
                    copy.tokenGrants.put(grant.getKey(), new HashSet<>(grant.getValue()));
                }
                accounts.put(item.getKey(), copy);
            }
        }
    }

    private void beginMutation() {
        if (mBeforeMutation == null) mBeforeMutation = new State(mAccounts, mUnclaimedLegacy);
    }
    // Calls to authenticators wait on their binding and answer: off the binder threads.
    private final java.util.concurrent.ScheduledExecutorService mSessionTimeout =
            Executors.newSingleThreadScheduledExecutor(r -> {
                Thread t = new Thread(r, "account-session-timeout"); t.setDaemon(true); return t;
            });
    private final ExecutorService mCalls = Executors.newCachedThreadPool(r -> {
        Thread t = new Thread(r, "accounts");
        t.setDaemon(true);
        return t;
    });

    public AccountManagerService(ActivityManagerService am, PackageManagerService pm) {
        this(am, pm, true);
    }

    AccountManagerService(ActivityManagerService am, PackageManagerService pm, boolean persistent) {
        mAm = am;
        mPm = pm;
        mCallers = am != null ? am.callers() : new CallerIdentity(pm, null);
        if (pm.get(GmsPackageSupport.PACKAGE_GMS) == null) {
            Entry dummy = new Entry();
            dummy.password = "dummy_password";
            mAccounts.put(DEFAULT_ACCOUNT, dummy);
        }
        if (persistent) load();
        else mStoreWriter = values -> { /* Stand-in accounts live only in this process. */ };
        if (am != null) am.setAccountManager(this);
        pm.onPackageRemovalCheck(this::checkPackageRemoval);
        pm.onPackageRemoval(this::packageRemoved);
    }

    private synchronized long packageEpoch(String name) { return mRemovalEpochs.getOrDefault(name, 0L); }

    private synchronized Caller captureCaller(String name, String code, boolean system, int uid, int pid) {
        if (!system && mPm.isRemovalPending(name)) throw new SecurityException("Account client is being removed");
        return new Caller(name, code, system, uid, pid, packageEpoch(name));
    }

    /** Retire client capabilities before an uninstall can publish removal or permit reinstall. */
    private synchronized void checkPackageRemoval(String name) {
        if (mAccounts.values().removeIf(e -> name.equals(e.owner))) {
            save();
        }
    }

    void packageRemoved(String name) {
        if (name == null || name.isEmpty()) throw new IllegalArgumentException("Missing removed package");
        List<Answer> cancel = new ArrayList<>();
        synchronized (this) {
            checkPackageRemoval(name);
            String prefix = name + "\u0000";
            boolean changed = mAccounts.values().stream().anyMatch(e ->
                    e.visibility.keySet().stream().anyMatch(k -> k.startsWith(prefix))
                    || e.tokenGrants.values().stream().anyMatch(g -> g.stream().anyMatch(k -> k.startsWith(prefix)))
                    || e.customTokens.keySet().stream().anyMatch(k -> k.startsWith(prefix)));
            if (changed) {
                beginMutation();
                for (Entry e : mAccounts.values()) {
                    e.visibility.keySet().removeIf(k -> k.startsWith(prefix));
                    for (Set<String> grants : e.tokenGrants.values()) grants.removeIf(k -> k.startsWith(prefix));
                    e.customTokens.keySet().removeIf(k -> k.startsWith(prefix));
                }
                save(); // Failed persistence restores all grants and stops removal.
            }
            mRemovalEpochs.put(name, packageEpoch(name) + 1);
            mSessions.packageRemoved(name);
            for (Answer answer : mAnswers) {
                if (name.equals(answer.owner.service.getPackageName())
                        || answer.client != null && name.equals(answer.client.name)) cancel.add(answer);
            }
        }
        for (Answer answer : cancel) answer.cancelForRemoval();
        // Provider-owned records/credentials are never deleted by client cleanup.
        AccountManager.invalidateLocalAccountsDataCaches();
    }

    /** Called only after ATM authenticated the source activity, PID and application thread. */
    synchronized ChooserSession prepareChooser(String packageName, int uid, int pid, Intent request) {
        mCallers.requirePackage(packageName);
        String code = packageIdentity(packageName);
        if (code == null) throw new SecurityException("Unknown chooser client code");
        return new ChooserSession(captureCaller(packageName, code, false, uid, pid), request);
    }

    final class ChooserSession {
        private final Caller client;
        private final Set<Account> allowedAccounts;
        private final Set<String> allowedTypes;
        private final Bundle addOptions;
        private final String authTokenType;
        private final String[] requiredFeatures;
        private final Runnable persist;
        private final Map<Account, Entry> displayed = new LinkedHashMap<>();

        ChooserSession(Caller client, Intent request) {
            this(client, request, AccountManagerService.this::save);
        }
        ChooserSession(Caller client, Intent request, Runnable persist) {
            this.client = client;
            this.persist = persist;
            ArrayList<Account> accounts = request.getParcelableArrayListExtra("allowableAccounts", Account.class);
            String[] types = request.getStringArrayExtra("allowableAccountTypes");
            String[] features = request.getStringArrayExtra("addAccountRequiredFeatures");
            requiredFeatures = features == null ? null : features.clone();
            if (accounts != null && accounts.size() > 1024 || types != null && types.length > 64
                    || requiredFeatures != null && requiredFeatures.length > 128) {
                throw new IllegalArgumentException("Account chooser filters too large");
            }
            allowedAccounts = accounts == null ? null : new HashSet<>(accounts);
            allowedTypes = types == null ? null : new HashSet<>(java.util.Arrays.asList(types));
            addOptions = options(client, request.getBundleExtra("addAccountOptions"));
            authTokenType = request.getStringExtra("authTokenType");
        }
        String packageName() { return client.name; }
        boolean active() { synchronized (AccountManagerService.this) { return callerUnchanged(client); } }
        private boolean permittedType(String type) { return allowedTypes == null || allowedTypes.contains(type); }
        private boolean eligible(Account account, Entry entry) {
            if (entry == null || entry.owner == null || entry.ownerIdentity == null
                    || !entry.ownerIdentity.equals(packageIdentity(entry.owner))
                    || !permittedType(account.type)
                    || allowedAccounts != null && !allowedAccounts.contains(account)) return false;
            Authenticator owner = authenticator(account.type);
            if (owner == null || !entry.owner.equals(owner.service.getPackageName())) return false;
            // Unset policy is a manual-choice candidate, while an explicit owner deny is final.
            return owns(client, entry) || !Integer.valueOf(AccountManager.VISIBILITY_NOT_VISIBLE)
                    .equals(entry.visibility.get(client.key()));
        }
        List<Account> accounts() {
            synchronized (AccountManagerService.this) {
                displayed.clear();
                if (callerUnchanged(client)) for (Map.Entry<Account, Entry> item : mAccounts.entrySet()) {
                    if (eligible(item.getKey(), item.getValue())) displayed.put(item.getKey(), item.getValue());
                }
                return new ArrayList<>(displayed.keySet());
            }
        }
        List<String> types() {
            synchronized (AccountManagerService.this) {
                List<String> result = new ArrayList<>();
                if (callerUnchanged(client)) for (Authenticator a : authenticators()) {
                    if (permittedType(a.type) && !result.contains(a.type)) result.add(a.type);
                }
                return result;
            }
        }
        Intent select(Account account, AccountChooser.Lifetime lifetime) {
            synchronized (AccountManagerService.this) {
                Entry entry = displayed.get(account);
                if (!callerUnchanged(client) || entry == null || mAccounts.get(account) != entry
                        || !eligible(account, entry)) return null;
                Integer previous = entry.visibility.get(client.key());
                int previousEffective = visibility(client, entry);
                boolean[] applied = {false};
                boolean committed = lifetime.whileActive(() -> {
                    if (!callerUnchanged(client) || !eligible(account, entry)) return;
                    beginMutation();
                    entry.visibility.put(client.key(), AccountManager.VISIBILITY_USER_MANAGED_VISIBLE);
                    persist.run();
                    applied[0] = true;
                });
                if (!committed || !applied[0] || !Integer.valueOf(AccountManager.VISIBILITY_USER_MANAGED_VISIBLE)
                        .equals(entry.visibility.get(client.key()))) return null;
                visibilityCommitted(client, previous, previousEffective,
                        AccountManager.VISIBILITY_USER_MANAGED_VISIBLE, visibility(client, entry));
                return new Intent().putExtra(AccountManager.KEY_ACCOUNT_NAME, account.name)
                        .putExtra(AccountManager.KEY_ACCOUNT_TYPE, account.type);
            }
        }
        boolean add(String type, AccountChooser.Launcher launcher, java.util.function.BooleanSupplier active) {
            if (!types().contains(type) || !active.getAsBoolean()) return false;
            Authenticator owner = authenticator(type);
            if (owner == null) return false;
            String ownerCode = packageIdentity(owner.service.getPackageName());
            Answer answer = call(owner, (auth, response) -> auth.addAccount(response, type,
                    authTokenType, requiredFeatures, new Bundle(addOptions)), client);
            CountDownLatch done = new CountDownLatch(1);
            java.util.concurrent.atomic.AtomicBoolean success = new java.util.concurrent.atomic.AtomicBoolean();
            java.util.concurrent.atomic.AtomicBoolean launched = new java.util.concurrent.atomic.AtomicBoolean();
            answer.deliverSession(new IAccountManagerResponse.Stub() {
                @Override public void onResult(Bundle result) {
                    if (!active.getAsBoolean()) { done.countDown(); return; }
                    Intent intent = result.getParcelable(AccountManager.KEY_INTENT, Intent.class);
                    if (intent == null) { success.set(true); done.countDown(); }
                    else if (!launched.compareAndSet(false, true)
                            || !launcher.launch(intent, () -> { success.set(true); done.countDown(); })) done.countDown();
                }
                @Override public void onError(int code, String message) { done.countDown(); }
            }, value -> {
                Authenticator current = authenticator(type);
                if (!active.getAsBoolean() || !callerUnchanged(client) || current == null
                        || !current.service.equals(owner.service) || ownerCode == null
                        || !ownerCode.equals(packageIdentity(owner.service.getPackageName()))) return null;
                // Unlike addAccount, adding through the chooser grants no visibility.
                return validatedResult(owner, value, null);
            });
            try {
                long end = SystemClock.elapsedRealtime() + TimeUnit.MINUTES.toMillis(10);
                while (!done.await(250, TimeUnit.MILLISECONDS)) {
                    if (!active.getAsBoolean() || SystemClock.elapsedRealtime() >= end) return false;
                }
                return success.get() && active.getAsBoolean();
            } catch (InterruptedException e) { Thread.currentThread().interrupt(); return false; }
        }
    }

    private synchronized Caller caller(String claimed) {
        if (claimed != null) mCallers.requirePackage(claimed);
        boolean system = mCallers.isSystem();
        String name = mCallers.callerPackage();
        String code = name != null ? packageIdentity(name) : null;
        if (!system && (name == null || code == null)) throw new SecurityException("Unknown account caller");
        return captureCaller(name, code, system, mCallers.callerUid(), mCallers.callerPid());
    }

    private synchronized String packageIdentity(String name) {
        ApkPackage pkg = mPm.get(name);
        if (pkg == null || mPm.isRemovalPending(name)) return null;
        if (mCodeIdentities.containsKey(pkg)) return mCodeIdentities.get(pkg);
        // A verified rotation may explicitly retain account authentication rights.
        String code = pkg.verifiedAuthSignerSha256();
        if (code != null) code = "cert:" + code;
        else if (pkg.parsed.getSigningDetails() == null
                || pkg.parsed.getSigningDetails().getSignatures() == null) {
            // Bind every installed split as well as base.apk. An unsigned update
            // cannot inherit credentials by keeping base unchanged and replacing a split.
            code = NotificationManagerService.codeIdentity(pkg);
        }
        mCodeIdentities.put(pkg, code);
        return code;
    }

    private static boolean synthetic(Account account, Entry e) {
        return DEFAULT_ACCOUNT.equals(account) && e != null && e.owner == null && e.ownerIdentity == null;
    }

    private synchronized boolean dummy(Account account) {
        return synthetic(account, mAccounts.get(account)) && mPm.get(GmsPackageSupport.PACKAGE_GMS) == null;
    }

    private boolean owns(Caller c, Entry e) {
        return c.system || e != null && c.name != null && c.code != null && c.name.equals(e.owner)
                && c.code.equals(e.ownerIdentity);
    }

    private void requireOwner(Caller c, Entry e) {
        if (e == null || !owns(c, e)) throw new SecurityException("Account belongs to its authenticator");
    }

    private int visibility(Caller c, Entry e) {
        if (e == null) return AccountManager.VISIBILITY_NOT_VISIBLE;
        if (owns(c, e)) return AccountManager.VISIBILITY_VISIBLE;
        return e.visibility.getOrDefault(c.key(), AccountManager.VISIBILITY_NOT_VISIBLE);
    }

    private boolean visible(Caller c, Account account, Entry e) {
        if (dummy(account)) return true;
        int value = visibility(c, e);
        return value == AccountManager.VISIBILITY_VISIBLE
                || value == AccountManager.VISIBILITY_USER_MANAGED_VISIBLE;
    }

    private static void user(int id) {
        if (id != 0) throw new SecurityException("Only the current Gesher user is supported");
    }

    private String targetKey(String packageName) {
        String code = packageIdentity(packageName);
        if (code == null) throw new SecurityException("Unknown account client package");
        return packageName + "\u0000" + code;
    }

    // --- accounts ----------------------------------------------------------------------

    @Override
    public Account[] getAccountsAsUser(String type, int userId, String opPackageName) {
        user(userId);
        return filterAccounts(type, caller(opPackageName));
    }

    @Override
    public Account[] getAccountsForPackage(String packageName, int uid, String opPackageName) {
        Caller c = caller(opPackageName);
        if (!c.system && (!packageName.equals(c.name) || uid != c.uid)) {
            throw new SecurityException("Cannot enumerate another account client");
        }
        return filterAccounts(null, c.system ? targetCaller(packageName) : c);
    }

    private synchronized Caller targetCaller(String name) {
        ApkPackage target = mPm.get(name);
        return captureCaller(name, packageIdentity(name), false, target == null ? -1 : target.uid(), -1);
    }

    @Override
    public Account[] getAccountsByTypeForPackage(String type, String packageName,
            String opPackageName) {
        return accountsByTypeForClient(type, packageName, caller(opPackageName));
    }

    private synchronized Account[] accountsByTypeForClient(String type, String packageName,
            Caller c) {
        if (c.system) return filterAccounts(type, targetCaller(packageName));
        if (packageName.equals(c.name)) return filterAccounts(type, c);
        // Authenticators query which of their own accounts a client may already
        // see (microG's account provider uses this for calling applications).
        // This typed delegation never grants visibility or credential access.
        Authenticator authenticator = authenticator(type);
        if (authenticator == null || !authenticator.service.getPackageName().equals(c.name)
                || !callerUnchanged(c)) throw new SecurityException("Account client mismatch");
        Caller target = targetCaller(packageName);
        if (target.code == null) throw new SecurityException("Unknown account client package");
        List<Account> result = new ArrayList<>();
        for (Map.Entry<Account, Entry> item : mAccounts.entrySet()) {
            Account account = item.getKey(); Entry entry = item.getValue();
            if (type.equals(account.type) && owns(c, entry) && visible(target, account, entry)) {
                result.add(account);
            }
        }
        return result.toArray(new Account[0]);
    }

    @Override
    public synchronized Map<String, Integer> getAccountsAndVisibilityForPackage(String packageName,
            String accountType) {
        return accountVisibilities(packageName, accountType, caller(null));
    }

    private synchronized Map<String, Integer> accountVisibilities(String packageName,
            String accountType, Caller c) {
        Caller target = packageName.equals(c.name) ? c : targetCaller(packageName);
        Map<String, Integer> result = new LinkedHashMap<>();
        for (Map.Entry<Account, Entry> item : mAccounts.entrySet()) {
            Account a = item.getKey(); Entry e = item.getValue();
            if (accountType != null && !accountType.equals(a.type)) continue;
            if (!c.system && !owns(c, e)
                    && (!packageName.equals(c.name) || !visible(c, a, e))) continue;
            if (!dummy(a)) result.put(a.name, visibility(target, e));
        }
        return result;
    }

    @Override
    public synchronized int getAccountVisibility(Account account, String packageName) {
        Caller c = caller(null); Entry e = mAccounts.get(account);
        if (e == null) return AccountManager.VISIBILITY_NOT_VISIBLE;
        if (!owns(c, e) && !packageName.equals(c.name)) throw new SecurityException("Account client mismatch");
        return visibility(targetCaller(packageName), e);
    }

    @Override
    public synchronized Map<String, Integer> getPackagesAndVisibilityForAccount(Account account) {
        Entry e = mAccounts.get(account); requireOwner(caller(null), e);
        Map<String, Integer> result = new LinkedHashMap<>();
        result.put(e.owner, AccountManager.VISIBILITY_VISIBLE);
        for (Map.Entry<String, Integer> item : e.visibility.entrySet()) {
            int split = item.getKey().indexOf('\u0000');
            if (split > 0) result.put(item.getKey().substring(0, split), item.getValue());
        }
        return result;
    }

    private static void validateVisibility(int value) {
        if (value != AccountManager.VISIBILITY_VISIBLE
                && value != AccountManager.VISIBILITY_USER_MANAGED_VISIBLE
                && value != AccountManager.VISIBILITY_NOT_VISIBLE
                && value != AccountManager.VISIBILITY_USER_MANAGED_NOT_VISIBLE
                && value != AccountManager.VISIBILITY_UNDEFINED) throw new IllegalArgumentException("visibility");
    }

    @Override
    public synchronized boolean setAccountVisibility(Account account, String packageName, int value) {
        Entry e = mAccounts.get(account); requireOwner(caller(null), e);
        validateVisibility(value);
        String key = targetKey(packageName);
        Caller target = targetCaller(packageName);
        Integer previous = e.visibility.get(key);
        int previousEffective = visibility(target, e);
        beginMutation();
        e.visibility.put(key, value);
        if (value != AccountManager.VISIBILITY_VISIBLE && value != AccountManager.VISIBILITY_USER_MANAGED_VISIBLE) {
            for (Set<String> grants : e.tokenGrants.values()) grants.remove(key);
            e.customTokens.keySet().removeIf(k -> k.startsWith(key + "\u0000"));
        }
        save();
        visibilityCommitted(target, previous, previousEffective, value, visibility(target, e));
        return true;
    }

    private static boolean visibleValue(int value) {
        return value == AccountManager.VISIBILITY_VISIBLE
                || value == AccountManager.VISIBILITY_USER_MANAGED_VISIBLE;
    }

    /** A failed persistence operation never invalidates or announces a new grant. */
    private void visibilityCommitted(Caller target, Integer previous, int previousEffective,
            int value, int currentEffective) {
        invalidateAccountListCache();
        if (previous == null || previous.intValue() != value) {
            mVisibilityNotifier.changed(target.name, target.code,
                    visibleValue(previousEffective) != visibleValue(currentEffective));
        }
    }

    private void invalidateAccountListCache() {
        try { AccountManager.invalidateLocalAccountsDataCaches(); }
        catch (RuntimeException e) {
            SystemServer.log(TAG + "account cache invalidation unavailable: " + e.getClass().getSimpleName());
        }
    }

    private void invalidateAccountUserDataCache() {
        try { AccountManager.invalidateLocalAccountUserDataCaches(); }
        catch (RuntimeException e) {
            SystemServer.log(TAG + "account user-data cache invalidation unavailable: " + e.getClass().getSimpleName());
        }
    }

    private void notifyVisibility(String packageName, String codeIdentity, boolean visibleTransition) {
        if (mAm == null) return;
        // Deliver after releasing the account/lifetime locks. No account name or
        // credential travels in either event, including on visibility revocation.
        mCalls.execute(() -> {
            synchronized (AccountManagerService.this) {
                if (!codeIdentity.equals(packageIdentity(packageName))) return;
            }
            long token = Binder.clearCallingIdentity();
            try {
                if (visibleTransition) {
                    Intent visible = new Intent(AccountManager.ACTION_VISIBLE_ACCOUNTS_CHANGED)
                            .setPackage(packageName).addFlags(Intent.FLAG_RECEIVER_REGISTERED_ONLY);
                    mAm.broadcastIntentWithFeature(null, null, visible, null, null, 0, null,
                            null, null, null, null, -1, null, false, false, 0);
                }
                Intent changed = new Intent(AccountManager.LOGIN_ACCOUNTS_CHANGED_ACTION)
                        .setPackage(packageName);
                mAm.broadcastIntentWithFeature(null, null, changed, null, null, 0, null,
                        null, null, null, null, -1, null, false, false, 0);
            } catch (RuntimeException e) {
                SystemServer.log(TAG + "account visibility notification unavailable: " + e.getClass().getSimpleName());
            } finally { Binder.restoreCallingIdentity(token); }
        });
    }

    @Override
    public synchronized boolean hasAccountAccess(Account account, String packageName,
            android.os.UserHandle userHandle) {
        if (userHandle != null) user(userHandle.getIdentifier());
        Caller c = caller(null);
        if (!c.system && !packageName.equals(c.name)) {
            requireOwner(c, mAccounts.get(account));
        }
        Entry e = mAccounts.get(account);
        return e != null && visible(targetCaller(packageName), account, e);
    }

    private synchronized Account[] filterAccounts(String type, Caller c) {
        if (!callerUnchanged(c)) return new Account[0];
        List<Account> list = new ArrayList<>();
        for (Map.Entry<Account, Entry> item : mAccounts.entrySet()) {
            Account a = item.getKey();
            if (synthetic(a, item.getValue()) && !dummy(a)) continue;
            if ((type == null || type.equals(a.type)) && visible(c, a, item.getValue())) list.add(a);
        }
        return list.toArray(new Account[0]);
    }

    synchronized boolean canAccessAccount(Account account, String packageName) {
        Entry e = mAccounts.get(account);
        return e != null && visible(targetCaller(packageName), account, e);
    }

    synchronized Account[] syncableAccounts() {
        List<Account> list = new ArrayList<>(mAccounts.keySet());
        if (synthetic(DEFAULT_ACCOUNT, mAccounts.get(DEFAULT_ACCOUNT))) list.remove(DEFAULT_ACCOUNT);
        return list.toArray(new Account[0]);
    }

    private synchronized boolean exists(Account account) {
        return account != null && mAccounts.containsKey(account);
    }

    @Override
    public boolean addAccountExplicitly(Account account, String password, Bundle extras,
            String opPackageName) {
        return addOwnedAccount(account, password, extras, null, opPackageName);
    }

    private boolean addOwnedAccount(Account account, String password, Bundle extras,
            Map initialVisibility, String opPackageName) {
        if (account == null) return false;
        Caller c = caller(opPackageName);
        Authenticator owner = authenticator(account.type);
        if (owner == null) throw new SecurityException("No trusted authenticator for account type");
        return commitOwnedAccount(c, owner, account, password, extras, initialVisibility);
    }

    private boolean commitOwnedAccount(Caller c, Authenticator owner, Account account,
            String password, Bundle extras, Map initialVisibility) {
        synchronized (this) {
            if (!callerUnchanged(c) || !authenticatorUnchanged(owner)) {
                throw new SecurityException("Account owner or caller is being removed");
            }
            String ownerName = owner.service.getPackageName();
            String ownerIdentity = packageIdentity(ownerName);
            if (ownerIdentity == null || !c.system && (!ownerName.equals(c.name) || !ownerIdentity.equals(c.code))) {
                throw new SecurityException("Only a current authenticator can create its accounts");
            }
            if (mAccounts.containsKey(account)) return false;
            Entry e = new Entry(); e.owner = ownerName; e.ownerIdentity = ownerIdentity; e.password = password;
            if (extras != null) {
                for (String key : extras.keySet()) {
                    Object value = extras.get(key); if (value != null) e.data.put(key, value.toString());
                }
            }
            if (initialVisibility != null) for (Object target : initialVisibility.keySet()) {
                Object value = initialVisibility.get(target);
                if (!(target instanceof String) || !(value instanceof Integer)) {
                    throw new IllegalArgumentException("Invalid account visibility");
                }
                validateVisibility((Integer) value);
                e.visibility.put(targetKey((String) target), (Integer) value);
            }
            beginMutation(); mAccounts.put(account, e); save();
        }
        changed(account, false); return true;
    }

    @Override
    public boolean addAccountExplicitlyWithVisibility(Account account, String password,
            Bundle extras, Map visibility, String opPackageName) {
        return addOwnedAccount(account, password, extras, visibility, opPackageName);
    }

    @Override
    public boolean removeAccountExplicitly(Account account) {
        if (account == null) return false;
        synchronized (this) { if (synthetic(account, mAccounts.get(account))) return false; }
        Entry removed;
        synchronized (this) {
            removed = mAccounts.get(account); requireOwner(caller(null), removed);
            beginMutation(); mAccounts.remove(account); save();
        }
        changed(account, true, removed); return true;
    }

    @Override
    public void removeAccountAsUser(IAccountManagerResponse response, Account account,
            boolean expectActivityLaunch, int userId) {
        user(userId);
        Bundle result = new Bundle();
        result.putBoolean(AccountManager.KEY_BOOLEAN_RESULT, removeAccountExplicitly(account));
        reply(response, result);
    }

    @Override
    public void renameAccount(IAccountManagerResponse response, Account account, String newName) {
        if (newName == null || newName.isEmpty()) throw new IllegalArgumentException("account name");
        Account renamed = new Account(newName, account.type);
        synchronized (this) {
            Entry e = mAccounts.get(account); requireOwner(caller(null), e);
            if (mAccounts.containsKey(renamed)) {
                error(response, AccountManager.ERROR_CODE_BAD_ARGUMENTS, "Account already exists"); return;
            }
            beginMutation(); e.data.put("gesher.previousAccountName", account.name);
            e.tokens.clear(); e.customTokens.clear();
            mAccounts.remove(account); mAccounts.put(renamed, e); save();
        }
        changed(renamed, false);
        Bundle result = new Bundle();
        result.putString(AccountManager.KEY_ACCOUNT_NAME, renamed.name);
        result.putString(AccountManager.KEY_ACCOUNT_TYPE, renamed.type); reply(response, result);
    }

    @Override
    public synchronized String getPreviousName(Account account) {
        Entry e = mAccounts.get(account); requireOwner(caller(null), e);
        return e.data.get("gesher.previousAccountName");
    }

    // --- passwords, tokens and user data -------------------------------------------------

    private Bundle options(Caller c, Bundle supplied) {
        Bundle out = supplied != null ? new Bundle(supplied) : new Bundle();
        String claimed = out.getString(AccountManager.KEY_ANDROID_PACKAGE_NAME);
        if (!c.system && claimed != null && !claimed.equals(c.name)) throw new SecurityException("Forged account caller");
        out.putString(AccountManager.KEY_ANDROID_PACKAGE_NAME, c.name);
        out.putInt(AccountManager.KEY_CALLER_UID, c.uid);
        out.putInt(AccountManager.KEY_CALLER_PID, c.pid);
        return out;
    }

    private static Bundle tokenResult(Account account, String token) {
        Bundle result = new Bundle();
        result.putString(AccountManager.KEY_ACCOUNT_NAME, account.name);
        result.putString(AccountManager.KEY_ACCOUNT_TYPE, account.type);
        result.putString(AccountManager.KEY_AUTHTOKEN, token); return result;
    }

    private static native boolean nativeConfirmToken(String appPackage, String accountDisplay,
            String tokenType);

    private boolean tokenAuthorized(Caller c, Entry e, String type) {
        Set<String> grants = e.tokenGrants.get(type);
        return owns(c, e) || grants != null && grants.contains(c.key());
    }

    private synchronized boolean callerUnchanged(Caller c) {
        if (c.system) return true;
        ApkPackage current = mPm.get(c.name);
        return current != null && c.code != null && !mPm.isRemovalPending(c.name) && c.removalEpoch == packageEpoch(c.name)
                && c.code.equals(packageIdentity(c.name))
                && (!GmsSignatureSpoofing.isEnabled() || current.uid() == c.uid);
    }

    private synchronized boolean authenticatorUnchanged(Authenticator a) {
        return a != null && !mPm.isRemovalPending(a.service.getPackageName())
                && a.removalEpoch == packageEpoch(a.service.getPackageName());
    }

    private boolean grantTokenInteractively(IAccountManagerResponse response, Account account,
            String type, Caller c, Authenticator a, boolean interactive) {
        if (!interactive) {
            error(response, AccountManager.ERROR_CODE_USER_RESTRICTED,
                    "Account visibility does not grant access to credentials");
            return false;
        }
        boolean consent;
        try {
            AccountSecretStore.ensureLoaded();
            consent = nativeConfirmToken(c.name, account.name, type);
        } catch (RuntimeException | LinkageError e) {
            error(response, AccountManager.ERROR_CODE_CANCELED, "Account credential consent unavailable");
            return false;
        }
        if (!consent) {
            error(response, AccountManager.ERROR_CODE_CANCELED, "Account credential access declined");
            return false;
        }
        synchronized (this) {
            Entry e = mAccounts.get(account);
            if (e == null || !callerUnchanged(c) || !authenticatorUnchanged(a) || !visible(c, account, e)
                    || !a.service.getPackageName().equals(e.owner)
                    || !e.ownerIdentity.equals(packageIdentity(e.owner))) {
                error(response, AccountManager.ERROR_CODE_CANCELED, "Account access changed during consent");
                return false;
            }
            beginMutation();
            e.tokenGrants.computeIfAbsent(type, key -> new HashSet<>()).add(c.key());
            try { save(); }
            catch (RuntimeException e2) {
                error(response, AccountManager.ERROR_CODE_REMOTE_EXCEPTION, "Credential grant could not be saved");
                return false;
            }
            return true;
        }
    }

    @Override
    public void getAuthToken(IAccountManagerResponse response, Account account,
            String authTokenType, boolean notifyOnAuthFailure, boolean expectActivityLaunch,
            Bundle supplied) {
        if (response == null) return;
        Caller c = caller(null);
        Bundle o = options(c, supplied);
        if (account == null || authTokenType == null) {
            error(response, AccountManager.ERROR_CODE_BAD_ARGUMENTS, "An account and token type are required"); return;
        }
        synchronized (this) {
            Entry e = mAccounts.get(account);
            if (dummy(account)) { reply(response, tokenResult(account, dummyToken(account))); return; }
            if (e == null || !visible(c, account, e)) throw new SecurityException("Account is not visible to this caller");
        }
        mCalls.execute(() -> {
            Authenticator a = authenticator(account.type);
            boolean needsGrant;
            synchronized (this) {
                Entry e = mAccounts.get(account);
                if (a == null || e == null || !a.service.getPackageName().equals(e.owner)
                        || !e.ownerIdentity.equals(packageIdentity(e.owner)) || !visible(c, account, e)
                        || !callerUnchanged(c)) {
                    error(response, AccountManager.ERROR_CODE_BAD_AUTHENTICATION, "Account authenticator unavailable"); return;
                }
                CachedToken custom = e.customTokens.get(c.key() + "\u0000" + authTokenType);
                if (a.customTokens && custom != null && custom.expires > System.currentTimeMillis()) {
                    reply(response, tokenResult(account, custom.value)); return;
                }
                needsGrant = !a.customTokens && !tokenAuthorized(c, e, authTokenType);
                String cached = e.tokens.get(authTokenType);
                if (!a.customTokens && cached != null && !needsGrant) {
                    reply(response, tokenResult(account, cached)); return;
                }
            }
            if (needsGrant && !grantTokenInteractively(response, account, authTokenType, c, a, expectActivityLaunch)) return;
            synchronized (this) {
                Entry e = mAccounts.get(account);
                if (e == null || !callerUnchanged(c) || !visible(c, account, e)
                        || !e.ownerIdentity.equals(packageIdentity(e.owner))
                        || !a.customTokens && !tokenAuthorized(c, e, authTokenType)) {
                    error(response, AccountManager.ERROR_CODE_CANCELED, "Account credential access revoked"); return;
                }
            }
            Answer answer = call(a, (auth, r) -> auth.getAuthToken(r, account, authTokenType, o), c);
            answer.deliverSession(response, b -> {
                Bundle result = validatedResult(a, b, account, true);
                if (result == null) return null;
                String token = result.getString(AccountManager.KEY_AUTHTOKEN);
                if (token != null) {
                    synchronized (this) {
                        Entry e = mAccounts.get(account);
                        if (e == null || !callerUnchanged(c) || !visible(c, account, e)
                                || !e.ownerIdentity.equals(packageIdentity(e.owner))
                                || !a.customTokens && !tokenAuthorized(c, e, authTokenType)) return null;
                        if (a.customTokens) {
                            long expiry = result.getLong("android.accounts.expiry", 0);
                            if (expiry > System.currentTimeMillis()) {
                                e.customTokens.put(c.key() + "\u0000" + authTokenType, new CachedToken(token, expiry));
                            }
                        } else {
                            beginMutation(); e.tokens.put(authTokenType, token);
                            save();
                        }
                    }
                }
                return result;
            });
        });
    }

    private static String dummyToken(Account account) {
        return "dummy_token_gesher_" + account.name.replace("@", "_at_");
    }

    @Override
    public synchronized String peekAuthToken(Account account, String authTokenType) {
        if (dummy(account)) return dummyToken(account);
        Entry e = mAccounts.get(account); requireOwner(caller(null), e);
        return e.tokens.get(authTokenType);
    }

    @Override
    public synchronized String getPassword(Account account) {
        Entry e = mAccounts.get(account);
        if (dummy(account)) return e != null ? e.password : null;
        requireOwner(caller(null), e); return e.password;
    }

    @Override
    public synchronized void setPassword(Account account, String password) {
        Entry e = mAccounts.get(account); requireOwner(caller(null), e);
        beginMutation(); e.password = password; e.tokens.clear(); e.customTokens.clear(); save();
    }

    @Override
    public void clearPassword(Account account) { setPassword(account, null); }

    @Override
    public synchronized String getUserData(Account account, String key) {
        Entry e = mAccounts.get(account);
        if (dummy(account)) return e != null ? e.data.getOrDefault(key, "dummy_user_data") : null;
        requireOwner(caller(null), e); return e.data.get(key);
    }

    @Override
    public synchronized void setUserData(Account account, String key, String value) {
        Entry e = mAccounts.get(account); requireOwner(caller(null), e);
        if (key == null) throw new IllegalArgumentException("key");
        beginMutation();
        if (value == null) e.data.remove(key); else e.data.put(key, value);
        save();
        invalidateAccountUserDataCache();
    }

    @Override
    public synchronized void setAuthToken(Account account, String authTokenType, String authToken) {
        Entry e = mAccounts.get(account); requireOwner(caller(null), e);
        if (authTokenType == null) throw new IllegalArgumentException("token type");
        beginMutation();
        if (authToken == null) e.tokens.remove(authTokenType); else e.tokens.put(authTokenType, authToken);
        e.customTokens.clear(); save();
    }

    @Override
    public synchronized void invalidateAuthToken(String accountType, String authToken) {
        if (authToken == null) throw new IllegalArgumentException("token");
        Caller c = caller(null);
        beginMutation();
        for (Map.Entry<Account, Entry> item : mAccounts.entrySet()) {
            if (!item.getKey().type.equals(accountType)) continue;
            Entry e = item.getValue();
            for (String type : new ArrayList<>(e.tokens.keySet())) {
                Set<String> grants = e.tokenGrants.get(type);
                if (authToken.equals(e.tokens.get(type)) && (owns(c, e) || grants != null && grants.contains(c.key()))) {
                    e.tokens.remove(type);
                }
            }
            e.customTokens.entrySet().removeIf(item2 -> authToken.equals(item2.getValue().value)
                    && (owns(c, e) || item2.getKey().startsWith(c.key() + "\u0000")));
        }
        save();
    }

    @Override
    public synchronized void updateAppPermission(Account account, String tokenType, int uid, boolean value) {
        Caller c = caller(null);
        if (!c.system) throw new SecurityException("Only system may change token grants");
        if (tokenType == null) throw new IllegalArgumentException("token type");
        Entry e = mAccounts.get(account);
        if (e == null) return;
        String[] packages = mPm.getPackagesForUid(uid);
        List<String> keys = new ArrayList<>();
        if (packages != null) for (String name : packages) keys.add(targetKey(name));
        beginMutation();
        for (String key : keys) {
            Set<String> grants = e.tokenGrants.computeIfAbsent(tokenType, k -> new HashSet<>());
            if (value) grants.add(key); else grants.remove(key);
        }
        save();
    }

    @Override
    public void registerAccountListener(String[] accountTypes, String opPackageName) { caller(opPackageName); }
    @Override
    public void unregisterAccountListener(String[] accountTypes, String opPackageName) { caller(opPackageName); }

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
            Bundle supplied, int userId) {
        user(userId);
        addAccount(response, accountType, authTokenType, requiredFeatures, expectActivityLaunch, supplied);
    }

    @Override
    public void addAccount(IAccountManagerResponse response, String accountType,
            String authTokenType, String[] requiredFeatures, boolean expectActivityLaunch,
            Bundle supplied) {
        Caller c = caller(null); Bundle o = options(c, supplied);
        mCalls.execute(() -> {
            Authenticator a = authenticator(accountType);
            if (a == null) { error(response, AccountManager.ERROR_CODE_REMOTE_EXCEPTION, "Authenticator unavailable"); return; }
            Answer answer = call(a, (auth, r) -> auth.addAccount(r, accountType, authTokenType, requiredFeatures, o), c);
            answer.deliverSession(response, b -> {
                Bundle result = validatedResult(a, b, null);
                if (result == null) return null;
                String name = result.getString(AccountManager.KEY_ACCOUNT_NAME);
                String type = result.getString(AccountManager.KEY_ACCOUNT_TYPE);
                if (name != null && type != null) {
                    if (!accountType.equals(type)) return null;
                    synchronized (this) {
                        Entry entry = mAccounts.get(new Account(name, type));
                        if (!callerUnchanged(c) || !authenticatorUnchanged(a)
                                || entry == null || !a.service.getPackageName().equals(entry.owner)
                                || !entry.ownerIdentity.equals(packageIdentity(entry.owner))) return null;
                        beginMutation();
                        if (!c.system) entry.visibility.put(c.key(), AccountManager.VISIBILITY_VISIBLE);
                        save();
                    }
                }
                return result;
            });
        });
    }

    @Override
    public void hasFeatures(IAccountManagerResponse response, Account account,
            String[] features, int userId, String opPackageName) {
        user(userId); Caller c = caller(opPackageName);
        synchronized (this) {
            if (!visible(c, account, mAccounts.get(account))) throw new SecurityException("Account is not visible");
        }
        mCalls.execute(() -> {
            Bundle result = new Bundle(); Boolean has = hasFeatures(account, features);
            if (has == null) { error(response, AccountManager.ERROR_CODE_REMOTE_EXCEPTION, "Authenticator did not answer"); return; }
            result.putBoolean(AccountManager.KEY_BOOLEAN_RESULT, has); reply(response, result);
        });
    }

    @Override
    public void getAccountsByFeatures(IAccountManagerResponse response, String accountType,
            String[] features, String opPackageName) {
        Caller c = caller(opPackageName);
        mCalls.execute(() -> {
            Bundle result = new Bundle();
            result.putParcelableArray(AccountManager.KEY_ACCOUNTS, accountsWithFeatures(accountType, features, c));
            reply(response, result);
        });
    }

    @Override
    public void getAccountByTypeAndFeatures(IAccountManagerResponse response,
            String accountType, String[] features, String opPackageName) {
        Caller c = caller(opPackageName);
        mCalls.execute(() -> {
            Account[] accounts = accountsWithFeatures(accountType, features, c);
            Bundle result = new Bundle();
            if (accounts.length == 1) {
                result.putString(AccountManager.KEY_ACCOUNT_NAME, accounts[0].name);
                result.putString(AccountManager.KEY_ACCOUNT_TYPE, accounts[0].type);
            }
            reply(response, result);
        });
    }

    private Account[] accountsWithFeatures(String type, String[] features, Caller c) {
        Account[] accounts = filterAccounts(type, c);
        if (features == null || features.length == 0) return accounts;
        List<Account> result = new ArrayList<>();
        for (Account a : accounts) if (Boolean.TRUE.equals(hasFeatures(a, features))) result.add(a);
        return result.toArray(new Account[0]);
    }

    private Boolean hasFeatures(Account account, String[] features) {
        if (features == null || features.length == 0) return exists(account);
        if (!exists(account) || DEFAULT_ACCOUNT.equals(account)) return false;
        Authenticator a = authenticator(account.type);
        if (a == null) return false;
        synchronized (this) {
            Entry e = mAccounts.get(account);
            if (!a.service.getPackageName().equals(e.owner) || !e.ownerIdentity.equals(packageIdentity(e.owner))) return false;
        }
        Answer answer = call(a, (auth, r) -> auth.hasFeatures(r, account, features));
        return answer.result != null ? answer.result.getBoolean(AccountManager.KEY_BOOLEAN_RESULT, false) : null;
    }

    @Override
    public void confirmCredentialsAsUser(IAccountManagerResponse response, Account account,
            Bundle supplied, boolean expectActivityLaunch, int userId) {
        user(userId); Caller c = caller(null); Bundle o = options(c, supplied);
        authenticatedCall(response, account, c, (auth, r) -> auth.confirmCredentials(r, account, o));
    }

    @Override
    public void updateCredentials(IAccountManagerResponse response, Account account,
            String tokenType, boolean expectActivityLaunch, Bundle supplied) {
        Caller c = caller(null); Bundle o = options(c, supplied);
        authenticatedCall(response, account, c, (auth, r) -> auth.updateCredentials(r, account, tokenType, o));
    }

    private void authenticatedCall(IAccountManagerResponse response, Account account, Caller c, Call operation) {
        synchronized (this) {
            Entry e = mAccounts.get(account);
            if (e == null || !visible(c, account, e)) throw new SecurityException("Account is not visible");
        }
        mCalls.execute(() -> {
            Authenticator a = authenticator(account.type);
            synchronized (this) {
                Entry e = mAccounts.get(account);
                if (a == null || e == null || !a.service.getPackageName().equals(e.owner)
                        || !e.ownerIdentity.equals(packageIdentity(e.owner))) {
                    error(response, AccountManager.ERROR_CODE_BAD_AUTHENTICATION, "Authenticator unavailable"); return;
                }
            }
            call(a, operation, c).deliverSession(response, b -> validatedResult(a, b, account));
        });
    }

    @Override
    public void editProperties(IAccountManagerResponse response, String accountType,
            boolean expectActivityLaunch) {
        Caller c = caller(null); Authenticator a = authenticator(accountType);
        if (a == null) { error(response, AccountManager.ERROR_CODE_REMOTE_EXCEPTION, "Authenticator unavailable"); return; }
        if (!c.system && (!a.service.getPackageName().equals(c.name)
                || !packageIdentity(c.name).equals(c.code))) throw new SecurityException("Authenticator owner required");
        mCalls.execute(() -> call(a, (auth, r) -> auth.editProperties(r, accountType), c)
                .deliverSession(response, b -> validatedResult(a, b, null)));
    }

    @Override
    public void getAuthTokenLabel(IAccountManagerResponse response, String type, String tokenType) {
        caller(null); Authenticator a = authenticator(type);
        if (a == null) { error(response, AccountManager.ERROR_CODE_REMOTE_EXCEPTION, "Authenticator unavailable"); return; }
        mCalls.execute(() -> {
            Answer answer = call(a, (auth, r) -> auth.getAuthTokenLabel(r, tokenType));
            if (answer.result == null) { answer.deliver(response); return; }
            Bundle result = new Bundle();
            result.putString(AccountManager.KEY_AUTH_TOKEN_LABEL, answer.result.getString(AccountManager.KEY_AUTH_TOKEN_LABEL));
            reply(response, result);
        });
    }

    @Override
    public synchronized boolean accountAuthenticated(Account account) {
        Entry e = mAccounts.get(account); requireOwner(caller(null), e);
        beginMutation(); e.authenticatedAt = System.currentTimeMillis(); save(); return true;
    }

    // Unsupported cross-user/session APIs must answer, never leave an app's future pending.
    @Override
    public void copyAccountToUser(IAccountManagerResponse response, Account account, int fromUser, int toUser) {
        caller(null);
        error(response, AccountManager.ERROR_CODE_UNSUPPORTED_OPERATION, "Cross-user account copying is unsupported");
    }

    @Override
    public void startAddAccountSession(IAccountManagerResponse response, String type, String tokenType,
            String[] features, boolean expectActivity, Bundle supplied) {
        Caller c = caller(null); Bundle o = options(c, supplied);
        startSession(response, type, null, c,
                (auth, r) -> auth.startAddAccountSession(r, type, tokenType, features, o));
    }

    @Override
    public void startUpdateCredentialsSession(IAccountManagerResponse response, Account account,
            String tokenType, boolean expectActivity, Bundle supplied) {
        Caller c = caller(null); Bundle o = options(c, supplied);
        synchronized (this) {
            Entry e = mAccounts.get(account);
            if (e == null || !visible(c, account, e)) throw new SecurityException("Account is not visible");
        }
        startSession(response, account.type, account, c,
                (auth, r) -> auth.startUpdateCredentialsSession(r, account, tokenType, o));
    }

    private void startSession(IAccountManagerResponse response, String type, Account account,
            Caller c, Call operation) {
        mCalls.execute(() -> {
            Authenticator a = authenticator(type);
            if (a == null) { error(response, AccountManager.ERROR_CODE_REMOTE_EXCEPTION, "Authenticator unavailable"); return; }
            String code = packageIdentity(a.service.getPackageName());
            synchronized (this) {
                Entry e = account != null ? mAccounts.get(account) : null;
                if (account != null && (e == null || !visible(c, account, e)
                        || !a.service.getPackageName().equals(e.owner) || !code.equals(e.ownerIdentity))) {
                    error(response, AccountManager.ERROR_CODE_BAD_AUTHENTICATION, "Account owner changed"); return;
                }
            }
            call(a, operation, c).deliverSession(response, b -> {
                if (!code.equals(packageIdentity(a.service.getPackageName()))) return null;
                Bundle internal = b.getBundle(AccountManager.KEY_ACCOUNT_SESSION_BUNDLE);
                Bundle checked = validatedResult(a, b, account);
                if (checked == null) return null;
                Bundle result = new Bundle();
                if (checked.containsKey(AccountManager.KEY_INTENT)) {
                    result.putParcelable(AccountManager.KEY_INTENT, checked.getParcelable(AccountManager.KEY_INTENT));
                }
                if (checked.containsKey(AccountManager.KEY_ERROR_CODE)) {
                    result.putInt(AccountManager.KEY_ERROR_CODE, checked.getInt(AccountManager.KEY_ERROR_CODE));
                    result.putString(AccountManager.KEY_ERROR_MESSAGE, "Authenticator session failed");
                }
                if (internal != null) {
                    String nestedType = internal.getString(AccountManager.KEY_ACCOUNT_TYPE);
                    if (nestedType != null && !type.equals(nestedType)) return null;
                    synchronized (this) {
                        if (!callerUnchanged(c) || !authenticatorUnchanged(a)) return null;
                        result.putBundle(AccountManager.KEY_ACCOUNT_SESSION_BUNDLE,
                                mSessions.issue(c.key(), c.uid, a.service.getPackageName(), code,
                                        type, account, internal, a.removalEpoch));
                    }
                } else if (!result.containsKey(AccountManager.KEY_INTENT)
                        && !result.containsKey(AccountManager.KEY_ERROR_CODE)) return null;
                return result;
            });
        });
    }

    @Override
    public void finishSessionAsUser(IAccountManagerResponse response, Bundle session,
            boolean expectActivity, Bundle appInfo, int userId) {
        user(userId); Caller c = caller(null);
        // Do not merge client-controlled appInfo into the sealed authenticator payload.
        options(c, appInfo); // Validate any claimed caller package, then use trusted fields only.
        AccountSessions.Pending pending = mSessions.consume(session, c.key(), c.uid, p -> {
            Authenticator a = authenticator(p.type);
            synchronized (this) {
                Entry e = p.account != null ? mAccounts.get(p.account) : null;
                return callerUnchanged(c) && a != null && p.ownerEpoch == packageEpoch(p.owner)
                        && a.service.getPackageName().equals(p.owner)
                        && p.ownerCode.equals(packageIdentity(p.owner))
                        && (p.account == null || e != null && visible(c, p.account, e)
                                && p.owner.equals(e.owner) && p.ownerCode.equals(e.ownerIdentity));
            }
        });
        Bundle internal = pending.data();
        internal.putString(AccountManager.KEY_ANDROID_PACKAGE_NAME, c.name);
        internal.putInt(AccountManager.KEY_CALLER_UID, c.uid);
        internal.putInt(AccountManager.KEY_CALLER_PID, c.pid);
        mCalls.execute(() -> {
            Authenticator a = authenticator(pending.type);
            if (!callerUnchanged(c) || pending.ownerEpoch != packageEpoch(pending.owner)
                    || a == null || !pending.owner.equals(a.service.getPackageName())
                    || !pending.ownerCode.equals(packageIdentity(pending.owner))) {
                error(response, AccountManager.ERROR_CODE_BAD_AUTHENTICATION, "Account session owner changed"); return;
            }
            call(a, (auth, r) -> auth.finishSession(r, pending.type, internal), c).deliverSession(response, b -> {
                Bundle result = validatedResult(a, b, pending.account);
                if (result == null || !pending.ownerCode.equals(packageIdentity(pending.owner))) return null;
                String name = result.getString(AccountManager.KEY_ACCOUNT_NAME);
                String type = result.getString(AccountManager.KEY_ACCOUNT_TYPE);
                if (name != null && type != null) {
                    if (!pending.type.equals(type)) return null;
                    synchronized (this) {
                        Entry e = mAccounts.get(new Account(name, type));
                        if (!callerUnchanged(c) || pending.ownerEpoch != packageEpoch(pending.owner)
                                || e == null || !pending.owner.equals(e.owner)
                                || !pending.ownerCode.equals(e.ownerIdentity)) return null;
                        beginMutation();
                        if (!c.system) e.visibility.put(c.key(), AccountManager.VISIBILITY_VISIBLE);
                        save();
                    }
                }
                return result;
            });
        });
    }

    @Override
    public void isCredentialsUpdateSuggested(IAccountManagerResponse response, Account account, String statusToken) {
        Caller c = caller(null);
        authenticatedCall(response, account, c, (auth, r) -> auth.isCredentialsUpdateSuggested(r, account, statusToken));
    }

    @Override
    public synchronized boolean someUserHasAccount(Account account) {
        if (!caller(null).system) throw new SecurityException("System account enumeration required");
        return exists(account);
    }

    private Bundle validatedResult(Authenticator a, Bundle input, Account expected) {
        return validatedResult(a, input, expected, false);
    }

    private Bundle validatedResult(Authenticator a, Bundle input, Account expected, boolean allowToken) {
        if (!authenticatorUnchanged(a)) return null;
        Bundle result = new Bundle(input);
        Intent intent = result.getParcelable(AccountManager.KEY_INTENT);
        if (intent != null) {
            ComponentName component = intent.getComponent();
            if (component == null || !a.service.getPackageName().equals(component.getPackageName())
                    || intent.getSelector() != null || intent.getClipData() != null
                    || (intent.getFlags() & (Intent.FLAG_GRANT_READ_URI_PERMISSION
                            | Intent.FLAG_GRANT_WRITE_URI_PERMISSION)) != 0
                    || result.containsKey(AccountManager.KEY_AUTHTOKEN)) return null;
        }
        if (result.containsKey(AccountManager.KEY_AUTHTOKEN)) {
            String name = result.getString(AccountManager.KEY_ACCOUNT_NAME);
            String type = result.getString(AccountManager.KEY_ACCOUNT_TYPE);
            if (name == null || type == null || expected != null
                    && (!expected.name.equals(name) || !expected.type.equals(type))) return null;
            synchronized (this) {
                Entry e = mAccounts.get(new Account(name, type));
                if (e == null || !a.service.getPackageName().equals(e.owner)
                        || !e.ownerIdentity.equals(packageIdentity(e.owner))) return null;
            }
        }
        result.remove(AccountManager.KEY_ACCOUNT_SESSION_BUNDLE);
        if (!allowToken) result.remove(AccountManager.KEY_AUTHTOKEN);
        result.remove(AccountManager.KEY_PASSWORD);
        result.remove(AccountManager.KEY_CALLER_UID);
        result.remove(AccountManager.KEY_CALLER_PID);
        return result;
    }

    /** The installed authenticators, from each package's manifest and XML. */
    private List<Authenticator> authenticators() {
        List<Authenticator> result = new ArrayList<>();
        Intent intent = new Intent(AccountManager.ACTION_AUTHENTICATOR_INTENT);
        for (ResolveInfo ri : mPm.services(intent, null, PackageManager.GET_META_DATA)) {
            ServiceInfo service = ri.serviceInfo;
            // Private authenticators (including official microG) are reachable
            // only by their owner or the trusted account service. An exported
            // authenticator must additionally require the platform bind permission.
            if (service == null || service.metaData == null || !service.enabled) continue;
            ApkPackage owner = mPm.get(service.packageName);
            if (owner == null || mPm.isRemovalPending(service.packageName) || !owner.parsed.isEnabled()) continue;
            if (service.exported && !"android.permission.BIND_ACCOUNT_AUTHENTICATOR"
                    .equals(service.permission)) continue;
            int xml = service.metaData.getInt(AccountManager.AUTHENTICATOR_META_DATA_NAME);
            if (xml == 0) continue;
            Authenticator a = parse(service, xml);
            if (a == null) continue;
            synchronized (this) {
                if (mPm.get(service.packageName) != owner || mPm.isRemovalPending(service.packageName)
                        || packageIdentity(service.packageName) == null) continue;
                if (GOOGLE_TYPE.equals(a.type) && (!GmsPackageSupport.PACKAGE_GMS.equals(service.packageName)
                        || !owner.hasVerifiedMicrogSigner())) continue;
                a.removalEpoch = packageEpoch(service.packageName);
                result.add(a);
            }
        }
        return result;
    }

    private Authenticator authenticator(String accountType) {
        if (accountType == null) return null;
        Authenticator found = null;
        for (Authenticator a : authenticators()) {
            if (!accountType.equals(a.type)) continue;
            if (found != null && !found.service.equals(a.service)) return null;
            found = a;
        }
        return found;
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
                        parser.getAttributeBooleanValue(ANDROID_NS, "customTokens", false)
                                || Boolean.TRUE.equals(service.metaData.get(
                                        AccountManager.AUTHENTICATOR_META_DATA_NAME + ".customTokens")));
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

    private interface ResultCheck { Bundle check(Bundle bundle); }

    /** Initial service answer plus the later callback from an authenticator login Activity. */
    private final class Answer extends IAccountAuthenticatorResponse.Stub {
        final CountDownLatch done = new CountDownLatch(1);
        final Authenticator owner;
        final Caller client;
        volatile Bundle result;
        volatile int code = AccountManager.ERROR_CODE_REMOTE_EXCEPTION;
        volatile String message = "Authenticator did not answer";
        private final List<Bundle> pending = new ArrayList<>();
        private java.util.function.Consumer<Bundle> listener;
        private java.util.function.BiConsumer<Integer, String> errorListener;
        private boolean complete, failed;

        Answer(Authenticator owner) { this(owner, null); }
        Answer(Authenticator owner, Caller client) {
            this.owner = owner; this.client = client;
            synchronized (AccountManagerService.this) { mAnswers.add(this); }
        }

        private boolean current() {
            return authenticatorUnchanged(owner) && (client == null || callerUnchanged(client));
        }

        void cancelForRemoval() {
            java.util.function.BiConsumer<Integer, String> listener;
            synchronized (this) {
                if (complete) return;
                complete = true; failed = true; result = null; pending.clear();
                code = AccountManager.ERROR_CODE_CANCELED; message = "Account package access removed";
                listener = errorListener; done.countDown();
            }
            if (listener != null) mCalls.execute(() -> listener.accept(
                    AccountManager.ERROR_CODE_CANCELED, "Account package access removed"));
        }

        private void authenticateCallback() {
            if (!mCallers.isSystem()) mCallers.requirePackage(owner.service.getPackageName());
        }

        @Override
        public void onResult(Bundle b) {
            authenticateCallback();
            if (!current()) { cancelForRemoval(); return; }
            Bundle value = b != null ? new Bundle(b) : new Bundle();
            java.util.function.Consumer<Bundle> deliver;
            synchronized (this) {
                if (complete) return;
                deliver = listener;
                if (deliver == null) {
                    if (result == null) result = value;
                    else pending.add(value);
                    done.countDown(); return;
                }
            }
            deliver.accept(value);
        }

        @Override
        public void onRequestContinued() {
            authenticateCallback(); if (!current()) cancelForRemoval();
        }

        @Override
        public void onError(int errorCode, String errorMessage) {
            authenticateCallback();
            if (!current()) { cancelForRemoval(); return; }
            java.util.function.BiConsumer<Integer, String> deliver;
            synchronized (this) {
                if (complete) return;
                code = errorCode; message = errorMessage; failed = true; deliver = errorListener;
                if (deliver != null) complete = true;
                done.countDown();
            }
            if (deliver != null) deliver.accept(errorCode, errorMessage);
        }

        synchronized void fail(String why) {
            if (complete) return;
            message = why; failed = true; done.countDown();
        }

        void deliver(IAccountManagerResponse response) {
            if (!current()) { cancelForRemoval(); error(response, code, message); return; }
            if (result != null) reply(response, result);
            else error(response, code, message);
        }

        void deliverSession(IAccountManagerResponse response, ResultCheck check) {
            if (!current()) { cancelForRemoval(); error(response, code, message); return; }
            List<Bundle> initial;
            synchronized (this) {
                if (result == null || failed) { complete = true; error(response, code, message); return; }
                listener = value -> {
                    synchronized (this) { if (complete) return; }
                    Bundle checked;
                    try { checked = current() ? check.check(value) : null; }
                    catch (RuntimeException e) { checked = null; }
                    synchronized (this) {
                        if (complete) return;
                        if (checked == null || !checked.containsKey(AccountManager.KEY_INTENT)) complete = true;
                    }
                    if (checked == null) error(response, AccountManager.ERROR_CODE_INVALID_RESPONSE, "Invalid authenticator response");
                    else reply(response, checked);
                };
                errorListener = (c, m) -> error(response, c, m);
                initial = new ArrayList<>(); initial.add(result); initial.addAll(pending); pending.clear();
            }
            for (Bundle value : initial) listener.accept(value);
            synchronized (this) { if (complete) return; }
            mSessionTimeout.schedule(() -> {
                synchronized (this) {
                    if (complete) return;
                    complete = true;
                }
                error(response, AccountManager.ERROR_CODE_CANCELED, "Authentication session expired");
            }, 10, TimeUnit.MINUTES);
        }
    }

    /** Binds {@code a}'s service, makes the call and waits for its answer (on mCalls). */
    private Answer call(Authenticator a, Call call) {
        return call(a, call, null);
    }

    private Answer call(Authenticator a, Call call, Caller client) {
        Answer answer = new Answer(a, client);
        if (!answer.current()) { answer.cancelForRemoval(); return answer; }
        IServiceConnection connection = new IServiceConnection.Stub() {
            @Override
            public void connected(ComponentName name, IBinder binder,
                    android.app.IBinderSession session, boolean dead) {
                if (binder == null) return;
                if (!answer.current()) { answer.cancelForRemoval(); return; }
                try {
                    call.run(IAccountAuthenticator.Stub.asInterface(binder), answer);
                } catch (RemoteException | RuntimeException e) {
                    answer.fail("Authenticator IPC failed");
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
            SystemServer.log(TAG + "authenticator binding failed: " + t.getClass().getSimpleName());
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
        // Authenticator error text can contain account data; never put it in device logs.
        if (response == null) return;
        try {
            response.onError(code, message);
        } catch (RemoteException e) {
            SystemServer.log(TAG + "the caller is gone: " + e);
        }
    }

    // --- changes and persistence ---------------------------------------------------------

    /** Notify only code identities allowed to see the account, including removal names. */
    private void changed(Account account, boolean removed) {
        Entry e;
        synchronized (this) { e = mAccounts.get(account); }
        changed(account, removed, e);
    }

    private void changed(Account account, boolean removed, Entry e) {
        if (e == null) return;
        // Once account caches have a nonce, later add/remove/rename operations
        // must invalidate them too; otherwise enabling the cache would make a
        // subsequent account change stale despite a correctly delivered event.
        invalidateAccountListCache();
        invalidateAccountUserDataCache();
        if (mAm == null) return;
        Set<String> recipients = new HashSet<>(); recipients.add(e.owner);
        synchronized (this) {
            for (Map.Entry<String, Integer> grant : e.visibility.entrySet()) {
                if (grant.getValue() != AccountManager.VISIBILITY_VISIBLE
                        && grant.getValue() != AccountManager.VISIBILITY_USER_MANAGED_VISIBLE) continue;
                int separator = grant.getKey().indexOf('\u0000');
                if (separator > 0) {
                    String name = grant.getKey().substring(0, separator);
                    if (grant.getKey().equals(name + "\u0000" + packageIdentity(name))) recipients.add(name);
                }
            }
        }
        long token = Binder.clearCallingIdentity();
        try {
            for (String recipient : recipients) {
                List<Intent> intents = new ArrayList<>();
                intents.add(new Intent(AccountManager.LOGIN_ACCOUNTS_CHANGED_ACTION).setPackage(recipient));
                intents.add(new Intent(AccountManager.ACTION_VISIBLE_ACCOUNTS_CHANGED).setPackage(recipient));
                if (removed) intents.add(new Intent(AccountManager.ACTION_ACCOUNT_REMOVED).setPackage(recipient)
                        .putExtra(AccountManager.KEY_ACCOUNT_NAME, account.name)
                        .putExtra(AccountManager.KEY_ACCOUNT_TYPE, account.type));
                for (Intent intent : intents) mAm.broadcastIntentWithFeature(null, null, intent,
                        null, null, 0, null, null, null, null, null, -1, null, false, false, 0);
            }
        } catch (Throwable t) {
            SystemServer.log(TAG + "account change delivery failed: " + t.getClass().getSimpleName());
        } finally { Binder.restoreCallingIdentity(token); }
    }

    private static int count(Properties p, String key) {
        int n = Integer.parseInt(p.getProperty(key, "0"));
        if (n < 0 || n > 100000) throw new IllegalStateException("Invalid protected account record count");
        return n;
    }

    private synchronized void load() {
        try {
            Properties p = mStore.read();
            if (p == null) {
                Properties legacy = Device.read(mLegacyFile);
                if (legacy != null && count(legacy, "count") > 0) {
                    // Old records have no trustworthy authenticator/code ownership. Seal
                    // them for recovery, but require re-authentication before exposing any.
                    ByteArrayOutputStream bytes = new ByteArrayOutputStream();
                    legacy.store(bytes, "Unclaimed legacy account records");
                    beginMutation();
                    mUnclaimedLegacy = Base64.getEncoder().encodeToString(bytes.toByteArray());
                    save();
                    java.nio.file.Files.deleteIfExists(mLegacyFile.toPath());
                    SystemServer.log(TAG + "legacy account records sealed; re-add accounts to establish ownership");
                }
                return;
            }
            mUnclaimedLegacy = p.getProperty("unclaimedLegacy");
            for (int i = 0; i < count(p, "count"); i++) {
                String prefix = i + ".";
                String name = p.getProperty(prefix + "name"), type = p.getProperty(prefix + "type");
                Entry e = new Entry();
                e.owner = p.getProperty(prefix + "owner");
                e.ownerIdentity = p.getProperty(prefix + "ownerIdentity");
                if (name == null || type == null || e.owner == null || e.ownerIdentity == null) {
                    throw new IllegalStateException("Invalid protected account ownership");
                }
                e.password = p.getProperty(prefix + "password");
                e.authenticatedAt = Long.parseLong(p.getProperty(prefix + "authenticatedAt", "0"));
                for (int j = 0; j < count(p, prefix + "data.count"); j++) {
                    e.data.put(p.getProperty(prefix + "data." + j + ".key"),
                            p.getProperty(prefix + "data." + j + ".value"));
                }
                for (int j = 0; j < count(p, prefix + "token.count"); j++) {
                    String tokenPrefix = prefix + "token." + j + ".";
                    String tokenType = p.getProperty(tokenPrefix + "type");
                    e.tokens.put(tokenType, p.getProperty(tokenPrefix + "value"));
                    Set<String> grants = new HashSet<>();
                    for (int k = 0; k < count(p, tokenPrefix + "grant.count"); k++) {
                        grants.add(p.getProperty(tokenPrefix + "grant." + k));
                    }
                    e.tokenGrants.put(tokenType, grants);
                }
                for (int j = 0; j < count(p, prefix + "grantType.count"); j++) {
                    String grantPrefix = prefix + "grantType." + j + ".";
                    Set<String> grants = new HashSet<>();
                    for (int k = 0; k < count(p, grantPrefix + "client.count"); k++) {
                        grants.add(p.getProperty(grantPrefix + "client." + k));
                    }
                    e.tokenGrants.put(p.getProperty(grantPrefix + "type"), grants);
                }
                for (int j = 0; j < count(p, prefix + "visibility.count"); j++) {
                    e.visibility.put(p.getProperty(prefix + "visibility." + j + ".client"),
                            Integer.parseInt(p.getProperty(prefix + "visibility." + j + ".value")));
                }
                mAccounts.put(new Account(name, type), e);
            }
        } catch (IOException | RuntimeException e) {
            throw new IllegalStateException("Cannot open protected account storage", e);
        }
    }

    /** Called with this locked. Never fall back to plaintext or log secret values. */
    private void save() {
        if (mBeforeMutation == null) throw new IllegalStateException("Account mutation has no transaction");
        try {
            Properties p = new Properties();
            if (mUnclaimedLegacy != null) p.setProperty("unclaimedLegacy", mUnclaimedLegacy);
            int i = 0;
            for (Map.Entry<Account, Entry> item : mAccounts.entrySet()) {
                if (synthetic(item.getKey(), item.getValue())) continue;
                String prefix = i + "."; Entry e = item.getValue();
                p.setProperty(prefix + "name", item.getKey().name);
                p.setProperty(prefix + "type", item.getKey().type);
                p.setProperty(prefix + "owner", e.owner);
                p.setProperty(prefix + "ownerIdentity", e.ownerIdentity);
                p.setProperty(prefix + "authenticatedAt", Long.toString(e.authenticatedAt));
                if (e.password != null) p.setProperty(prefix + "password", e.password);
                int j = 0;
                for (Map.Entry<String, String> data : e.data.entrySet()) {
                    p.setProperty(prefix + "data." + j + ".key", data.getKey());
                    p.setProperty(prefix + "data." + j + ".value", data.getValue()); j++;
                }
                p.setProperty(prefix + "data.count", Integer.toString(j)); j = 0;
                for (Map.Entry<String, String> token : e.tokens.entrySet()) {
                    String tokenPrefix = prefix + "token." + j + ".";
                    p.setProperty(tokenPrefix + "type", token.getKey());
                    p.setProperty(tokenPrefix + "value", token.getValue());
                    int k = 0;
                    Set<String> grants = e.tokenGrants.get(token.getKey());
                    if (grants != null) for (String grant : grants) p.setProperty(tokenPrefix + "grant." + k++, grant);
                    p.setProperty(tokenPrefix + "grant.count", Integer.toString(k)); j++;
                }
                p.setProperty(prefix + "token.count", Integer.toString(j)); j = 0;
                for (Map.Entry<String, Set<String>> grant : e.tokenGrants.entrySet()) {
                    String grantPrefix = prefix + "grantType." + j + ".";
                    p.setProperty(grantPrefix + "type", grant.getKey());
                    int k = 0;
                    for (String client : grant.getValue()) p.setProperty(grantPrefix + "client." + k++, client);
                    p.setProperty(grantPrefix + "client.count", Integer.toString(k)); j++;
                }
                p.setProperty(prefix + "grantType.count", Integer.toString(j)); j = 0;
                for (Map.Entry<String, Integer> visibility : e.visibility.entrySet()) {
                    p.setProperty(prefix + "visibility." + j + ".client", visibility.getKey());
                    p.setProperty(prefix + "visibility." + j + ".value", Integer.toString(visibility.getValue())); j++;
                }
                p.setProperty(prefix + "visibility.count", Integer.toString(j)); i++;
            }
            p.setProperty("count", Integer.toString(i));
            mStoreWriter.write(p);
            mBeforeMutation = null;
        } catch (IOException | RuntimeException e) {
            State before = mBeforeMutation;
            mAccounts.clear(); mAccounts.putAll(before.accounts);
            mUnclaimedLegacy = before.legacy;
            mBeforeMutation = null;
            throw new IllegalStateException("Cannot save protected account storage", e);
        }
    }
}
