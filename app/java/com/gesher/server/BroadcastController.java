/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.server;

import android.app.ActivityManager;
import android.app.IApplicationThread;
import android.content.IIntentReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.content.pm.ActivityInfo;
import android.content.pm.PackageManager;
import android.content.pm.ResolveInfo;
import android.content.res.CompatibilityInfo;
import android.os.Bundle;
import android.os.Binder;
import android.os.IBinder;
import android.os.Process;
import android.os.RemoteException;

import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.Set;
import java.util.Collections;
import java.util.IdentityHashMap;
import java.util.concurrent.Executors;
import java.util.concurrent.ScheduledExecutorService;
import java.util.concurrent.ScheduledFuture;
import java.util.concurrent.TimeUnit;

/**
 * Broadcasts, lite (BroadcastController and BroadcastQueue): receivers registered at run
 * time ({@code registerReceiver}) and the manifest's {@code <receiver>}s, delivered in
 * order, one intent at a time, not ordered (no results, no abort), not sticky.
 *
 * <ul>
 * <li>A registered receiver gets an implicit broadcast its filter matches, through its
 *     process's {@code scheduleRegisteredReceiver} (→ the LoadedApk ReceiverDispatcher →
 *     onReceive on the main thread).
 * <li>A manifest receiver gets a broadcast naming it (explicit), or an implicit one sent
 *     to its package ({@code setPackage}) that its filter matches, as Android delivers
 *     implicit broadcasts to manifest receivers only then: {@code scheduleReceiver} (→
 *     handleReceiver: a new instance, onReceive, {@code finishReceiver}).
 * </ul>
 */
final class BroadcastController {
    private final ActivityTaskManagerService mAtm;
    private final PackageManagerService mPm;
    private final CallerIdentity mCallers;
    private static final long RECEIVER_TIMEOUT_MS = 60_000;
    private final long mReceiverTimeoutMillis;
    private final ScheduledExecutorService mTimeouts = Executors.newSingleThreadScheduledExecutor(r -> {
        Thread thread = new Thread(r, "broadcast-timeout");
        thread.setDaemon(true);
        return thread;
    });

    private static final class Sender {
        final String packageName;
        final int uid;
        final boolean system;
        Sender(String packageName, int uid, boolean system) {
            this.packageName = packageName; this.uid = uid; this.system = system;
        }
    }

    private static final class ReceiverFilter {
        final IntentFilter filter;
        final String permission;
        final boolean exported;
        ReceiverFilter(IntentFilter filter, String permission, int flags) {
            this.filter = new IntentFilter(filter);
            this.permission = permission;
            exported = (flags & Context.RECEIVER_NOT_EXPORTED) == 0;
        }
    }

    /** A registered receiver: ReceiverList, with its filters. */
    private static final class ReceiverList {
        final IIntentReceiver receiver;
        final ActivityTaskManagerService.ProcessRecord app;
        final String packageName;
        final List<ReceiverFilter> filters = new ArrayList<>();

        ReceiverList(IIntentReceiver receiver, ActivityTaskManagerService.ProcessRecord app,
                String packageName) {
            this.receiver = receiver;
            this.app = app;
            this.packageName = packageName;
        }
    }

    // Guarded by this.
    private final Map<IBinder, ReceiverList> mRegistered = new LinkedHashMap<>();
    private final List<ManifestDelivery> mManifest = new ArrayList<>();
    private final Set<ActivityTaskManagerService.ProcessRecord> mTimedOut =
            Collections.newSetFromMap(new IdentityHashMap<>());

    /** Reservations exist before any callback, including while a process is starting. */
    private static final class ManifestDelivery {
        final ActivityInfo info;
        final Intent intent;
        final ApkPackage owner;
        final int sendingUid;
        final String sendingPackage;
        ActivityTaskManagerService.ProcessRecord app;
        boolean dispatched;
        ScheduledFuture<?> timeout;
        long timeoutGeneration;

        ManifestDelivery(ActivityInfo info, Intent intent, ApkPackage owner, Sender sender) {
            this.info = info;
            this.intent = new Intent(intent).setComponent(
                    new android.content.ComponentName(info.packageName, info.name));
            this.owner = owner;
            sendingUid = sender.uid;
            sendingPackage = sender.packageName;
        }
    }

