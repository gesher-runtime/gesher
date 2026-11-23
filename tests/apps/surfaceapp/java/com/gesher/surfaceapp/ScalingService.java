/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.surfaceapp;

import android.app.Service;
import android.content.Intent;
import android.os.Binder;
import android.os.IBinder;
import android.os.Parcel;
import android.os.Process;
import android.os.RemoteException;
import android.view.Surface;

/** Explicit private test service in :scaling, exercising real Surface Binder parcels. */
public final class ScalingService extends Service {
    static final String DESCRIPTOR = "com.gesher.surfaceapp.ScalingService";
    static final int OPEN = IBinder.FIRST_CALL_TRANSACTION;
    static final int RENDER = OPEN + 1, REPLACE = OPEN + 2, RESET = OPEN + 3, CLOSE = OPEN + 4;
    private final Object lock = new Object();
    private ScalingProducer producer;
    private Surface surface, texture;
    private final Binder binder = new Binder() {
        @Override protected boolean onTransact(int code, Parcel data, Parcel reply, int flags)
                throws RemoteException {
            if (code < OPEN || code > CLOSE) return super.onTransact(code, data, reply, flags);
            data.enforceInterface(DESCRIPTOR);
            synchronized (lock) {
                try {
                    if (code == OPEN) {
                        dispose();
                        surface = Surface.CREATOR.createFromParcel(data);
                        texture = Surface.CREATOR.createFromParcel(data);
                        try { producer = new ScalingProducer(surface, texture); }
                        catch (Throwable failure) { dispose(); throw failure; }
                    } else if (code == CLOSE) {
                        dispose();
                    } else {
                        ScalingProducer.require(producer != null, "service has no producer");
                        if (code == RENDER) producer.render(data.readInt());
                        else if (code == RESET) producer.resetDefaults();
                        else if (code == REPLACE) {
                            Surface replacement = Surface.CREATOR.createFromParcel(data);
                            try { producer.replaceTexture(replacement); }
                            catch (Throwable failure) { replacement.release(); throw failure; }
                            texture.release();
                            texture = replacement;
                        }
                    }
                    reply.writeNoException();
                    reply.writeInt(Process.myPid());
                } catch (Throwable failure) {
                    failure.printStackTrace();
                    reply.writeException(new IllegalStateException("scaling producer failed", failure));
                }
            }
            return true;
        }
    };

    private void dispose() {
        try {
            if (producer != null) producer.close();
        } finally {
            producer = null;
            if (surface != null) { surface.release(); surface = null; }
            if (texture != null) { texture.release(); texture = null; }
        }
    }

    @Override public IBinder onBind(Intent intent) { return binder; }

    @Override public void onDestroy() {
        synchronized (lock) { dispose(); }
        super.onDestroy();
    }
}
