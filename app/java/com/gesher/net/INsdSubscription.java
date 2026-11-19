/* Copyright (C) 2026 The Gesher Authors. SPDX-License-Identifier: Apache-2.0 */
package com.gesher.net;

import android.net.nsd.NsdServiceInfo;
import android.os.Binder;
import android.os.IBinder;
import android.os.IInterface;
import android.os.Parcel;
import android.os.RemoteException;

/** Private service-owner protocol; READY is internal progress, never a public success callback. */
public interface INsdSubscription extends IInterface {
    String SERVICE_NAME = "gesher.nsd.subscription";
    String DESCRIPTOR = "com.gesher.net.INsdSubscription";
    int UPDATED = 1, LOST = 2, FAILED = 3, UNREGISTERED = 4, READY = 5;
    int ERROR_CLIENT_UNAVAILABLE = 1;

    void register(IClient client, int requestId, NsdServiceInfo selector) throws RemoteException;
    void unregister(IClient client, int requestId) throws RemoteException;

    interface IClient extends IInterface {
        String DESCRIPTOR = "com.gesher.net.INsdSubscription$IClient";
        void event(int requestId, int event, int error, long scopeId, NsdServiceInfo info)
                throws RemoteException;

        abstract class Stub extends Binder implements IClient {
            public Stub() { attachInterface(this, DESCRIPTOR); }
            public static IClient asInterface(IBinder binder) {
                if (binder == null) return null;
                IInterface local = binder.queryLocalInterface(DESCRIPTOR);
                return local instanceof IClient ? (IClient) local : new ClientProxy(binder);
            }
            @Override public IBinder asBinder() { return this; }
            @Override protected boolean onTransact(int code, Parcel data, Parcel reply, int flags)
                    throws RemoteException {
                if (code == INTERFACE_TRANSACTION) { reply.writeString(DESCRIPTOR); return true; }
                if (code != FIRST_CALL_TRANSACTION) return super.onTransact(code, data, reply, flags);
                data.enforceInterface(DESCRIPTOR);
                int id = data.readInt(), event = data.readInt(), error = data.readInt();
                long scope = data.readLong();
                NsdServiceInfo info = data.readTypedObject(NsdServiceInfo.CREATOR);
                data.enforceNoDataAvail();
                event(id, event, error, scope, info);
                return true;
            }
            private static final class ClientProxy implements IClient {
                private final IBinder remote;
                ClientProxy(IBinder remote) { this.remote = remote; }
                public IBinder asBinder() { return remote; }
                public void event(int id, int event, int error, long scope, NsdServiceInfo info)
                        throws RemoteException {
                    Parcel data = Parcel.obtain();
                    try {
                        data.writeInterfaceToken(DESCRIPTOR);
                        data.writeInt(id); data.writeInt(event); data.writeInt(error);
                        data.writeLong(scope); data.writeTypedObject(info, 0);
                        if (!remote.transact(FIRST_CALL_TRANSACTION, data, null, FLAG_ONEWAY)) {
                            throw new RemoteException("NSD subscription callback is unavailable");
                        }
                    } finally { data.recycle(); }
                }
            }
        }
    }

    abstract class Stub extends Binder implements INsdSubscription {
        public Stub() { attachInterface(this, DESCRIPTOR); }
        public static INsdSubscription asInterface(IBinder binder) {
            if (binder == null) return null;
            IInterface local = binder.queryLocalInterface(DESCRIPTOR);
            return local instanceof INsdSubscription ? (INsdSubscription) local : new Proxy(binder);
        }
        @Override public IBinder asBinder() { return this; }
        @Override protected boolean onTransact(int code, Parcel data, Parcel reply, int flags)
                throws RemoteException {
            if (code == INTERFACE_TRANSACTION) { reply.writeString(DESCRIPTOR); return true; }
            if (code != FIRST_CALL_TRANSACTION && code != FIRST_CALL_TRANSACTION + 1) {
                return super.onTransact(code, data, reply, flags);
            }
            data.enforceInterface(DESCRIPTOR);
            IClient client = IClient.Stub.asInterface(data.readStrongBinder());
            int id = data.readInt();
            if (code == FIRST_CALL_TRANSACTION) {
                NsdServiceInfo selector = data.readTypedObject(NsdServiceInfo.CREATOR);
                data.enforceNoDataAvail();
                register(client, id, selector);
            } else {
                data.enforceNoDataAvail();
                unregister(client, id);
            }
            reply.writeNoException();
            return true;
        }
        private static final class Proxy implements INsdSubscription {
            private final IBinder remote;
            Proxy(IBinder remote) { this.remote = remote; }
            public IBinder asBinder() { return remote; }
            public void register(IClient client, int id, NsdServiceInfo selector) throws RemoteException {
                Parcel data = Parcel.obtain(), reply = Parcel.obtain();
                try {
                    data.writeInterfaceToken(DESCRIPTOR); data.writeStrongInterface(client);
                    data.writeInt(id); data.writeTypedObject(selector, 0);
                    if (!remote.transact(FIRST_CALL_TRANSACTION, data, reply, 0)) {
                        throw new RemoteException("NSD subscription service is unavailable");
                    }
                    reply.readException();
                } finally { reply.recycle(); data.recycle(); }
            }
            public void unregister(IClient client, int id) throws RemoteException {
                Parcel data = Parcel.obtain(), reply = Parcel.obtain();
                try {
                    data.writeInterfaceToken(DESCRIPTOR); data.writeStrongInterface(client);
                    data.writeInt(id);
                    if (!remote.transact(FIRST_CALL_TRANSACTION + 1, data, reply, 0)) {
                        throw new RemoteException("NSD subscription service is unavailable");
                    }
                    reply.readException();
                } finally { reply.recycle(); data.recycle(); }
            }
        }
    }
}
