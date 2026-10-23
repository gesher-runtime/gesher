/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.server;

import android.app.ActivityManager;
import android.app.IApplicationThread;
import android.app.IServiceConnection;
import android.app.Notification;
import android.app.Service;
import android.app.ServiceStartArgs;
import android.content.ComponentName;
import android.content.Context;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.content.pm.ParceledListSlice;
import android.content.pm.ResolveInfo;
import android.content.pm.ServiceInfo;
import android.content.res.CompatibilityInfo;
import android.os.Binder;
import android.os.IBinder;
import android.os.RemoteException;

import java.util.ArrayList;
import java.util.HashMap;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;

/**
 * ActiveServices, lite: started and bound services, in the app processes
 * {@link ActivityTaskManagerService} keeps.
 *
 * <ul>
 * <li>{@code startService}: the service is created in its package's process
 *     ({@code scheduleCreateService} → onCreate) if it is not, then gets the start
 *     ({@code scheduleServiceArgs} → onStartCommand, start ids from 1).
 * <li>{@code stopService}, {@code stopSelf} ({@code stopServiceToken}, for the last start
 *     id or any): no longer started; destroyed ({@code scheduleStopService} → onDestroy)
 *     unless a connection with BIND_AUTO_CREATE keeps it.
 * <li>{@code bindService}: a binding per distinct intent (Intent.FilterComparison); the
 *     first connection asks the service for its Binder ({@code scheduleBindService} →
 *     onBind → {@code publishService}), which every connection gets
 *     ({@code IServiceConnection.connected}); the Binder crosses binder as any object, so
 *     in the service's own process the client gets the very object onBind returned.
 * <li>{@code unbindService}: once a binding has no connection left,
 *     {@code scheduleUnbindService} → onUnbind; then the service is destroyed if nothing
 *     else keeps it.
 * <li>{@code startForeground}: {@code setServiceForeground} posts the notification to the
 *     stand-in notification manager ({@link NotificationManagerService}); nothing enforces
 *     the foreground service rules (types, the 10 s to call startForeground).
 * </ul>
 *
 * <p>A service whose process is gone goes with it ({@link #processDied}).
 */
final class ActiveServices {
    private final ActivityTaskManagerService mAtm;
    private final PackageManagerService mPm;
    private final NotificationManagerService mNotifications;

    /** A service: ServiceRecord. */
    private static final class ServiceRecord extends Binder {
        final ServiceInfo info;
        final ComponentName name;
        ActivityTaskManagerService.ProcessRecord app;
        boolean created;
        boolean startRequested;
        int lastStartId;
        int foregroundId;
        final List<ServiceStartArgs> pendingStarts = new ArrayList<>();
        final Map<Intent.FilterComparison, IntentBindRecord> bindings = new LinkedHashMap<>();

        ServiceRecord(ServiceInfo info) {
            super("ServiceRecord " + info.name);
            this.info = info;
            this.name = new ComponentName(info.packageName, info.name);
        }

        boolean hasAutoCreateConnections() {
            for (IntentBindRecord b : bindings.values()) {
                for (ConnectionRecord c : b.connections) {
                    if ((c.flags & Context.BIND_AUTO_CREATE) != 0) return true;
                }
            }
            return false;
        }

        @Override
        public String toString() {
            return name.flattenToShortString();
        }
    }

    /** A binding of a service with one intent: IntentBindRecord. */
    private static final class IntentBindRecord extends Binder {
        final ServiceRecord service;
        final Intent intent;
        final List<ConnectionRecord> connections = new ArrayList<>();
        boolean requested;   // scheduleBindService sent
        boolean received;    // publishService came
        IBinder binder;

        IntentBindRecord(ServiceRecord service, Intent intent) {
            super("IntentBindRecord " + service.info.name);
            this.service = service;
            this.intent = intent;
        }
    }

    /** A client's connection: ConnectionRecord. */
    private static final class ConnectionRecord {
        final IntentBindRecord binding;
        final IServiceConnection conn;
        final long flags;

        ConnectionRecord(IntentBindRecord binding, IServiceConnection conn, long flags) {
            this.binding = binding;
            this.conn = conn;
            this.flags = flags;
        }
    }

