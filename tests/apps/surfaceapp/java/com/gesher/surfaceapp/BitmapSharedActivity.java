/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package com.gesher.surfaceapp;

import android.app.Activity;
import android.content.ComponentName;
import android.content.Intent;
import android.content.ServiceConnection;
import android.graphics.Bitmap;
import android.os.Bundle;
import android.os.IBinder;
import android.os.Parcel;
import android.os.Process;
import android.util.Log;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;

/** Actual separate-process Binder/Skia storage acceptance, public SDK only. */
public final class BitmapSharedActivity extends Activity {
    @Override public void onCreate(Bundle state) {
        super.onCreate(state);
        new Thread(this::run, "bitmap-shared-test").start();
    }
    private void run() {
        final CountDownLatch connected = new CountDownLatch(1);
        final IBinder[] endpoint = new IBinder[1];
        ServiceConnection connection = new ServiceConnection() {
            @Override public void onServiceConnected(ComponentName name, IBinder binder) {
                endpoint[0] = binder; connected.countDown();
            }
            @Override public void onServiceDisconnected(ComponentName name) {}
        };
        boolean bound = false;
        try {
            bound = bindService(new Intent(this, BitmapRelayService.class), connection, BIND_AUTO_CREATE);
            BitmapSharedContract.require(bound && connected.await(30, TimeUnit.SECONDS), "separate relay connection");
            int remotePid = -1;
            for (int cycle = 0; cycle < 2; ++cycle) for (int[] size : new int[][] {{9, 7}, {641, 359}}) {
                Bitmap source = BitmapSharedContract.create(size[0], size[1]);
                Parcel data = Parcel.obtain(), reply = Parcel.obtain();
                try {
                    data.writeInterfaceToken(BitmapSharedContract.DESCRIPTOR);
                    source.writeToParcel(data, 0);
                    BitmapSharedContract.require(data.hasFileDescriptors(), "small and large shared Bitmap carry a descriptor");
                    BitmapSharedContract.require(endpoint[0].transact(BitmapSharedContract.STORE, data, reply, 0), "store handled");
                    reply.readException();
                    int observed = reply.readInt();
                    BitmapSharedContract.require(observed != Process.myPid() && (remotePid < 0 || observed == remotePid),
                            "real independent recipient process remains the same");
                    remotePid = observed;
                } finally { source.recycle(); data.recycle(); reply.recycle(); }

                Bitmap returned = null;
                try {
                    data = Parcel.obtain(); reply = Parcel.obtain();
                    try {
                        data.writeInterfaceToken(BitmapSharedContract.DESCRIPTOR);
                        BitmapSharedContract.require(endpoint[0].transact(BitmapSharedContract.FORWARD, data, reply, 0), "forward handled");
                        reply.readException();
                        BitmapSharedContract.require(reply.readInt() == remotePid, "same retained recipient");
                        returned = Bitmap.CREATOR.createFromParcel(reply);
                    } finally { data.recycle(); reply.recycle(); }
                    data = Parcel.obtain(); reply = Parcel.obtain();
                    try {
                        data.writeInterfaceToken(BitmapSharedContract.DESCRIPTOR);
                        BitmapSharedContract.require(endpoint[0].transact(BitmapSharedContract.EMPTY, data, reply, 0), "retirement handled");
                        reply.readException();
                        BitmapSharedContract.require(reply.readInt() == remotePid, "relay confirms its sender retired");
                    } finally { data.recycle(); reply.recycle(); }
                    BitmapSharedContract.require(returned.getWidth() == size[0] && returned.getHeight() == size[1], "returned dimensions");
                    BitmapSharedContract.verify(returned);
                    Log.i("BitmapShared", "shared bitmap: " + size[0] + "x" + size[1] + " cycle " + cycle
                            + " exact pixels after both senders recycled; parent pid " + Process.myPid()
                            + " relay pid " + remotePid);
                } finally { if (returned != null) returned.recycle(); }
            }
            Log.i("BitmapShared", "shared bitmap: PASS 8 Bitmap hops through separate Binder process; immutable identity, pixels, metadata and independent retirement verified");
        } catch (Throwable failure) {
            Log.e("BitmapShared", "shared bitmap: FAILED", failure);
        } finally {
            if (bound) unbindService(connection);
            runOnUiThread(this::finish);
        }
    }
}
