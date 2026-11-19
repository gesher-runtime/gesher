/* Copyright (C) 2026 The Gesher Authors. SPDX-License-Identifier: Apache-2.0 */
package com.gesher.net;

import android.net.nsd.AdvertisingRequest;
import android.net.nsd.NsdManager;
import android.net.nsd.NsdServiceInfo;
import android.os.IBinder;
import android.os.RemoteException;
import android.os.ServiceManager;
import android.os.ServiceSpecificException;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.Map;

/** One manager's connection to the real service-process registration owner. */
public final class NsdRegistrationClient {
    private NsdRegistrationClient() {}
    public interface Session {
        void update(AdvertisingRequest request, int otherActiveRequests);
        void stop();
    }
    public interface Callback {
        void registered(NsdServiceInfo result);
        void failed(int error, boolean retained);
        void unregistered();
        void unregisterFailed(int error);
    }
    public interface Factory {
        Session start(AdvertisingRequest request, int otherActiveRequests, Callback callback);
        /** Failed-update transactions can remain owned by a living service connection. */
        default int retainedCount() { return 0; }
    }

    /** Does not allocate a Binder or contact the service until registration is requested. */
    public static Factory factory() {
        return new Factory() {
            private Connection connection;
            public Session start(AdvertisingRequest request, int other, Callback callback) {
                Connection value;
                synchronized (this) {
                    if (connection == null) connection = new Connection();
                    value = connection;
                }
                return value.start(request, other, callback);
            }
            public int retainedCount() {
                Connection value;
                synchronized (this) { value = connection; }
                return value == null ? 0 : value.retainedCount();
            }
        };
    }

    private static final class Connection extends INsdRegistration.IClient.Stub {
        private final Map<Integer, Operation> operations = new HashMap<>();
        private int nextId = 1;
        private Remote remote;

        private final class Remote implements IBinder.DeathRecipient {
            final IBinder binder;
            final INsdRegistration service;
            boolean dead;
            int retained;
            Remote(IBinder binder) {
                this.binder = binder;
                service = INsdRegistration.Stub.asInterface(binder);
            }
            @Override public void binderDied() { serviceDied(this); }
        }

        private Remote connect() throws RemoteException {
            synchronized (this) { if (remote != null) return remote; }
            IBinder binder = ServiceManager.getService(INsdRegistration.SERVICE_NAME);
            if (binder == null) throw new RemoteException("NSD registration service is unavailable");
            Remote candidate = new Remote(binder);
            // Linking may report death inline. No connection monitor is held,
            // and a not-yet-published candidate owns no operation callbacks.
            binder.linkToDeath(candidate, 0);
            Remote selected;
            synchronized (this) {
                if (!candidate.dead && binder.isBinderAlive() && remote == null) remote = candidate;
                selected = remote;
            }
            if (selected != candidate) binder.unlinkToDeath(candidate, 0);
            if (selected == null) throw new RemoteException("NSD service died during connection");
            return selected;
        }

        synchronized int retainedCount() { return remote == null ? 0 : remote.retained; }

        Session start(AdvertisingRequest request, int other, Callback callback) {
            Remote selected;
            Operation operation;
            try {
                for (;;) {
                    selected = connect();
                    synchronized (this) {
                        if (selected != remote || selected.dead) continue;
                        if (nextId <= 0) {
                            throw new IllegalStateException("NSD listener identifiers exhausted");
                        }
                        operation = new Operation(nextId++, selected, callback);
                        operations.put(operation.id, operation);
                        break;
                    }
                }
            } catch (RemoteException unavailable) {
                callback.failed(NsdManager.FAILURE_INTERNAL_ERROR, false);
                return new Session() {
                    public void update(AdvertisingRequest ignored, int count) {
                        throw new IllegalArgumentException("listener not registered");
                    }
                    public void stop() {}
                };
            }
            try { selected.service.register(this, operation.id, request, other); }
            catch (RemoteException unavailable) {
                event(operation.id, INsdRegistration.FAILED,
                        NsdManager.FAILURE_INTERNAL_ERROR, false, null);
            } catch (ServiceSpecificException unavailable) {
                if (unavailable.errorCode != INsdRegistration.ERROR_CLIENT_UNAVAILABLE) {
                    synchronized (this) {
                        operations.remove(operation.id, operation);
                        operation.callback = null;
                    }
                    throw unavailable;
                }
                event(operation.id, INsdRegistration.FAILED,
                        NsdManager.FAILURE_INTERNAL_ERROR, false, null);
            } catch (RuntimeException failure) {
                synchronized (this) {
                    operations.remove(operation.id, operation);
                    operation.callback = null;
                }
                throw failure;
            }
            return operation;
        }

