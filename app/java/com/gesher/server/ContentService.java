/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.server;

import android.accounts.Account;
import android.content.ComponentName;
import android.content.ContentResolver;
import android.content.IContentService;
import android.content.ISyncStatusObserver;
import android.content.PeriodicSync;
import android.content.SyncAdapterType;
import android.content.SyncInfo;
import android.content.SyncRequest;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.database.IContentObserver;
import android.net.Uri;
import android.os.Bundle;
import android.os.Binder;
import android.os.IBinder;
import android.os.RemoteException;

import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.concurrent.CopyOnWriteArrayList;
import java.util.function.BiConsumer;

/**
 * {@code content}: ContentService's observers (hwsvc2 track, ADR 0051).
 * {@code ContentResolver.registerContentObserver} registers an observer for a URI (and its
 * descendants if asked); {@code notifyChange} (a provider's, usually) tells the observers
 * whose URI is the changed one, one of its descendants (unless
 * NOTIFY_SKIP_NOTIFY_FOR_DESCENDANTS) or an ancestor registered for descendants, as
 * ContentService's ObserverNode tree does, each once with all its URIs
 * ({@code onChangeEtc}); the observer that made the change only if it wants its own
 * changes. Change listeners in the system server ({@link #addChangeListener}: the job
 * scheduler's content URI triggers) see every change. Syncs and their settings are
 * {@link SyncManager}'s; a change notified with NOTIFY_SYNC_TO_NETWORK starts an upload
 * sync of its authority.
 */
final class ContentService extends IContentService.Default {
    /** An observer: ObserverEntry. */
    private final class Observer implements IBinder.DeathRecipient {
        final Uri uri;
        final boolean descendants;
        final IContentObserver observer;
        final ApkPackage owner;

        Observer(Uri uri, boolean descendants, IContentObserver observer, ApkPackage owner) {
            this.uri = uri;
            this.descendants = descendants;
            this.observer = observer;
            this.owner = owner;
        }

        @Override
        public void binderDied() {
            removeObserver(observer);
        }
    }

    private final List<Observer> mObservers = new ArrayList<>();  // guarded by itself
    private final SyncManager mSync;
    private final ActivityManagerService mAm;
    private final PackageManagerService mPm;

    ContentService(SyncManager sync) {
        mSync = sync;
        mAm = sync.activityManager();
        mPm = sync.packageManager();
    }

    private ApkPackage caller(String claimed, boolean verifyClaim) {
        if (!GmsSignatureSpoofing.isEnabled()) return null;
        CallerIdentity identity = mAm.callers();
        if (identity.isSystem()) {
            if (!verifyClaim || claimed == null || "android".equals(claimed)) return null;
            ApkPackage pkg = mPm.get(claimed);
            if (pkg == null) throw new SecurityException("Unknown content caller package");
            return pkg;
        }
        String actual = verifyClaim ? identity.requirePackage(claimed) : identity.callerPackage();
        ApkPackage pkg = mPm.get(actual);
        if (pkg == null) throw new SecurityException("Content caller was removed");
        return pkg;
    }

    private void user(int userId) {
        if (GmsSignatureSpoofing.isEnabled() && userId != 0) {
            throw new SecurityException("Only content/sync user 0 is available");
        }
    }

    private boolean allowed(ApkPackage owner, Uri uri, int mode) {
        return owner == null || mAm.uriGrants().canAccess(owner, uri, mode, 0);
    }

    private void requireReadSettings() {
        ApkPackage owner = caller(null, false);
        if (owner != null && mPm.permissions().checkPermission(owner.packageName,
                "android.permission.READ_SYNC_SETTINGS") != PackageManager.PERMISSION_GRANTED) {
            throw new SecurityException("Reading sync settings requires READ_SYNC_SETTINGS");
        }
    }
    private static final List<BiConsumer<Uri[], Integer>> sListeners =
            new CopyOnWriteArrayList<>();