    // Guarded by this.
    private final Map<ComponentName, ServiceRecord> mServices = new HashMap<>();
    private final Map<IBinder, List<ConnectionRecord>> mConnections = new HashMap<>();
    private long mBindSeq;

    ActiveServices(ActivityTaskManagerService atm, PackageManagerService pm,
            NotificationManagerService notifications) {
        mAtm = atm;
        mPm = pm;
        mNotifications = notifications;
    }

    private ServiceInfo resolve(Intent service, String resolvedType) {
        List<ResolveInfo> all = mPm.services(service, resolvedType,
                PackageManager.GET_META_DATA | PackageManager.GET_SHARED_LIBRARY_FILES);
        return all.isEmpty() ? null : all.get(0).serviceInfo;
    }

    private ServiceRecord record(ServiceInfo info) {
        ComponentName name = new ComponentName(info.packageName, info.name);
        ServiceRecord r = mServices.get(name);
        if (r == null) {
            r = new ServiceRecord(info);
            mServices.put(name, r);
        }
        return r;
    }

    private ServiceRecord byToken(IBinder token) {
        return token instanceof ServiceRecord && mServices.containsValue(token)
                ? (ServiceRecord) token : null;
    }

    // --- IActivityManager ------------------------------------------------------------

    ComponentName startService(IApplicationThread caller, Intent service, String resolvedType,
            boolean requireForeground, String callingPackage) {
        ServiceInfo info = resolve(service, resolvedType);
        if (info == null) {
            SystemServer.log("startService: nothing handles " + service);
            return null;
        }
        ServiceRecord r;
        synchronized (this) {
            r = record(info);
            r.startRequested = true;
            int startId = ++r.lastStartId;
            r.pendingStarts.add(new ServiceStartArgs(false /* taskRemoved */, startId,
                    0 /* flags */, new Intent(service)));
        }
        SystemServer.log("startService " + r + " from " + callingPackage
                + (requireForeground ? " (foreground)" : ""));
        bringUp(r);
        return r.name;
    }

    int stopService(IApplicationThread caller, Intent service, String resolvedType) {
        ServiceInfo info = resolve(service, resolvedType);
        if (info == null) return 0;
        ServiceRecord r;
        synchronized (this) {
            r = mServices.get(new ComponentName(info.packageName, info.name));
            if (r == null || !r.startRequested) return 0;
            r.startRequested = false;
            r.pendingStarts.clear();
        }
        SystemServer.log("stopService " + r);
        maybeBringDown(r);
        return 1;
    }

    boolean stopServiceToken(ComponentName className, IBinder token, int startId) {
        ServiceRecord r;
        synchronized (this) {
            r = byToken(token);
            if (r == null) return false;
            // stopSelf(startId) stops only if no later start came.
            if (startId >= 0 && startId != r.lastStartId) return false;
            r.startRequested = false;
            r.pendingStarts.clear();
        }
        SystemServer.log("stopSelf " + r + (startId >= 0 ? " (start id " + startId + ")" : ""));
        maybeBringDown(r);
        return true;
    }

    int bindService(IApplicationThread caller, Intent service, String resolvedType,
            IServiceConnection connection, long flags, String callingPackage) {
        ServiceInfo info = resolve(service, resolvedType);
        if (info == null) {
            SystemServer.log("bindService: nothing handles " + service);
            return 0;
        }
        ServiceRecord r;
        IntentBindRecord b;
        IBinder published = null;
        synchronized (this) {
            r = record(info);
            Intent.FilterComparison key = new Intent.FilterComparison(new Intent(service));
            b = r.bindings.get(key);
            if (b == null) {
                b = new IntentBindRecord(r, key.getIntent());
                r.bindings.put(key, b);
            }
            ConnectionRecord c = new ConnectionRecord(b, connection, flags);
            b.connections.add(c);
            mConnections.computeIfAbsent(connection.asBinder(), k -> new ArrayList<>()).add(c);
            if (b.received) published = b.binder;
        }
        SystemServer.log("bindService " + r + " from " + callingPackage + " (flags 0x"
                + Long.toHexString(flags) + ")");
        if (published != null) {
            connected(connection, r, published);
        } else if ((flags & Context.BIND_AUTO_CREATE) != 0 || r.created) {
            bringUp(r);
        }
        return 1;
    }

