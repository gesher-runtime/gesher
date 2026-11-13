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
import android.os.SystemClock;
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
import java.util.function.LongSupplier;

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
        final WorkKey key;
        final Adapter adapter;
        final Account account;
        final Bundle extras;
        final long start = mElapsed.getAsLong();
        volatile long heartbeat = start;
        volatile ISyncAdapter binder;
        IServiceConnection connection;
        boolean done, cancelled;  // guarded by SyncManager.this
        boolean bound; // lifecycle executor only; successful bind owns exactly one unbind

        Active(WorkKey key, Adapter adapter, Account account, Bundle extras) {
            this.key = key;
            this.adapter = adapter;
            this.account = account;
            this.extras = extras;
        }

        @Override
        public void sendHeartbeat() {
            heartbeat = mElapsed.getAsLong();
        }

        @Override
        public void onFinished(SyncResult result) {
            mTimer.execute(() -> finished(this, result, "finished"));
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
    private final LongSupplier mElapsed, mWall;
    private final ScheduledExecutorService mTimer;
    private final DeviceWork.Source mWorkSource = this::nextDueElapsed;
    private static ScheduledExecutorService timer() {
        return Executors.newSingleThreadScheduledExecutor(r -> {
            Thread t = new Thread(r, "sync"); t.setDaemon(true); return t;
        });
    }

    /** Transient identity must not alias account names/types containing delimiters. */
    private static final class Endpoint {
        final Account account;
        final String authority;
        Endpoint(Account account, String authority) { this.account = account; this.authority = authority; }
        @Override public boolean equals(Object other) {
            return other instanceof Endpoint e && account.equals(e.account) && authority.equals(e.authority);
        }
        @Override public int hashCode() { return Objects.hash(account, authority); }
    }
    private static final class WorkKey {
        final ComponentName service;
        final Endpoint endpoint;
        WorkKey(Adapter adapter, Account account) {
            service = adapter.service; endpoint = new Endpoint(account, adapter.authority);
        }
        @Override public boolean equals(Object other) {
            return other instanceof WorkKey k && service.equals(k.service) && endpoint.equals(k.endpoint);
        }
        @Override public int hashCode() { return Objects.hash(service, endpoint); }
    }
    private static final class Pending {
        final Adapter adapter;
        final Account account;
        final WorkKey key;
        final Bundle extras;
        final boolean independent;
        final long earliest;
        Pending(Adapter adapter, Account account, Bundle extras, long earliest, boolean independent) {
            this.adapter = adapter; this.account = account; this.extras = new Bundle(extras);
            this.key = new WorkKey(adapter, account); this.earliest = earliest; this.independent = independent;
        }
    }
    private static final class Backoff {
        final long until, delay;
        Backoff(long until, long delay) { this.until = until; this.delay = delay; }
    }
    private final List<Observer> mObservers = new CopyOnWriteArrayList<>();
    /** Parsed adapters by service, with the APK they were read from. */
    private final Map<ComponentName, Object[]> mParsed = new HashMap<>();  // guarded by itself
    // Guarded by this.
    private final Properties mSettings = new Properties();
    private final Map<WorkKey, Active> mActive = new LinkedHashMap<>();
    private final List<Pending> mPending = new ArrayList<>();
    private final Map<Endpoint, Backoff> mBackoff = new HashMap<>();
    private final Map<Endpoint, Long> mDelayUntil = new HashMap<>();
    private ScheduledFuture<?> mWake;
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
        this(am, pm, SystemClock::elapsedRealtime, System::currentTimeMillis, timer());
    }

    SyncManager(ActivityManagerService am, PackageManagerService pm, LongSupplier elapsed,
            LongSupplier wall, ScheduledExecutorService executor) {
        mAm = am; mPm = pm; mElapsed = elapsed; mWall = wall; mTimer = executor;
        load();
        DeviceWork.add(mWorkSource);
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
            mPending.removeIf(p -> callerCanAccess(p.account) && matches(p.account,
                    p.adapter.authority, account, authority));
            mBackoff.keySet().removeIf(e -> callerCanAccess(e.account)
                    && matches(e.account, e.authority, account, authority));
            for (Active active : mActive.values()) {
                if (callerCanAccess(active.account) && matches(active.account,
                        active.adapter.authority, account, authority)) {
                    // Atomic with pending removal: a completion cannot promote a retry
                    // between the cancellation request and its lifecycle retirement.
                    active.cancelled = true;
                    cancel.add(active);
                }
            }
        }
        // Never wait for a bind/start call or an adapter's cancellation callback.
        // The serial lane records any late successful binding before releasing it.
        for (Active active : cancel) mTimer.execute(() -> cancelActive(active, "cancelled"));
        changed(ContentResolver.SYNC_OBSERVER_TYPE_PENDING);
        mTimer.execute(this::run);
    }

    private static boolean matches(Account actual, String provider, Account account, String authority) {
        return (account == null || account.equals(actual)) && (authority == null || authority.equals(provider));
    }

    private static String id(Account account, String authority) {
        // Persisted settings/periodic-key migration is separate from transient admission.
        return account.type + "/" + account.name + "/" + authority;
    }

    private void enqueue(Adapter adapter, Account account, Bundle extras) {
        if (!mAccountAccess.test(account, adapter.service.getPackageName())) return;
        Pending pending = new Pending(adapter, account, extras, 0, false);
        synchronized (this) {
            Pending existing = null;
            for (Pending p : mPending) {
                if (!p.independent && p.key.equals(pending.key)) { existing = p; break; }
            }
            if (existing == null) mPending.add(pending);
            else existing.extras.putAll(extras);
        }
        changed(ContentResolver.SYNC_OBSERVER_TYPE_PENDING);
        mTimer.execute(this::run);
    }

    private boolean conflicts(Pending pending) {
        for (Active running : mActive.values()) {
            if (running.account.type.equals(pending.account.type)
                    && running.adapter.authority.equals(pending.adapter.authority)
                    && (!running.adapter.parallel || running.account.name.equals(pending.account.name))) return true;
        }
        return false;
    }

    private static long add(long now, long delay) {
        return delay > 0 && now > Long.MAX_VALUE - delay ? Long.MAX_VALUE : now + Math.max(0, delay);
    }

    private long due(Pending pending) {
        long due = pending.earliest;
        if (!pending.extras.getBoolean(ContentResolver.SYNC_EXTRAS_IGNORE_BACKOFF, false)) {
            Backoff backoff = mBackoff.get(pending.key.endpoint);
            if (backoff != null) due = Math.max(due, backoff.until);
            due = Math.max(due, mDelayUntil.getOrDefault(pending.key.endpoint, 0L));
        }
        return due;
    }

    /** Active syncs are due now even when the device's general background grace expires. */
    private synchronized long nextDueElapsed() {
        if (!mActive.isEmpty()) return 0;
        long next = Long.MAX_VALUE, now = mElapsed.getAsLong();
        for (Pending p : mPending) next = Math.min(next, due(p));
        for (ScheduledFuture<?> future : mPeriodic.values()) {
            if (!future.isDone()) next = Math.min(next, add(now, future.getDelay(TimeUnit.MILLISECONDS)));
        }
        for (ScheduledFuture<?> future : mUploads.values()) {
            if (!future.isDone()) next = Math.min(next, add(now, future.getDelay(TimeUnit.MILLISECONDS)));
        }
        return next;
    }

    /** Called on the serial lifecycle lane; no external callback runs under this monitor. */
    private void run() {
        List<Active> start = new ArrayList<>();
        synchronized (this) {
            if (mWake != null) { mWake.cancel(false); mWake = null; }
            long now = mElapsed.getAsLong(), wake = Long.MAX_VALUE;
            for (java.util.Iterator<Pending> it = mPending.iterator(); it.hasNext();) {
                Pending p = it.next();
                if (mActive.containsKey(p.key) || conflicts(p)) continue;
                long due = due(p);
                if (due > now) { wake = Math.min(wake, due); continue; }
                Active a = new Active(p.key, p.adapter, p.account, p.extras);
                mActive.put(p.key, a); start.add(a); it.remove();
            }
            if (wake != Long.MAX_VALUE) mWake = mTimer.schedule(this::run, wake - now, TimeUnit.MILLISECONDS);
        }
        if (start.isEmpty()) return;
        changed(ContentResolver.SYNC_OBSERVER_TYPE_PENDING | ContentResolver.SYNC_OBSERVER_TYPE_ACTIVE);
        for (Active active : start) bind(active);
    }

    private boolean stopped(Active active) {
        synchronized (this) { return active.done || active.cancelled; }
    }

    private void bind(Active active) {
        if (stopped(active) || !mAccountAccess.test(active.account, active.adapter.service.getPackageName())) {
            cancelActive(active, "cancelled before binding"); return;
        }
        active.connection = new IServiceConnection.Stub() {
            @Override public void connected(ComponentName name, IBinder binder,
                    android.app.IBinderSession session, boolean dead) {
                // A synchronous bind callback must not run startSync before bind's
                // successful return establishes ownership of this exact connection.
                mTimer.execute(() -> connectedAdapter(active, binder, dead));
            }
        };
        int bound = 0;
        long identity = Binder.clearCallingIdentity();
        try {
            bound = mAm.bindService(null, null, new Intent("android.content.SyncAdapter")
                    .setComponent(active.adapter.service), null, active.connection,
                    Context.BIND_AUTO_CREATE, "android", 0);
        } catch (RuntimeException failure) {
            SystemServer.log(TAG + "adapter binding failed: " + failure);
        } finally { Binder.restoreCallingIdentity(identity); }
        active.bound = bound > 0;
        if (stopped(active)) { cancelActive(active, "cancelled during binding"); return; }
        if (!active.bound) { failed(active, "adapter could not be bound"); return; }
        watch(active);
    }

    private void connectedAdapter(Active active, IBinder binder, boolean dead) {
        if (stopped(active)) { cancelActive(active, "retired connection"); return; }
        if (dead || binder == null) { failed(active, "adapter disconnected"); return; }
        if (active.binder != null) return;
        if (!mAccountAccess.test(active.account, active.adapter.service.getPackageName())) {
            cancelActive(active, "account access revoked"); return;
        }
        active.binder = ISyncAdapter.Stub.asInterface(binder);
        try { active.binder.startSync(active, active.adapter.authority, active.account, active.extras); }
        catch (RemoteException | RuntimeException failure) { failed(active, "adapter start failed: " + failure); }
    }

    private void failed(Active active, String why) {
        SyncResult result = new SyncResult(); result.stats.numIoExceptions = 1;
        finished(active, result, why);
    }

    private void watch(Active active) {
        mTimer.schedule(() -> {
            if (stopped(active)) return;
            if (mElapsed.getAsLong() - active.heartbeat >= SYNC_TIMEOUT_MS) cancelActive(active, "timed out");
            else watch(active);
        }, SYNC_TIMEOUT_MS, TimeUnit.MILLISECONDS);
    }

    private void cancelActive(Active active, String why) {
        synchronized (this) { if (active.done) return; }
        if (active.binder != null) {
            try { active.binder.cancelSync(active); }
            catch (RemoteException | RuntimeException ignored) { }
        }
        finished(active, null, why);
    }

    private void releaseBinding(Active active) {
        if (!active.bound) return;
        active.bound = false;
        long identity = Binder.clearCallingIdentity();
        try { mAm.unbindService(active.connection); }
        catch (RuntimeException failure) { SystemServer.log(TAG + "adapter unbind failed: " + failure); }
        finally { Binder.restoreCallingIdentity(identity); }
    }

    private void finished(Active active, SyncResult result, String how) {
        synchronized (this) {
            if (active.done) return;
            active.done = true;
            if (mActive.get(active.key) == active) mActive.remove(active.key);
            if (!active.cancelled && result != null) {
                Endpoint endpoint = active.key.endpoint;
                long now = mElapsed.getAsLong();
                if (result.hasError()) {
                    Backoff previous = mBackoff.get(endpoint);
                    if (previous == null || now >= previous.until) {
                        long delay = previous == null
                                ? java.util.concurrent.ThreadLocalRandom.current().nextLong(30000, 33000)
                                : Math.min(3600000, previous.delay * 2);
                        mBackoff.put(endpoint, new Backoff(add(now, delay), delay));
                    }
                    Bundle retry = new Bundle(active.extras);
                    retry.remove(ContentResolver.SYNC_EXTRAS_IGNORE_BACKOFF);
                    boolean reschedule;
                    long earliest = 0;
                    if (retry.getBoolean(ContentResolver.SYNC_EXTRAS_DO_NOT_RETRY) && !result.syncAlreadyInProgress) {
                        reschedule = false;
                    } else if (retry.getBoolean(ContentResolver.SYNC_EXTRAS_UPLOAD) && !result.syncAlreadyInProgress) {
                        retry.remove(ContentResolver.SYNC_EXTRAS_UPLOAD); reschedule = true;
                    } else if (result.tooManyRetries) reschedule = false;
                    else if (result.madeSomeProgress()) reschedule = true;
                    else if (result.syncAlreadyInProgress) { reschedule = true; earliest = add(now, 10000); }
                    else reschedule = result.hasSoftError();
                    if (reschedule) mPending.add(new Pending(active.adapter, active.account, retry, earliest, true));
                } else mBackoff.remove(endpoint);
                long until = result.delayUntil <= 0 ? 0 : result.delayUntil > Long.MAX_VALUE / 1000
                        ? Long.MAX_VALUE : result.delayUntil * 1000;
                long wall = mWall.getAsLong();
                if (until > wall) mDelayUntil.put(endpoint, add(now, until - wall));
                else mDelayUntil.remove(endpoint);
                if (result.fullSyncRequested) {
                    // A fresh full request must not inherit retry/upload/do-not-retry extras.
                    mPending.add(new Pending(active.adapter, active.account, new Bundle(), 0, true));
                }
            }
        }
        releaseBinding(active);
        SystemServer.log(TAG + active.adapter.authority + ": " + how + " in "
                + (mElapsed.getAsLong() - active.start) + " ms");
        changed(ContentResolver.SYNC_OBSERVER_TYPE_ACTIVE | ContentResolver.SYNC_OBSERVER_TYPE_PENDING);
        mTimer.execute(this::run);
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
        for (Pending p : mPending) {
            if (callerCanAccess(p.account) && matches(p.account, p.adapter.authority, account, authority)) return true;
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