    /** Every change notified from now on (its URIs and flags), on the notifier's thread. */
    static void addChangeListener(BiConsumer<Uri[], Integer> listener) {
        sListeners.add(listener);
    }

    @Override
    public void registerContentObserver(Uri uri, boolean notifyForDescendants,
            IContentObserver observer, int userHandle, int targetSdkVersion) {
        if (uri == null || observer == null) return;
        user(userHandle);
        ApkPackage owner = caller(null, false);
        if (GmsSignatureSpoofing.isEnabled()) uri = UriGrants.checkedUri(uri, userHandle);
        if (!allowed(owner, uri, Intent.FLAG_GRANT_READ_URI_PERMISSION)) {
            throw new SecurityException("Observing content requires URI read access");
        }
        Observer o = new Observer(uri, notifyForDescendants, observer, owner);
        try {
            observer.asBinder().linkToDeath(o, 0);
        } catch (RemoteException e) {
            return;  // already gone
        }
        synchronized (mObservers) {
            mObservers.add(o);
        }
    }

    @Override
    public void unregisterContentObserver(IContentObserver observer) {
        if (observer == null) return;
        ApkPackage owner = caller(null, false);
        synchronized (mObservers) {
            for (Observer o : mObservers) {
                if (o.observer.asBinder().equals(observer.asBinder()) && owner != null && o.owner != owner) {
                    throw new SecurityException("Cannot unregister another content observer");
                }
            }
        }
        removeObserver(observer);
    }

    private void removeObserver(IContentObserver observer) {
        synchronized (mObservers) {
            for (int i = mObservers.size() - 1; i >= 0; i--) {
                Observer o = mObservers.get(i);
                if (o.observer.asBinder() == observer.asBinder()) {
                    mObservers.remove(i);
                    try {
                        o.observer.asBinder().unlinkToDeath(o, 0);
                    } catch (Throwable t) {
                        // not linked
                    }
                }
            }
        }
    }

    @Override
    public void notifyChange(Uri[] uris, IContentObserver observer,
            boolean observerWantsSelfNotifications, int flags, int userHandle,
            int targetSdkVersion, String callingPackage) {
        if (uris == null || uris.length == 0) return;
        user(userHandle);
        ApkPackage notifier = caller(callingPackage, true);
        uris = uris.clone();
        // Validate the complete batch before any observer, sync or system listener sees it.
        for (int i = 0; i < uris.length; i++) {
            if (GmsSignatureSpoofing.isEnabled()) uris[i] = UriGrants.checkedUri(uris[i], userHandle);
            if (!allowed(notifier, uris[i], Intent.FLAG_GRANT_WRITE_URI_PERMISSION)) {
                throw new SecurityException("Notifying content requires URI write access");
            }
        }
        // Each observer once, with the URIs it is told of (ContentService's ObserverCollector).
        Map<IBinder, List<Uri>> told = new LinkedHashMap<>();
        Map<IBinder, Observer> whom = new LinkedHashMap<>();
        synchronized (mObservers) {
            for (Uri uri : uris) {
                for (Observer o : mObservers) {
                    if (!matches(o, uri, flags)) continue;
                    if (!allowed(o.owner, o.uri, Intent.FLAG_GRANT_READ_URI_PERMISSION)
                            || !allowed(o.owner, uri, Intent.FLAG_GRANT_READ_URI_PERMISSION)) continue;
                    boolean self = observer != null
                            && o.observer.asBinder() == observer.asBinder();
                    if (self && !observerWantsSelfNotifications) continue;
                    IBinder b = o.observer.asBinder();
                    told.computeIfAbsent(b, k -> new ArrayList<>()).add(uri);
                    whom.putIfAbsent(b, o);
                }
            }
        }
        for (Map.Entry<IBinder, List<Uri>> e : told.entrySet()) {
            Observer o = whom.get(e.getKey());
            // Earlier observer callbacks can revoke a grant or replace another
            // observer's installed code. Recheck at delivery, after those callbacks.
            List<Uri> authorized = new ArrayList<>();
            for (Uri uri : e.getValue()) {
                if (allowed(o.owner, o.uri, Intent.FLAG_GRANT_READ_URI_PERMISSION)
                        && allowed(o.owner, uri, Intent.FLAG_GRANT_READ_URI_PERMISSION)) authorized.add(uri);
            }
            if (authorized.isEmpty()) continue;
            boolean self = observer != null && e.getKey() == observer.asBinder();
            try {
                o.observer.onChangeEtc(self, authorized.toArray(new Uri[0]), flags,
                        userHandle < 0 ? 0 : userHandle);
            } catch (RemoteException ex) {
                removeObserver(o.observer);
            }
        }
        if ((flags & ContentResolver.NOTIFY_SYNC_TO_NETWORK) != 0) {
            for (Uri uri : uris) mSync.localChange(uri, notifier);
        }
        for (BiConsumer<Uri[], Integer> l : sListeners) {
            try {
                l.accept(uris.clone(), flags);
            } catch (Throwable t) {
                SystemServer.log("content: a change listener failed: " + t);
            }
        }
    }

