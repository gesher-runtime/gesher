/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.app.window;

import android.os.IBinder;
import android.os.IInterface;
import android.os.Parcel;
import android.os.RemoteException;

/**
 * The desktop X's conditional close of another owned AppTask. The task service knows
 * which tasks the user minimized, including those in another process. This operation is
 * narrower than Android's ordinary, unconditional AppTask.finishAndRemoveTask.
 */
public final class AppWindowTasks {
    private AppWindowTasks() { }

    public static final String DESCRIPTOR = "com.gesher.IAppWindowTask";
    /** Separate descriptor and extension code, outside IAppTask's generated methods. */
    public static final int FINISH_NON_MINIMIZED = IBinder.FIRST_CALL_TRANSACTION + 0x4700;

    /** Normal Binder local-interface path for a service in this process. */
    public interface Local extends IInterface {
        boolean finishFromAppWindow() throws RemoteException;
    }

    /** True only when the service committed the close; false leaves a minimized task. */
    public static boolean finishNonMinimized(IBinder task) throws RemoteException {
        IInterface local = task.queryLocalInterface(DESCRIPTOR);
        if (local instanceof Local windowTask) return windowTask.finishFromAppWindow();
        Parcel data = Parcel.obtain();
        Parcel reply = Parcel.obtain();
        try {
            data.writeInterfaceToken(DESCRIPTOR);
            if (!task.transact(FINISH_NON_MINIMIZED, data, reply, 0)) {
                throw new RemoteException("Conditional app-window task close is unavailable");
            }
            reply.readException();
            return reply.readBoolean();
        } finally {
            reply.recycle();
            data.recycle();
        }
    }
}
