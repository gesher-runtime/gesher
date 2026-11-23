/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package com.gesher.server;

import android.Manifest;
import android.app.AppOpsManager;
import android.content.pm.PackageManager;
import android.os.Binder;
import android.os.IBinder;
import android.os.Parcel;
import android.os.RemoteException;
import com.gesher.sensors.NativeSensorAccess;
import java.util.HashMap;
import java.util.HashSet;
import java.util.NoSuchElementException;
import java.util.Map;
import java.util.Set;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;

/** Internal sensorservice policy transport. The native bridge owns no permission
 * authority: its grants come from the caller's attached process and installed APK. */
final class SensorAccessService extends Binder {
    private final PackageManagerService packages;
    private final ActivityTaskManagerService activities;
    private final SensorAccessPolicy policy = new SensorAccessPolicy();
    private final Map<ActivityTaskManagerService.ProcessRecord, Session> sessions = new HashMap<>();
    private final Set<ActivityTaskManagerService.ProcessRecord> opening = new HashSet<>();
    private final ExecutorService notifications = Executors.newSingleThreadExecutor(r -> {
        Thread thread = new Thread(r, "gesher-sensor-access");
        thread.setDaemon(true);
        return thread;
    });

    SensorAccessService(PackageManagerService packages, ActivityTaskManagerService activities) {
        super(NativeSensorAccess.DESCRIPTOR);
        this.packages = packages;
        this.activities = activities;
        packages.onPackageRemoval(this::retire);
        packages.onPackageChange(pkg -> retire(pkg.packageName));
    }

    void activityStatesChanged(Map<Integer, AppOpsService.UidState> states) {
        Map<Integer, Boolean> visible = new HashMap<>();
        for (var entry : states.entrySet()) {
            visible.put(entry.getKey(), entry.getValue().state() <= AppOpsManager.UID_STATE_FOREGROUND);
        }
        policy.activities(visible);
    }

    void foregroundServicesChanged(Set<Integer> uids) { policy.foregroundServices(uids); }

    private void retire(String name) {
        Session[] captured;
        synchronized (sessions) { captured = sessions.values().toArray(new Session[0]); }
        for (Session session : captured) if (session.owner.packageName.equals(name)) session.close();
    }

    private ActivityTaskManagerService.ProcessRecord callingProcess() {
        ActivityTaskManagerService.ProcessRecord process = activities.authenticatedProcessForPid(
                Binder.getCallingPid(), Binder.getCallingUid());
        if (process == null) throw new SecurityException("Unrecognized sensor application identity");
        return process;
    }

    @Override protected boolean onTransact(int code, Parcel data, Parcel reply, int flags)
            throws RemoteException {
        if (code != NativeSensorAccess.OPEN) return super.onTransact(code, data, reply, flags);
        data.enforceInterface(NativeSensorAccess.DESCRIPTOR);
        IBinder callback = data.readStrongBinder();
        data.enforceNoDataAvail();
        if (callback == null || reply == null) throw new IllegalArgumentException("Missing sensor subscription");
        Session session = openSubscription(callback);
        reply.writeNoException();
        reply.writeStrongBinder(session);
        writeState(reply, session.subscription.snapshot());
        return true;
    }

    private Session openSubscription(IBinder callback) throws RemoteException {
        ActivityTaskManagerService.ProcessRecord process = callingProcess();
        synchronized (sessions) {
            // This transport supplies one process-wide native policy snapshot.
            // Individual NDK event queues do not need their own Binder sessions.
            if (sessions.containsKey(process) || !opening.add(process)) {
                throw new IllegalStateException("Sensor policy is already subscribed for this process");
            }
        }
        try {
            Session session = new Session(process, callback);
            try {
                callback.linkToDeath(session, 0);
                synchronized (sessions) { if (!session.closed) sessions.put(process, session); }
                if (session.closed || callingProcess() != process) {
                    throw new SecurityException("Sensor application identity retired during registration");
                }
                return session;
            } catch (RemoteException | RuntimeException failure) {
                session.close();
                throw failure;
            }
        } finally {
            synchronized (sessions) { opening.remove(process); }
        }
    }

    private static void writeState(Parcel parcel, SensorAccessPolicy.State state) {
        parcel.writeLong(state.generation());
        parcel.writeInt(state.flags());
    }

    private final class Session extends Binder implements IBinder.DeathRecipient {
        final ActivityTaskManagerService.ProcessRecord process;
        final ApkPackage owner;
        final IBinder callback;
        final SensorAccessPolicy.Subscription subscription;
        volatile boolean closed;

        Session(ActivityTaskManagerService.ProcessRecord process, IBinder callback) {
            super(NativeSensorAccess.SESSION_DESCRIPTOR);
            this.process = process;
            owner = process.packageCode;
            this.callback = callback;
            boolean capped = owner.parsed.getTargetSdkVersion() >= 31
                    && packages.permissions().checkPermission(owner.packageName,
                            Manifest.permission.HIGH_SAMPLING_RATE_SENSORS) != PackageManager.PERMISSION_GRANTED;
            subscription = policy.open(owner.uid(), capped, owner.parsed.isDebuggable(),
                    state -> notifications.execute(() -> deliver(state)));
        }

        private void deliver(SensorAccessPolicy.State state) {
            // No controller/policy monitor is held across application Binder code.
            Parcel data = Parcel.obtain();
            try {
                data.writeInterfaceToken(NativeSensorAccess.CALLBACK_DESCRIPTOR);
                writeState(data, state);
                if (!callback.transact(NativeSensorAccess.CHANGED, data, null, IBinder.FLAG_ONEWAY)) close();
            } catch (RemoteException | RuntimeException dead) { close(); }
            finally { data.recycle(); }
        }

        @Override protected boolean onTransact(int code, Parcel data, Parcel reply, int flags)
                throws RemoteException {
            if (code != NativeSensorAccess.CLOSE) return super.onTransact(code, data, reply, flags);
            data.enforceInterface(NativeSensorAccess.SESSION_DESCRIPTOR);
            data.enforceNoDataAvail();
            if (callingProcess() != process)
                throw new SecurityException("Sensor subscription belongs to another process");
            close();
            if (reply != null) reply.writeNoException();
            return true;
        }

        void close() {
            synchronized (sessions) {
                if (closed) return;
                closed = true;
                sessions.remove(process, this);
            }
            subscription.close();
            try { callback.unlinkToDeath(this, 0); }
            catch (NoSuchElementException ignored) { } // Registration may have failed.
        }

        @Override public void binderDied() { close(); }
    }
}