        @Override public void event(int id, int event, int error, boolean retained,
                NsdServiceInfo info) {
            if (event < INsdRegistration.REGISTERED || event > INsdRegistration.UNREGISTER_FAILED) {
                throw new IllegalArgumentException("Unknown NSD registration event");
            }
            Operation operation;
            Callback callback;
            synchronized (this) {
                operation = operations.get(id);
                if (operation == null) return;
                callback = operation.callback;
                if (event != INsdRegistration.REGISTERED) {
                    operations.remove(id);
                    operation.callback = null;
                    if (retained && !operation.remote.dead) operation.remote.retained++;
                }
            }
            if (callback == null) return;
            switch (event) {
                case INsdRegistration.REGISTERED: callback.registered(info); break;
                case INsdRegistration.FAILED: callback.failed(error, retained); break;
                case INsdRegistration.UNREGISTERED: callback.unregistered(); break;
                case INsdRegistration.UNREGISTER_FAILED: callback.unregisterFailed(error); break;
                default: throw new AssertionError(event);
            }
        }

        private void serviceDied(Remote source) {
            ArrayList<Operation> failed = new ArrayList<>();
            ArrayList<Callback> callbacks = new ArrayList<>();
            synchronized (this) {
                if (source.dead) return;
                source.dead = true;
                source.retained = 0;
                if (remote == source) remote = null;
                for (Operation operation : new ArrayList<>(operations.values())) {
                    if (operation.remote != source) continue;
                    operations.remove(operation.id);
                    if (operation.callback != null) {
                        failed.add(operation);
                        callbacks.add(operation.callback);
                        operation.callback = null;
                    }
                }
            }
            for (int i = 0; i < failed.size(); i++) {
                if (failed.get(i).stopping) {
                    callbacks.get(i).unregisterFailed(NsdManager.FAILURE_INTERNAL_ERROR);
                } else callbacks.get(i).failed(NsdManager.FAILURE_INTERNAL_ERROR, false);
            }
        }

        private final class Operation implements Session {
            final int id;
            final Remote remote;
            Callback callback;
            volatile boolean stopping;
            Operation(int id, Remote remote, Callback callback) {
                this.id = id; this.remote = remote; this.callback = callback;
            }
            public void update(AdvertisingRequest request, int other) {
                synchronized (Connection.this) {
                    if (operations.get(id) != this) {
                        throw new IllegalArgumentException("listener not registered");
                    }
                }
                try { remote.service.register(Connection.this, id, request, other); }
                catch (RemoteException unavailable) {
                    // A live old server can still own its transaction. Its death
                    // clears this retained count; never reconnect an old ID.
                    event(id, INsdRegistration.FAILED, NsdManager.FAILURE_INTERNAL_ERROR, true, null);
                } catch (ServiceSpecificException unavailable) {
                    if (unavailable.errorCode != INsdRegistration.ERROR_CLIENT_UNAVAILABLE) throw unavailable;
                    event(id, INsdRegistration.FAILED, NsdManager.FAILURE_INTERNAL_ERROR, true, null);
                }
            }
            public void stop() {
                synchronized (Connection.this) {
                    if (operations.get(id) != this || stopping) return;
                    stopping = true;
                }
                try { remote.service.unregister(Connection.this, id); }
                catch (RemoteException unavailable) {
                    event(id, INsdRegistration.UNREGISTER_FAILED,
                            NsdManager.FAILURE_INTERNAL_ERROR, true, null);
                }
            }
        }
    }
}
