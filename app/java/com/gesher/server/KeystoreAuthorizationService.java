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

import java.util.HashMap;
import java.util.Map;

/**
 * {@code gesher.keystore}: {@link KeystoreAuthorization} for the AndroidKeyStore provider
 * in each app's process (ADR 0095), as keystore2 is to AOSP's provider. A plain Binder
 * with its own transaction codes, called by {@link Client} (no AIDL interface of the
 * framework fits: Gesher has no keystore2 service).
 *
 * <p>Any app may call it. A caller gets back only what its own blob holds, and an
 * operation belongs to the uid that began it.
 */
public final class KeystoreAuthorizationService extends Binder {
    public static final String NAME = "gesher.keystore";
    static final String DESCRIPTOR = "com.gesher.IKeystoreAuthorization";

    static final int AVAILABILITY = IBinder.FIRST_CALL_TRANSACTION;
    static final int SEAL = IBinder.FIRST_CALL_TRANSACTION + 1;
    static final int RELEASE = IBinder.FIRST_CALL_TRANSACTION + 2;
    static final int BEGIN = IBinder.FIRST_CALL_TRANSACTION + 3;
    static final int FINISH = IBinder.FIRST_CALL_TRANSACTION + 4;
    static final int ABORT = IBinder.FIRST_CALL_TRANSACTION + 5;

    /** What the app's keystore asks; {@link KeystoreAuthorization}'s answers. */
    public interface Authorizer {
        int availability();

        KeystoreAuthorization.Reply seal(byte[] material, int timeoutSeconds, int authType);

        KeystoreAuthorization.Reply release(byte[] blob);

        KeystoreAuthorization.Reply begin(byte[] blob);

        KeystoreAuthorization.Reply finish(long handle);

        void abort(long handle);
    }

    private final KeystoreAuthorization mKeys;
    private final Map<IBinder, IBinder.DeathRecipient> mClients = new HashMap<>();

    KeystoreAuthorizationService(KeystoreAuthorization keys) {
        super(DESCRIPTOR);
        mKeys = keys;
    }

    @Override
    protected boolean onTransact(int code, Parcel data, Parcel reply, int flags)
            throws RemoteException {
        if (code < AVAILABILITY || code > ABORT) {
            return super.onTransact(code, data, reply, flags);
        }
        data.enforceInterface(DESCRIPTOR);
        final int uid = Binder.getCallingUid();
        final int pid = Binder.getCallingPid();
        KeystoreAuthorization.Reply result;
        switch (code) {
            case AVAILABILITY:
                result = KeystoreAuthorization.Reply.of(mKeys.availability());
                break;
            case SEAL: {
                byte[] material = data.createByteArray();
                int timeout = data.readInt();
                int authType = data.readInt();
                result = mKeys.seal(pid, material, timeout, authType);
                break;
            }
            case RELEASE:
                result = mKeys.release(data.createByteArray());
                break;
            case BEGIN: {
                IBinder client = data.readStrongBinder();
                byte[] blob = data.createByteArray();
                if (client == null) {
                    result = KeystoreAuthorization.Reply.of(KeystoreAuthorization.FAILED);
                    break;
                }
                watch(client);
                result = mKeys.begin(uid, client, blob);
                break;
            }
            case FINISH:
                result = mKeys.finish(uid, data.readLong());
                break;
            default:
                mKeys.abort(uid, data.readLong());
                result = KeystoreAuthorization.Reply.of(KeystoreAuthorization.OK);
                break;
        }
        reply.writeNoException();
        reply.writeInt(result.status);
        reply.writeLong(result.handle);
        reply.writeByteArray(result.data);
        return true;
    }

    /** Ends the client's operations with its process. */
    private void watch(IBinder client) {
        synchronized (mClients) {
            if (mClients.containsKey(client)) return;
            IBinder.DeathRecipient recipient = () -> {
                synchronized (mClients) {
                    mClients.remove(client);
                }
                mKeys.clientDied(client);
            };
            try {
                client.linkToDeath(recipient, 0);
                mClients.put(client, recipient);
            } catch (RemoteException dead) {
                // Nothing begun yet under this client; begin() below is harmless and the
                // per-uid bound prunes it.
            }
        }
    }

    /** The service's client in an app's process. */
    public static final class Client implements Authorizer {
        private final IBinder mService;
        private final IBinder mToken = new Binder();

        private Client(IBinder service) {
            mService = service;
        }

        /** The client, or null when this device has no such service. */
        public static Client get() {
            IBinder service = ServiceManager.getService(NAME);
            return service != null ? new Client(service) : null;
        }

        @Override
        public int availability() {
            KeystoreAuthorization.Reply reply = call(AVAILABILITY, data -> { });
            // A failed call is no answer that Windows Hello is there.
            return reply.status == KeystoreAuthorization.OK ? KeystoreAuthorization.OK
                    : KeystoreAuthorization.UNAVAILABLE;
        }

        @Override
        public KeystoreAuthorization.Reply seal(byte[] material, int timeoutSeconds,
                int authType) {
            return call(SEAL, data -> {
                data.writeByteArray(material);
                data.writeInt(timeoutSeconds);
                data.writeInt(authType);
            });
        }

        @Override
        public KeystoreAuthorization.Reply release(byte[] blob) {
            return call(RELEASE, data -> data.writeByteArray(blob));
        }

        @Override
        public KeystoreAuthorization.Reply begin(byte[] blob) {
            return call(BEGIN, data -> {
                data.writeStrongBinder(mToken);
                data.writeByteArray(blob);
            });
        }

        @Override
        public KeystoreAuthorization.Reply finish(long handle) {
            return call(FINISH, data -> data.writeLong(handle));
        }

        @Override
        public void abort(long handle) {
            call(ABORT, data -> data.writeLong(handle));
        }

        private interface Arguments {
            void write(Parcel data);
        }

        private KeystoreAuthorization.Reply call(int code, Arguments arguments) {
            Parcel data = Parcel.obtain();
            Parcel reply = Parcel.obtain();
            try {
                data.writeInterfaceToken(DESCRIPTOR);
                arguments.write(data);
                if (!mService.transact(code, data, reply, 0)) {
                    return KeystoreAuthorization.Reply.of(KeystoreAuthorization.FAILED);
                }
                reply.readException();
                int status = reply.readInt();
                long handle = reply.readLong();
                return new KeystoreAuthorization.Reply(status, reply.createByteArray(), handle);
            } catch (RemoteException | RuntimeException e) {
                KeystoreAuthorization.log("the keystore authorization service did not answer: "
                        + e);
                return KeystoreAuthorization.Reply.of(KeystoreAuthorization.FAILED);
            } finally {
                reply.recycle();
                data.recycle();
            }
        }
    }
}
