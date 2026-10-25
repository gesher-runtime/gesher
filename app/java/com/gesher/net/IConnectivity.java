/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.net;

import android.os.Binder;
import android.os.IBinder;
import android.os.IInterface;
import android.os.Parcel;
import android.os.RemoteException;

/**
 * The connectivity service's binder interface ({@code connectivity}; docs/network.md),
 * written as AIDL would generate it (gesher-app has no AIDL step): Windows' networks as a
 * {@link WindowsNetworks} snapshot, and listeners called with each new one. The service is
 * {@code com.gesher.server.ConnectivityService}; android.net.ConnectivityManager is its
 * client. Callers need ACCESS_NETWORK_STATE, as on Android.
 */
public interface IConnectivity extends IInterface {
    String DESCRIPTOR = "com.gesher.net.IConnectivity";

    /** The current snapshot. */
    String getSnapshot() throws RemoteException;

    /** Calls {@code listener} with every new snapshot, until it dies or is removed. */
    void addListener(IListener listener) throws RemoteException;

    void removeListener(IListener listener) throws RemoteException;

    /** A listener of snapshots, called one way. */
    interface IListener extends IInterface {
        String DESCRIPTOR = "com.gesher.net.IConnectivity$IListener";

        void onSnapshot(String snapshot) throws RemoteException;

        abstract class Stub extends Binder implements IListener {
            static final int TRANSACTION_onSnapshot = IBinder.FIRST_CALL_TRANSACTION;

            public Stub() {
                attachInterface(this, DESCRIPTOR);
            }

            public static IListener asInterface(IBinder b) {
                if (b == null) return null;
                IInterface local = b.queryLocalInterface(DESCRIPTOR);
                if (local instanceof IListener) return (IListener) local;
                return new Proxy(b);
            }

            @Override
            public IBinder asBinder() {
                return this;
            }

            @Override
            protected boolean onTransact(int code, Parcel data, Parcel reply, int flags)
                    throws RemoteException {
                if (code == TRANSACTION_onSnapshot) {
                    data.enforceInterface(DESCRIPTOR);
                    onSnapshot(data.readString());
                    return true;
                }
                return super.onTransact(code, data, reply, flags);
            }

            private static final class Proxy implements IListener {
                private final IBinder mRemote;

                Proxy(IBinder remote) {
                    mRemote = remote;
                }

                @Override
                public IBinder asBinder() {
                    return mRemote;
                }

                @Override
                public void onSnapshot(String snapshot) throws RemoteException {
                    Parcel data = Parcel.obtain();
                    try {
                        data.writeInterfaceToken(DESCRIPTOR);
                        data.writeString(snapshot);
                        mRemote.transact(TRANSACTION_onSnapshot, data, null, IBinder.FLAG_ONEWAY);
                    } finally {
                        data.recycle();
                    }
                }
            }
        }
    }

    abstract class Stub extends Binder implements IConnectivity {
        static final int TRANSACTION_getSnapshot = IBinder.FIRST_CALL_TRANSACTION;
        static final int TRANSACTION_addListener = IBinder.FIRST_CALL_TRANSACTION + 1;
        static final int TRANSACTION_removeListener = IBinder.FIRST_CALL_TRANSACTION + 2;

        public Stub() {
            attachInterface(this, DESCRIPTOR);
        }

        public static IConnectivity asInterface(IBinder b) {
            if (b == null) return null;
            IInterface local = b.queryLocalInterface(DESCRIPTOR);
            if (local instanceof IConnectivity) return (IConnectivity) local;
            return new Proxy(b);
        }

        @Override
        public IBinder asBinder() {
            return this;
        }

        @Override
        protected boolean onTransact(int code, Parcel data, Parcel reply, int flags)
                throws RemoteException {
            switch (code) {
                case TRANSACTION_getSnapshot: {
                    data.enforceInterface(DESCRIPTOR);
                    String s = getSnapshot();
                    reply.writeNoException();
                    reply.writeString(s);
                    return true;
                }
                case TRANSACTION_addListener: {
                    data.enforceInterface(DESCRIPTOR);
                    addListener(IListener.Stub.asInterface(data.readStrongBinder()));
                    reply.writeNoException();
                    return true;
                }
                case TRANSACTION_removeListener: {
                    data.enforceInterface(DESCRIPTOR);
                    removeListener(IListener.Stub.asInterface(data.readStrongBinder()));
                    reply.writeNoException();
                    return true;
                }
                default:
                    return super.onTransact(code, data, reply, flags);
            }
        }

        private static final class Proxy implements IConnectivity {
            private final IBinder mRemote;

            Proxy(IBinder remote) {
                mRemote = remote;
            }

            @Override
            public IBinder asBinder() {
                return mRemote;
            }

            @Override
            public String getSnapshot() throws RemoteException {
                Parcel data = Parcel.obtain();
                Parcel reply = Parcel.obtain();
                try {
                    data.writeInterfaceToken(DESCRIPTOR);
                    mRemote.transact(TRANSACTION_getSnapshot, data, reply, 0);
                    reply.readException();
                    return reply.readString();
                } finally {
                    reply.recycle();
                    data.recycle();
                }
            }

            @Override
            public void addListener(IListener listener) throws RemoteException {
                call(TRANSACTION_addListener, listener);
            }

            @Override
            public void removeListener(IListener listener) throws RemoteException {
                call(TRANSACTION_removeListener, listener);
            }

            private void call(int code, IListener listener) throws RemoteException {
                Parcel data = Parcel.obtain();
                Parcel reply = Parcel.obtain();
                try {
                    data.writeInterfaceToken(DESCRIPTOR);
                    data.writeStrongBinder(listener != null ? listener.asBinder() : null);
                    mRemote.transact(code, data, reply, 0);
                    reply.readException();
                } finally {
                    reply.recycle();
                    data.recycle();
                }
            }
        }
    }
}
