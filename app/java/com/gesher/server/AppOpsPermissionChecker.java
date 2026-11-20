/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.server;

import android.Manifest;
import android.app.AppOpsManager;
import android.app.SyncNotedAppOp;
import android.content.AttributionSourceState;
import android.content.pm.PackageManager;
import android.content.pm.PermissionInfo;
import android.os.IBinder;
import android.os.RemoteException;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.HashSet;
import java.util.List;
import java.util.Map;
import java.util.Objects;
import java.util.Set;
import java.util.concurrent.atomic.AtomicInteger;
import static android.app.AppOpsManager.*;
import static android.content.PermissionChecker.*;

/** PermissionChecker's runtime/app-op checks and balanced, trusted attribution delivery. */
final class AppOpsPermissionChecker extends android.permission.IPermissionChecker.Default {
    private final PackageManagerService mPm;
    private final AppOpsService mOps;
    private final Object mLock = new Object();
    private final Map<IBinder, List<Delivery>> mDeliveries = new HashMap<>();
    private final AtomicInteger mChainIds = new AtomicInteger();

    AppOpsPermissionChecker(PackageManagerService pm) {
        mPm = pm; mOps = pm.appOps();
        pm.onPackageRemoval(name -> retireDeliveries(name, null));
        pm.onPackageChange(pkg -> retireDeliveries(pkg.packageName, pkg));
    }

    private void retireDeliveries(String name, ApkPackage current) {
        List<Delivery> stale = new ArrayList<>();
        synchronized (mLock) {
            for (List<Delivery> deliveries : mDeliveries.values()) for (Delivery delivery : deliveries) {
                if (delivery.owners.containsKey(name) && delivery.owners.get(name) != current) stale.add(delivery);
            }
        }
        for (Delivery delivery : stale) delivery.finish();
    }

    private record Access(int op, AttributionSourceState source) { }
    private final class Delivery implements IBinder.DeathRecipient {
        final int op;
        final boolean fromDatasource;
        final AttributionSourceState source;
        final Map<String, ApkPackage> owners = new HashMap<>();
        final List<Access> accesses = new ArrayList<>();
        final Set<IBinder> linked = new HashSet<>();
        boolean ended;
        Delivery(int op, AttributionSourceState source, boolean fromDatasource) {
            this.op = op; this.source = AppOpsAttribution.copy(source, true); this.fromDatasource = fromDatasource;
            for (AttributionSourceState node : AppOpsAttribution.chain(this.source)) {
                owners.put(node.packageName, mPm.get(node.packageName));
            }
        }
        boolean link() {
            for (AttributionSourceState node : AppOpsAttribution.chain(source)) {
                synchronized (this) {
                    if (ended) return false;
                    if (node.token == null || linked.contains(node.token)) continue;
                }
                try { node.token.linkToDeath(this, 0); }
                catch (RemoteException dead) { finish(); return false; }
                synchronized (this) {
                    if (ended) { node.token.unlinkToDeath(this, 0); return false; }
                    linked.add(node.token);
                }
            }
            return true;
        }
        @Override public void binderDied() { finish(); }
        synchronized boolean add(int op, AttributionSourceState node) {
            if (ended) { mOps.finishAttributed(source.token, op, node); return false; }
            accesses.add(new Access(op, AppOpsAttribution.copy(node, true))); return true;
        }
        void finish() {
            List<Access> started;
            Set<IBinder> tokens;
            synchronized (this) {
                if (ended) return;
                ended = true; started = new ArrayList<>(accesses); accesses.clear();
                tokens = new HashSet<>(linked); linked.clear();
            }
            synchronized (mLock) {
                List<Delivery> records = mDeliveries.get(source.token);
                if (records != null) {
                    records.remove(this); if (records.isEmpty()) mDeliveries.remove(source.token);
                }
            }
            for (int i = started.size() - 1; i >= 0; i--) {
                Access access = started.get(i); mOps.finishAttributed(source.token, access.op, access.source);
            }
            for (IBinder token : tokens) token.unlinkToDeath(this, 0);
        }
    }