    void publishService(IBinder token, IBinder bindToken, IBinder service) {
        ServiceRecord r;
        List<IServiceConnection> clients = new ArrayList<>();
        synchronized (this) {
            r = byToken(token);
            if (r == null || !(bindToken instanceof IntentBindRecord)) return;
            IntentBindRecord b = (IntentBindRecord) bindToken;
            if (b.service != r) return;
            b.binder = service;
            b.received = true;
            for (ConnectionRecord c : b.connections) clients.add(c.conn);
        }
        SystemServer.log("publishService " + r + ": "
                + (service != null ? service.getClass().getSimpleName() : "null") + " to "
                + clients.size() + " connection(s)");
        for (IServiceConnection conn : clients) connected(conn, r, service);
    }

    private void connected(IServiceConnection conn, ServiceRecord r, IBinder service) {
        try {
            conn.connected(r.name, service, null /* session */, false /* dead */);
        } catch (RemoteException e) {
            SystemServer.log("connected " + r + ": " + e);
        }
    }

    boolean unbindService(IServiceConnection connection) {
        List<ConnectionRecord> removed;
        List<IntentBindRecord> unbind = new ArrayList<>();
        List<ServiceRecord> services = new ArrayList<>();
        synchronized (this) {
            removed = mConnections.remove(connection.asBinder());
            if (removed == null) return false;
            for (ConnectionRecord c : removed) {
                IntentBindRecord b = c.binding;
                b.connections.remove(c);
                if (b.connections.isEmpty() && b.requested) {
                    b.requested = false;
                    b.received = false;
                    b.binder = null;
                    unbind.add(b);
                }
                if (!services.contains(b.service)) services.add(b.service);
            }
        }
        for (ServiceRecord r : services) SystemServer.log("unbindService " + r);
        for (IntentBindRecord b : unbind) {
            ServiceRecord r = b.service;
            if (r.app == null || !r.created) continue;
            SystemServer.log("scheduleUnbindService " + r);
            try {
                r.app.thread.scheduleUnbindService(r, b, b.intent);
            } catch (RemoteException e) {
                SystemServer.log("scheduleUnbindService " + r + ": " + e);
            }
        }
        for (ServiceRecord r : services) maybeBringDown(r);
        return true;
    }

    void unbindFinished(IBinder token, IBinder bindToken) {
        // onUnbind returned true: onRebind on the next bind would follow. Bindings start
        // over here instead (onBind again).
    }

    void serviceDoneExecuting(IBinder token, int type, int startId, int res) {
        ServiceRecord r;
        synchronized (this) {
            r = token instanceof ServiceRecord ? (ServiceRecord) token : null;
        }
        if (r == null) return;
        String what;
        switch (type) {
            case 0: what = "created"; break;          // SERVICE_DONE_EXECUTING_ANON
            case 1: what = "started (start id " + startId + ", result " + res + ")"; break;
            case 2: what = "destroyed"; break;        // SERVICE_DONE_EXECUTING_STOP
            case 3: what = "rebound"; break;
            case 4: what = "unbound"; break;
            default: what = "done (" + type + ")";
        }
        SystemServer.log("serviceDoneExecuting " + r + ": " + what);
    }

    void setServiceForeground(ComponentName className, IBinder token, int id,
            Notification notification, int flags, int foregroundServiceType) {
        ServiceRecord r;
        int previous;
        synchronized (this) {
            r = byToken(token);
            if (r == null) return;
            previous = r.foregroundId;
            r.foregroundId = id;
        }
        if (id != 0) {
            SystemServer.log("setServiceForeground " + r + ": foreground, notification " + id
                    + " (type 0x" + Integer.toHexString(foregroundServiceType) + ")");
            mNotifications.post(r.info.packageName, null, id, notification);
        } else {
            SystemServer.log("setServiceForeground " + r + ": background"
                    + ((flags & Service.STOP_FOREGROUND_REMOVE) != 0
                            ? ", notification removed" : ""));
            if (previous != 0 && (flags & Service.STOP_FOREGROUND_REMOVE) != 0) {
                mNotifications.cancel(r.info.packageName, null, previous);
            }
        }
    }