    BroadcastController(ActivityTaskManagerService atm, PackageManagerService pm) {
        this(atm, pm, RECEIVER_TIMEOUT_MS);
    }

    /** A short deadline can be injected by the offline lifecycle regression. */
    BroadcastController(ActivityTaskManagerService atm, PackageManagerService pm, long timeoutMillis) {
        if (timeoutMillis <= 0) throw new IllegalArgumentException("Invalid receiver timeout");
        mAtm = atm;
        mPm = pm;
        mCallers = new CallerIdentity(pm, atm);
        mReceiverTimeoutMillis = timeoutMillis;
    }

    Intent registerReceiver(IApplicationThread caller, String callerPackage,
            IIntentReceiver receiver, IntentFilter filter, int callingPid) {
        return registerReceiver(caller, callerPackage, receiver, filter, callingPid, null, 0);
    }

    Intent registerReceiver(IApplicationThread caller, String callerPackage,
            IIntentReceiver receiver, IntentFilter filter, int callingPid,
            String requiredPermission, int flags) {
        if (filter == null) throw new IllegalArgumentException("Missing receiver filter");
        if ((flags & Context.RECEIVER_EXPORTED) != 0 && (flags & Context.RECEIVER_NOT_EXPORTED) != 0) {
            throw new IllegalArgumentException("Conflicting receiver export flags");
        }
        Sender sender = sender(callerPackage, callingPid);
        ActivityTaskManagerService.ProcessRecord app = mAtm.callerProcess(callingPid);
        if (!mCallers.isSystem()) {
            if (app == null || caller == null || !caller.asBinder().equals(app.thread.asBinder())) {
                throw new SecurityException("Receiver registration requires the caller's application thread");
            }
        }
        // A query for sticky broadcasts (devsvc: the system's, StickyBroadcasts).
        if (receiver == null) return StickyBroadcasts.match(filter);
        if (app == null) {
            SystemServer.log("registerReceiver from pid " + callingPid
                    + ": not an attached process");
            return null;
        }
        synchronized (this) {
            ReceiverList rl = mRegistered.get(receiver.asBinder());
            if (rl == null) {
                rl = new ReceiverList(receiver, app, sender.packageName);
                mRegistered.put(receiver.asBinder(), rl);
            } else if (rl.app != app || !java.util.Objects.equals(rl.packageName, sender.packageName)) {
                throw new SecurityException("Receiver Binder belongs to another application");
            }
            rl.filters.add(new ReceiverFilter(filter, requiredPermission, flags));
        }
        StringBuilder actions = new StringBuilder();
        for (int i = 0; i < filter.countActions(); i++) {
            if (i > 0) actions.append(", ");
            actions.append(filter.getAction(i));
        }
        SystemServer.log("registerReceiver from " + callerPackage + ": " + actions);
        // A sticky broadcast the filter matches is returned and delivered (devsvc).
        Intent sticky = StickyBroadcasts.match(filter);
        if (sticky != null) {
            try {
                app.thread.scheduleRegisteredReceiver(receiver, sticky,
                        android.app.Activity.RESULT_OK, null, null, false /* ordered */,
                        true /* sticky */, true /* assumeDelivered */, 0,
                        ActivityManager.PROCESS_STATE_TOP, Process.myUid(), "android");
            } catch (RemoteException e) {
                SystemServer.log("scheduleRegisteredReceiver (sticky): " + e);
            }
        }
        return sticky;
    }

    void unregisterReceiver(IIntentReceiver receiver) {
        ReceiverList rl;
        synchronized (this) {
            rl = mRegistered.get(receiver.asBinder());
            if (rl != null && !mCallers.isSystem() && rl.app.pid != Binder.getCallingPid()) {
                throw new SecurityException("Cannot unregister another application's receiver");
            }
            mRegistered.remove(receiver.asBinder());
        }
        if (rl != null) SystemServer.log("unregisterReceiver of " + rl.packageName);
    }

    int broadcastIntent(Intent intent, String resolvedType, String callingPackage,
            int callingPid) {
        return broadcastIntent(intent, resolvedType, callingPackage, callingPid, null, null, null);
    }

