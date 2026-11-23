/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.app.window;

import android.app.AppOpsManager;
import android.os.Binder;
import android.os.IBinder;
import android.os.RemoteException;

import com.android.internal.app.IAppOpsService;
import com.gesher.server.AlertWindowPolicy;

/** Client-side AppOps lifetime of one admitted alert window. */
final class AlertWindowOperation implements AutoCloseable {
    static final int OP = AppOpsManager.OP_SYSTEM_ALERT_WINDOW;
    private final IBinder mToken = new Binder();
    private final IAppOpsService mOps;
    private final AlertWindowPolicy.Owner mOwner;
    private volatile boolean mVisible;

    AlertWindowOperation(IAppOpsService ops, AlertWindowPolicy.Owner owner) {
        mOps = ops;
        mOwner = owner;
    }

    boolean visible() {
        return mVisible;
    }

    /** On the window's Looper. Reconcile current policy rather than trust an event's age. */
    void refresh() {
        try {
            int mode = mVisible
                    ? mOps.checkOperation(OP, mOwner.uid(), mOwner.packageName())
                    : mOps.startOperation(mToken, OP, mOwner.uid(), mOwner.packageName(), null,
                            true, false, "alert-window", false, 0, -1).getOpMode();
            if (mode == AppOpsManager.MODE_ALLOWED || mode == AppOpsManager.MODE_DEFAULT) {
                mVisible = true;
                return;
            }
        } catch (RemoteException | SecurityException unavailable) {
            // A lost policy authority cannot leave an overlay visible.
        }
        close();
    }

    @Override public void close() {
        if (!mVisible) return;
        mVisible = false;
        try {
            mOps.finishOperation(mToken, OP, mOwner.uid(), mOwner.packageName(), null);
        } catch (RemoteException | SecurityException unavailable) {
            // The remote owner also retires operations on token/process loss.
        }
    }
}