    int getForegroundServiceType(ComponentName className, IBinder token) {
        synchronized (this) {
            ServiceRecord r = byToken(token);
            return r != null && r.foregroundId != 0 ? r.info.getForegroundServiceType() : 0;
        }
    }

    IBinder peekService(Intent service, String resolvedType) {
        ServiceInfo info = resolve(service, resolvedType);
        if (info == null) return null;
        synchronized (this) {
            ServiceRecord r = mServices.get(new ComponentName(info.packageName, info.name));
            if (r == null) return null;
            IntentBindRecord b = r.bindings.get(new Intent.FilterComparison(service));
            return b != null ? b.binder : null;
        }
    }

    // --- bringing services up and down -------------------------------------------------

    /** ActiveServices.bringUpServiceLocked: create, then the starts and bind requests. */
    private void bringUp(ServiceRecord r) {
        mAtm.withProcess(r.info.packageName, app -> {
            boolean create;
            List<ServiceStartArgs> starts;
            List<IntentBindRecord> binds = new ArrayList<>();
            synchronized (this) {
                if (mServices.get(r.name) != r) return;  // stopped meanwhile
                if (r.app != null && r.app != app) return;
                r.app = app;
                create = !r.created;
                r.created = true;
                starts = new ArrayList<>(r.pendingStarts);
                r.pendingStarts.clear();
                for (IntentBindRecord b : r.bindings.values()) {
                    if (!b.requested && !b.connections.isEmpty()) {
                        b.requested = true;
                        binds.add(b);
                    }
                }
            }
            try {
                if (create) {
                    SystemServer.log("scheduleCreateService " + r + " in " + app);
                    app.thread.scheduleCreateService(r, r.info,
                            CompatibilityInfo.DEFAULT_COMPATIBILITY_INFO,
                            ActivityManager.PROCESS_STATE_SERVICE);
                }
                if (!starts.isEmpty()) {
                    SystemServer.log("scheduleServiceArgs " + r + ": " + starts.size()
                            + " start(s)");
                    app.thread.scheduleServiceArgs(r, new ParceledListSlice<>(starts));
                }
                for (IntentBindRecord b : binds) {
                    long seq;
                    synchronized (this) {
                        seq = ++mBindSeq;
                    }
                    SystemServer.log("scheduleBindService " + r);
                    app.thread.scheduleBindService(r, b, b.intent, false /* rebind */,
                            ActivityManager.PROCESS_STATE_SERVICE, seq);
                }
            } catch (RemoteException e) {
                SystemServer.log("bringing up " + r + " failed: " + e);
            }
        });
    }

    /** ActiveServices.bringDownServiceIfNeededLocked: destroy it if nothing keeps it. */
    private void maybeBringDown(ServiceRecord r) {
        synchronized (this) {
            if (r.startRequested || r.hasAutoCreateConnections()) return;
            if (mServices.get(r.name) != r) return;
            mServices.remove(r.name);
            for (IntentBindRecord b : r.bindings.values()) {
                for (ConnectionRecord c : b.connections) {
                    List<ConnectionRecord> l = mConnections.get(c.conn.asBinder());
                    if (l != null) l.remove(c);
                }
            }
        }
        if (r.foregroundId != 0) mNotifications.cancel(r.info.packageName, null, r.foregroundId);
        if (r.app == null || !r.created) return;
        SystemServer.log("scheduleStopService " + r);
        try {
            r.app.thread.scheduleStopService(r);
        } catch (RemoteException e) {
            SystemServer.log("scheduleStopService " + r + ": " + e);
        }
    }

    /** The process went away: so did its services. */
    void processDied(ActivityTaskManagerService.ProcessRecord app) {
        synchronized (this) {
            mServices.values().removeIf(r -> r.app == app);
        }
    }
}
