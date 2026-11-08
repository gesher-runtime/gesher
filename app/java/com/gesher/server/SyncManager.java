/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.server;

import android.accounts.Account;
import android.app.IServiceConnection;
import android.content.ComponentName;
import android.content.ContentResolver;
import android.content.Context;
import android.content.ISyncAdapter;
import android.content.ISyncContext;
import android.content.ISyncStatusObserver;
import android.content.Intent;
import android.content.PeriodicSync;
import android.content.SyncAdapterType;
import android.content.SyncInfo;
import android.content.SyncResult;
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
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.Objects;
import java.util.Properties;
import java.util.concurrent.CopyOnWriteArrayList;
import java.util.concurrent.Executors;
import java.util.concurrent.ScheduledExecutorService;
import java.util.concurrent.ScheduledFuture;
import java.util.concurrent.TimeUnit;
import java.util.function.Supplier;

/**
 * ContentService's sync side (docs/services.md, "Sync"): apps' sync adapters (services for
 * {@code android.content.SyncAdapter} with a {@code sync-adapter} XML) run for their
 * account type's accounts and authority, as Android's SyncManager runs them: on
 * {@code requestSync}, on their periodic syncs, and to upload after a provider's change
 * notified with {@code NOTIFY_SYNC_TO_NETWORK}. One sync of an account and authority at a
 * time; a request while it runs runs it again after. The settings (master and per
 * authority automatic sync, syncable, periodic syncs) are kept in
 * {@code <data>/system/sync.properties}; automatic sync is on unless an app turns it off.
 */
final class SyncManager {
    private static final String TAG = "sync: ";
    private static final String ANDROID_NS = "http://schemas.android.com/apk/res/android";
    /** How long after an upload change its sync starts (Android: 30 s), so changes batch. */
    private static final long UPLOAD_DELAY_MS = 3000;
    /** How long after the system server starts its accounts are synced. */
    private static final long START_DELAY_MS = 10000;
    /** A sync that neither finishes nor sends a heartbeat for this long is cancelled. */
    private static final long SYNC_TIMEOUT_MS = TimeUnit.MINUTES.toMillis(30);

    /** An app's sync adapter. */
    static final class Adapter {
        final String authority;
        final String accountType;
        final ComponentName service;
        final boolean userVisible, supportsUploading, alwaysSyncable, parallel;
        final String settingsActivity;

        Adapter(String authority, String accountType, ComponentName service, boolean userVisible,
                boolean supportsUploading, boolean alwaysSyncable, boolean parallel,
                String settingsActivity) {
            this.authority = authority;
            this.accountType = accountType;
            this.service = service;
            this.userVisible = userVisible;
            this.supportsUploading = supportsUploading;
            this.alwaysSyncable = alwaysSyncable;
            this.parallel = parallel;
            this.settingsActivity = settingsActivity;
        }

        SyncAdapterType type() {
            return new SyncAdapterType(authority, accountType, userVisible, supportsUploading,
                    alwaysSyncable, parallel, settingsActivity, service.getPackageName());
        }
    }

    /** A running sync: its adapter's binding and its ISyncContext. */
    private final class Active extends ISyncContext.Stub {
        final String key;
        final Adapter adapter;
        final Account account;
        final Bundle extras;
        final long start = System.currentTimeMillis();
        volatile long heartbeat = start;
        volatile ISyncAdapter binder;
        IServiceConnection connection;
        boolean done;  // guarded by SyncManager.this

        Active(String key, Adapter adapter, Account account, Bundle extras) {
            this.key = key;
            this.adapter = adapter;
            this.account = account;
            this.extras = extras;
        }

        @Override
        public void sendHeartbeat() {
            heartbeat = System.currentTimeMillis();
        }

        @Override
        public void onFinished(SyncResult result) {
            finished(this, result == null ? "done" : result.toDebugString());
        }
    }

