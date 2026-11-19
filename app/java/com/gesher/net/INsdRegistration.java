/* Copyright (C) 2026 The Gesher Authors. SPDX-License-Identifier: Apache-2.0 */
package com.gesher.net;

import android.net.nsd.AdvertisingRequest;
import android.net.nsd.NsdServiceInfo;
import android.os.Binder;
import android.os.IBinder;
import android.os.IInterface;
import android.os.Parcel;
import android.os.RemoteException;

/** Private Binder protocol between NsdManager and the actual system-service owner. */
public interface INsdRegistration extends IInterface {
    String SERVICE_NAME = "gesher.nsd.registration";
    String DESCRIPTOR = "com.gesher.net.INsdRegistration";
    int REGISTERED = 1, FAILED = 2, UNREGISTERED = 3, UNREGISTER_FAILED = 4;
    /** Private ServiceSpecificException code; never an Android NSD API error value. */
    int ERROR_CLIENT_UNAVAILABLE = 1;

    void register(IClient client, int requestId, AdvertisingRequest request,
            int otherActiveRequests) throws RemoteException;
    void unregister(IClient client, int requestId) throws RemoteException;

    interface IClient extends IInterface {
        String DESCRIPTOR = "com.gesher.net.INsdRegistration$IClient";
        void event(int requestId, int event, int error, boolean retained,
                NsdServiceInfo info) throws RemoteException;

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
                if (code == INTERFACE_TRANSACTION) {
                    reply.writeString(DESCRIPTOR); return true;
                }
                if (code != FIRST_CALL_TRANSACTION) return super.onTransact(code, data, reply, flags);
                data.enforceInterface(DESCRIPTOR);
                int id = data.readInt(), event = data.readInt(), error = data.readInt();
                boolean retained = data.readBoolean();
                NsdServiceInfo info = data.readTypedObject(NsdServiceInfo.CREATOR);
                data.enforceNoDataAvail();
                event(id, event, error, retained, info);
                return true;
            }

            private static final class ClientProxy implements IClient {
                private final IBinder remote;
                ClientProxy(IBinder remote) { this.remote = remote; }
                public IBinder asBinder() { return remote; }
                public void event(int id, int event, int error, boolean retained,
                        NsdServiceInfo info) throws RemoteException {
                    Parcel data = Parcel.obtain();
                    try {
                        data.writeInterfaceToken(DESCRIPTOR);
                        data.writeInt(id); data.writeInt(event); data.writeInt(error);
                        data.writeBoolean(retained); data.writeTypedObject(info, 0);
                        if (!remote.transact(FIRST_CALL_TRANSACTION, data, null, FLAG_ONEWAY)) {
                            throw new RemoteException("NSD client callback is unavailable");
                        }
                    } finally { data.recycle(); }
                }
            }
        }
    }

    abstract class Stub extends Binder implements INsdRegistration {
        public Stub() { attachInterface(this, DESCRIPTOR); }
        public static INsdRegistration asInterface(IBinder binder) {
            if (binder == null) return null;
            IInterface local = binder.queryLocalInterface(DESCRIPTOR);
            return local instanceof INsdRegistration ? (INsdRegistration) local : new Proxy(binder);
        }
        @Override public IBinder asBinder() { return this; }
        @Override protected boolean onTransact(int code, Parcel data, Parcel reply, int flags)
                throws RemoteException {
            if (code == INTERFACE_TRANSACTION) {
                reply.writeString(DESCRIPTOR); return true;
            }
            if (code != FIRST_CALL_TRANSACTION && code != FIRST_CALL_TRANSACTION + 1) {
                return super.onTransact(code, data, reply, flags);
            }
            data.enforceInterface(DESCRIPTOR);
            IClient client = IClient.Stub.asInterface(data.readStrongBinder());
            int id = data.readInt();
            if (code == FIRST_CALL_TRANSACTION) {
                AdvertisingRequest request = data.readTypedObject(AdvertisingRequest.CREATOR);
                int other = data.readInt();
                data.enforceNoDataAvail();
                register(client, id, request, other);
            } else {
                data.enforceNoDataAvail();
                unregister(client, id);
            }
            reply.writeNoException();
            return true;
        }

        private static final class Proxy implements INsdRegistration {
            private final IBinder remote;
            Proxy(IBinder remote) { this.remote = remote; }
            public IBinder asBinder() { return remote; }
            public void register(IClient client, int id, AdvertisingRequest request, int other)
                    throws RemoteException {
                Parcel data = Parcel.obtain(), reply = Parcel.obtain();
                try {
                    data.writeInterfaceToken(DESCRIPTOR); data.writeStrongInterface(client);
                    data.writeInt(id); data.writeTypedObject(request, 0); data.writeInt(other);
                    if (!remote.transact(FIRST_CALL_TRANSACTION, data, reply, 0)) {
                        throw new RemoteException("NSD registration service is unavailable");
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
                        throw new RemoteException("NSD registration service is unavailable");
                    }
                    reply.readException();
                } finally { reply.recycle(); data.recycle(); }
            }
        }
    }
}
