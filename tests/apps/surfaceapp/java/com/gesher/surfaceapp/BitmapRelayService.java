/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package com.gesher.surfaceapp;

import android.app.Service;
import android.content.Intent;
import android.graphics.Bitmap;
import android.os.Binder;
import android.os.IBinder;
import android.os.Parcel;
import android.os.Process;
import android.os.RemoteException;

/** Runs in :bitmapRelay; holds the receiver's own native Bitmap across calls. */
public final class BitmapRelayService extends Service {
    private final Object lock = new Object();
    private Bitmap held;
    private final Binder binder = new Binder() {
        @Override protected boolean onTransact(int code, Parcel data, Parcel reply, int flags)
                throws RemoteException {
            if (code < BitmapSharedContract.STORE || code > BitmapSharedContract.EMPTY)
                return super.onTransact(code, data, reply, flags);
            data.enforceInterface(BitmapSharedContract.DESCRIPTOR);
            synchronized (lock) {
                if (code == BitmapSharedContract.STORE) {
                    Bitmap incoming = Bitmap.CREATOR.createFromParcel(data);
                    try {
                        BitmapSharedContract.require(held == null, "previous bitmap must have retired");
                        BitmapSharedContract.verify(incoming);
                        held = incoming;
                    } finally { if (incoming != null && held != incoming) incoming.recycle(); }
                    reply.writeNoException(); reply.writeInt(Process.myPid());
                } else if (code == BitmapSharedContract.FORWARD) {
                    // This call is made only after the original sender recycled
                    // its Bitmap and its outgoing Parcel.
                    BitmapSharedContract.verify(held);
                    reply.writeNoException(); reply.writeInt(Process.myPid());
                    held.writeToParcel(reply, 0);
                    BitmapSharedContract.require(reply.hasFileDescriptors(), "forwarded reply must own its descriptor");
                    held.recycle(); held = null;
                } else {
                    BitmapSharedContract.require(held == null, "relay released its Bitmap before reply was received");
                    reply.writeNoException(); reply.writeInt(Process.myPid());
                }
            }
            return true;
        }
    };
    @Override public IBinder onBind(Intent intent) { return binder; }
    @Override public void onDestroy() {
        synchronized (lock) { if (held != null) { held.recycle(); held = null; } }
        super.onDestroy();
    }
}