    int broadcastIntent(Intent intent, String resolvedType, String callingPackage,
            int callingPid, String[] requiredPermissions, String[] excludePermissions,
            String[] excludePackages) {
        if (intent == null) throw new IllegalArgumentException("Missing broadcast intent");
        Sender sender = sender(callingPackage, callingPid);
        intent = new Intent(intent);
        if (GmsSignatureSpoofing.isEnabled() && !sender.system
                && mPm.isProtectedBroadcast(intent.getAction())) {
            throw new SecurityException("Only the system can send a protected broadcast");
        }
        enforcePushSender(sender, intent);
        callingPackage = sender.packageName;
        // Registered receivers: implicit broadcasts only.
        List<ReceiverList> registered = new ArrayList<>();
        if (intent.getComponent() == null) {
            synchronized (this) {
                for (ReceiverList rl : mRegistered.values()) {
                    if (intent.getPackage() != null && !intent.getPackage().equals(rl.packageName)) {
                        continue;
                    }
                    if (!receiverPermissions(rl.packageName, requiredPermissions,
                            excludePermissions, excludePackages)) continue;
                    for (ReceiverFilter registeredFilter : rl.filters) {
                        if (!componentAllowed(sender, rl.packageName,
                                registeredFilter.exported, registeredFilter.permission)) continue;
                        if (registeredFilter.filter.match(intent.getAction(), resolvedType != null ? resolvedType
                                : intent.getType(), intent.getScheme(), intent.getData(),
                                intent.getCategories(), "gesher") >= 0) {
                            registered.add(rl);
                            break;
                        }
                    }
                }
            }
        }
        // Manifest receivers: explicit, or implicit to a package, or C2DM/FCM push actions.
        List<ActivityInfo> manifest = new ArrayList<>();
        if (intent.getComponent() != null || intent.getPackage() != null
                || "com.google.android.c2dm.intent.RECEIVE".equals(intent.getAction())
                || "com.google.android.c2dm.intent.REGISTRATION".equals(intent.getAction())) {
            for (ResolveInfo ri : mPm.receivers(intent, resolvedType,
                    PackageManager.GET_META_DATA | PackageManager.GET_SHARED_LIBRARY_FILES)) {
                ActivityInfo info = ri.activityInfo;
                if (componentAllowed(sender, info.packageName, info.exported, info.permission)
                        && receiverPermissions(info.packageName, requiredPermissions,
                                excludePermissions, excludePackages)) manifest.add(info);
            }
        }
        SystemServer.log("broadcastIntent " + intent.toShortString(false, true, true, false)
                + " from " + callingPackage + ": " + registered.size() + " registered, "
                + manifest.size() + " manifest receiver(s)");
        int sendingUid = sender.uid;
        final String sendingPackage = sender.packageName;
        Map<String, List<ManifestDelivery>> batches = new LinkedHashMap<>();
        synchronized (this) {
            // Reserve the whole batch before registered callbacks or withProcess can run
            // synchronously. finishReceiver for a fast first receiver must see the rest.
            for (ActivityInfo info : manifest) {
                ApkPackage owner = mPm.get(info.packageName);
                if (owner == null) continue;
                ManifestDelivery delivery = new ManifestDelivery(info, intent, owner, sender);
                mManifest.add(delivery);
                armTimeout(delivery);
                batches.computeIfAbsent(info.packageName, ignored -> new ArrayList<>()).add(delivery);
            }
        }
        for (ReceiverList rl : registered) {
            try {
                rl.app.thread.scheduleRegisteredReceiver(rl.receiver, intent,
                        android.app.Activity.RESULT_OK, null /* data */, null /* extras */,
                        false /* ordered */, false /* sticky */, true /* assumeDelivered */,
                        0 /* sendingUser */, ActivityManager.PROCESS_STATE_TOP, sendingUid,
                        sendingPackage);
            } catch (RemoteException e) {
                SystemServer.log("scheduleRegisteredReceiver: " + e);
            }
        }
        for (Map.Entry<String, List<ManifestDelivery>> batch : batches.entrySet()) {
            long identity = Binder.clearCallingIdentity();
            try {
                mAtm.withProcess(batch.getKey(), "broadcast to " + batch.getKey(), app -> {
                    synchronized (BroadcastController.this) {
                        for (ManifestDelivery delivery : batch.getValue()) {
                            if (!mManifest.contains(delivery)) continue;
                            if (app.exiting || mTimedOut.contains(app)
                                    || delivery.owner != mPm.get(delivery.info.packageName)) {
                                removeDelivery(delivery);
                            } else {
                                delivery.app = app;
                            }
                        }
                    }
                    scheduleNext(app);
                    mAtm.maybeExit(app);
                });
            } catch (RuntimeException failedStart) {
                synchronized (this) {
                    for (ManifestDelivery delivery : batch.getValue()) removeDelivery(delivery);
                }
                throw failedStart;
            } finally { Binder.restoreCallingIdentity(identity); }
        }
        return ActivityManager.BROADCAST_SUCCESS;
    }

