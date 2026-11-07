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
import android.os.Parcel;

import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.Set;
import java.util.Collections;
import java.util.IdentityHashMap;
import java.util.concurrent.ScheduledFuture;
import java.util.concurrent.ScheduledThreadPoolExecutor;
import java.util.concurrent.TimeUnit;
import java.util.function.BooleanSupplier;

/**
 * Broadcasts, lite (BroadcastController and BroadcastQueue): receivers registered at run
 * time ({@code registerReceiver}) and the manifest's {@code <receiver>}s. Ordered sends
 * retain results through explicit receiver completion and one final caller callback.
 * Mutable sticky broadcasts are unsupported.
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
    private Runnable mBeforeSenderAuthentication; // Server-local direct-caller race fixture.
    private static final long RECEIVER_TIMEOUT_MS = 60_000;
    private static final int MAX_ORDERED = 16, MAX_ORDERED_PER_SENDER = 4, MAX_ORDERED_TARGETS = 128;
    private static final long MAX_ORDERED_MILLIS = 5 * 60_000;
    private final long mReceiverTimeoutMillis;
    private final ScheduledThreadPoolExecutor mTimeouts = new ScheduledThreadPoolExecutor(1, r -> {
        Thread thread = new Thread(r, "broadcast-timeout");
        thread.setDaemon(true);
        return thread;
    });

    private static final class Sender {
        final String packageName;
        final int uid;
        final boolean system;
        final BooleanSupplier lifetime;
        final BooleanSupplier acceptance;
        Sender(String packageName, int uid, boolean system, BooleanSupplier lifetime) {
            this(packageName, uid, system, lifetime, lifetime);
        }
        Sender(String packageName, int uid, boolean system, BooleanSupplier lifetime, BooleanSupplier acceptance) {
            this.packageName = packageName; this.uid = uid; this.system = system;
            this.lifetime = lifetime;
            this.acceptance = acceptance;
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
        final ApkPackage owner;
        final List<ReceiverFilter> filters = new ArrayList<>();

        ReceiverList(IIntentReceiver receiver, ActivityTaskManagerService.ProcessRecord app,
                String packageName, ApkPackage owner) {
            this.receiver = receiver;
            this.app = app;
            this.packageName = packageName;
            this.owner = owner;
        }
    }

    // Guarded by this.
    private final Map<IBinder, ReceiverList> mRegistered = new LinkedHashMap<>();
    private final List<ManifestDelivery> mManifest = new ArrayList<>();
    private final List<OrderedBroadcast> mOrdered = new ArrayList<>();
    private final Map<ActivityTaskManagerService.ProcessRecord, OrderedDelivery> mOrderedActive =
            new IdentityHashMap<>();
    private long mNextReservation;
    private boolean mOrderedPumping;
    private final Set<ActivityTaskManagerService.ProcessRecord> mTimedOut =
            Collections.newSetFromMap(new IdentityHashMap<>());

    /** Reservations exist before any callback, including while a process is starting. */
    private static final class ManifestDelivery {
        final ActivityInfo info;
        final Intent intent;
        final ApkPackage owner;
        final int sendingUid;
        final String sendingPackage;
        final Sender sender;
        ActivityTaskManagerService.ProcessRecord app;
        boolean dispatched;
        ScheduledFuture<?> timeout;
        long timeoutGeneration;
        long sequence;

        ManifestDelivery(ActivityInfo info, Intent intent, ApkPackage owner, Sender sender) {
            this.info = info;
            this.intent = new Intent(intent).setComponent(
                    new android.content.ComponentName(info.packageName, info.name));
            this.owner = owner;
            sendingUid = sender.uid;
            sendingPackage = sender.packageName;
            this.sender = sender;
        }
    }

    private static final class OrderedTarget {
        final ReceiverList registered;
        final ReceiverFilter filter;
        final ActivityInfo info;
        final ApkPackage owner;
        final int priority;
        OrderedTarget(ReceiverList registered, ReceiverFilter filter) {
            this.registered = registered; this.filter = filter; info = null;
            owner = registered.owner; priority = filter.filter.getPriority();
        }
        OrderedTarget(ActivityInfo info, ApkPackage owner, int priority) {
            registered = null; filter = null; this.info = new ActivityInfo(info);
            this.owner = owner; this.priority = priority;
        }
        String packageName() { return registered != null ? registered.packageName : info.packageName; }
    }

    private static final class OrderedBroadcast {
        final Intent intent;
        final String resolvedType;
        final Sender sender;
        final ApkPackage senderCode;
        final ActivityTaskManagerService.ProcessRecord senderApp;
        final List<OrderedTarget> targets;
        final String[] required, excluded, excludedPackages;
        final boolean ordered;
        IIntentReceiver resultTo;
        int next, resultCode;
        String resultData;
        Bundle resultExtras;
        boolean aborted, expired, finalizing;
        OrderedDelivery active;
        ScheduledFuture<?> deadline;
        OrderedBroadcast(Intent intent, String resolvedType, Sender sender, ApkPackage senderCode,
                ActivityTaskManagerService.ProcessRecord senderApp, List<OrderedTarget> targets,
                String[] required, String[] excluded, String[] excludedPackages, boolean ordered,
                IIntentReceiver resultTo, int code, String data, Bundle extras) {
            this.intent = intent; this.resolvedType = resolvedType; this.sender = sender;
            this.senderCode = senderCode; this.senderApp = senderApp; this.targets = targets;
            this.required = required; this.excluded = excluded; this.excludedPackages = excludedPackages;
            this.ordered = ordered; this.resultTo = resultTo;
            resultCode = code; resultData = data; resultExtras = extras;
        }
    }

    private static final class OrderedDelivery {
        final OrderedBroadcast chain;
        final OrderedTarget target;
        final long sequence;
        ActivityTaskManagerService.ProcessRecord app;
        boolean starting, dispatched, finished;
        ScheduledFuture<?> timeout;
        long timeoutGeneration;
        OrderedDelivery(OrderedBroadcast chain, OrderedTarget target, long sequence) {
            this.chain = chain; this.target = target; this.sequence = sequence;
            app = target.registered != null ? target.registered.app : null;
        }
    }

    /** Full public send path; private system dispatch continues to use the legacy overloads. */
    int broadcastIntent(IApplicationThread caller, Intent intent, String resolvedType,
            String callingPackage, int callingPid, IIntentReceiver resultTo, int resultCode,
            String resultData, Bundle resultExtras, String[] requiredPermissions,
            String[] excludePermissions, String[] excludePackages, boolean ordered) {
        Sender origin = sender(callingPackage, callingPid);
        ActivityTaskManagerService.ProcessRecord app = mAtm.callerProcess(callingPid);
        // The standalone driver has no attached server process. Its local,
        // unacknowledged sends still pass through the ordinary sender policy.
        if (!ordered && resultTo == null && app == null
                && callingPid == Process.myPid() && callingPid == Binder.getCallingPid()) {
            return broadcastIntent(intent, resolvedType, origin,
                    requiredPermissions, excludePermissions, excludePackages);
        }
        if (caller != null && (app == null || app.exiting || app.thread == null
                || !caller.asBinder().equals(app.thread.asBinder())
                || callingPid != Binder.getCallingPid() || app.uid != Binder.getCallingUid())) {
            throw new SecurityException("Broadcast sender requires its own attached application thread");
        }
        if (caller == null && !mCallers.isSystem()) {
            throw new SecurityException("Missing broadcast application thread");
        }
        if (!ordered && resultTo == null) {
            return broadcastIntent(intent, resolvedType, origin,
                    requiredPermissions, excludePermissions, excludePackages);
        }
        if (intent == null) throw new IllegalArgumentException("Missing broadcast intent");
        Intent snapshot = new Intent(intent);
        validateBroadcastSender(origin, snapshot);
        boundedIntent(snapshot);
        String[] required = copy(requiredPermissions), excluded = copy(excludePermissions),
                excludedPackages = copy(excludePackages);
        Bundle extras = resultExtras(resultExtras);
        if (resultData != null && resultData.length() > 65536) throw new IllegalArgumentException("Broadcast result data too large");
        List<OrderedTarget> targets = new ArrayList<>();
        synchronized (this) {
            if (snapshot.getComponent() == null) for (ReceiverList rl : mRegistered.values()) {
                if (snapshot.getPackage() != null && !snapshot.getPackage().equals(rl.packageName)
                        || !receiverPermissions(rl.packageName, required, excluded, excludedPackages)) continue;
                ReceiverFilter best = null;
                for (ReceiverFilter filter : rl.filters) {
                    if (componentAllowed(origin, rl.packageName, filter.exported, filter.permission)
                            && filter.filter.match(snapshot.getAction(), resolvedType != null
                                    ? resolvedType : snapshot.getType(), snapshot.getScheme(),
                                    snapshot.getData(), snapshot.getCategories(), "gesher") >= 0
                            && (best == null || filter.filter.getPriority() > best.filter.getPriority())) best = filter;
                }
                if (best != null) targets.add(new OrderedTarget(rl, best));
            }
        }
        if (snapshot.getComponent() != null || snapshot.getPackage() != null
                || "com.google.android.c2dm.intent.RECEIVE".equals(snapshot.getAction())
                || "com.google.android.c2dm.intent.REGISTRATION".equals(snapshot.getAction())) {
            synchronized (mPm) {
            for (ResolveInfo ri : mPm.receivers(snapshot, resolvedType,
                    PackageManager.GET_META_DATA | PackageManager.GET_SHARED_LIBRARY_FILES)) {
                ActivityInfo info = ri.activityInfo;
                ApkPackage owner = mPm.get(info.packageName);
                if (owner != null && componentAllowed(origin, info.packageName, info.exported, info.permission)
                        && receiverPermissions(info.packageName, required, excluded, excludedPackages)) {
                    targets.add(new OrderedTarget(info, owner, ri.priority));
                }
            }
            }
        }
        if (targets.size() > MAX_ORDERED_TARGETS) throw new IllegalStateException("Too many acknowledged broadcast receivers");
        targets.sort((a, b) -> Integer.compare(b.priority, a.priority)); // stable for equal priorities
        OrderedBroadcast chain = new OrderedBroadcast(snapshot, resolvedType, origin,
                mPm.get(origin.packageName), app, targets, required, excluded, excludedPackages,
                ordered, resultTo, resultCode, resultData, extras);
        synchronized (this) {
            if (!origin.acceptance.getAsBoolean()) return ActivityManager.START_CANCELED;
            int owned = 0;
            for (OrderedBroadcast pending : mOrdered) {
                if (pending.sender.packageName.equals(origin.packageName)) owned++;
            }
            if (mOrdered.size() >= MAX_ORDERED || owned >= MAX_ORDERED_PER_SENDER) {
                throw new IllegalStateException("Acknowledged broadcast queue is full");
            }
            mOrdered.add(chain);
            long budget = mReceiverTimeoutMillis >= MAX_ORDERED_MILLIS / 4
                    ? MAX_ORDERED_MILLIS : mReceiverTimeoutMillis * 4;
            chain.deadline = mTimeouts.schedule(() -> expireOrdered(chain), budget, TimeUnit.MILLISECONDS);
        }
        pumpOrdered();
        return ActivityManager.BROADCAST_SUCCESS;
    }

    private static String[] copy(String[] source) {
        if (source == null) return null;
        if (source.length > 128) throw new IllegalArgumentException("Broadcast filter array too large");
        return source.clone();
    }

    private static Bundle resultExtras(Bundle source) {
        if (source == null) return null;
        Bundle copy = new Bundle(source);
        if (copy.hasFileDescriptors()) throw new IllegalArgumentException("Broadcast results cannot contain descriptors");
        copy.setAllowFds(false);
        Parcel parcel = Parcel.obtain();
        try {
            parcel.writeBundle(copy);
            if (parcel.dataSize() > 256 * 1024) throw new IllegalArgumentException("Broadcast result extras too large");
        } finally { parcel.recycle(); }
        return copy;
    }

    private static void boundedIntent(Intent intent) {
        Parcel parcel = Parcel.obtain();
        try {
            intent.writeToParcel(parcel, 0);
            if (parcel.hasFileDescriptors() || parcel.dataSize() > 512 * 1024) {
                throw new IllegalArgumentException("Broadcast payload is unsupported or too large");
            }
        } finally { parcel.recycle(); }
    }

    private void validateBroadcastSender(Sender sender, Intent intent) {
        if (GmsSignatureSpoofing.isEnabled() && !sender.system && mPm.isProtectedBroadcast(intent.getAction())) {
            throw new SecurityException("Only the system can send a protected broadcast");
        }
        enforcePushSender(sender, intent);
    }

    /** Guarded by this; reservations sharing the framework's app-thread finish token. */
    private boolean legacyActive(ActivityTaskManagerService.ProcessRecord app) {
        for (ManifestDelivery delivery : mManifest) {
            if (delivery.app == app && delivery.dispatched) return true;
        }
        return false;
    }

    private boolean olderLegacy(ActivityTaskManagerService.ProcessRecord app, long sequence) {
        for (ManifestDelivery delivery : mManifest) {
            if (delivery.app == app && delivery.sequence < sequence) return true;
        }
        return false;
    }

    private OrderedDelivery firstOrdered(ActivityTaskManagerService.ProcessRecord app) {
        OrderedDelivery first = null;
        for (OrderedBroadcast chain : mOrdered) {
            OrderedDelivery delivery = chain.active;
            if (delivery != null && delivery.app == app && !delivery.finished
                    && (first == null || delivery.sequence < first.sequence)) first = delivery;
        }
        return first;
    }

    private boolean senderCurrent(OrderedBroadcast chain) {
        if (!chain.sender.lifetime.getAsBoolean()) return false;
        if (!chain.sender.system && (chain.senderCode == null
                || mPm.get(chain.sender.packageName) != chain.senderCode)) return false;
        ActivityTaskManagerService.ProcessRecord app = chain.senderApp;
        return app == null || !app.exiting && mAtm.callerProcess(app.pid) == app;
    }

    /** Recheck permissions and code identity at dispatch, after any asynchronous wait. */
    private boolean targetCurrent(OrderedBroadcast chain, OrderedTarget target) {
        if (!chain.sender.lifetime.getAsBoolean() || target.owner == null
                || mPm.get(target.packageName()) != target.owner || mPm.isRemovalPending(target.packageName())
                || !receiverPermissions(target.packageName(), chain.required,
                        chain.excluded, chain.excludedPackages)) return false;
        if (target.registered != null) {
            ReceiverList rl = target.registered;
            return mRegistered.get(rl.receiver.asBinder()) == rl && !rl.app.exiting
                    && mAtm.callerProcess(rl.app.pid) == rl.app
                    && !mTimedOut.contains(rl.app)
                    && componentAllowed(chain.sender, rl.packageName,
                            target.filter.exported, target.filter.permission);
        }
        ActivityInfo fresh = mPm.getReceiverInfo(new android.content.ComponentName(
                target.info.packageName, target.info.name), PackageManager.GET_META_DATA, 0);
        return fresh != null && fresh.enabled && fresh.applicationInfo != null
                && fresh.applicationInfo.enabled && componentAllowed(chain.sender,
                        fresh.packageName, fresh.exported, fresh.permission);
    }

    /** Guarded by this. All external work is returned as an action and runs without the lock. */
    private Runnable nextOrderedAction() {
        for (OrderedBroadcast chain : new ArrayList<>(mOrdered)) {
            if (chain.finalizing) continue;
            if (!senderCurrent(chain)) chain.expired = true;
            if (chain.expired || chain.aborted) {
                if (chain.active != null && !chain.active.dispatched) completeOrdered(chain.active);
                chain.next = chain.targets.size();
            }
            while (chain.active == null && chain.next < chain.targets.size()) {
                OrderedTarget target = chain.targets.get(chain.next++);
                if (!targetCurrent(chain, target)) continue;
                chain.active = new OrderedDelivery(chain, target, ++mNextReservation);
            }
            if (chain.active == null) {
                chain.finalizing = true;
                return () -> finishOrdered(chain);
            }
            OrderedDelivery delivery = chain.active;
            if (delivery.dispatched) continue; // only finish/death/timeout may release an acknowledged slot
            if (!targetCurrent(chain, delivery.target)) {
                completeOrdered(delivery);
                return () -> { }; // continue draining without recursive dispatch
            }
            if (delivery.app == null || !delivery.app.ready) {
                if (delivery.starting) continue;
                delivery.starting = true;
                armOrderedTimeout(delivery);
                return () -> startOrderedProcess(delivery);
            }
            if (delivery.app.exiting || mTimedOut.contains(delivery.app)
                    || mAtm.callerProcess(delivery.app.pid) != delivery.app) continue;
            if (mOrderedActive.containsKey(delivery.app) || legacyActive(delivery.app)
                    || firstOrdered(delivery.app) != delivery
                    || olderLegacy(delivery.app, delivery.sequence)) continue;
            delivery.dispatched = true;
            mOrderedActive.put(delivery.app, delivery);
            armOrderedTimeout(delivery);
            return () -> dispatchOrdered(delivery);
        }
        return null;
    }

    /** A trampoline permits immediate finish/reentrant sends without growing the Java stack. */
    private void pumpOrdered() {
        synchronized (this) {
            if (mOrderedPumping) return;
            mOrderedPumping = true;
        }
        boolean released = false;
        try {
            while (true) {
                Runnable action;
                synchronized (this) {
                    action = nextOrderedAction();
                    if (action == null) { mOrderedPumping = false; released = true; return; }
                }
                long identity = Binder.clearCallingIdentity();
                try { action.run(); }
                finally { Binder.restoreCallingIdentity(identity); }
            }
        } finally {
            if (!released) synchronized (this) { mOrderedPumping = false; }
        }
    }

    private void startOrderedProcess(OrderedDelivery delivery) {
        try {
            if (!senderCurrent(delivery.chain)) {
                synchronized (this) { completeOrdered(delivery); }
                pumpOrdered();
                return;
            }
            ActivityInfo info = delivery.target.info;
            String pkg = delivery.target.packageName();
            String processName = info != null ? info.processName : delivery.target.registered.app.processName;
            mAtm.withProcess(pkg, processName,
                    "acknowledged broadcast to " + pkg, app -> {
                        synchronized (BroadcastController.this) {
                            if (delivery.finished || delivery.chain.active != delivery) return;
                            if (!senderCurrent(delivery.chain) || app.exiting || mTimedOut.contains(app)
                                    || delivery.target.registered != null
                                        && delivery.target.registered.app != app
                                    || !targetCurrent(delivery.chain, delivery.target)) {
                                completeOrdered(delivery);
                            } else {
                                delivery.app = app;
                                if (delivery.timeout != null) delivery.timeout.cancel(false);
                                delivery.timeout = null;
                                ++delivery.timeoutGeneration;
                            }
                        }
                        pumpOrdered();
                        maybeExitCurrent(app);
                    });
        } catch (RuntimeException failed) {
            synchronized (this) { completeOrdered(delivery); }
            SystemServer.log("acknowledged broadcast process unavailable: " + failed.getClass().getSimpleName());
            pumpOrdered();
        }
    }

    private void dispatchOrdered(OrderedDelivery delivery) {
        OrderedBroadcast chain = delivery.chain;
        ActivityTaskManagerService.ProcessRecord app = delivery.app;
        Intent intent = new Intent(chain.intent);
        Bundle extras;
        int code;
        String data;
        synchronized (this) {
            if (delivery.finished || chain.active != delivery) return;
            if (!senderCurrent(chain) || !targetCurrent(chain, delivery.target)) {
                completeOrdered(delivery);
                return;
            }
            code = chain.resultCode; data = chain.resultData;
            extras = resultExtras(chain.resultExtras);
        }
        try {
            synchronized (this) {
                if (!senderCurrent(chain) || !targetCurrent(chain, delivery.target)) {
                    completeOrdered(delivery);
                    return;
                }
            }
            if (delivery.target.registered != null) {
                app.thread.scheduleRegisteredReceiver(delivery.target.registered.receiver, intent,
                        code, data, extras, chain.ordered, false, false /* must acknowledge */,
                        0, ActivityManager.PROCESS_STATE_RECEIVER, chain.sender.uid, chain.sender.packageName);
            } else {
                ActivityInfo info = delivery.target.info;
                intent.setComponent(new android.content.ComponentName(info.packageName, info.name));
                app.thread.scheduleReceiver(intent, info, CompatibilityInfo.DEFAULT_COMPATIBILITY_INFO,
                        code, data, extras, chain.ordered, false /* must acknowledge */, 0,
                        ActivityManager.PROCESS_STATE_RECEIVER, chain.sender.uid, chain.sender.packageName);
            }
        } catch (RemoteException | RuntimeException failed) {
            boolean retire = false;
            synchronized (this) {
                if (!delivery.finished) {
                    mTimedOut.add(app);
                    completeOrdered(delivery);
                    retire = true;
                }
            }
            SystemServer.log("acknowledged broadcast delivery failed: " + failed.getClass().getSimpleName());
            if (retire) retireTimedOut(app);
            if (!retire) { scheduleNext(app); maybeExitCurrent(app); }
        }
    }

    /** Guarded by this. Completion is idempotent for timeout/death/dispatch races. */
    private void completeOrdered(OrderedDelivery delivery) {
        if (delivery.finished) return;
        delivery.finished = true;
        ++delivery.timeoutGeneration;
        if (delivery.timeout != null) delivery.timeout.cancel(false);
        delivery.timeout = null;
        if (delivery.app != null) mOrderedActive.remove(delivery.app, delivery);
        if (delivery.chain.active == delivery) delivery.chain.active = null;
    }

    /** Guarded by this. A cold process and every active acknowledged receiver are bounded. */
    private void armOrderedTimeout(OrderedDelivery delivery) {
        if (delivery.timeout != null) delivery.timeout.cancel(false);
        long generation = ++delivery.timeoutGeneration;
        delivery.timeout = mTimeouts.schedule(() -> expireOrderedDelivery(delivery, generation),
                mReceiverTimeoutMillis, TimeUnit.MILLISECONDS);
    }

    private void retireTimedOut(ActivityTaskManagerService.ProcessRecord app) {
        if (app == null) return;
        synchronized (this) {
            mTimedOut.add(app);
            for (ManifestDelivery legacy : new ArrayList<>(mManifest)) {
                if (legacy.app == app) removeDelivery(legacy);
            }
            for (OrderedBroadcast chain : mOrdered) {
                if (chain.active != null && chain.active.app == app) completeOrdered(chain.active);
            }
        }
        mAtm.receiverTimedOut(app);
    }

    private void expireOrderedDelivery(OrderedDelivery delivery, long generation) {
        ActivityTaskManagerService.ProcessRecord retire;
        synchronized (this) {
            if (delivery.finished || delivery.chain.active != delivery
                    || delivery.timeoutGeneration != generation) return;
            retire = delivery.dispatched ? delivery.app : null;
            if (retire != null) mTimedOut.add(retire);
            completeOrdered(delivery);
        }
        retireTimedOut(retire); // no late app-thread completion can consume a newer generation
        pumpOrdered();
        if (retire == null && delivery.app != null) {
            scheduleNext(delivery.app); maybeExitCurrent(delivery.app);
        }
    }

    private void expireOrdered(OrderedBroadcast chain) {
        ActivityTaskManagerService.ProcessRecord retire = null;
        synchronized (this) {
            if (!mOrdered.contains(chain) || chain.finalizing) return;
            chain.expired = true;
            if (chain.active != null) {
                if (chain.active.dispatched) retire = chain.active.app;
                if (retire != null) mTimedOut.add(retire);
                completeOrdered(chain.active);
            }
        }
        retireTimedOut(retire);
        pumpOrdered();
    }

    private void finishOrdered(OrderedBroadcast chain) {
        IIntentReceiver result;
        boolean valid;
        synchronized (this) { result = chain.resultTo; valid = senderCurrent(chain); }
        try {
            if (result != null && valid && senderCurrent(chain)) {
                if (chain.senderApp != null) {
                    // AOSP's final callback is not itself ordered and needs no finish acknowledgement.
                    chain.senderApp.thread.scheduleRegisteredReceiver(result, new Intent(chain.intent),
                            chain.resultCode, chain.resultData, resultExtras(chain.resultExtras), false,
                            false, true, 0, ActivityManager.PROCESS_STATE_RECEIVER,
                            chain.sender.uid, chain.sender.packageName);
                } else {
                    // Only trusted local/host senders may omit an attached application thread.
                    result.performReceive(new Intent(chain.intent), chain.resultCode, chain.resultData,
                            resultExtras(chain.resultExtras), false, false, 0);
                }
            }
        } catch (RemoteException | RuntimeException failed) {
            SystemServer.log("broadcast final result unavailable: " + failed.getClass().getSimpleName());
        } finally {
            synchronized (this) {
                chain.resultTo = null;
                if (chain.deadline != null) chain.deadline.cancel(false);
                mOrdered.remove(chain);
            }
            maybeExitCurrent(chain.senderApp);
        }
    }

    private void maybeExitCurrent(ActivityTaskManagerService.ProcessRecord app) {
        if (app != null && mAtm.callerProcess(app.pid) == app) mAtm.maybeExit(app);
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
        mTimeouts.setRemoveOnCancelPolicy(true);
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
                rl = new ReceiverList(receiver, app, sender.packageName, mPm.get(sender.packageName));
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
        pumpOrdered();
    }

    int broadcastIntent(Intent intent, String resolvedType, String callingPackage,
            int callingPid) {
        return broadcastIntent(intent, resolvedType, callingPackage, callingPid, null, null, null);
    }

    int broadcastIntent(Intent intent, String resolvedType, String callingPackage,
            int callingPid, String[] requiredPermissions, String[] excludePermissions,
            String[] excludePackages) {
        return broadcastIntent(intent, resolvedType, callingPackage, callingPid,
                requiredPermissions, excludePermissions, excludePackages, () -> true);
    }

    int broadcastIntent(Intent intent, String resolvedType, String callingPackage,
            int callingPid, String[] requiredPermissions, String[] excludePermissions,
            String[] excludePackages, BooleanSupplier lifetime) {
        if (!lifetime.getAsBoolean()) return ActivityManager.START_CANCELED;
        if (intent == null) throw new IllegalArgumentException("Missing broadcast intent");
        return broadcastIntent(intent, resolvedType, sender(callingPackage, callingPid, lifetime),
                requiredPermissions, excludePermissions, excludePackages);
    }

    private int broadcastIntent(Intent intent, String resolvedType, Sender sender,
            String[] requiredPermissions, String[] excludePermissions, String[] excludePackages) {
        if (intent == null) throw new IllegalArgumentException("Missing broadcast intent");
        intent = new Intent(intent);
        validateBroadcastSender(sender, intent);
        String callingPackage = sender.packageName;
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
        List<ManifestDelivery> manifest = new ArrayList<>();
        if (intent.getComponent() != null || intent.getPackage() != null
                || "com.google.android.c2dm.intent.RECEIVE".equals(intent.getAction())
                || "com.google.android.c2dm.intent.REGISTRATION".equals(intent.getAction())) {
            synchronized (mPm) {
            for (ResolveInfo ri : mPm.receivers(intent, resolvedType,
                    PackageManager.GET_META_DATA | PackageManager.GET_SHARED_LIBRARY_FILES)) {
                ActivityInfo info = ri.activityInfo;
                ApkPackage owner = mPm.get(info.packageName);
                if (owner != null && componentAllowed(sender, info.packageName, info.exported, info.permission)
                        && receiverPermissions(info.packageName, requiredPermissions,
                                excludePermissions, excludePackages)) {
                    manifest.add(new ManifestDelivery(info, intent, owner, sender));
                }
            }
            }
        }
        SystemServer.log("broadcastIntent " + intent.toShortString(false, true, true, false)
                + " from " + callingPackage + ": " + registered.size() + " registered, "
                + manifest.size() + " manifest receiver(s)");
        int sendingUid = sender.uid;
        final String sendingPackage = sender.packageName;
        Map<String, List<ManifestDelivery>> batches = new LinkedHashMap<>();
        synchronized (this) {
            if (!sender.acceptance.getAsBoolean()) return ActivityManager.START_CANCELED;
            // Reserve the whole batch before registered callbacks or withProcess can run
            // synchronously. finishReceiver for a fast first receiver must see the rest.
            for (ManifestDelivery delivery : manifest) {
                if (!deliveryCurrent(delivery)) continue;
                ActivityInfo info = delivery.info;
                delivery.sequence = ++mNextReservation;
                mManifest.add(delivery);
                armTimeout(delivery);
                batches.computeIfAbsent(info.packageName + "\n"
                        + mAtm.componentProcess(info.packageName, info.processName),
                        ignored -> new ArrayList<>()).add(delivery);
            }
        }
        for (ReceiverList rl : registered) {
            if (!sender.lifetime.getAsBoolean()) break;
            if (mPm.get(rl.packageName) != rl.owner || mPm.isRemovalPending(rl.packageName)) continue;
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
            if (!sender.lifetime.getAsBoolean()) {
                synchronized (this) {
                    for (List<ManifestDelivery> stale : batches.values()) {
                        for (ManifestDelivery delivery : stale) removeDelivery(delivery);
                    }
                }
                return ActivityManager.START_CANCELED;
            }
            long identity = Binder.clearCallingIdentity();
            try {
                ActivityInfo target = batch.getValue().get(0).info;
                mAtm.withProcess(target.packageName, target.processName,
                        "broadcast to " + target.packageName, app -> {
                    synchronized (BroadcastController.this) {
                        for (ManifestDelivery delivery : batch.getValue()) {
                            if (!mManifest.contains(delivery)) continue;
                            if (app.exiting || mTimedOut.contains(app) || !deliveryCurrent(delivery)) {
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
        return sender(claimed, pid, () -> true);
    }

    private Sender sender(String claimed, int pid, BooleanSupplier delegatedLifetime) {
        final int originalPid = Binder.getCallingPid(), uid = Binder.getCallingUid();
        final ActivityTaskManagerService.ProcessRecord app;
        final String originalName;
        final ApkPackage originalCode;
        synchronized (mAtm) {
            app = mAtm.callerProcess(originalPid);
            originalName = app == null ? claimed : app.packageName;
            originalCode = mPm.get(originalName);
        }
        Runnable beforeAuthentication = mBeforeSenderAuthentication;
        if (beforeAuthentication != null) beforeAuthentication.run();
        if (mCallers.isSystem()) {
            // PendingIntent/job dispatch on behalf of an app keeps that app's
            // permissions even when the trusted server cleared transport identity.
            ApkPackage owner = claimed == null ? null : mPm.get(claimed);
            if (owner != null) return new Sender(owner.packageName, owner.uid(), false,
                    () -> delegatedLifetime.getAsBoolean() && mPm.get(owner.packageName) == owner
                            && !mPm.isRemovalPending(owner.packageName));
            if (claimed != null && !"android".equals(claimed)) {
                throw new SecurityException("Unknown broadcast sender package");
            }
            return new Sender("android", Process.SYSTEM_UID, true,
                    () -> delegatedLifetime.getAsBoolean() && !mPm.isRemovalPending("android"));
        }
        if (pid != originalPid) throw new SecurityException("Forged broadcast sender PID");
        String actual = mCallers.requirePackage(claimed);
        if (app == null || !actual.equals(originalName) || app.uid != uid
                || originalCode == null || originalCode.uid() != uid) {
            throw new SecurityException("Broadcast caller generation is unrecognized");
        }
        BooleanSupplier lifetime = () -> delegatedLifetime.getAsBoolean()
                && mPm.get(actual) == originalCode && !mPm.isRemovalPending(actual);
        // Caller liveness gates acceptance; accepted nonordered receiver work keeps
        // only package authority, independent of normal exit or later PID reuse.
        BooleanSupplier acceptance = () -> lifetime.getAsBoolean()
                && mAtm.callerProcess(originalPid) == app && !app.exiting;
        if (!acceptance.getAsBoolean()) throw new SecurityException("Broadcast caller retired during authentication");
        return new Sender(actual, uid, false, lifetime, acceptance);
    }

    private boolean componentAllowed(Sender sender, String owner, boolean exported, String permission) {
        if (!sender.lifetime.getAsBoolean()) return false;
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
            OrderedDelivery ordered = mOrderedActive.get(app);
            if (ordered != null) {
                if (ordered.chain.ordered) {
                    if (resultData != null && resultData.length() > 65536) {
                        throw new IllegalArgumentException("Broadcast result data too large");
                    }
                    Bundle extras = resultExtras(map);
                    ordered.chain.resultCode = resultCode;
                    ordered.chain.resultData = resultData;
                    ordered.chain.resultExtras = extras;
                    ordered.chain.aborted = abortBroadcast && (ordered.chain.intent.getFlags()
                            & Intent.FLAG_RECEIVER_NO_ABORT) == 0;
                }
                completeOrdered(ordered);
            } else {
            // ActivityThread uses its application-thread Binder for every manifest receiver.
            // Only one may be dispatched per process, so an async finish is unambiguous.
            for (ManifestDelivery delivery : mManifest) {
                if (delivery.app == app && delivery.dispatched) {
                    removeDelivery(delivery);
                    break;
                }
            }
            }
        }
        long identity = Binder.clearCallingIdentity();
        try {
            pumpOrdered();
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
        for (OrderedBroadcast chain : mOrdered) {
            if (chain.senderApp == app) return true;
            if (chain.active != null && chain.active.app == app) return true;
            for (int i = chain.next; i < chain.targets.size(); i++) {
                OrderedTarget target = chain.targets.get(i);
                if (target.registered != null ? target.registered.app == app
                        : target.packageName().equals(app.packageName)
                            && mAtm.componentProcess(target.info.packageName, target.info.processName)
                                    .equals(app.processName)) return true;
            }
            if (chain.active != null && chain.active.app == null && chain.active.target.info != null
                    && chain.active.target.packageName().equals(app.packageName)
                    && mAtm.componentProcess(chain.active.target.info.packageName,
                            chain.active.target.info.processName).equals(app.processName)) return true;
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
        pumpOrdered();
    }

    private void scheduleNext(ActivityTaskManagerService.ProcessRecord app) {
        while (true) {
            ManifestDelivery next = null;
            synchronized (this) {
                if (app.exiting || mTimedOut.contains(app) || mAtm.callerProcess(app.pid) != app) return;
                if (mOrderedActive.containsKey(app)) return;
                for (ManifestDelivery delivery : mManifest) {
                    if (delivery.app == app && delivery.dispatched) return;
                    if (next == null && delivery.app == app) next = delivery;
                }
                if (next == null) return;
                OrderedDelivery waiting = firstOrdered(app);
                if (waiting != null && waiting.sequence < next.sequence) return;
                if (!deliveryCurrent(next)) {
                    removeDelivery(next);
                    continue;
                }
                next.dispatched = true;
                armTimeout(next);
            }
            SystemServer.log("scheduleReceiver " + next.info.packageName + "/" + next.info.name
                    + " in " + app);
            try {
                if (!deliveryCurrent(next)) {
                    synchronized (this) { removeDelivery(next); }
                    continue;
                }
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

    private boolean deliveryCurrent(ManifestDelivery delivery) {
        return delivery.sender.lifetime.getAsBoolean()
                && delivery.owner == mPm.get(delivery.info.packageName)
                && !mPm.isRemovalPending(delivery.info.packageName);
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
            for (OrderedBroadcast chain : mOrdered) {
                if (chain.active != null && chain.active.app == app) completeOrdered(chain.active);
                if (chain.senderApp == app) chain.expired = true;
            }
        }
        pumpOrdered();
    }
}
