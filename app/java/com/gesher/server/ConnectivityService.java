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
 * Windows reports a change of an interface, an address or a route (and every 15 s, for
 * what the Network List Manager learns, such as validation or cost), and calls its
 * listeners, the ConnectivityManagers of app processes, with each new one. Callers need
 * ACCESS_NETWORK_STATE, as ConnectivityService enforces it; INTERNET is enforced in the
 * app's own process ({@link #internetEnvironment}).
 */
public final class ConnectivityService extends IConnectivity.Stub {
    public static final String ACCESS_NETWORK_STATE = "android.permission.ACCESS_NETWORK_STATE";
    public static final String INTERNET = "android.permission.INTERNET";

    private static volatile ConnectivityService sInstance;

    private final PackageManagerService mPm;
    private final ActivityTaskManagerService mAtm;
    private final List<IConnectivity.IListener> mListeners = new ArrayList<>();
    private String mSnapshot;
    private Thread mWatcher;

    ConnectivityService(PackageManagerService pm, ActivityTaskManagerService atm) {
        mPm = pm;
        mAtm = atm;
        sInstance = this;
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
        IBinder b = listener.asBinder();
        synchronized (this) {
            for (IConnectivity.IListener l : mListeners) {
                if (l.asBinder() == b) return;
            }
            mListeners.add(listener);
            startWatching();
        }
        if (!(b instanceof Binder)) {
            b.linkToDeath(() -> removeListener(listener), 0);
        }
    }

    @Override
    public void removeListener(IConnectivity.IListener listener) {
        if (listener == null) return;
        synchronized (this) {
            mListeners.removeIf(l -> l.asBinder() == listener.asBinder());
        }
    }

    private synchronized String snapshot() {
        if (mSnapshot == null) mSnapshot = WindowsNetworks.snapshot();
        return mSnapshot;
    }

    private synchronized void startWatching() {
        if (mWatcher != null) return;
        mWatcher = new Thread(this::watch, "ConnectivityThread");
        mWatcher.setDaemon(true);
        mWatcher.start();
    }

    private void watch() {
        while (true) {
            WindowsNetworks.waitForChange(15_000);
            String now = WindowsNetworks.snapshot();
            List<IConnectivity.IListener> listeners;
            synchronized (this) {
                if (now.equals(mSnapshot)) continue;
                mSnapshot = now;
                listeners = new ArrayList<>(mListeners);
            }
            if (Boolean.getBoolean("gesher.net.trace")) {
                SystemServer.log("connectivity: Windows' networks changed:\n" + now);
            }
            for (IConnectivity.IListener l : listeners) {
                try {
                    l.onSnapshot(now);
                } catch (RemoteException | RuntimeException e) {
                    removeListener(l);
                }
            }
        }
    }
}
