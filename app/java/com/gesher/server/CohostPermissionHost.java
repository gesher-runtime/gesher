/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.server;

import android.content.pm.PackageManager;

import java.util.concurrent.CompletableFuture;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.TimeoutException;

/** Permission checks and host consent for one admitted standalone code and live process. */
final class CohostPermissionHost implements AutoCloseable {
    static final int CHECK = 1;
    static final int REQUEST = 2;
    static final int FLAGS = 3;
    static final int CAN_REQUEST = 4;
    static final int APP_OP_MODE = 5;

    private final PermissionPolicy.ScopedOwner mOwner;
    private final PermissionRequests mRequests;
    private volatile long mLease;

    CohostPermissionHost(PackageManagerService pm, PermissionRequests requests, String apk)
            throws Exception {
        mRequests = requests;
        mOwner = pm.permissions().openScopedOwner(apk, this::isLive);
    }

    ApkPackage code() { return mOwner.code(); }
    Zygote.StandaloneInput input() { return mOwner.input(); }

    // The native launcher binds only the kernel process it actually created,
    // after admission and before ResumeThread. No guest supplies this value.
    private synchronized void bindNativeLease(long lease) {
        if (lease <= 0 || mLease != 0) throw new IllegalStateException("Already bound");
        mLease = lease;
    }

    private boolean isLive() {
        long lease = mLease;
        return lease != 0 && Zygote.nativeCohostPermissionLive(lease);
    }

    /** Invoked only by the pinned Windows transport. */
    private int[] dispatch(int operation, String[] names) throws Exception {
        if (!mOwner.isCurrent()) throw new SecurityException("Standalone permission owner retired");
        switch (operation) {
            case CHECK: return new int[] {mOwner.check(names[0])};
            case FLAGS: return new int[] {mOwner.flags(names[0])};
            case CAN_REQUEST: return new int[] {mOwner.canRequest(names[0]) ? 1 : 0};
            case APP_OP_MODE: return new int[] {mOwner.mode(names[0])};
            case REQUEST: {
                CompletableFuture<int[]> reply = new CompletableFuture<>();
                mRequests.requestScoped(mOwner, names, intent -> reply.complete(intent.getIntArrayExtra(
                        PackageManager.EXTRA_REQUEST_PERMISSIONS_RESULTS)));
                while (mOwner.isCurrent()) {
                    try {
                        return reply.get(100, TimeUnit.MILLISECONDS);
                    } catch (TimeoutException pending) {
                        // PermissionRequests keeps the same native default-No
                        // consent policy as an installed app.
                    }
                }
                throw new SecurityException("Standalone permission owner retired");
            }
            default: throw new IllegalArgumentException("Invalid standalone permission operation");
        }
    }

    @Override public void close() {
        mOwner.close();
    }
}