    @Override public int checkPermission(String permission, AttributionSourceState source, String message,
            boolean forDelivery, boolean startDelivery, boolean fromDatasource, int attributedOp) {
        Objects.requireNonNull(permission); Objects.requireNonNull(source);
        PermissionInfo info = mPm.permissions().getPermissionInfo(permission, null, 0);
        if (info == null) return PERMISSION_HARD_DENIED;
        return check(permission, info, AppOpsManager.permissionToOpCode(permission), source, message,
                forDelivery, startDelivery, fromDatasource, attributedOp);
    }
    @Override public int checkOp(int op, AttributionSourceState source, String message, boolean forDelivery, boolean startDelivery) {
        if (op < 0 || source == null || source.packageName == null) return PERMISSION_HARD_DENIED;
        AppOpsService.validateOp(op);
        return check(null, null, op, source, message, forDelivery, startDelivery, false, OP_NONE);
    }

    private boolean permission(String permission, AttributionSourceState node) {
        if (mPm.permissions().checkUidPermission(node.uid, permission, node.deviceId) != PackageManager.PERMISSION_GRANTED) return false;
        if (node.renouncedPermissions != null && mPm.permissions().checkUidPermission(node.uid,
                Manifest.permission.RENOUNCE_PERMISSIONS, node.deviceId) == PackageManager.PERMISSION_GRANTED) {
            for (String renounced : node.renouncedPermissions) if (permission.equals(renounced)) return false;
        }
        return true;
    }
    private int check(String permission, PermissionInfo info, int op, AttributionSourceState supplied, String message,
            boolean forDelivery, boolean startDelivery, boolean fromDatasource, int attributedOp) {
        AttributionSourceState source = AppOpsAttribution.copy(supplied, true);
        List<AttributionSourceState> chain = AppOpsAttribution.chain(source);
        boolean appOpPermission = info != null && info.isAppOp();
        boolean runtime = info != null && info.isRuntime();
        // An app-op permission has no usable authorization path without its mapped op.
        // Runtime modifier permissions without an op still follow the permission check.
        if (appOpPermission && op == OP_NONE) return PERMISSION_HARD_DENIED;
        for (AttributionSourceState node : chain) {
            if (node.packageName == null) {
                String[] packages = mPm.getPackagesForUid(node.uid);
                if (packages != null && packages.length > 0) node.packageName = packages[0];
                else if (node.uid == android.os.Process.SYSTEM_UID) node.packageName = "android";
                else return PERMISSION_HARD_DENIED;
            }
            if (mOps.checkPackage(node.uid, node.packageName) != MODE_ALLOWED) return PERMISSION_HARD_DENIED;
        }
        // A preflight permission query does not blame a process or start a resource.
        // Delivery authentication is additionally enforced by the note/start transaction.
        if (forDelivery && (permission == null || appOpPermission || runtime)) {
            try { mOps.verifyAttributionSource(source); }
            catch (SecurityException invalid) { return PERMISSION_HARD_DENIED; }
        }
        for (int i = 0; i < chain.size(); i++) {
            AttributionSourceState node = chain.get(i);
            if ((permission == null || appOpPermission || runtime) && i + 1 < chain.size() && !(fromDatasource && i == 0)
                    && !mPm.permissions().isRegisteredAttributionSource(node)) return PERMISSION_HARD_DENIED;
            if (permission != null && !appOpPermission && !(fromDatasource && i == 0)
                    && !permission(permission, node)) return PERMISSION_HARD_DENIED;
        }
        if (info != null && !appOpPermission && !runtime || op == OP_NONE) return PERMISSION_GRANTED;
        // No virtual-device backend exists. Reject before linking any delivery token, creating
        // accounting, or dispatching observer work, including the proxy side of a chain.
        for (AttributionSourceState node : chain) if (node.deviceId != 0) {
            return appOpPermission ? PERMISSION_HARD_DENIED : PERMISSION_SOFT_DENIED;
        }
        if (attributedOp != OP_NONE) AppOpsService.validateOp(attributedOp);
        int blamedOp = attributedOp == OP_NONE ? op : attributedOp;
        boolean start = forDelivery && startDelivery && !appOpPermission;
        if (start && source.token == null) return PERMISSION_HARD_DENIED;
        int chainId = start && chain.size() > 1 ? mChainIds.updateAndGet(value -> value == Integer.MAX_VALUE ? 1 : value + 1) : ATTRIBUTION_CHAIN_ID_NONE;
        boolean hasChain = chainId != ATTRIBUTION_CHAIN_ID_NONE;
        boolean trustedStart = !hasChain || permission(Manifest.permission.UPDATE_APP_OPS_STATS, source);
        Delivery delivery = start ? new Delivery(blamedOp, source, fromDatasource) : null;
        if (delivery != null && !delivery.link()) return PERMISSION_HARD_DENIED;
        boolean success = false;
        try {
            for (int index = 0; index < chain.size(); index++) {
                AttributionSourceState current = chain.get(index);
                boolean singleReceiver = fromDatasource && index == 0 && chain.size() == 2;
                boolean self = singleReceiver || index == chain.size() - 1;
                AttributionSourceState accessor = singleReceiver ? chain.get(1) : current;
                boolean skip = fromDatasource || index > 0;
                boolean trustedLink = hasChain && trustedStart
                        && (index == 0 || mPm.permissions().isRegisteredAttributionSource(current))
                        && (index + 1 == chain.size()
                                || mPm.permissions().isRegisteredAttributionSource(chain.get(index + 1)));
                int proxyFlags = hasChain && !skip
                        ? attributionFlags(index, chain.size(), fromDatasource, self, true, trustedLink)
                        : ATTRIBUTION_FLAGS_NONE;
                int proxiedFlags = hasChain
                        ? attributionFlags(index + 1, chain.size(), fromDatasource, self, false, trustedLink)
                        : ATTRIBUTION_FLAGS_NONE;
                int mode = transaction(op, blamedOp, source.token, current, accessor, self, singleReceiver,
                        skip, message, forDelivery, start, proxyFlags, proxiedFlags, chainId, delivery);
                if (appOpPermission) {
                    if (mode == MODE_ERRORED || mode == MODE_IGNORED) return PERMISSION_HARD_DENIED;
                    if (mode == MODE_DEFAULT) {
                        if (!skip && !permission(permission, current)) return PERMISSION_HARD_DENIED;
                        if (index + 1 < chain.size() && !permission(permission, chain.get(index + 1))) return PERMISSION_HARD_DENIED;
                    }
                } else {
                    if (mode == MODE_ERRORED) return PERMISSION_HARD_DENIED;
                    if (mode == MODE_IGNORED) return PERMISSION_SOFT_DENIED;
                }
                if (self || index + 2 >= chain.size()) break;
            }
            if (delivery != null) {
                synchronized (delivery) {
                    if (delivery.ended) return PERMISSION_HARD_DENIED;
                    synchronized (mLock) {
                        if (delivery.owners.entrySet().stream().anyMatch(owner ->
                                mPm.get(owner.getKey()) != owner.getValue() || mPm.isRemovalPending(owner.getKey()))) {
                            return PERMISSION_HARD_DENIED;
                        }
                        mDeliveries.computeIfAbsent(source.token, ignored -> new ArrayList<>()).add(delivery);
                    }
                }
            }
            success = true;
            return PERMISSION_GRANTED;
        } finally { if (!success && delivery != null) delivery.finish(); }
    }

