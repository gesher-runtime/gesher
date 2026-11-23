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

import java.util.ArrayDeque;
import java.util.ArrayList;
import java.util.Comparator;
import java.util.HashMap;
import java.util.IdentityHashMap;
import java.util.List;
import java.util.Map;
import java.util.concurrent.CopyOnWriteArrayList;
import java.util.function.BiConsumer;

/**
 * {@code content}: ContentService's observers (hwsvc2 track, ADR 0051).
 * {@code ContentResolver.registerContentObserver} registers an observer for a URI (and its
 * descendants if asked); {@code notifyChange} (a provider's, usually) tells the observers
 * whose URI is the changed one, one of its descendants or an ancestor registered for
 * descendants, as Android's ObserverNode tree does. NOTIFY_SKIP_NOTIFY_FOR_DESCENDANTS
 * filters descendants-enabled subscriptions at the changed URI and below it, while
 * exact-only subscriptions still fire. Each observer is called once with all its URIs
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
        final IBinder binder;
        final ApkPackage owner;
        boolean retired;  // guarded by mObservers, including before publication
        long order;
        ObserverNode node;

        Observer(Uri uri, boolean descendants, IContentObserver observer, ApkPackage owner) {
            this.uri = uri;
            this.descendants = descendants;
            this.observer = observer;
            this.binder = observer.asBinder();
            this.owner = owner;
        }

        @Override
        public void binderDied() {
            List<Observer> removed;
            synchronized (mObservers) {
                if (retired) return;
                // Death may arrive after linkToDeath succeeds but before the
                // registering thread publishes this entry in mObservers.
                retired = true;
                removed = removeObserversLocked(binder);
            }
            for (Observer entry : removed) unlink(entry);
        }
    }

    /** A path component. Empty branches are removed when their last registration retires. */
    private static final class ObserverNode {
        final ObserverNode parent;
        final String segment;
        Map<String, ObserverNode> children = Map.of();
        List<Observer> observers = List.of();
        int subtreeSize;

        ObserverNode(ObserverNode parent, String segment) {
            this.parent = parent;
            this.segment = segment;
        }
    }

    private record ObserverSelection(List<Observer> observers, boolean needsMatch) { }

    private static final ObserverSelection NO_OBSERVERS = new ObserverSelection(List.of(), false);
    private final List<Observer> mObservers = new ArrayList<>();  // guarded by itself
    private final ObserverSelection mAllObservers = new ObserverSelection(mObservers, false);
    private final ObserverSelection mLinearObservers = new ObserverSelection(mObservers, true);
    // This index is a candidate selector, not an authorization cache. Both the
    // registration's URI and every changed URI are checked again for each delivery.
    private final Map<String, Map<String, ObserverNode>> mObserverRoots = new HashMap<>();
    private long mNextObserverOrder;  // guarded by mObservers

    private void indexObserverLocked(Observer observer) {
        Map<String, ObserverNode> authorities = mObserverRoots.computeIfAbsent(
                observer.uri.getScheme(), key -> new HashMap<>());
        ObserverNode node = authorities.computeIfAbsent(observer.uri.getAuthority(),
                key -> new ObserverNode(null, null));
        for (String segment : observer.uri.getPathSegments()) {
            ObserverNode child = node.children.get(segment);
            if (child == null) {
                if (node.children.isEmpty()) node.children = new HashMap<>();
                child = new ObserverNode(node, segment);
                node.children.put(segment, child);
            }
            node = child;
        }
        observer.order = mNextObserverOrder++;
        observer.node = node;
        if (node.observers.isEmpty()) node.observers = new ArrayList<>();
        node.observers.add(observer);
        for (ObserverNode ancestor = node; ancestor != null; ancestor = ancestor.parent) ancestor.subtreeSize++;
    }

    private void unindexObserverLocked(Observer observer) {
        ObserverNode node = observer.node;
        if (node == null) return;
        observer.node = null;
        node.observers.remove(observer);
        for (ObserverNode ancestor = node; ancestor != null; ancestor = ancestor.parent) ancestor.subtreeSize--;
        if (node.observers.isEmpty()) node.observers = List.of();
        while (node.observers.isEmpty() && node.children.isEmpty()) {
            ObserverNode parent = node.parent;
            if (parent == null) {
                Map<String, ObserverNode> authorities = mObserverRoots.get(observer.uri.getScheme());
                authorities.remove(observer.uri.getAuthority());
                if (authorities.isEmpty()) mObserverRoots.remove(observer.uri.getScheme());
                break;
            }
            parent.children.remove(node.segment);
            if (parent.children.isEmpty()) parent.children = Map.of();
            node = parent;
        }
    }

    /** Select narrow paths by index; broad invalidations retain the ordered linear walk. */
    private ObserverSelection candidateObserversLocked(Uri changed, int flags) {
        if (mObservers.isEmpty()) return NO_OBSERVERS;
        Map<String, ObserverNode> authorities = mObserverRoots.get(changed.getScheme());
        ObserverNode node = authorities == null ? null : authorities.get(changed.getAuthority());
        if (node == null) return NO_OBSERVERS;
        ObserverNode ancestor = null;
        for (String segment : changed.getPathSegments()) {
            ancestor = node;
            node = node.children.get(segment);
            if (node == null) break;
        }
        // A complete subtree already proves the URI predicate for every live
        // registration. Keep the original ordered registry and avoid repeating
        // its scheme/authority/path comparisons for an all-observer invalidation.
        if (node != null && node.subtreeSize == mObservers.size()
                && (flags & ContentResolver.NOTIFY_SKIP_NOTIFY_FOR_DESCENDANTS) == 0) return mAllObservers;
        int candidateCount = node == null ? 0 : node.subtreeSize;
        boolean ancestorsEmpty = true;
        for (ObserverNode parent = ancestor; parent != null; parent = parent.parent) {
            candidateCount += parent.observers.size();
            if (!parent.observers.isEmpty()) ancestorsEmpty = false;
        }
        // Count ancestor subscriptions too: a narrow URI can still notify most
        // observers when they all subscribe to the same ancestor. For such broad
        // deliveries, avoid collecting/sorting a second copy of the registry.
        if (candidateCount >= Math.max(16, mObservers.size() / 2)) return mLinearObservers;
        if (ancestorsEmpty && node == null) return NO_OBSERVERS;
        if (ancestorsEmpty && node.children.isEmpty()
                && (flags & ContentResolver.NOTIFY_SKIP_NOTIFY_FOR_DESCENDANTS) == 0) {
            return new ObserverSelection(node.observers, false);
        }
        List<Observer> candidates = new ArrayList<>();
        for (; ancestor != null; ancestor = ancestor.parent) {
            for (Observer observer : ancestor.observers) {
                if (observer.descendants) candidates.add(observer);
            }
        }
        if (node != null) {
            boolean skipDescendants = (flags & ContentResolver.NOTIFY_SKIP_NOTIFY_FOR_DESCENDANTS) != 0;
            for (Observer observer : node.observers) {
                if (!skipDescendants || !observer.descendants) candidates.add(observer);
            }
            if (!node.children.isEmpty()) {
                // Iterative traversal handles deep application URIs without
                // putting their path length on the system-server stack.
                ArrayDeque<ObserverNode> pending = new ArrayDeque<>(node.children.values());
                while (!pending.isEmpty()) {
                    ObserverNode current = pending.removeLast();
                    for (Observer observer : current.observers) {
                        if (!skipDescendants || !observer.descendants) candidates.add(observer);
                    }
                    pending.addAll(current.children.values());
                }
            }
        }
        // Preserve first-seen callback order, including multiple registrations
        // per Binder. The tree's traversal order does not determine delivery.
        candidates.sort(Comparator.comparingLong(observer -> observer.order));
        return new ObserverSelection(candidates, false);
    }
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
            o.binder.linkToDeath(o, 0);
        } catch (RemoteException e) {
            return;  // already gone
        }
        boolean retained;
        synchronized (mObservers) {
            retained = !o.retired;
            if (retained) {
                indexObserverLocked(o);
                mObservers.add(o);
            }
        }
        if (!retained) unlink(o);
    }

    @Override
    public void unregisterContentObserver(IContentObserver observer) {
        if (observer == null) return;
        ApkPackage owner = caller(null, false);
        IBinder binder = observer.asBinder();
        List<Observer> removed;
        synchronized (mObservers) {
            for (Observer o : mObservers) {
                if (o.binder == binder && owner != null && o.owner != owner) {
                    throw new SecurityException("Cannot unregister another content observer");
                }
            }
            removed = removeObserversLocked(binder);
        }
        for (Observer entry : removed) unlink(entry);
    }

    private void removeObserver(Observer observer) {
        List<Observer> removed;
        synchronized (mObservers) {
            // A snapshotted callback can fail after this entry was replaced.
            if (observer.retired) return;
            removed = removeObserversLocked(observer.binder);
        }
        for (Observer entry : removed) unlink(entry);
    }

    private List<Observer> removeObserversLocked(IBinder binder) {
        List<Observer> removed = new ArrayList<>();
        for (int i = mObservers.size() - 1; i >= 0; i--) {
            Observer entry = mObservers.get(i);
            if (entry.binder == binder) {
                entry.retired = true;
                unindexObserverLocked(entry);
                removed.add(mObservers.remove(i));
            }
        }
        return removed;
    }

    private void unlink(Observer entry) {
        try {
            entry.binder.unlinkToDeath(entry, 0);
        } catch (Throwable ignored) {
            // A death notification can already have retired the native link.
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
        IBinder notifyingObserver = observer == null ? null : observer.asBinder();
        Map<IBinder, List<Uri>> told = null;
        // Preserve first-seen delivery order without calling Binder hashCode or
        // equals implementations while the observer registry is locked.
        List<Observer> whom = null;
        synchronized (mObservers) {
            for (Uri uri : uris) {
                ObserverSelection selection = candidateObserversLocked(uri, flags);
                List<Observer> candidates = selection.observers;
                if (candidates.isEmpty()) continue;
                if (told == null) {
                    told = new IdentityHashMap<>();
                    whom = new ArrayList<>();
                }
                for (Observer o : candidates) {
                    if (selection.needsMatch && !matches(o.uri, o.descendants, uri, flags)) continue;
                    if (!allowed(o.owner, o.uri, Intent.FLAG_GRANT_READ_URI_PERMISSION)
                            || !allowed(o.owner, uri, Intent.FLAG_GRANT_READ_URI_PERMISSION)) continue;
                    boolean self = notifyingObserver != null && o.binder == notifyingObserver;
                    if (self && !observerWantsSelfNotifications) continue;
                    List<Uri> changes = told.get(o.binder);
                    if (changes == null) {
                        changes = new ArrayList<>();
                        told.put(o.binder, changes);
                        whom.add(o);
                    }
                    changes.add(uri);
                }
            }
        }
        if (whom != null) for (Observer o : whom) {
            // Earlier observer callbacks can revoke a grant or replace another
            // observer's installed code. Recheck at delivery, after those callbacks.
            List<Uri> authorized = new ArrayList<>();
            for (Uri uri : told.get(o.binder)) {
                if (allowed(o.owner, o.uri, Intent.FLAG_GRANT_READ_URI_PERMISSION)
                        && allowed(o.owner, uri, Intent.FLAG_GRANT_READ_URI_PERMISSION)) authorized.add(uri);
            }
            if (authorized.isEmpty()) continue;
            boolean self = notifyingObserver != null && o.binder == notifyingObserver;
            try {
                o.observer.onChangeEtc(self, authorized.toArray(new Uri[0]), flags,
                        userHandle < 0 ? 0 : userHandle);
            } catch (RemoteException ex) {
                removeObserver(o);
            } catch (RuntimeException ex) {
                // A local observer runs on this thread. Its exception must not
                // suppress other observers, sync scheduling or system listeners.
                SystemServer.log("content: an observer callback failed: " + ex);
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
        if (r.size() < c.size()) return descendants;  // an ancestor of the change
        // Android's ObserverNode treats equal and descendant registrations as
        // leaf observers. The skip flag filters their subscription mode, not
        // their position in the subtree; exact-only subscriptions still fire.
        return !descendants || (flags & ContentResolver.NOTIFY_SKIP_NOTIFY_FOR_DESCENDANTS) == 0;
    }

    private static boolean same(String a, String b) {
        return a == null ? b == null : a.equals(b);
    }
}
