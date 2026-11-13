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
import android.os.SystemClock;

import java.util.ArrayList;
import java.util.HashMap;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.concurrent.Executors;
import java.util.concurrent.ScheduledExecutorService;
import java.util.concurrent.ScheduledFuture;
import java.util.concurrent.TimeUnit;
import java.util.function.BooleanSupplier;

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
 * <p>Unexpected process death restarts sticky/redelivery services and live auto-create
 * bindings. An intentional exit/force-stop cancels recovery ({@link #processDied}).
 */
final class ActiveServices {
    private final ActivityTaskManagerService mAtm;
    private final PackageManagerService mPm;
    private final NotificationManagerService mNotifications;
    private final CallerIdentity mCallers;
    private Runnable mBeforeSenderAuthentication; // Server-local direct-caller race fixture.

    private static final class Sender {
        final String packageName;
        final boolean system;
        final BooleanSupplier lifetime;
        final BooleanSupplier acceptance;
        Sender(String packageName, boolean system, BooleanSupplier lifetime) {
            this(packageName, system, lifetime, lifetime);
        }
        Sender(String packageName, boolean system, BooleanSupplier lifetime, BooleanSupplier acceptance) {
            this.packageName = packageName; this.system = system; this.lifetime = lifetime;
            this.acceptance = acceptance;
        }
    }

    private static final class PendingStart {
        final ServiceStartArgs args;
        final BooleanSupplier lifetime;
        PendingStart(ServiceStartArgs args, BooleanSupplier lifetime) {
            this.args = args; this.lifetime = lifetime;
        }
    }

    /** A service: ServiceRecord. */
    private static final class ServiceRecord extends Binder {
        final ServiceInfo info;
        final ComponentName name;
        final ApkPackage owner;
        ActivityTaskManagerService.ProcessRecord app;
        boolean created;
        boolean startRequested;
        int lastStartId;
        int foregroundId;
        String foregroundChannelId;
        int startResult = Service.START_STICKY_COMPATIBILITY;
        Intent lastStartIntent;
        BooleanSupplier startLifetime = () -> true;
        long createdAt;
        int restartAttempts;
        ScheduledFuture<?> restart;
        final List<PendingStart> pendingStarts = new ArrayList<>();
        final Map<Intent.FilterComparison, IntentBindRecord> bindings = new LinkedHashMap<>();

        ServiceRecord(ServiceInfo info, ApkPackage owner) {
            super("ServiceRecord " + info.name);
            this.info = info;
            this.name = new ComponentName(info.packageName, info.name);
            this.owner = owner;
        }

        boolean hasAutoCreateConnections() {
            for (IntentBindRecord b : bindings.values()) {
                for (ConnectionRecord c : b.connections) {
                    if ((c.flags & Context.BIND_AUTO_CREATE) != 0 && c.lifetime.getAsBoolean()) return true;
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
        final IBinder caller;
        final int callerPid;
        final BooleanSupplier lifetime;

        ConnectionRecord(IntentBindRecord binding, IServiceConnection conn, long flags,
                IApplicationThread caller, BooleanSupplier lifetime) {
            this.binding = binding;
            this.conn = conn;
            this.flags = flags;
            this.caller = caller == null ? null : caller.asBinder();
            this.callerPid = Binder.getCallingPid();
            this.lifetime = lifetime;
        }
    }

    // Guarded by this.
    private final Map<ComponentName, ServiceRecord> mServices = new HashMap<>();
    private final Map<IBinder, List<ConnectionRecord>> mConnections = new HashMap<>();
    private long mBindSeq;
    private final ScheduledExecutorService mRestarts = Executors.newSingleThreadScheduledExecutor(r -> {
        Thread thread = new Thread(r, "service-restarts");
        thread.setDaemon(true);
        return thread;
    });

    ActiveServices(ActivityTaskManagerService atm, PackageManagerService pm,
            NotificationManagerService notifications) {
        mAtm = atm;
        mPm = pm;
        mNotifications = notifications;
        mCallers = new CallerIdentity(pm, atm);
    }

    private Sender sender(IApplicationThread thread, String claimed, boolean requireThread) {
        return sender(thread, claimed, requireThread, () -> true);
    }

    private Sender sender(IApplicationThread thread, String claimed, boolean requireThread,
            BooleanSupplier delegatedLifetime) {
        final int pid = Binder.getCallingPid(), uid = Binder.getCallingUid();
        final ActivityTaskManagerService.ProcessRecord app;
        final String originalName;
        final ApkPackage originalCode;
        synchronized (mAtm) {
            app = mAtm.callerProcess(pid);
            originalName = app == null ? claimed : app.packageName;
            originalCode = mPm.get(originalName);
        }
        Runnable beforeAuthentication = mBeforeSenderAuthentication;
        if (beforeAuthentication != null) beforeAuthentication.run();
        if (mCallers.isSystem()) {
            ApkPackage owner = claimed == null ? null : mPm.get(claimed);
            if (owner != null) return new Sender(owner.packageName, false,
                    () -> delegatedLifetime.getAsBoolean() && mPm.get(owner.packageName) == owner
                            && !mPm.isRemovalPending(owner.packageName));
            if (claimed != null && !"android".equals(claimed)) {
                throw new SecurityException("Unknown service caller package");
            }
            return new Sender("android", true, () -> delegatedLifetime.getAsBoolean()
                    && !mPm.isRemovalPending("android"));
        }
        String actual = mCallers.callerPackage();
        if (claimed != null && !actual.equals(claimed)) {
            throw new SecurityException("Caller cannot act for another service package");
        }
        if (app == null || !actual.equals(originalName) || app.uid != uid
                || originalCode == null || originalCode.uid() != uid || (requireThread && thread == null)
                || (thread != null && !thread.asBinder().equals(app.thread.asBinder()))) {
            throw new SecurityException("Service call requires the caller's application thread");
        }
        BooleanSupplier lifetime = () -> delegatedLifetime.getAsBoolean()
                && mPm.get(actual) == originalCode && !mPm.isRemovalPending(actual);
        // The original process must authorize publication. Accepted service work
        // belongs to the package incarnation and survives an ordinary caller exit.
        BooleanSupplier acceptance = () -> lifetime.getAsBoolean()
                && mAtm.callerProcess(pid) == app && !app.exiting;
        if (!acceptance.getAsBoolean()) throw new SecurityException("Service caller retired during authentication");
        return new Sender(actual, false, lifetime, acceptance);
    }

    private void enforceAccess(ServiceInfo info, Sender sender) {
        if (sender.system || info.packageName.equals(sender.packageName)) return;
        if (!info.exported) throw new SecurityException("Service is not exported");
        if (info.permission != null && !info.permission.isEmpty()
                && mPm.permissions().checkPermission(sender.packageName, info.permission)
                        != PackageManager.PERMISSION_GRANTED) {
            throw new SecurityException("Service requires " + info.permission);
        }
    }

    private void requireServiceOwner(ServiceRecord service) {
        if (mCallers.isSystem()) return;
        String actual = mCallers.callerPackage();
        if (!service.info.packageName.equals(actual) || service.app == null
                || service.app.pid != Binder.getCallingPid()) {
            throw new SecurityException("Service token belongs to another application");
        }
    }

    private ServiceInfo resolve(Intent service, String resolvedType) {
        List<ResolveInfo> all = mPm.services(service, resolvedType,
                PackageManager.GET_META_DATA | PackageManager.GET_SHARED_LIBRARY_FILES);
        ServiceInfo info = all.isEmpty() ? null : all.get(0).serviceInfo;
        // Play services' stand-in package (GmsPackageSupport) has no process to run its
        // services in: one started for it ends at once ("No data directory found"), and a
        // client's binding waits for its timeout (WhatsApp's SmsRetriever: 3 minutes before
        // the code request). Not bound, the client library fails at once.
        if (info != null && GmsPackageSupport.isSyntheticPackage(info.packageName)
                && mPm.get(info.packageName) == null) {
            return null;
        }
        return info;
    }

    private ServiceRecord record(ServiceInfo info, ApkPackage owner) {
        ComponentName name = new ComponentName(info.packageName, info.name);
        ServiceRecord r = mServices.get(name);
        if (r == null || r.owner != owner) {
            r = new ServiceRecord(info, owner);
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
        return startService(caller, service, resolvedType, requireForeground, callingPackage, () -> true);
    }

    ComponentName startService(IApplicationThread caller, Intent service, String resolvedType,
            boolean requireForeground, String callingPackage, BooleanSupplier lifetime) {
        if (!lifetime.getAsBoolean()) return null;
        Sender sender = sender(caller, callingPackage, true, lifetime);
        ServiceInfo info;
        ApkPackage target;
        synchronized (mPm) {
            info = resolve(service, resolvedType);
            target = info == null ? null : mPm.get(info.packageName);
        }
        if (info == null) {
            SystemServer.log("startService: nothing handles " + service);
            return null;
        }
        enforceAccess(info, sender);
        ServiceRecord r;
        synchronized (this) {
            if (!sender.acceptance.getAsBoolean() || target == null
                    || mPm.get(info.packageName) != target || mPm.isRemovalPending(info.packageName)) return null;
            r = record(info, target);
            r.startRequested = true;
            r.lastStartIntent = new Intent(service);
            r.startLifetime = sender.lifetime;
            int startId = ++r.lastStartId;
            r.pendingStarts.add(new PendingStart(new ServiceStartArgs(false /* taskRemoved */, startId,
                    0 /* flags */, new Intent(service)), sender.lifetime));
        }
        SystemServer.log("startService " + r + " from " + callingPackage
                + (requireForeground ? " (foreground)" : ""));
        long identity = Binder.clearCallingIdentity();
        try { bringUp(r); } finally { Binder.restoreCallingIdentity(identity); }
        return r.name;
    }

    int stopService(IApplicationThread caller, Intent service, String resolvedType) {
        Sender sender = sender(caller, null, true);
        ServiceInfo info = resolve(service, resolvedType);
        if (info == null) return 0;
        enforceAccess(info, sender);
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
            requireServiceOwner(r);
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
        return bindService(caller, service, resolvedType, connection, flags, callingPackage, () -> true);
    }

    int bindService(IApplicationThread caller, Intent service, String resolvedType,
            IServiceConnection connection, long flags, String callingPackage, BooleanSupplier lifetime) {
        if (!lifetime.getAsBoolean()) return 0;
        Sender sender = sender(caller, callingPackage, true, lifetime);
        if (connection == null) throw new IllegalArgumentException("Missing service connection");
        ServiceInfo info;
        ApkPackage target;
        synchronized (mPm) {
            info = resolve(service, resolvedType);
            target = info == null ? null : mPm.get(info.packageName);
        }
        if (info == null) {
            SystemServer.log("bindService: nothing handles " + service);
            return 0;
        }
        enforceAccess(info, sender);
        ServiceRecord r;
        IntentBindRecord b;
        IBinder published = null;
        ConnectionRecord connectionRecord;
        synchronized (this) {
            if (!sender.acceptance.getAsBoolean() || target == null
                    || mPm.get(info.packageName) != target || mPm.isRemovalPending(info.packageName)) return 0;
            r = record(info, target);
            Intent.FilterComparison key = new Intent.FilterComparison(new Intent(service));
            b = r.bindings.get(key);
            if (b == null) {
                b = new IntentBindRecord(r, key.getIntent());
                r.bindings.put(key, b);
            }
            ConnectionRecord c = new ConnectionRecord(b, connection, flags, caller, sender.lifetime);
            connectionRecord = c;
            b.connections.add(c);
            mConnections.computeIfAbsent(connection.asBinder(), k -> new ArrayList<>()).add(c);
            if (b.received) published = b.binder;
            // A new client: the idle reaper must not take this as an unused process.
            if (r.app != null) r.app.markActive(SystemClock.uptimeMillis());
        }
        SystemServer.log("bindService " + r + " from " + callingPackage + " (flags 0x"
                + Long.toHexString(flags) + ")");
        long identity = Binder.clearCallingIdentity();
        try {
            if (published != null) {
                connected(connectionRecord, published);
            } else if ((flags & Context.BIND_AUTO_CREATE) != 0 || r.created) {
                bringUp(r);
            }
        } finally { Binder.restoreCallingIdentity(identity); }
        return 1;
    }

    void publishService(IBinder token, IBinder bindToken, IBinder service) {
        ServiceRecord r;
        List<ConnectionRecord> clients = new ArrayList<>();
        synchronized (this) {
            r = byToken(token);
            if (r == null || !(bindToken instanceof IntentBindRecord)) return;
            requireServiceOwner(r);
            IntentBindRecord b = (IntentBindRecord) bindToken;
            if (b.service != r) return;
            b.binder = service;
            b.received = true;
            for (ConnectionRecord c : b.connections) clients.add(c);
        }
        SystemServer.log("publishService " + r + ": "
                + (service != null ? service.getClass().getSimpleName() : "null") + " to "
                + clients.size() + " connection(s)");
        for (ConnectionRecord client : clients) connected(client, service);
    }

    private void connected(ConnectionRecord connection, IBinder service) {
        ServiceRecord r = connection.binding.service;
        if (!connection.lifetime.getAsBoolean() || service != null && !serviceCurrent(r)) return;
        try {
            connection.conn.connected(r.name, service, null /* session */, false /* dead */);
        } catch (RemoteException e) {
            SystemServer.log("connected " + r + ": " + e);
        }
    }

    boolean unbindService(IServiceConnection connection) {
        List<ConnectionRecord> removed;
        List<IntentBindRecord> unbind = new ArrayList<>();
        List<ServiceRecord> services = new ArrayList<>();
        synchronized (this) {
            List<ConnectionRecord> existing = mConnections.get(connection.asBinder());
            if (existing != null && !mCallers.isSystem()) {
                mCallers.callerPackage();
                for (ConnectionRecord record : existing) {
                    if (record.callerPid != Binder.getCallingPid()) {
                        throw new SecurityException("Cannot unbind another application's connection");
                    }
                }
            }
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
            if (r != null) requireServiceOwner(r);
            if (r != null && type == 1 && startId == r.lastStartId) {
                r.startResult = res;
            }
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
        mNotifications.withChannelLifecycle(() -> setServiceForegroundLocked(className, token,
                id, notification, flags, foregroundServiceType));
    }

    private void setServiceForegroundLocked(ComponentName className, IBinder token, int id,
            Notification notification, int flags, int foregroundServiceType) {
        ServiceRecord r;
        int previous;
        ApkPackage captured;
        synchronized (this) {
            r = byToken(token);
            if (r == null) return;
            captured = mPm.get(r.info.packageName);
            requireServiceOwner(r);
            if (id != 0 && notification == null) throw new IllegalArgumentException("Missing foreground notification");
            previous = r.foregroundId;
            r.foregroundId = id;
            r.foregroundChannelId = id == 0 ? null : notification.getChannelId();
        }
        if (id != 0) {
            // FOREGROUND_SERVICE_TYPE_MANIFEST (startForeground(id, notification)): the
            // manifest's types.
            int type = foregroundServiceType == ServiceInfo.FOREGROUND_SERVICE_TYPE_MANIFEST
                    ? r.info.getForegroundServiceType() : foregroundServiceType;
            SystemServer.log("setServiceForeground " + r + ": foreground, notification " + id
                    + " (service type 0x" + Integer.toHexString(type) + ")");
            mNotifications.postFor(captured, r.info.packageName, null, id, notification);
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
            if (r != null) requireServiceOwner(r);
            return r != null && r.foregroundId != 0 ? r.info.getForegroundServiceType() : 0;
        }
    }

    synchronized boolean hasForegroundServiceNotification(String pkg, String channelId) {
        for (ServiceRecord record : mServices.values()) {
            if (record.foregroundId != 0 && record.info.packageName.equals(pkg)
                    && java.util.Objects.equals(record.foregroundChannelId, channelId)
                    && serviceCurrent(record)) return true;
        }
        return false;
    }

    IBinder peekService(Intent service, String resolvedType) {
        Sender sender = sender(null, null, false);
        ServiceInfo info = resolve(service, resolvedType);
        if (info == null) return null;
        enforceAccess(info, sender);
        synchronized (this) {
            ServiceRecord r = mServices.get(new ComponentName(info.packageName, info.name));
            if (r == null) return null;
            IntentBindRecord b = r.bindings.get(new Intent.FilterComparison(service));
            return b != null ? b.binder : null;
        }
    }

    // --- bringing services up and down -------------------------------------------------

    private boolean serviceCurrent(ServiceRecord r) {
        return r.owner != null && mPm.get(r.info.packageName) == r.owner
                && !mPm.isRemovalPending(r.info.packageName);
    }

    /** Guarded by this. Retire queued authority without entering any application. */
    private void pruneRetired(ServiceRecord r) {
        r.pendingStarts.removeIf(start -> !start.lifetime.getAsBoolean());
        if (!r.startLifetime.getAsBoolean()) {
            r.startRequested = false;
            r.lastStartIntent = null;
        }
        for (IntentBindRecord binding : r.bindings.values()) {
            binding.connections.removeIf(connection -> {
                if (connection.lifetime.getAsBoolean()) return false;
                List<ConnectionRecord> owned = mConnections.get(connection.conn.asBinder());
                if (owned != null) {
                    owned.remove(connection);
                    if (owned.isEmpty()) mConnections.remove(connection.conn.asBinder());
                }
                return true;
            });
        }
    }

    private synchronized boolean workCurrent(ServiceRecord r) {
        pruneRetired(r);
        if (mServices.get(r.name) != r || !serviceCurrent(r)) return false;
        if (r.startRequested) return true;
        for (IntentBindRecord binding : r.bindings.values()) {
            if (!binding.connections.isEmpty()) return true;
        }
        return false;
    }

    /** ActiveServices.bringUpServiceLocked: create, then the starts and bind requests. */
    private void bringUp(ServiceRecord r) {
        if (!workCurrent(r)) { maybeBringDown(r); return; }
        mAtm.withProcess(r.info.packageName, r.info.processName, "service " + r, app -> {
            if (!workCurrent(r)) { maybeBringDown(r); return; }
            boolean create;
            List<PendingStart> starts;
            List<IntentBindRecord> binds = new ArrayList<>();
            synchronized (this) {
                if (!workCurrent(r)) return;
                if (mServices.get(r.name) != r) return;  // stopped meanwhile
                if (r.app != null && r.app != app) return;
                r.app = app;
                create = !r.created;
                r.created = true;
                if (create) r.createdAt = SystemClock.uptimeMillis();
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
                    if (!workCurrent(r)) { maybeBringDown(r); return; }
                    SystemServer.log("scheduleCreateService " + r + " in " + app);
                    app.thread.scheduleCreateService(r, r.info,
                            CompatibilityInfo.DEFAULT_COMPATIBILITY_INFO,
                            ActivityManager.PROCESS_STATE_SERVICE);
                }
                // Recheck after scheduleCreateService: a local callback can remove
                // and reinstall the creator before its queued start is delivered.
                List<ServiceStartArgs> currentStarts = new ArrayList<>();
                for (PendingStart start : starts) {
                    if (start.lifetime.getAsBoolean() && serviceCurrent(r)) currentStarts.add(start.args);
                }
                if (!currentStarts.isEmpty() && workCurrent(r)) {
                    SystemServer.log("scheduleServiceArgs " + r + ": " + currentStarts.size()
                            + " start(s)");
                    app.thread.scheduleServiceArgs(r, new ParceledListSlice<>(currentStarts));
                }
                for (IntentBindRecord b : binds) {
                    long seq;
                    synchronized (this) {
                        pruneRetired(r);
                        if (!serviceCurrent(r) || b.connections.isEmpty()) { b.requested = false; continue; }
                        seq = ++mBindSeq;
                    }
                    if (!workCurrent(r)) return;
                    SystemServer.log("scheduleBindService " + r);
                    app.thread.scheduleBindService(r, b, b.intent, false /* rebind */,
                            ActivityManager.PROCESS_STATE_SERVICE, seq);
                }
                if (!workCurrent(r)) maybeBringDown(r);
            } catch (RemoteException e) {
                SystemServer.log("bringing up " + r + " failed: " + e);
            }
        });
    }

    /** ActiveServices.bringDownServiceIfNeededLocked: destroy it if nothing keeps it. */
    private void maybeBringDown(ServiceRecord r) {
        synchronized (this) {
            pruneRetired(r);
            if (r.startRequested || r.hasAutoCreateConnections()) return;
            if (mServices.get(r.name) != r) return;
            mServices.remove(r.name);
            if (r.restart != null) { r.restart.cancel(false); r.restart = null; }
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
        // A process kept by the service alone ends now (scheduleExit follows the stop).
        mAtm.maybeExit(r.app);
    }

    /** Whether a service runs (or is being brought up) in the process. */
    synchronized boolean hasServices(ActivityTaskManagerService.ProcessRecord app) {
        for (ServiceRecord r : mServices.values()) {
            if (r.app == app) return true;
        }
        return false;
    }

    /**
     * Whether a service of the process is in use, as OomAdjuster ranks a service process
     * above a cached one: a service still being brought up or with a restart pending, a
     * started (or foreground) service, or one bound by another process or by the system
     * (JobScheduler, sync). A binding from the process itself does not count ("binding to
     * oneself is not interesting"): such a process is cached and may be reaped.
     */
    synchronized boolean inUse(ActivityTaskManagerService.ProcessRecord app) {
        IBinder self = app.thread != null ? app.thread.asBinder() : null;
        for (ServiceRecord r : mServices.values()) {
            if (r.app != app) continue;
            if (!r.created || r.restart != null || r.startRequested || !r.pendingStarts.isEmpty()
                    || r.foregroundId != 0) return true;
            for (IntentBindRecord b : r.bindings.values()) {
                for (ConnectionRecord c : b.connections) {
                    if (c.caller == null || !c.caller.equals(self)) return true;
                }
            }
        }
        return false;
    }

    /** Explicit force-stop/uninstall also cancels a service waiting for its retry timer. */
    void stopPackage(String packageName) {
        List<ServiceRecord> stopped = new ArrayList<>();
        List<ConnectionRecord> disconnected = new ArrayList<>();
        synchronized (this) {
            for (ServiceRecord r : mServices.values()) {
                if (!packageName.equals(r.info.packageName)) continue;
                r.startRequested = false;
                r.pendingStarts.clear();
                if (r.restart != null) { r.restart.cancel(false); r.restart = null; }
                for (IntentBindRecord b : r.bindings.values()) {
                    for (ConnectionRecord c : b.connections) {
                        disconnected.add(c);
                        List<ConnectionRecord> list = mConnections.get(c.conn.asBinder());
                        if (list != null) list.remove(c);
                    }
                    b.connections.clear();
                }
                stopped.add(r);
            }
        }
        for (ConnectionRecord c : disconnected) connected(c, null);
        for (ServiceRecord r : stopped) maybeBringDown(r);
    }

    /** The process went away: release dead bindings and recover Android sticky services. */
    void processDied(ActivityTaskManagerService.ProcessRecord app) {
        List<ServiceRecord> recovered = new ArrayList<>();
        List<ServiceRecord> reconsider = new ArrayList<>();
        List<ConnectionRecord> disconnected = new ArrayList<>();
        synchronized (this) {
            // A dead client's BIND_AUTO_CREATE must not keep somebody else's process
            // alive forever. System-owned bindings have no app caller and survive.
            mConnections.entrySet().removeIf(entry -> {
                entry.getValue().removeIf(c -> {
                    if (c.caller == null || !c.caller.equals(app.thread.asBinder())) return false;
                    c.binding.connections.remove(c);
                    if (!reconsider.contains(c.binding.service)) reconsider.add(c.binding.service);
                    return true;
                });
                return entry.getValue().isEmpty();
            });
            for (ServiceRecord r : new ArrayList<>(mServices.values())) {
                if (r.app != app) continue;
                pruneRetired(r);
                if (r.foregroundId != 0) {
                    mNotifications.cancel(r.info.packageName, null, r.foregroundId);
                    r.foregroundId = 0;
                    r.foregroundChannelId = null;
                }
                r.app = null;
                r.created = false;
                for (IntentBindRecord b : r.bindings.values()) {
                    disconnected.addAll(b.connections);
                    b.requested = false;
                    b.received = false;
                    b.binder = null;
                }
                boolean started = r.startRequested && (r.startResult != Service.START_NOT_STICKY
                        || !r.pendingStarts.isEmpty());
                if (!started) r.startRequested = false;
                // An intentional exit ends the service for good. But the idle reaper ends
                // only a process nothing used (no started service, no binding from another
                // process): a start or a binding (to its published Binder) that came in the
                // moment it was told to exit is recovered, by a new process.
                boolean recover = (!app.exiting || app.reaped)
                        && (started || r.hasAutoCreateConnections());
                if (serviceCurrent(r) && recover) {
                    if (r.startRequested && r.pendingStarts.isEmpty() && started) {
                        Intent redelivery = r.startResult == Service.START_REDELIVER_INTENT
                                && r.lastStartIntent != null ? new Intent(r.lastStartIntent) : null;
                        int flags = redelivery != null ? Service.START_FLAG_REDELIVERY : 0;
                        r.pendingStarts.add(new PendingStart(new ServiceStartArgs(false,
                                ++r.lastStartId, flags, redelivery), r.startLifetime));
                    }
                    if (SystemClock.uptimeMillis() - r.createdAt > 60_000) r.restartAttempts = 0;
                    recovered.add(r);
                } else {
                    r.startRequested = false;
                    mServices.remove(r.name);
                    for (IntentBindRecord b : r.bindings.values()) {
                        for (ConnectionRecord c : b.connections) {
                            List<ConnectionRecord> list = mConnections.get(c.conn.asBinder());
                            if (list != null) list.remove(c);
                        }
                    }
                }
            }
        }
        for (ConnectionRecord c : disconnected) connected(c, null);
        for (ServiceRecord r : reconsider) maybeBringDown(r);
        for (ServiceRecord r : recovered) scheduleRestart(r);
    }

    private void scheduleRestart(ServiceRecord r) {
        synchronized (this) {
            if (mServices.get(r.name) != r || r.restart != null) return;
            long delay = Math.min(60_000, 1000L << Math.min(r.restartAttempts++, 6));
            SystemServer.log("restarting service " + r + " after " + delay + " ms");
            r.restart = mRestarts.schedule(() -> {
                synchronized (ActiveServices.this) {
                    r.restart = null;
                    pruneRetired(r);
                    if (mServices.get(r.name) != r
                            || !serviceCurrent(r) || (!r.startRequested && !r.hasAutoCreateConnections())) return;
                }
                try { bringUp(r); }
                catch (RuntimeException e) {
                    SystemServer.log("restarting service " + r + " failed: " + e);
                    scheduleRestart(r);
                }
            }, delay, TimeUnit.MILLISECONDS);
        }
    }
}