    /** AOSP PermissionChecker assigns roles per link, excluding a datasource itself. */
    private static int attributionFlags(int index, int size, boolean fromDatasource,
            boolean self, boolean proxy, boolean trusted) {
        if (index >= size) return ATTRIBUTION_FLAGS_NONE;
        int flags = trusted ? ATTRIBUTION_FLAG_TRUSTED : ATTRIBUTION_FLAGS_NONE;
        if (proxy) {
            if (self || !fromDatasource && index == 0) return flags | ATTRIBUTION_FLAG_ACCESSOR;
        } else {
            if (self) return flags | ATTRIBUTION_FLAG_RECEIVER;
            if (fromDatasource && index == 1) return flags | ATTRIBUTION_FLAG_ACCESSOR;
            if (index + 1 == size) return flags | ATTRIBUTION_FLAG_RECEIVER;
        }
        if (fromDatasource && index == 0) return ATTRIBUTION_FLAGS_NONE;
        return flags | ATTRIBUTION_FLAG_INTERMEDIARY;
    }

    private int transaction(int permissionOp, int blamedOp, IBinder token, AttributionSourceState current,
            AttributionSourceState accessor, boolean self, boolean singleReceiver, boolean skip, String message,
            boolean forDelivery, boolean start, int proxyFlags, int proxiedFlags, int chainId, Delivery delivery) {
        if (!forDelivery) {
            int mode = skip && !singleReceiver ? MODE_ALLOWED : mOps.checkOperationRawForDevice(permissionOp,
                    accessor.uid, accessor.packageName, accessor.attributionTag, accessor.deviceId);
            if (!self && mode == MODE_ALLOWED && accessor.next != null && accessor.next.length == 1) {
                AttributionSourceState next = accessor.next[0];
                mode = mOps.checkOperationRawForDevice(permissionOp, next.uid, next.packageName, next.attributionTag, next.deviceId);
            }
            return mode;
        }
        int checked = blamedOp == permissionOp ? MODE_ALLOWED : mOps.checkOperationForDevice(permissionOp,
                accessor.uid, accessor.packageName, accessor.attributionTag, accessor.deviceId);
        if (checked == MODE_ERRORED) return checked;
        SyncNotedAppOp result;
        boolean proxy = !self;
        try {
            if (self) {
                result = start ? mOps.startOperationForDevice(token, blamedOp, accessor.uid, accessor.packageName,
                        accessor.attributionTag, accessor.deviceId, false, false, message, false, proxyFlags, chainId)
                        : mOps.noteOperationForDevice(blamedOp, accessor.uid, accessor.packageName,
                                accessor.attributionTag, accessor.deviceId, false, message, false);
            } else {
                result = start ? mOps.startProxyOperationWithState(token, blamedOp, accessor, false, false, message,
                        false, skip, proxyFlags, proxiedFlags, chainId)
                        : mOps.noteProxyOperationWithState(blamedOp, accessor, false, message, false, skip);
            }
        } catch (SecurityException notTrustedDatasource) {
            if (!singleReceiver) throw notTrustedDatasource;
            proxy = true; accessor = current;
            result = start ? mOps.startProxyOperationWithState(token, blamedOp, current, false, false, message,
                    false, skip, proxyFlags, proxiedFlags, chainId)
                    : mOps.noteProxyOperationWithState(blamedOp, current, false, message, false, skip);
        }
        if (start && result.getOpMode() == MODE_ALLOWED) {
            if (!proxy) delivery.add(blamedOp, accessor);
            else {
                if (!(skip && mOps.canSkipProxy(accessor))) delivery.add(blamedOp, accessor);
                delivery.add(blamedOp, accessor.next[0]);
            }
        }
        return Math.max(checked, result.getOpMode());
    }

    @Override public void finishDataDelivery(int op, AttributionSourceState source, boolean fromDatasource) {
        if (op == OP_NONE) return;
        AppOpsService.validateOp(op); Objects.requireNonNull(source);
        mOps.verifyAttributionSource(source);
        Delivery found = null;
        synchronized (mLock) {
            List<Delivery> deliveries = mDeliveries.get(source.token);
            if (deliveries != null) for (int i = deliveries.size() - 1; i >= 0; i--) {
                Delivery candidate = deliveries.get(i);
                if (candidate.op == op && candidate.fromDatasource == fromDatasource
                        && AppOpsAttribution.equalExceptRootToken(candidate.source, source)) { found = candidate; break; }
            }
        }
        if (found != null) found.finish();
    }
}