    private final ActivityManagerService mAm;
    private final PackageManagerService mPm;
    private final File mFile = new File(Device.dataDir(), "system/sync.properties");
    private volatile Supplier<Account[]> mAccounts = () -> new Account[0];
    private volatile java.util.function.BiPredicate<Account, String> mAccountAccess = (a, p) -> false;

    private boolean callerCanAccess(Account account) {
        return mAm.callers().isSystem() || account != null
                && mAccountAccess.test(account, mAm.callers().callerPackage());
    }

    private void requireAccountAccess(Account account) {
        if (!callerCanAccess(account)) throw new SecurityException("Sync account is not visible to caller");
    }
    private final ScheduledExecutorService mTimer = Executors.newSingleThreadScheduledExecutor(r -> {
        Thread t = new Thread(r, "sync");
        t.setDaemon(true);
        return t;
    });
    private final List<Observer> mObservers = new CopyOnWriteArrayList<>();
    /** Parsed adapters by service, with the APK they were read from. */
    private final Map<ComponentName, Object[]> mParsed = new HashMap<>();  // guarded by itself
    // Guarded by this.
    private final Properties mSettings = new Properties();
    private final Map<String, Active> mActive = new LinkedHashMap<>();
    /** Waiting syncs: their extras (merged), by key. */
    private final Map<String, Object[]> mPending = new LinkedHashMap<>();
    private final Map<String, ScheduledFuture<?>> mPeriodic = new HashMap<>();
    private final Map<String, ScheduledFuture<?>> mUploads = new HashMap<>();

    private static final class Observer {
        final int mask;
        final ISyncStatusObserver callback;

        Observer(int mask, ISyncStatusObserver callback) {
            this.mask = mask;
            this.callback = callback;
        }
    }

    SyncManager(ActivityManagerService am, PackageManagerService pm) {
        mAm = am;
        mPm = pm;
        load();
    }

    ActivityManagerService activityManager() { return mAm; }
    PackageManagerService packageManager() { return mPm; }

    private void requireSyncSettingsWrite() {
        if (!GmsSignatureSpoofing.isEnabled() || mAm.callers().isSystem()) return;
        String pkg = mAm.callers().callerPackage();
        if (mPm.permissions().checkPermission(pkg, "android.permission.WRITE_SYNC_SETTINGS")
                != android.content.pm.PackageManager.PERMISSION_GRANTED) {
            throw new SecurityException("Changing sync settings requires WRITE_SYNC_SETTINGS");
        }
    }

    /**
     * The accounts to sync (AccountManagerService's, once it is up), the periodic syncs, and
     * a sync of every account's authorities soon after, as Android's SyncManager syncs them
     * when the device starts (its accounts updated).
     */
    void start(Supplier<Account[]> accounts, java.util.function.BiPredicate<Account, String> access) {
        mAccounts = accounts;
        mAccountAccess = access;
        synchronized (this) {
            for (String name : mSettings.stringPropertyNames()) {
                if (name.startsWith("periodic.")) schedulePeriodic(name, false);
            }
        }
        mTimer.schedule(() -> requestSync(null, null, new Bundle(), false),
                START_DELAY_MS, TimeUnit.MILLISECONDS);
    }

    // --- settings ------------------------------------------------------------------------

    synchronized boolean getMasterSyncAutomatically() {
        return !"false".equals(mSettings.getProperty("master"));
    }

    void setMasterSyncAutomatically(boolean flag) {
        requireSyncSettingsWrite();
        synchronized (this) {
            mSettings.setProperty("master", Boolean.toString(flag));
            save();
        }
        changed(ContentResolver.SYNC_OBSERVER_TYPE_SETTINGS);
    }

    synchronized boolean getSyncAutomatically(Account account, String authority) {
        if (!callerCanAccess(account)) return false;
        if (account == null || authority == null) return false;
        return !"false".equals(mSettings.getProperty("auto." + id(account, authority)));
    }