    private Sender sender(String claimed, int pid) {
        if (mCallers.isSystem()) {
            // PendingIntent/job dispatch on behalf of an app keeps that app's
            // permissions even when the trusted server cleared transport identity.
            ApkPackage owner = claimed == null ? null : mPm.get(claimed);
            if (owner != null) return new Sender(owner.packageName, owner.uid(), false);
            if (claimed != null && !"android".equals(claimed)) {
                throw new SecurityException("Unknown broadcast sender package");
            }
            return new Sender("android", Process.SYSTEM_UID, true);
        }
        if (pid != Binder.getCallingPid()) throw new SecurityException("Forged broadcast sender PID");
        String actual = mCallers.requirePackage(claimed);
        return new Sender(actual, Binder.getCallingUid(), false);
    }

    private boolean componentAllowed(Sender sender, String owner, boolean exported, String permission) {
        if (sender.system || owner.equals(sender.packageName)) return true;
        return exported && (permission == null || permission.isEmpty()
                || mPm.permissions().checkPermission(sender.packageName, permission) == PackageManager.PERMISSION_GRANTED);
    }

    private void enforcePushSender(Sender sender, Intent intent) {
        String action = intent.getAction();
        if (sender.system || (!"com.google.android.c2dm.intent.RECEIVE".equals(action)
                && !"com.google.android.c2dm.intent.REGISTRATION".equals(action))) return;
        String target = intent.getComponent() == null ? intent.getPackage()
                : intent.getComponent().getPackageName();
        // Sending only to one's own package cannot forge another app's FCM delivery.
        if (sender.packageName.equals(target)) return;
        if (mPm.permissions().checkPermission(sender.packageName,
                "com.google.android.c2dm.permission.SEND") != PackageManager.PERMISSION_GRANTED) {
            throw new SecurityException("C2DM broadcast requires the SEND permission");
        }
    }

    private boolean receiverPermissions(String pkg, String[] required, String[] excluded,
            String[] excludedPackages) {
        if (excludedPackages != null) for (String excludedPackage : excludedPackages) {
            if (pkg.equals(excludedPackage)) return false;
        }
        if (required != null) for (String permission : required) {
            if (permission != null && mPm.permissions().checkPermission(pkg, permission)
                    != PackageManager.PERMISSION_GRANTED) return false;
        }
        if (excluded != null) for (String permission : excluded) {
            if (permission != null && mPm.permissions().checkPermission(pkg, permission)
                    == PackageManager.PERMISSION_GRANTED) return false;
        }
        return true;
    }

    void finishReceiver(IBinder who, int resultCode, String resultData, Bundle map,
            boolean abortBroadcast, int flags) {
        ActivityTaskManagerService.ProcessRecord app = mAtm.callerProcess(Binder.getCallingPid());
        if (app == null || !app.ready || app.exiting || app.packageName == null
                || app.uid != Binder.getCallingUid() || who == null
                || !who.equals(app.thread.asBinder())) {
            throw new SecurityException("Receiver completion requires its attached PID and application thread");
        }
        synchronized (this) {
            if (mTimedOut.contains(app)) throw new SecurityException("Receiver process generation timed out");
            // ActivityThread uses its application-thread Binder for every manifest receiver.
            // Only one may be dispatched per process, so an async finish is unambiguous.
            for (ManifestDelivery delivery : mManifest) {
                if (delivery.app == app && delivery.dispatched) {
                    removeDelivery(delivery);
                    break;
                }
            }
        }
        long identity = Binder.clearCallingIdentity();
        try {
            scheduleNext(app);
            mAtm.maybeExit(app);
        } finally { Binder.restoreCallingIdentity(identity); }
    }