    // --- sync (SyncManager) -------------------------------------------------------------

    @Override
    public void requestSync(Account account, String authority, Bundle extras,
            String callingPackage) {
        caller(callingPackage, true);
        mSync.requestSync(account, authority, extras, false);
    }

    @Override
    public void sync(SyncRequest request, String callingPackage) {
        syncAsUser(request, 0, callingPackage);
    }

    @Override
    public void syncAsUser(SyncRequest request, int userId, String callingPackage) {
        user(userId);
        caller(callingPackage, true);
        if (request == null) return;
        if (request.isPeriodic()) {
            mSync.addPeriodicSync(request.getAccount(), request.getProvider(),
                    request.getBundle(), request.getSyncRunTime());
        } else {
            mSync.requestSync(request.getAccount(), request.getProvider(), request.getBundle(),
                    false);
        }
    }

    @Override
    public void cancelSync(Account account, String authority, ComponentName cname) {
        caller(null, false);
        mSync.cancelSync(account, authority);
    }

    @Override
    public void cancelSyncAsUser(Account account, String authority, ComponentName cname,
            int userId) {
        user(userId);
        caller(null, false);
        mSync.cancelSync(account, authority);
    }

    @Override
    public void cancelRequest(SyncRequest request) {
        caller(null, false);
        if (request == null) return;
        if (request.isPeriodic()) {
            mSync.removePeriodicSync(request.getAccount(), request.getProvider(),
                    request.getBundle());
        } else {
            mSync.cancelSync(request.getAccount(), request.getProvider());
        }
    }

    @Override
    public boolean getSyncAutomatically(Account account, String providerName) {
        requireReadSettings();
        return mSync.getSyncAutomatically(account, providerName);
    }

    @Override
    public boolean getSyncAutomaticallyAsUser(Account account, String providerName, int userId) {
        user(userId);
        requireReadSettings();
        return mSync.getSyncAutomatically(account, providerName);
    }

    @Override
    public void setSyncAutomatically(Account account, String providerName, boolean sync) {
        mSync.setSyncAutomatically(account, providerName, sync);
    }

    @Override
    public void setSyncAutomaticallyAsUser(Account account, String providerName, boolean sync,
            int userId) {
        user(userId);
        mSync.setSyncAutomatically(account, providerName, sync);
    }

    @Override
    public List<PeriodicSync> getPeriodicSyncs(Account account, String providerName,
            ComponentName cname) {
        requireReadSettings();
        return mSync.getPeriodicSyncs(account, providerName);
    }

    @Override
    public void addPeriodicSync(Account account, String providerName, Bundle extras,
            long pollFrequency) {
        mSync.addPeriodicSync(account, providerName, extras, pollFrequency);
    }

    @Override
    public void removePeriodicSync(Account account, String providerName, Bundle extras) {
        mSync.removePeriodicSync(account, providerName, extras);
    }