    void setSyncAutomatically(Account account, String authority, boolean sync) {
        requireSyncSettingsWrite();
        if (account != null) requireAccountAccess(account);
        if (account == null || authority == null) return;
        synchronized (this) {
            mSettings.setProperty("auto." + id(account, authority), Boolean.toString(sync));
            save();
        }
        changed(ContentResolver.SYNC_OBSERVER_TYPE_SETTINGS);
        if (sync) requestSync(account, authority, new Bundle(), false);
    }

    int getIsSyncable(Account account, String authority) {
        if (!callerCanAccess(account)) return 0;
        if (account == null || authority == null) return 0;
        String v;
        synchronized (this) {
            v = mSettings.getProperty("syncable." + id(account, authority));
        }
        if (v != null) {
            try {
                return Integer.parseInt(v);
            } catch (NumberFormatException e) {
                // the default
            }
        }
        return adapter(account.type, authority) != null ? 1 : 0;
    }

    void setIsSyncable(Account account, String authority, int syncable) {
        requireSyncSettingsWrite();
        if (account != null) requireAccountAccess(account);
        if (account == null || authority == null) return;
        synchronized (this) {
            mSettings.setProperty("syncable." + id(account, authority),
                    Integer.toString(Math.max(-1, Math.min(1, syncable))));
            save();
        }
        changed(ContentResolver.SYNC_OBSERVER_TYPE_SETTINGS);
    }

    // --- adapters ------------------------------------------------------------------------

    /** The installed sync adapters, from each package's manifest and XML. */
    List<Adapter> adapters() {
        List<Adapter> result = new ArrayList<>();
        Intent intent = new Intent("android.content.SyncAdapter");
        for (ResolveInfo ri : mPm.services(intent, null, PackageManager.GET_META_DATA)) {
            ServiceInfo service = ri.serviceInfo;
            if (service == null || service.metaData == null) continue;
            int xml = service.metaData.getInt("android.content.SyncAdapter");
            if (xml == 0) continue;
            ComponentName component = new ComponentName(service.packageName, service.name);
            String apk = service.applicationInfo.sourceDir;
            Adapter a;
            synchronized (mParsed) {
                Object[] cached = mParsed.get(component);
                if (cached != null && Objects.equals(cached[0], apk)) {
                    a = (Adapter) cached[1];
                } else {
                    a = parse(service, component, xml);
                    mParsed.put(component, new Object[] {apk, a});
                }
            }
            if (a != null) result.add(a);
        }
        return result;
    }

    Adapter adapter(String accountType, String authority) {
        for (Adapter a : adapters()) {
            if (a.accountType.equals(accountType) && a.authority.equals(authority)) return a;
        }
        return null;
    }

    SyncAdapterType[] getSyncAdapterTypes() {
        List<SyncAdapterType> types = new ArrayList<>();
        for (Adapter a : adapters()) types.add(a.type());
        return types.toArray(new SyncAdapterType[0]);
    }

    String[] getSyncAdapterPackagesForAuthority(String authority) {
        List<String> packages = new ArrayList<>();
        for (Adapter a : adapters()) {
            if (a.authority.equals(authority) && !packages.contains(a.service.getPackageName())) {
                packages.add(a.service.getPackageName());
            }
        }
        return packages.toArray(new String[0]);
    }