    /** Whether queued or currently executing manifest work keeps this process alive. */
    synchronized boolean hasReceivers(ActivityTaskManagerService.ProcessRecord app) {
        for (ManifestDelivery delivery : mManifest) {
            if (delivery.app == app || (delivery.app == null
                    && delivery.info.packageName.equals(app.packageName))) return true;
        }
        return false;
    }

    /** Guarded by this. Pending starts and active receivers both have bounded lifetimes. */
    private void armTimeout(ManifestDelivery delivery) {
        if (delivery.timeout != null) delivery.timeout.cancel(false);
        long generation = ++delivery.timeoutGeneration;
        delivery.timeout = mTimeouts.schedule(() -> expire(delivery, generation),
                mReceiverTimeoutMillis, TimeUnit.MILLISECONDS);
    }

    /** Guarded by this. */
    private boolean removeDelivery(ManifestDelivery delivery) {
        if (!mManifest.remove(delivery)) return false;
        if (delivery.timeout != null) delivery.timeout.cancel(false);
        delivery.timeout = null;
        return true;
    }

    private void expire(ManifestDelivery delivery, long generation) {
        ActivityTaskManagerService.ProcessRecord app;
        boolean active;
        synchronized (this) {
            if (delivery.timeoutGeneration != generation) return;
            if (!removeDelivery(delivery)) return;
            app = delivery.app;
            active = app != null && delivery.dispatched;
            if (active) {
                // A late finish carries only the application-thread token. It must
                // never consume a later delivery, so retire the entire process generation.
                mTimedOut.add(app);
                for (ManifestDelivery queued : new ArrayList<>(mManifest)) {
                    if (queued.app == app) removeDelivery(queued);
                }
            }
        }
        SystemServer.log("manifest receiver timed out: " + delivery.info.packageName + "/" + delivery.info.name);
        if (active) {
            mAtm.receiverTimedOut(app);
        } else if (app != null) {
            scheduleNext(app);
            mAtm.maybeExit(app);
        }
    }

    private void scheduleNext(ActivityTaskManagerService.ProcessRecord app) {
        while (true) {
            ManifestDelivery next = null;
            synchronized (this) {
                if (app.exiting || mTimedOut.contains(app) || mAtm.callerProcess(app.pid) != app) return;
                for (ManifestDelivery delivery : mManifest) {
                    if (delivery.app == app && delivery.dispatched) return;
                    if (next == null && delivery.app == app) next = delivery;
                }
                if (next == null) return;
                if (next.owner != mPm.get(next.info.packageName)) {
                    removeDelivery(next);
                    continue;
                }
                next.dispatched = true;
                armTimeout(next);
            }
            SystemServer.log("scheduleReceiver " + next.info.packageName + "/" + next.info.name
                    + " in " + app);
            try {
                app.thread.scheduleReceiver(next.intent, next.info,
                        CompatibilityInfo.DEFAULT_COMPATIBILITY_INFO,
                        android.app.Activity.RESULT_OK, null /* data */, null /* extras */,
                        false /* ordered */, false /* assumeDelivered */, 0 /* sendingUser */,
                        ActivityManager.PROCESS_STATE_RECEIVER, next.sendingUid, next.sendingPackage);
                return;
            } catch (RemoteException | RuntimeException failedDelivery) {
                SystemServer.log("scheduleReceiver: " + failedDelivery);
                synchronized (this) { removeDelivery(next); }
                // Drain failed Binder deliveries without recursively growing the server stack.
            }
        }
    }

    /** The process went away: so did its registered receivers. */
    void processDied(ActivityTaskManagerService.ProcessRecord app) {
        synchronized (this) {
            mRegistered.values().removeIf(rl -> rl.app == app);
            mTimedOut.remove(app);
            for (ManifestDelivery delivery : new ArrayList<>(mManifest)) {
                // Unassigned work may already target a replacement process that
                // started after ATM removed this old generation but before cleanup.
                if (delivery.app == app) removeDelivery(delivery);
            }
        }
    }
}
