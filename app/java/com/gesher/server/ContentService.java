/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.server;

import android.content.ContentResolver;
import android.content.IContentService;
import android.database.IContentObserver;
import android.net.Uri;
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
 * scheduler's content URI triggers) see every change. No sync adapters (requestSync and
 * the sync settings keep their defaults).
 */
final class ContentService extends IContentService.Default {
    /** An observer: ObserverEntry. */
    private final class Observer implements IBinder.DeathRecipient {
        final Uri uri;
        final boolean descendants;
        final IContentObserver observer;

        Observer(Uri uri, boolean descendants, IContentObserver observer) {
            this.uri = uri;
            this.descendants = descendants;
            this.observer = observer;
        }

        @Override
        public void binderDied() {
            unregisterContentObserver(observer);
        }
    }

    private final List<Observer> mObservers = new ArrayList<>();  // guarded by itself
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
        Observer o = new Observer(uri, notifyForDescendants, observer);
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
        // Each observer once, with the URIs it is told of (ContentService's ObserverCollector).
        Map<IBinder, List<Uri>> told = new LinkedHashMap<>();
        Map<IBinder, Observer> whom = new LinkedHashMap<>();
        synchronized (mObservers) {
            for (Uri uri : uris) {
                for (Observer o : mObservers) {
                    if (!matches(o, uri, flags)) continue;
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
            boolean self = observer != null && e.getKey() == observer.asBinder();
            try {
                o.observer.onChangeEtc(self, e.getValue().toArray(new Uri[0]), flags,
                        userHandle < 0 ? 0 : userHandle);
            } catch (RemoteException ex) {
                unregisterContentObserver(o.observer);
            }
        }
        for (BiConsumer<Uri[], Integer> l : sListeners) {
            try {
                l.accept(uris, flags);
            } catch (Throwable t) {
                SystemServer.log("content: a change listener failed: " + t);
            }
        }
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
