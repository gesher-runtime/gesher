/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.server;

import android.os.Binder;
import android.os.IBinder;
import android.os.Process;
import android.os.RemoteException;

import com.gesher.net.IConnectivity;
import com.gesher.net.WindowsNetworks;

import java.util.ArrayList;
import java.util.Collection;
import java.util.List;

/**
 * {@code connectivity}: ConnectivityService on Windows (docs/network.md, ADR 0037). It
 * keeps a snapshot of Windows' networks ({@link WindowsNetworks}), taken again whenever
 * Windows reports a change of an interface, an address or a route (and every 15 s while
 * observed, for what the Network List Manager learns, such as validation or cost), and calls its
 * listeners, the ConnectivityManagers of app processes, with each new one. Callers need
 * ACCESS_NETWORK_STATE, as ConnectivityService enforces it; INTERNET is enforced in the
 * app's own process ({@link #internetEnvironment}).
 */
public final class ConnectivityService extends IConnectivity.Stub implements WindowsHost.Listener {
    public static final String ACCESS_NETWORK_STATE = "android.permission.ACCESS_NETWORK_STATE";
    public static final String INTERNET = "android.permission.INTERNET";
    private static final int OBSERVATION_INTERVAL_MS = 15_000;

    private static volatile ConnectivityService sInstance;

    private final PackageManagerService mPm;
    private final ActivityTaskManagerService mAtm;
    private final List<ListenerRecord> mListeners = new ArrayList<>();
    private final List<Runnable> mSystemListeners = new ArrayList<>();
    // Guarded by this. A native wait can drain after demand ends; mWatching, not
    // the presence of that retiring thread, decides whether a cached answer is current.
    private boolean mWatching;
    private boolean mBroadcastDemand;
    private long mBroadcastGeneration;
    private String mSnapshot;
    private String mPublishedSnapshot;
    private Thread mWatcher;
    private boolean mRefreshing;
    private boolean mRefreshPending;

    private final class ListenerRecord implements IBinder.DeathRecipient {
        final IConnectivity.IListener listener;
        final IBinder binder;
        boolean retired; // Guarded by ConnectivityService.this, including pre-publication death.
        ListenerRecord(IConnectivity.IListener listener) {
            this.listener = listener;
            binder = listener.asBinder();
        }
        @Override public void binderDied() { removeListener(this); }
        void unlink() {
            if (!(binder instanceof Binder)) {
                try { binder.unlinkToDeath(this, 0); }
                catch (java.util.NoSuchElementException alreadyDead) { /* Death consumed the link. */ }
            }
        }
    }

    ConnectivityService(PackageManagerService pm, ActivityTaskManagerService atm) {
        mPm = pm;
        mAtm = atm;
        sInstance = this;
        StickyBroadcasts.addDynamic(android.net.ConnectivityManager.CONNECTIVITY_ACTION,
                this::broadcastSnapshot, this::setBroadcastDemand);
    }

    /** Default-network baseline for the current broadcast receivers; null before their first read. */
    private String mBroadcastState;

    private synchronized android.content.Intent broadcastSnapshot() {
        WindowsNetworks.Snapshot current = WindowsNetworks.parse(snapshot());
        // Registration precedes this query. Seed its baseline here if the watcher
        // has not run yet, so a change between the sticky answer and its first read
        // still produces the required broadcast.
        if (mBroadcastDemand && mBroadcastState == null) mBroadcastState = broadcastState(current);
        return android.net.ConnectivityManager.connectivityAction(current);
    }

    private static String broadcastState(WindowsNetworks.Snapshot snapshot) {
        WindowsNetworks.Net network = snapshot.defaultNetwork();
        return network == null ? "none" : network.netId() + " " + network.kind;
    }

    private synchronized void setBroadcastDemand(boolean required) {
        if (mBroadcastDemand == required) return;
        mBroadcastDemand = required;
        mBroadcastState = null;
        mBroadcastGeneration++;
        updateWatchingLocked();
    }

    /**
     * The first receiver gets current state through its sticky query. Later changes
     * are delivered normally. Only the refresh dispatcher calls this, under our monitor.
     */
    private android.content.Intent broadcastChangeLocked(String text) {
        if (!mBroadcastDemand) return null;
        WindowsNetworks.Snapshot s = WindowsNetworks.parse(text);
        String state = broadcastState(s);
        String previous = mBroadcastState;
        mBroadcastState = state;
        return previous == null || state.equals(previous) ? null
                : android.net.ConnectivityManager.connectivityAction(s);
    }

    private void sendBroadcast(android.content.Intent intent, long generation) {
        synchronized (this) {
            if (!mBroadcastDemand || generation != mBroadcastGeneration) return;
        }
        Services.Created created = Services.sLast;
        if (created != null) {
            try {
                created.am.sendBroadcastFor("android", intent, null);
            } catch (RuntimeException e) {
                SystemServer.log("connectivity: CONNECTIVITY_ACTION not sent: " + e);
            }
        }
    }

