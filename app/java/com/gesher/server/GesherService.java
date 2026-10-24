/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.server;

import android.os.Binder;
import android.os.IBinder;
import android.os.Parcel;
import android.os.RemoteException;
import android.os.ServiceManager;

/**
 * {@code gesher}: what the launcher asks of the device that Android asks of other services
 * (PackageInstaller sessions, {@code adb shell}): install and uninstall a package, a
 * package's log, the exit code of its last process, the device's status, shutting down.
 * A plain Binder with its own transaction codes, called by {@link Client} (Parcels written
 * by hand: no AIDL interface of the framework fits).
 */
public final class GesherService extends Binder {
    public static final String NAME = "gesher";
    public static final String DESCRIPTOR = "com.gesher.IGesher";

    static final int INSTALL = IBinder.FIRST_CALL_TRANSACTION;
    static final int UNINSTALL = IBinder.FIRST_CALL_TRANSACTION + 1;
    static final int STATUS = IBinder.FIRST_CALL_TRANSACTION + 2;
    static final int SHUTDOWN = IBinder.FIRST_CALL_TRANSACTION + 3;
    static final int LAST_EXIT = IBinder.FIRST_CALL_TRANSACTION + 4;
    /** The window mode of a package's next process (WindowManagerService.setWindowMode). */
    static final int WINDOW_MODE = IBinder.FIRST_CALL_TRANSACTION + 5;

    /** What the service does; the system server implements it. */
    interface Device {
        String[] install(String apk) throws Exception;
        boolean uninstall(String packageName) throws Exception;
        String status();
        void shutdown(String reason);
        /** The exit code of the package's last process, or Integer.MIN_VALUE. */
        int lastExitCode(String packageName);
        /** A call came: the device is not idle. */
        void touch();
    }

    private final Device mDevice;

    GesherService(Device device) {
        super(DESCRIPTOR);
        mDevice = device;
    }

    @Override
    protected boolean onTransact(int code, Parcel data, Parcel reply, int flags)
            throws RemoteException {
        if (code < INSTALL || code > WINDOW_MODE) {
            return super.onTransact(code, data, reply, flags);
        }
        data.enforceInterface(DESCRIPTOR);
        mDevice.touch();
        try {
            switch (code) {
                case INSTALL: {
                    String[] result = mDevice.install(data.readString());
                    reply.writeNoException();
                    reply.writeStringArray(result);
                    return true;
                }
                case UNINSTALL: {
                    boolean removed = mDevice.uninstall(data.readString());
                    reply.writeNoException();
                    reply.writeBoolean(removed);
                    return true;
                }
                case STATUS:
                    reply.writeNoException();
                    reply.writeString(mDevice.status());
                    return true;
                case SHUTDOWN:
                    mDevice.shutdown(data.readString());
                    reply.writeNoException();
                    return true;
                case LAST_EXIT:
                    int exit = mDevice.lastExitCode(data.readString());
                    reply.writeNoException();
                    reply.writeInt(exit);
                    return true;
                case WINDOW_MODE: {
                    String pkg = data.readString();
                    WindowManagerService.setWindowMode(pkg, data.readString());
                    reply.writeNoException();
                    return true;
                }
                default:
                    return false;
            }
        } catch (Exception e) {
            SystemServer.log("gesher service: transaction " + code + " failed: " + e);
            reply.setDataPosition(0);
            reply.writeException(e instanceof RuntimeException ? e
                    : new IllegalStateException(e.toString(), e));
            return true;
        }
    }

    /** The launcher's side. */
    public static final class Client {
        private final IBinder mBinder;

        private Client(IBinder binder) {
            mBinder = binder;
        }

        /** The device's service, or null if it is not registered. */
        public static Client find() {
            IBinder b = ServiceManager.checkService(NAME);
            return b == null ? null : new Client(b);
        }

        private Parcel call(int code, String arg) throws RemoteException {
            Parcel data = Parcel.obtain();
            Parcel reply = Parcel.obtain();
            try {
                data.writeInterfaceToken(DESCRIPTOR);
                if (arg != null) data.writeString(arg);
                mBinder.transact(code, data, reply, 0);
                reply.readException();
                return reply;
            } catch (RuntimeException | RemoteException e) {
                reply.recycle();
                throw e;
            } finally {
                data.recycle();
            }
        }

        /** Installs or updates: package, what was done, code path, version code. */
        public String[] install(String apk) throws RemoteException {
            Parcel reply = call(INSTALL, apk);
            try {
                return reply.createStringArray();
            } finally {
                reply.recycle();
            }
        }

        public boolean uninstall(String packageName) throws RemoteException {
            Parcel reply = call(UNINSTALL, packageName);
            try {
                return reply.readBoolean();
            } finally {
                reply.recycle();
            }
        }

        public String status() throws RemoteException {
            Parcel reply = call(STATUS, null);
            try {
                return reply.readString();
            } finally {
                reply.recycle();
            }
        }

        public void shutdown(String reason) throws RemoteException {
            call(SHUTDOWN, reason).recycle();
        }

        /**
         * The window mode ({@code window}, {@code hidden}, {@code headless}) of the
         * package's processes the system server starts from now on.
         */
        public void setWindowMode(String packageName, String mode) throws RemoteException {
            Parcel data = Parcel.obtain();
            Parcel reply = Parcel.obtain();
            try {
                data.writeInterfaceToken(DESCRIPTOR);
                data.writeString(packageName);
                data.writeString(mode);
                mBinder.transact(WINDOW_MODE, data, reply, 0);
                reply.readException();
            } finally {
                data.recycle();
                reply.recycle();
            }
        }

        public int lastExitCode(String packageName) throws RemoteException {
            Parcel reply = call(LAST_EXIT, packageName);
            try {
                return reply.readInt();
            } finally {
                reply.recycle();
            }
        }
    }
}
