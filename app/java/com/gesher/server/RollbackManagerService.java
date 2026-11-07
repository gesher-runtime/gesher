/* Copyright (C) 2026 The Gesher Authors. SPDX-License-Identifier: Apache-2.0 */
package com.gesher.server;

import android.Manifest;
import android.content.IIntentSender;
import android.content.Intent;
import android.content.IntentSender;
import android.content.pm.PackageManager;
import android.content.pm.ParceledListSlice;
import android.content.rollback.IRollbackManager;
import android.content.rollback.RollbackInfo;
import android.content.rollback.RollbackManager;
import android.os.RemoteException;

import java.util.Collections;
import java.util.Objects;
import java.util.function.Supplier;

/**
 * No Android rollback snapshots exist on this desktop. Authorized history queries
 * are empty; committing any ID reports the platform's rollback-unavailable result.
 * Snapshot, restore, staging and test administration are explicitly unsupported.
 * Registration does not advertise rollback support or grant any permission.
 */
final class RollbackManagerService extends IRollbackManager.Default {
    private final PackageManagerService mPm;
    private final CallerIdentity mCallers;
    private final Supplier<String> mTestCaller;

    RollbackManagerService(PackageManagerService pm, ActivityTaskManagerService atm) {
        this(pm, atm, null);
    }

    /** Server-local test injection, never exposed through Binder or app configuration. */
    RollbackManagerService(PackageManagerService pm, ActivityTaskManagerService atm,
            Supplier<String> testCaller) {
        mPm = pm;
        mCallers = new CallerIdentity(pm, atm);
        mTestCaller = testCaller;
    }

    private static final class Caller {
        final String packageName;
        final ApkPackage code;
        final boolean host;

        Caller(String packageName, ApkPackage code, boolean host) {
            this.packageName = packageName;
            this.code = code;
            this.host = host;
        }
    }

    private Caller caller() {
        if (mTestCaller == null && mCallers.isSystem()) return new Caller("android", null, true);
        String name = mTestCaller == null ? mCallers.callerPackage() : mTestCaller.get();
        ApkPackage code = mPm.get(name);
        if (code == null || (mTestCaller == null && code.uid() != mCallers.callerUid())) {
            throw new SecurityException("Unrecognized rollback caller");
        }
        return new Caller(name, code, false);
    }

    private boolean granted(Caller caller, String permission) {
        return caller.host || mPm.permissions().checkPermission(caller.packageName, permission)
                == PackageManager.PERMISSION_GRANTED;
    }

    private Caller manage() {
        Caller caller = caller();
        if (!granted(caller, Manifest.permission.MANAGE_ROLLBACKS)
                && !granted(caller, Manifest.permission.TEST_MANAGE_ROLLBACKS)) {
            throw new SecurityException("Rollback access requires MANAGE_ROLLBACKS or TEST_MANAGE_ROLLBACKS");
        }
        current(caller);
        return caller;
    }

    private void test() {
        Caller caller = caller();
        if (!granted(caller, Manifest.permission.TEST_MANAGE_ROLLBACKS)) {
            throw new SecurityException("Rollback administration requires TEST_MANAGE_ROLLBACKS");
        }
        current(caller);
    }

    private void host() {
        if (!caller().host) throw new SecurityException("Rollback lifecycle requires the host");
    }

    private void current(Caller caller) {
        if (!caller.host && mPm.get(caller.packageName) != caller.code) {
            throw new SecurityException("Rollback caller code changed");
        }
    }

    @Override public ParceledListSlice<RollbackInfo> getAvailableRollbacks() {
        manage();
        return new ParceledListSlice<>(Collections.emptyList());
    }

    @Override public ParceledListSlice<RollbackInfo> getRecentlyCommittedRollbacks() {
        manage();
        return new ParceledListSlice<>(Collections.emptyList());
    }

    @Override public void commitRollback(int rollbackId, ParceledListSlice causePackages,
            String callerPackageName, IntentSender statusReceiver) {
        Caller caller = manage();
        if (callerPackageName == null || (!caller.host
                && !caller.packageName.equals(callerPackageName))) {
            throw new SecurityException("Rollback caller package does not match its identity");
        }
        ApkPackage claimedCode = mPm.get(callerPackageName);
        if (claimedCode == null && !(caller.host && "android".equals(callerPackageName))) {
            throw new SecurityException("Unknown rollback caller package");
        }
        Objects.requireNonNull(causePackages, "causePackages");
        IIntentSender target = Objects.requireNonNull(
                Objects.requireNonNull(statusReceiver, "statusReceiver").getTarget(), "status target");
        Intent result = new Intent()
                .putExtra(RollbackManager.EXTRA_STATUS,
                        RollbackManager.STATUS_FAILURE_ROLLBACK_UNAVAILABLE)
                .putExtra(RollbackManager.EXTRA_STATUS_MESSAGE, "Rollback unavailable");
        current(caller);
        if (mPm.get(callerPackageName) != claimedCode) {
            throw new SecurityException("Rollback caller code changed");
        }
        // IntentSender is a capability. Preserve the calling Binder identity and
        // send only this fixed failure; never dispatch as an elevated server caller.
        try { target.send(0, result, null, null, null, null, null); }
        catch (RemoteException ignored) {
            // As in the platform, a disappeared result receiver cannot be notified.
        }
    }

    @Override public void snapshotAndRestoreUserData(String packageName, int[] userIds,
            int appId, long ceDataInode, String seInfo, int token) {
        host();
    }

    @Override public int notifyStagedSession(int sessionId) {
        host();
        return -1;
    }

    @Override public void reloadPersistedData() { test(); }
    @Override public void expireRollbackForPackage(String packageName) { test(); }
    @Override public void blockRollbackManager(long millis) { test(); }
}