    /**
     * What an app process of {@code packageName} gets in its environment for Android's
     * INTERNET permission: {@code GESHER_INET=0} without it (gesher-base then refuses inet
     * sockets and name lookups, as Android without the inet group), or null.
     */
    public static String internetEnvironment(String packageName) {
        ConnectivityService service = sInstance;
        ApkPackage p = service != null ? service.find(packageName) : null;
        return p != null && !requests(p, INTERNET) ? "GESHER_INET=0" : null;
    }

    /**
     * Applies Android's INTERNET permission to this process, an app process of {@code pkg}
     * that the system server did not start (gesher-app's single process):
     * {@link WindowsNetworks#setInternetAllowed}.
     */
    public static void applyInternetPermission(ApkPackage pkg) {
        WindowsNetworks.setInternetAllowed(requests(pkg, INTERNET));
    }

    /** As {@link #applyInternetPermission(ApkPackage)}, from the package's PackageInfo. */
    public static void applyInternetPermission(android.content.pm.PackageInfo info) {
        String[] requested = info != null ? info.requestedPermissions : null;
        WindowsNetworks.setInternetAllowed(
                requested != null && java.util.Arrays.asList(requested).contains(INTERNET));
    }

    /** Whether {@code pkg} asks for {@code permission} (normal ones are granted at install). */
    static boolean requests(ApkPackage pkg, String permission) {
        Collection<String> requested = pkg.parsed.getRequestedPermissions();
        return requested != null && requested.contains(permission);
    }

    private ApkPackage find(String packageName) {
        for (ApkPackage p : mPm.packages()) {
            if (p.packageName.equals(packageName)) return p;
        }
        return null;
    }

    /** The package of the calling process, null for the system (this process). */
    private ApkPackage caller() {
        int pid = Binder.getCallingPid();
        String name = null;
        if (mAtm != null) {
            ActivityTaskManagerService.ProcessRecord p = mAtm.callerProcess(pid);
            if (p != null) name = p.packageName;
        }
        if (name != null) return find(name);
        if (pid == Process.myPid() && !inSystemServer()) {
            // gesher-app's single process: its one app.
            Collection<ApkPackage> all = mPm.packages();
            return all.size() == 1 ? all.iterator().next() : null;
        }
        return null;  // the system server itself
    }

    private static Boolean sInSystemServer;

    private static boolean inSystemServer() {
        if (sInSystemServer == null) {
            String cmd;
            try {
                cmd = Zygote.nativeCommandLine();
            } catch (Throwable t) {
                cmd = "";
            }
            sInSystemServer = cmd.contains(" " + SystemServer.class.getName());
        }
        return sInSystemServer;
    }

    private void enforceNetworkState() {
        ApkPackage p = caller();
        if (p != null && !requests(p, ACCESS_NETWORK_STATE)) {
            throw new SecurityException(p.packageName + " (pid " + Binder.getCallingPid()
                    + ") was not granted " + ACCESS_NETWORK_STATE);
        }
    }

    @Override
    public String getSnapshot() {
        enforceNetworkState();
        return snapshot();
    }

    @Override
    public void addListener(IConnectivity.IListener listener) throws RemoteException {
        enforceNetworkState();
        if (listener == null) return;
        ListenerRecord record = new ListenerRecord(listener);
        // Link before publication. A failed/dead endpoint must not acquire demand;
        // death during linking marks this exact record retired before it can be added.
        if (!(record.binder instanceof Binder)) record.binder.linkToDeath(record, 0);
        boolean added = false;
        synchronized (this) {
            boolean duplicate = false;
            for (ListenerRecord existing : mListeners) {
                if (existing.binder == record.binder) { duplicate = true; break; }
            }
            if (!record.retired && !duplicate) {
                mListeners.add(record);
                updateWatchingLocked();
                added = true;
            }
        }
        if (!added) record.unlink();
    }

    @Override
    public void removeListener(IConnectivity.IListener listener) {
        if (listener == null) return;
        ListenerRecord removed = null;
        synchronized (this) {
            for (ListenerRecord record : mListeners) {
                if (record.binder == listener.asBinder()) { removed = record; break; }
            }
            if (removed != null) retireLocked(removed);
        }
        if (removed != null) removed.unlink();
    }

    private void retireLocked(ListenerRecord record) {
        record.retired = true;
        mListeners.remove(record);
        updateWatchingLocked();
    }

    private void removeListener(ListenerRecord record) {
        boolean registered;
        synchronized (this) {
            registered = mListeners.contains(record);
            retireLocked(record);
        }
        if (registered) record.unlink();
    }

    /** In-process system observers do not inherit an app's Binder identity. */
    synchronized void addSystemListener(Runnable listener) {
        if (!mSystemListeners.contains(listener)) mSystemListeners.add(listener);
        updateWatchingLocked();
    }

