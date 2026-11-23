/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.server;

import android.os.Binder;
import android.os.IBinder;
import android.os.Parcel;
import android.os.ParcelFileDescriptor;
import android.os.Process;
import android.os.RemoteException;
import android.os.ServiceManager;
import android.os.SystemClock;

import java.io.IOException;
import java.util.List;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;

/** Host-only rendezvous for the single policy owner of this Android data root. */
public final class CohostOwnerService extends Binder {
    private static final String NAME = "gesher.cohost";
    private static final String DESCRIPTOR = "com.gesher.ICohostOwner";
    private static final int PROMOTE = IBinder.FIRST_CALL_TRANSACTION;
    private static final int RUN = PROMOTE + 1;
    private final CountDownLatch mPromotion = new CountDownLatch(1);
    private final AtomicInteger mActive = new AtomicInteger();
    private volatile long mLastUse = SystemClock.uptimeMillis();
    private final PackageManagerService mPm;
    private final PermissionRequests mPermissions;
    private final String mVm;

    CohostOwnerService(PackageManagerService pm, PermissionRequests permissions, String vm) {
        super(DESCRIPTOR);
        mPm = pm;
        mPermissions = permissions;
        mVm = vm;
    }

    void publish() {
        ServiceManager.addService(NAME, this);
    }

    boolean awaitPromotion() throws InterruptedException {
        return mPromotion.await(1, TimeUnit.SECONDS);
    }

    boolean hasActiveLaunch() { return mActive.get() != 0; }
    long lastUse() { return mLastUse; }

    @Override protected boolean onTransact(int code, Parcel input, Parcel output, int flags)
            throws RemoteException {
        if (code != PROMOTE && code != RUN) return super.onTransact(code, input, output, flags);
        input.enforceInterface(DESCRIPTOR);
        // A protected broker supplies the exact shell role from the caller's
        // Windows process. A guest never gets this UID from its environment.
        if (Binder.getCallingUid() != Process.SHELL_UID) {
            throw new SecurityException("Only the authenticated host may publish the device");
        }
        if (code == PROMOTE) {
            input.enforceNoDataAvail();
            mPromotion.countDown();
            output.writeNoException();
            return true;
        }
        String apk = input.readString();
        String data = input.readString();
        String[] options = input.createStringArray();
        IBinder owner = input.readStrongBinder();
        int status;
        try (ParcelFileDescriptor in = input.readTypedObject(ParcelFileDescriptor.CREATOR);
             ParcelFileDescriptor out = input.readTypedObject(ParcelFileDescriptor.CREATOR);
             ParcelFileDescriptor err = input.readTypedObject(ParcelFileDescriptor.CREATOR)) {
            input.enforceNoDataAvail();
            if (apk == null || options == null || owner == null || in == null || out == null || err == null) {
                throw new IllegalArgumentException("Incomplete standalone application request");
            }
            status = run(apk, data, options, owner, new int[]{in.getFd(), out.getFd(), err.getFd()});
        } catch (RuntimeException | RemoteException failure) {
            throw failure;
        } catch (Exception failure) {
            throw new IllegalStateException("Standalone application launch failed", failure);
        }
        output.writeNoException();
        output.writeInt(status);
        return true;
    }

    private int run(String apk, String data, String[] options, IBinder owner, int[] streams)
            throws Exception {
        mActive.incrementAndGet();
        try (CohostPermissionHost host = new CohostPermissionHost(mPm, mPermissions, apk)) {
            IBinder.DeathRecipient disconnected = host::cancel;
            owner.linkToDeath(disconnected, 0);
            try {
                return host.runApplication(mVm, data, List.of(options), streams);
            } finally {
                try { owner.unlinkToDeath(disconnected, 0); }
                catch (java.util.NoSuchElementException alreadyDisconnected) { }
            }
        } finally {
            mLastUse = SystemClock.uptimeMillis();
            mActive.decrementAndGet();
        }
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

    public static int launch(String apk, String data, List<String> options)
            throws IOException, RemoteException {
        IBinder service = ServiceManager.checkService(NAME);
        if (service == null) throw new IllegalStateException("No protected policy owner is ready");
        int[] stdio = Zygote.captureCohostStreams();
        try (ParcelFileDescriptor in = ParcelFileDescriptor.adoptFd(stdio[0]);
             ParcelFileDescriptor out = ParcelFileDescriptor.adoptFd(stdio[1]);
             ParcelFileDescriptor err = ParcelFileDescriptor.adoptFd(stdio[2])) {
            Parcel input = Parcel.obtain();
            Parcel output = Parcel.obtain();
            try {
                input.writeInterfaceToken(DESCRIPTOR);
                input.writeString(apk);
                input.writeString(data);
                input.writeStringArray(options.toArray(String[]::new));
                input.writeStrongBinder(new Binder());
                input.writeTypedObject(in, 0);
                input.writeTypedObject(out, 0);
                input.writeTypedObject(err, 0);
                if (!service.transact(RUN, input, output, 0)) {
                    throw new IllegalStateException("The protected policy owner declined the launch");
                }
                output.readException();
                return output.readInt();
            } finally {
                output.recycle();
                input.recycle();
            }
        } finally {
            Zygote.finishScopedStandardStreams();
        }
    }
}