    @Override
    public int getIsSyncable(Account account, String providerName) {
        requireReadSettings();
        return mSync.getIsSyncable(account, providerName);
    }

    @Override
    public int getIsSyncableAsUser(Account account, String providerName, int userId) {
        user(userId);
        requireReadSettings();
        return mSync.getIsSyncable(account, providerName);
    }

    @Override
    public void setIsSyncable(Account account, String providerName, int syncable) {
        mSync.setIsSyncable(account, providerName, syncable);
    }

    @Override
    public void setIsSyncableAsUser(Account account, String providerName, int syncable,
            int userId) {
        user(userId);
        mSync.setIsSyncable(account, providerName, syncable);
    }

    @Override
    public void setMasterSyncAutomatically(boolean flag) {
        mSync.setMasterSyncAutomatically(flag);
    }

    @Override
    public void setMasterSyncAutomaticallyAsUser(boolean flag, int userId) {
        user(userId);
        mSync.setMasterSyncAutomatically(flag);
    }

    @Override
    public boolean getMasterSyncAutomatically() {
        requireReadSettings();
        return mSync.getMasterSyncAutomatically();
    }

    @Override
    public boolean getMasterSyncAutomaticallyAsUser(int userId) {
        user(userId);
        requireReadSettings();
        return mSync.getMasterSyncAutomatically();
    }

    @Override
    public List<SyncInfo> getCurrentSyncs() {
        return mSync.getCurrentSyncs();
    }

    @Override
    public List<SyncInfo> getCurrentSyncsAsUser(int userId) {
        user(userId);
        return mSync.getCurrentSyncs();
    }

    @Override
    public SyncAdapterType[] getSyncAdapterTypes() {
        return mSync.getSyncAdapterTypes();
    }

    @Override
    public SyncAdapterType[] getSyncAdapterTypesAsUser(int userId) {
        user(userId);
        return mSync.getSyncAdapterTypes();
    }

    @Override
    public String[] getSyncAdapterPackagesForAuthorityAsUser(String authority, int userId) {
        user(userId);
        return mSync.getSyncAdapterPackagesForAuthority(authority);
    }

    @Override
    public boolean isSyncActive(Account account, String authority, ComponentName cname) {
        return mSync.isSyncActive(account, authority);
    }

    @Override
    public boolean isSyncPending(Account account, String authority, ComponentName cname) {
        return mSync.isSyncPending(account, authority);
    }

    @Override
    public boolean isSyncPendingAsUser(Account account, String authority, ComponentName cname,
            int userId) {
        user(userId);
        return mSync.isSyncPending(account, authority);
    }

    @Override
    public void addStatusChangeListener(int mask, ISyncStatusObserver callback) {
        mSync.addStatusChangeListener(mask, callback);
    }

    @Override
    public void removeStatusChangeListener(ISyncStatusObserver callback) {
        mSync.removeStatusChangeListener(callback);
    }

    /** Whether an observer of {@code o.uri} is told of a change of {@code changed}. */
    static boolean matches(Uri registered, boolean descendants, Uri changed, int flags) {
        if (!same(registered.getScheme(), changed.getScheme())
                || !same(registered.getAuthority(), changed.getAuthority())) {
            return false;
        }
        List<String> r = registered.getPathSegments();
        List<String> c = changed.getPathSegments();
        int common = Math.min(r.size(), c.size());
        for (int i = 0; i < common; i++) {
            if (!r.get(i).equals(c.get(i))) return false;
        }
        if (r.size() == c.size()) return true;
        if (r.size() < c.size()) return descendants;  // an ancestor of the change
        // A descendant of the change.
        return (flags & ContentResolver.NOTIFY_SKIP_NOTIFY_FOR_DESCENDANTS) == 0;
    }

    private static boolean matches(Observer o, Uri changed, int flags) {
        return matches(o.uri, o.descendants, changed, flags);
    }

    private static boolean same(String a, String b) {
        return a == null ? b == null : a.equals(b);
    }
}