    synchronized void removeSystemListener(Runnable listener) {
        mSystemListeners.remove(listener);
        updateWatchingLocked();
    }

    /** A scheduler resume can precede our WindowsHost callback. Keep publication separate. */
    synchronized void invalidateSnapshot() { mSnapshot = null; }

    String snapshotForSystem() { return snapshot(); }

    private synchronized String snapshot() {
        // Queries alone do not start the watcher. Do not cache their first answer
        // forever, or an app polling after a reconnect will keep seeing startup
        // state. Once subscribed, the watcher owns the shared snapshot instead.
        if (!mWatching) return WindowsNetworks.snapshot();
        if (mSnapshot == null) mSnapshot = WindowsNetworks.snapshot();
        return mSnapshot;
    }

    private void updateWatchingLocked() {
        boolean required = !mListeners.isEmpty() || !mSystemListeners.isEmpty() || mBroadcastDemand;
        if (required == mWatching) return;
        mWatching = required;
        mSnapshot = null;
        mPublishedSnapshot = null;
        mRefreshPending = false;
        if (required) {
            WindowsHost.addListener(this);
            startWatcherLocked();
        } else {
            WindowsHost.removeListener(this);
            // Native network waits are not interruptible. Their one outstanding wait
            // drains within 15 s (+ the native 300 ms debounce), without another read.
            // Reacquiring demand before then reuses that thread and an invalidated cache.
            notifyAll();
        }
    }

    private void startWatcherLocked() {
        if (mWatcher != null) return;
        mWatcher = new Thread(this::watch, "ConnectivityThread");
        mWatcher.setDaemon(true);
        mWatcher.start();
    }

    private void watch() {
        try {
            while (true) {
                refresh();
                synchronized (this) { if (!mWatching) return; }
                try {
                    WindowsNetworks.waitForChange(OBSERVATION_INTERVAL_MS);
                } catch (RuntimeException | LinkageError unavailable) {
                    SystemServer.log("connectivity: network notifications unavailable: " + unavailable);
                    synchronized (this) {
                        if (!mWatching) return;
                        wait(OBSERVATION_INTERVAL_MS);
                    }
                }
            }
        } catch (InterruptedException interrupted) {
            Thread.currentThread().interrupt();
        } finally {
            synchronized (this) {
                mWatcher = null;
                // A first owner may arrive between the final demand check and this
                // cleanup. Only the retiring thread can hand its slot to a successor.
                if (mWatching) startWatcherLocked();
            }
        }
    }

    @Override public void onWindowsEvents(int events) {
        if ((events & WindowsHost.EVENT_RESUMED) != 0) refresh();
    }

    /**
     * Serializes native observations and publication, including resume, without holding
     * a service/event lock across callbacks. Overlapping refresh signals coalesce to one
     * further read; no snapshot queue or extra worker can grow behind a slow listener.
     */
    private void refresh() {
        synchronized (this) {
            if (!mWatching) return;
            mRefreshPending = true;
            if (mRefreshing) return;
            mRefreshing = true;
        }
        try {
            while (true) {
                String now;
                List<ListenerRecord> listeners;
                List<Runnable> systemListeners;
                android.content.Intent broadcast;
                long broadcastGeneration;
                boolean changed;
                synchronized (this) {
                    if (!mWatching || !mRefreshPending) {
                        mRefreshing = false;
                        return;
                    }
                    mRefreshPending = false;
                    now = WindowsNetworks.snapshot();
                    mSnapshot = now;
                    broadcast = broadcastChangeLocked(now);
                    broadcastGeneration = mBroadcastGeneration;
                    changed = !now.equals(mPublishedSnapshot);
                    if (!changed) {
                        listeners = java.util.Collections.emptyList();
                        systemListeners = java.util.Collections.emptyList();
                    } else {
                        mPublishedSnapshot = now;
                        listeners = new ArrayList<>(mListeners);
                        systemListeners = new ArrayList<>(mSystemListeners);
                    }
                }
                if (changed && Boolean.getBoolean("gesher.net.trace")) {
                    SystemServer.log("connectivity: Windows' networks changed:\n" + now);
                }
                if (broadcast != null) sendBroadcast(broadcast, broadcastGeneration);
                for (Runnable listener : systemListeners) {
                    synchronized (this) { if (!mSystemListeners.contains(listener)) continue; }
                    try { listener.run(); }
                    catch (RuntimeException e) { SystemServer.log("connectivity: system listener failed: " + e); }
                }
                for (ListenerRecord record : listeners) {
                    synchronized (this) { if (record.retired) continue; }
                    try { record.listener.onSnapshot(now); }
                    catch (RemoteException | RuntimeException e) { removeListener(record); }
                }
            }
        } catch (RuntimeException | LinkageError unavailable) {
            synchronized (this) {
                mRefreshing = false;
                mRefreshPending = false;
            }
            SystemServer.log("connectivity: network observation unavailable: " + unavailable);
        }
    }
}
