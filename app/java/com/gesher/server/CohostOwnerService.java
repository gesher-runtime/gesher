/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.server;

import android.os.Binder;
import android.os.IBinder;
import android.os.Parcel;
import android.os.Process;
import android.os.RemoteException;
import android.os.ServiceManager;

import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;

/** Host-only rendezvous for the single policy owner of this Android data root. */
public final class CohostOwnerService extends Binder {
    private static final String NAME = "gesher.cohost";
    private static final String DESCRIPTOR = "com.gesher.ICohostOwner";
    private static final int PROMOTE = IBinder.FIRST_CALL_TRANSACTION;
    private final CountDownLatch mPromotion = new CountDownLatch(1);

    CohostOwnerService() {
        super(DESCRIPTOR);
    }

    void publish() {
        ServiceManager.addService(NAME, this);
    }

    boolean awaitPromotion() throws InterruptedException {
        return mPromotion.await(1, TimeUnit.SECONDS);
    }

    @Override protected boolean onTransact(int code, Parcel input, Parcel output, int flags)
            throws RemoteException {
        if (code != PROMOTE) return super.onTransact(code, input, output, flags);
        input.enforceInterface(DESCRIPTOR);
        // A protected broker supplies the exact shell role from the caller's
        // Windows process. A guest never gets this UID from its environment.
        if (Binder.getCallingUid() != Process.SHELL_UID) {
            throw new SecurityException("Only the authenticated host may publish the device");
        }
        input.enforceNoDataAvail();
        mPromotion.countDown();
        output.writeNoException();
        return true;
    }

    public static void promote() throws RemoteException {
        IBinder service = ServiceManager.checkService(NAME);
        if (service == null) throw new IllegalStateException("No protected policy owner is ready");
        Parcel input = Parcel.obtain();
        Parcel output = Parcel.obtain();
        try {
            input.writeInterfaceToken(DESCRIPTOR);
            if (!service.transact(PROMOTE, input, output, 0)) {
                throw new IllegalStateException("The protected policy owner declined promotion");
            }
            output.readException();
        } finally {
            output.recycle();
            input.recycle();
        }
    }
}
