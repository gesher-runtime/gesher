/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package com.gesher.recordapp;

import android.app.Service;
import android.content.Intent;
import android.os.Binder;
import android.os.IBinder;
import android.os.Parcel;
import android.os.Process;
import android.os.RemoteException;
import android.view.Surface;

/** Private explicit producer fixture: the ordinary Surface parcel crosses Binder. */
public final class PersistentInputService extends Service {
    static final String DESCRIPTOR = "com.gesher.recordapp.PersistentInputService";
    static final int OPEN = IBinder.FIRST_CALL_TRANSACTION, DRAW = OPEN + 1, CLOSE = OPEN + 2;
    private final Object lock = new Object();
    private Surface surface;
    private PersistentInputRenderer renderer;
    private final Binder binder = new Binder() {
        @Override protected boolean onTransact(int code, Parcel data, Parcel reply, int flags)
                throws RemoteException {
            if (code < OPEN || code > CLOSE) return super.onTransact(code, data, reply, flags);
            data.enforceInterface(DESCRIPTOR);
            synchronized (lock) {
                try {
                    if (code == OPEN) {
                        PersistentInputActivity.require(renderer == null, "producer may only be opened once");
                        surface = Surface.CREATOR.createFromParcel(data);
                        // A Surface parcel intentionally recreates a producer
                        // reference, not MediaCodec's private consumer token.
                        try { renderer = new PersistentInputRenderer(surface, PersistentInputActivity.SIZE); }
                        catch (Throwable failure) { dispose(); throw failure; }
                    } else if (code == DRAW) {
                        PersistentInputActivity.require(renderer != null, "producer has no EGL connection");
                        renderer.draw(data.readInt());
                    } else dispose();
                    reply.writeNoException(); reply.writeInt(Process.myPid());
                } catch (Throwable failure) {
                    failure.printStackTrace();
                    reply.writeException(new IllegalStateException("persistent EGL producer failed", failure));
                }
            }
            return true;
        }
    };
    private void dispose() {
        try { if (renderer != null) renderer.close(); }
        finally {
            renderer = null;
            if (surface != null) { surface.release(); surface = null; }
        }
    }
    @Override public IBinder onBind(Intent intent) { return binder; }
    @Override public void onDestroy() {
        synchronized (lock) { dispose(); }
        super.onDestroy();
    }
}
