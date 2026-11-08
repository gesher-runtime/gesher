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
import android.app.WaitResult;
import android.content.Intent;

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
    /** A click on a toast, which Windows' toast activator gave a launcher (ADR 0051). */
    static final int TOAST_ACTIVATED = IBinder.FIRST_CALL_TRANSACTION + 6;
    /** The proxy of a package's next process (com.gesher.net.ProxySetting.setForLaunch). */
    static final int PROXY = IBinder.FIRST_CALL_TRANSACTION + 7;
    /** A host launch with an exact, retained process-generation exit receipt. */
    static final int START_ACTIVITY = IBinder.FIRST_CALL_TRANSACTION + 8;

    static final String EXIT_DESCRIPTOR = "com.gesher.ILaunchExit";
    static final int EXIT_STATUS = IBinder.FIRST_CALL_TRANSACTION;
    public static final int EXIT_STARTING = 0;
    public static final int EXIT_RUNNING = 1;
    public static final int EXIT_FINISHED = 2;
    public static final int EXIT_UNAVAILABLE = 3;

    /** The resolved launch; the receipt belongs to its actual process generation. */
    public static final class LaunchResult {
        public final WaitResult result;
        public final int taskId;
        public final IBinder exitReceipt;

        public LaunchResult(WaitResult result, int taskId, IBinder exitReceipt) {
            this.result = java.util.Objects.requireNonNull(result);
            this.taskId = taskId;
            this.exitReceipt = exitReceipt;
        }
    }

    /** Unknown status is distinct from every possible 32-bit Windows exit code. */
    public static final class LaunchExit {
        public final int state;
        public final boolean hasExitCode;
        public final int exitCode;

        LaunchExit(int state, boolean hasExitCode, int exitCode) {
            if (state < EXIT_STARTING || state > EXIT_UNAVAILABLE
                    || hasExitCode != (state == EXIT_FINISHED)) {
                throw new IllegalStateException("Invalid launch exit receipt status");
            }
            this.state = state;
            this.hasExitCode = hasExitCode;
            this.exitCode = exitCode;
        }
    }

    /** What the service does; the system server implements it. */
    interface Device {
        String[] install(String apk) throws Exception;
        boolean uninstall(String packageName) throws Exception;
        String status();
        void shutdown(String reason);
        /** The exit code of the package's last process, or Integer.MIN_VALUE. */
        int lastExitCode(String packageName);
        /** Uses the normal activity-start checks and binds its resolved process generation. */
        default LaunchResult startActivityAndWait(Intent intent) throws Exception {
            throw new UnsupportedOperationException("Generation-bound host launch unavailable");
        }
        /** A call came: the device is not idle. */
        void touch();
        /** A click on a toast of {@code appId} with the toast's {@code arguments}. */
        void toastActivated(String appId, String arguments);
    }

    private final Device mDevice;
    private final CallerIdentity mCallers;

    GesherService(Device device) {
        super(DESCRIPTOR);
        mDevice = device;
        Services.Created created = Services.sLast;
        mCallers = created == null ? null : new CallerIdentity(created.pm, created.atm);
    }

    @Override
    protected boolean onTransact(int code, Parcel data, Parcel reply, int flags)
            throws RemoteException {
        if (code < INSTALL || code > START_ACTIVITY) {
            return super.onTransact(code, data, reply, flags);
        }
        data.enforceInterface(DESCRIPTOR);
        if (mCallers == null ? Binder.getCallingPid() != android.os.Process.myPid()
                : !mCallers.isSystem()) {
            throw new SecurityException("The Gesher control service is restricted to the host shell");
        }
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
                case TOAST_ACTIVATED: {
                    String appId = data.readString();
                    mDevice.toastActivated(appId, data.readString());
                    reply.writeNoException();
                    return true;
                }
                case PROXY: {
                    String pkg = data.readString();
                    com.gesher.net.ProxySetting.setForLaunch(pkg, data.readString());
                    reply.writeNoException();
                    return true;
                }
                case START_ACTIVITY: {
                    Intent intent = data.readTypedObject(Intent.CREATOR);
                    data.enforceNoDataAvail();
                    if (intent == null) throw new IllegalArgumentException("Missing launch intent");
                    LaunchResult result = mDevice.startActivityAndWait(intent);
                    reply.writeNoException();
                    reply.writeTypedObject(result.result, 0);
                    reply.writeInt(result.taskId);
                    reply.writeStrongBinder(result.exitReceipt);
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

        /**
         * The proxy ({@code host:port}, {@code direct}, {@code windows}; null: the saved
         * settings) of the package's processes the system server starts from now on.
         */
        public void setProxy(String packageName, String value) throws RemoteException {
            Parcel data = Parcel.obtain();
            Parcel reply = Parcel.obtain();
            try {
                data.writeInterfaceToken(DESCRIPTOR);
                data.writeString(packageName);
                data.writeString(value);
                mBinder.transact(PROXY, data, reply, 0);
                reply.readException();
            } finally {
                data.recycle();
                reply.recycle();
            }
        }

        /** Hands the device a toast click that Windows gave this process (the activator). */
        public void toastActivated(String appId, String arguments) throws RemoteException {
            Parcel data = Parcel.obtain();
            Parcel reply = Parcel.obtain();
            try {
                data.writeInterfaceToken(DESCRIPTOR);
                data.writeString(appId);
                data.writeString(arguments);
                mBinder.transact(TOAST_ACTIVATED, data, reply, 0);
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

        /** Starts through the host control service without a package-wide exit lookup. */
        public LaunchResult startActivityAndWait(Intent intent) throws RemoteException {
            Parcel data = Parcel.obtain();
            Parcel reply = Parcel.obtain();
            try {
                data.writeInterfaceToken(DESCRIPTOR);
                data.writeTypedObject(java.util.Objects.requireNonNull(intent), 0);
                if (!mBinder.transact(START_ACTIVITY, data, reply, 0)) {
                    throw new RemoteException("Gesher device does not support generation-bound launches");
                }
                reply.readException();
                WaitResult result = reply.readTypedObject(WaitResult.CREATOR);
                int taskId = reply.readInt();
                IBinder receipt = reply.readStrongBinder();
                reply.enforceNoDataAvail();
                return new LaunchResult(result, taskId, receipt);
            } finally {
                data.recycle();
                reply.recycle();
            }
        }

        /** Reads only the status of the process generation captured by a host launch. */
        public LaunchExit launchExit(IBinder receipt) throws RemoteException {
            Parcel data = Parcel.obtain();
            Parcel reply = Parcel.obtain();
            try {
                data.writeInterfaceToken(EXIT_DESCRIPTOR);
                if (!java.util.Objects.requireNonNull(receipt).transact(EXIT_STATUS, data, reply, 0)) {
                    throw new RemoteException("Launch exit receipt is unavailable");
                }
                reply.readException();
                LaunchExit result = new LaunchExit(reply.readInt(), reply.readBoolean(), reply.readInt());
                reply.enforceNoDataAvail();
                return result;
            } finally {
                data.recycle();
                reply.recycle();
            }
        }
    }
}