    /** A {@code sync-adapter} element, read with the app's resources. */
    private static Adapter parse(ServiceInfo service, ComponentName component, int xml) {
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
                if (event != XmlPullParser.START_TAG || !"sync-adapter".equals(parser.getName())) {
                    return null;
                }
                String authority = string(res, parser, "contentAuthority");
                String type = string(res, parser, "accountType");
                if (authority == null || authority.isEmpty() || type == null || type.isEmpty()) {
                    return null;
                }
                return new Adapter(authority, type, component,
                        parser.getAttributeBooleanValue(ANDROID_NS, "userVisible", true),
                        parser.getAttributeBooleanValue(ANDROID_NS, "supportsUploading", true),
                        parser.getAttributeBooleanValue(ANDROID_NS, "isAlwaysSyncable", false),
                        parser.getAttributeBooleanValue(ANDROID_NS, "allowParallelSyncs", false),
                        string(res, parser, "settingsActivity"));
            }
        } catch (Exception e) {
            SystemServer.log(TAG + "the sync adapter of " + component.flattenToShortString()
                    + " was not read: " + e);
            return null;
        }
    }

    private static String string(Resources res, XmlResourceParser parser, String name) {
        int id = parser.getAttributeResourceValue(ANDROID_NS, name, 0);
        return id != 0 ? res.getString(id) : parser.getAttributeValue(ANDROID_NS, name);
    }

    // --- requests ------------------------------------------------------------------------

    /**
     * A sync of the authority for the account, or for every account with an adapter for it
     * (account null), and for every authority of the account (authority null). Without
     * {@code SYNC_EXTRAS_MANUAL} (or {@code ignoreSettings}) only where automatic sync is on.
     */
    void requestSync(Account account, String authority, Bundle extras, boolean upload) {
        requestSync(account, authority, extras, upload, null);
    }

    private void requestSync(Account account, String authority, Bundle extras, boolean upload, ApkPackage notifier) {
        if (notifier != null && mPm.get(notifier.packageName) != notifier) return;
        Bundle e = extras != null ? new Bundle(extras) : new Bundle();
        if (upload) e.putBoolean(ContentResolver.SYNC_EXTRAS_UPLOAD, true);
        boolean manual = e.getBoolean(ContentResolver.SYNC_EXTRAS_MANUAL, false)
                || e.getBoolean(ContentResolver.SYNC_EXTRAS_IGNORE_SETTINGS, false);
        if (!upload) {
            SystemServer.log(TAG + "requested: "
                    + (authority != null ? authority : "every authority") + " of "
                    + (account != null ? "account type " + account.type : "visible accounts")
                    + (manual ? ", manual" : ""));
        }
        for (Adapter a : adapters()) {
            if (authority != null && !a.authority.equals(authority)) continue;
            if (upload && !a.supportsUploading) continue;
            for (Account acc : accounts(a.accountType)) {
                if (account != null && !account.equals(acc)) continue;
                if (notifier != null && !mAccountAccess.test(acc, notifier.packageName)) continue;
                if (!callerCanAccess(acc) || !mAccountAccess.test(acc, a.service.getPackageName())) continue;
                if (getIsSyncable(acc, a.authority) == 0) continue;
                if (!manual && (!getMasterSyncAutomatically()
                        || !getSyncAutomatically(acc, a.authority))) {
                    continue;
                }
                enqueue(a, acc, e);
            }
        }
    }

    /** The accounts of the type that exist now. */
    private List<Account> accounts(String type) {
        List<Account> result = new ArrayList<>();
        for (Account a : mAccounts.get()) {
            if (a.type.equals(type)) result.add(a);
        }
        return result;
    }

    /** After a provider's change notified to the network: an upload sync of its authority. */
    void localChange(android.net.Uri uri, ApkPackage notifier) {
        String authority = uri.getAuthority();
        if (authority == null) return;
        String key = uri + "|" + (notifier == null ? "android" : notifier.packageName + "@" + System.identityHashCode(notifier));
        synchronized (this) {
            if (mUploads.containsKey(key)) return;
            mUploads.put(key, mTimer.schedule(() -> {
                synchronized (SyncManager.this) {
                    mUploads.remove(key);
                }
                if (notifier != null && !mAm.uriGrants().canAccess(notifier, uri,
                        android.content.Intent.FLAG_GRANT_WRITE_URI_PERMISSION, 0)) return;
                requestSync(null, authority, new Bundle(), true, notifier);
            }, UPLOAD_DELAY_MS, TimeUnit.MILLISECONDS));
        }
    }

    void cancelSync(Account account, String authority) {
        if (account != null) requireAccountAccess(account);
        List<Active> cancel = new ArrayList<>();
        synchronized (this) {
            mPending.entrySet().removeIf(en -> callerCanAccess((Account) ((Object[]) en.getValue())[1])
                    && matches((Object[]) en.getValue(), account, authority));
            for (Active a : mActive.values()) {
                if (callerCanAccess(a.account) && (account == null || account.equals(a.account))
                        && (authority == null || authority.equals(a.adapter.authority))) {
                    cancel.add(a);
                }
            }
        }
        for (Active a : cancel) {
            ISyncAdapter binder = a.binder;
            try {
                if (binder != null) binder.cancelSync(a);
            } catch (RemoteException e) {
                // gone
            }
            finished(a, "cancelled");
        }
        changed(ContentResolver.SYNC_OBSERVER_TYPE_PENDING);
    }

    private static boolean matches(Object[] pending, Account account, String authority) {
        Adapter a = (Adapter) pending[0];
        return (account == null || account.equals(pending[1]))
                && (authority == null || authority.equals(a.authority));
    }

    private static String key(Adapter a, Account account) {
        return a.service.flattenToShortString() + "/" + id(account, a.authority);
    }

    private static String id(Account account, String authority) {
        return account.type + "/" + account.name + "/" + authority;
    }

    private void enqueue(Adapter a, Account account, Bundle extras) {
        if (!mAccountAccess.test(account, a.service.getPackageName())) return;
        String key = key(a, account);
        synchronized (this) {
            Object[] pending = mPending.get(key);
            if (pending != null) {
                ((Bundle) pending[2]).putAll(extras);
            } else {
                mPending.put(key, new Object[] {a, account, new Bundle(extras)});
            }
        }
        changed(ContentResolver.SYNC_OBSERVER_TYPE_PENDING);
        mTimer.execute(this::run);
    }

    /** Starts the pending syncs whose account and authority are not syncing. */
    private void run() {
        List<Active> start = new ArrayList<>();
        synchronized (this) {
            for (java.util.Iterator<Map.Entry<String, Object[]>> it = mPending.entrySet().iterator();
                    it.hasNext(); ) {
                Map.Entry<String, Object[]> en = it.next();
                if (mActive.containsKey(en.getKey())) continue;
                Object[] p = en.getValue();
                Active a = new Active(en.getKey(), (Adapter) p[0], (Account) p[1], (Bundle) p[2]);
                mActive.put(en.getKey(), a);
                start.add(a);
                it.remove();
            }
        }
        if (start.isEmpty()) return;
        changed(ContentResolver.SYNC_OBSERVER_TYPE_PENDING | ContentResolver.SYNC_OBSERVER_TYPE_ACTIVE);
        for (Active a : start) bind(a);
    }

    private void bind(Active a) {
        if (!mAccountAccess.test(a.account, a.adapter.service.getPackageName())) {
            finished(a, "account access revoked"); return;
        }
        SystemServer.log(TAG + "syncing " + a.adapter.authority + " for account type " + a.account.type
                + " (" + a.adapter.service.flattenToShortString() + ")"
                + (a.extras.getBoolean(ContentResolver.SYNC_EXTRAS_UPLOAD) ? ", upload" : ""));
        a.connection = new IServiceConnection.Stub() {
            @Override
            public void connected(ComponentName name, IBinder binder,
                    android.app.IBinderSession session, boolean dead) {
                if (binder == null) return;
                synchronized (SyncManager.this) {
                    if (a.done || a.binder != null) return;
                    a.binder = ISyncAdapter.Stub.asInterface(binder);
                }
                try {
                    if (!mAccountAccess.test(a.account, a.adapter.service.getPackageName())) {
                        finished(a, "account access revoked"); return;
                    }
                    a.binder.startSync(a, a.adapter.authority, a.account, a.extras);
                } catch (RemoteException | RuntimeException e) {
                    finished(a, "the adapter failed: " + e);
                }
            }
        };
        int bound;
        long token = Binder.clearCallingIdentity();
        try {
            bound = mAm.bindService(null, null, new Intent("android.content.SyncAdapter")
                    .setComponent(a.adapter.service), null, a.connection,
                    Context.BIND_AUTO_CREATE, "android", 0);
        } catch (Throwable t) {
            SystemServer.log(TAG + "binding " + a.adapter.service.flattenToShortString()
                    + " failed: " + t);
            bound = 0;
        } finally {
            Binder.restoreCallingIdentity(token);
        }
        if (bound == 0) {
            finished(a, "the adapter could not be bound");
            return;
        }
        watch(a);
    }

    private void watch(Active a) {
        mTimer.schedule(() -> {
            synchronized (this) {
                if (a.done) return;
            }
            if (System.currentTimeMillis() - a.heartbeat >= SYNC_TIMEOUT_MS) {
                ISyncAdapter binder = a.binder;
                try {
                    if (binder != null) binder.cancelSync(a);
                } catch (RemoteException e) {
                    // gone
                }
                finished(a, "timed out");
            } else {
                watch(a);
            }
        }, SYNC_TIMEOUT_MS, TimeUnit.MILLISECONDS);
    }

    private void finished(Active a, String how) {
        synchronized (this) {
            if (a.done) return;
            a.done = true;
            if (mActive.get(a.key) == a) mActive.remove(a.key);
        }
        SystemServer.log(TAG + a.adapter.authority + " for account type " + a.account.type + ": " + how
                + " in " + (System.currentTimeMillis() - a.start) + " ms");
        // Active was checked/completed above. Its binding belongs to the system,
        // while onFinished can still carry the sync adapter's Binder identity.
        long identity = Binder.clearCallingIdentity();
        try {
            if (a.connection != null) mAm.unbindService(a.connection);
        } catch (Throwable t) {
            // not bound
        } finally { Binder.restoreCallingIdentity(identity); }
        changed(ContentResolver.SYNC_OBSERVER_TYPE_ACTIVE);
        mTimer.execute(this::run);  // a request while it ran
    }

    // --- periodic syncs ------------------------------------------------------------------

    void addPeriodicSync(Account account, String authority, Bundle extras, long seconds) {
        requireSyncSettingsWrite();
        if (account != null) requireAccountAccess(account);
        if (account == null || authority == null) return;
        // Android's minimum period is an hour's quarter.
        long period = Math.max(seconds, 15 * 60);
        String name = "periodic." + id(account, authority) + "|" + extrasKey(extras);
        synchronized (this) {
            String value = period + "|" + encode(extras);
            if (value.equals(mSettings.getProperty(name))) return;
            mSettings.setProperty(name, value);
            save();
            schedulePeriodic(name, true);
        }
    }

    void removePeriodicSync(Account account, String authority, Bundle extras) {
        requireSyncSettingsWrite();
        if (account != null) requireAccountAccess(account);
        if (account == null || authority == null) return;
        String name = "periodic." + id(account, authority) + "|" + extrasKey(extras);
        synchronized (this) {
            if (mSettings.remove(name) == null) return;
            save();
            ScheduledFuture<?> f = mPeriodic.remove(name);
            if (f != null) f.cancel(false);
        }
    }

    List<PeriodicSync> getPeriodicSyncs(Account account, String authority) {
        if (!callerCanAccess(account)) return java.util.Collections.emptyList();
        List<PeriodicSync> result = new ArrayList<>();
        if (account == null || authority == null) return result;
        String prefix = "periodic." + id(account, authority) + "|";
        synchronized (this) {
            for (String name : mSettings.stringPropertyNames()) {
                if (!name.startsWith(prefix)) continue;
                String[] v = mSettings.getProperty(name).split("\\|", 2);
                long period = Long.parseLong(v[0]);
                result.add(new PeriodicSync(account, authority,
                        decode(v.length > 1 ? v[1] : ""), period));
            }
        }
        return result;
    }

    /** Runs the periodic sync every period (the first after one, or now when it is new). */
    private void schedulePeriodic(String name, boolean now) {
        ScheduledFuture<?> old = mPeriodic.remove(name);
        if (old != null) old.cancel(false);
        String[] v = mSettings.getProperty(name).split("\\|", 2);
        long period;
        try {
            period = Long.parseLong(v[0]);
        } catch (NumberFormatException e) {
            return;
        }
        String[] id = name.substring("periodic.".length(), name.indexOf('|')).split("/", 3);
        if (id.length < 3) return;
        Account account = new Account(id[1], id[0]);
        String authority = id[2];
        Bundle extras = decode(v.length > 1 ? v[1] : "");
        mPeriodic.put(name, mTimer.scheduleAtFixedRate(
                () -> requestSync(account, authority, extras, false),
                now ? 0 : period, period, TimeUnit.SECONDS));
    }

    private static String extrasKey(Bundle extras) {
        return Integer.toHexString(encode(extras).hashCode());
    }

    /** A periodic sync's extras as they are saved: the parcelled Bundle, in base64. */
    private static String encode(Bundle extras) {
        if (extras == null || extras.isEmpty()) return "";
        android.os.Parcel p = android.os.Parcel.obtain();
        try {
            p.writeBundle(extras);
            return android.util.Base64.encodeToString(p.marshall(), android.util.Base64.NO_WRAP);
        } finally {
            p.recycle();
        }
    }

    private static Bundle decode(String s) {
        if (s == null || s.isEmpty()) return new Bundle();
        android.os.Parcel p = android.os.Parcel.obtain();
        try {
            byte[] b = android.util.Base64.decode(s, android.util.Base64.NO_WRAP);
            p.unmarshall(b, 0, b.length);
            p.setDataPosition(0);
            Bundle extras = p.readBundle(SyncManager.class.getClassLoader());
            return extras != null ? extras : new Bundle();
        } catch (RuntimeException e) {
            return new Bundle();
        } finally {
            p.recycle();
        }
    }

    // --- state ---------------------------------------------------------------------------

    synchronized boolean isSyncActive(Account account, String authority) {
        for (Active a : mActive.values()) {
            if (callerCanAccess(a.account) && (account == null || account.equals(a.account))
                    && (authority == null || authority.equals(a.adapter.authority))) {
                return true;
            }
        }
        return false;
    }

    synchronized boolean isSyncPending(Account account, String authority) {
        for (Object[] p : mPending.values()) {
            if (callerCanAccess((Account) p[1]) && matches(p, account, authority)) return true;
        }
        return false;
    }

    synchronized List<SyncInfo> getCurrentSyncs() {
        List<SyncInfo> result = new ArrayList<>();
        int i = 0;
        for (Active a : mActive.values()) {
            if (callerCanAccess(a.account)) result.add(new SyncInfo(i++, a.account, a.adapter.authority, a.start));
        }
        return result;
    }

    void addStatusChangeListener(int mask, ISyncStatusObserver callback) {
        if (callback != null) mObservers.add(new Observer(mask, callback));
    }

    void removeStatusChangeListener(ISyncStatusObserver callback) {
        if (callback == null) return;
        mObservers.removeIf(o -> o.callback.asBinder() == callback.asBinder());
    }

    private void changed(int which) {
        for (Observer o : mObservers) {
            if ((o.mask & which) == 0) continue;
            try {
                o.callback.onStatusChanged(which);
            } catch (RemoteException e) {
                mObservers.remove(o);
            }
        }
    }

    private synchronized void load() {
        if (!mFile.isFile()) return;
        try (InputStream in = new FileInputStream(mFile)) {
            mSettings.load(in);
        } catch (IOException e) {
            SystemServer.log(TAG + mFile + ": " + e);
        }
    }

    private synchronized void save() {
        mFile.getParentFile().mkdirs();
        try (OutputStream out = new FileOutputStream(mFile)) {
            mSettings.store(out, "Gesher's sync settings");
        } catch (IOException e) {
            SystemServer.log(TAG + mFile + ": " + e);
        }
    }
}
