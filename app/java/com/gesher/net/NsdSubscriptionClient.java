/* Copyright (C) 2026 The Gesher Authors. SPDX-License-Identifier: Apache-2.0 */
package com.gesher.net;

import android.net.nsd.NsdManager;
import android.net.nsd.NsdServiceInfo;
import android.os.IBinder;
import android.os.RemoteException;
import android.os.ServiceManager;
import android.os.ServiceSpecificException;
import java.util.ArrayDeque;
import java.util.ArrayList;
import java.util.Collections;
import java.util.HashMap;
import java.util.HashSet;
import java.util.IdentityHashMap;
import java.util.Map;
import java.util.Objects;
import java.util.Set;
import java.util.concurrent.Executors;
import java.util.concurrent.ScheduledExecutorService;
import java.util.concurrent.TimeUnit;

/** Continuing app ownership across an actual NSD service-process replacement. */
public final class NsdSubscriptionClient {
    private NsdSubscriptionClient() {}
    public interface Session { void stop(); }
    public interface Callback {
        void updated(NsdServiceInfo info);
        void lost();
        void failed(int error);
        void unregistered();
    }
    public interface Factory {
        Session start(NsdServiceInfo selector, Callback callback);
        default int retainedCount() { return 0; }
    }
    interface Connector { IBinder find(); }
    interface Scheduler { void later(Runnable action, long delayMillis); }
    private static final class RetryThread {
        static final ScheduledExecutorService EXECUTOR = Executors.newSingleThreadScheduledExecutor(action -> {
            Thread thread = new Thread(action, "nsd-subscription-reconnect");
            thread.setDaemon(true);
            return thread;
        });
    }
    /** Lazy: ordinary manager construction does not allocate a connection or retry thread. */
    public static Factory factory() {
        return withConnector(() -> ServiceManager.getService(INsdSubscription.SERVICE_NAME),
                (action, delay) -> RetryThread.EXECUTOR.schedule(action, delay, TimeUnit.MILLISECONDS));
    }
    static Factory withConnector(Connector connector, Scheduler scheduler) {
        return new Connection(connector, scheduler);
    }

    private static final class Connection implements Factory {
        private static final int MAX_OPERATIONS = 1024, MAX_DELIVERIES = 4096;
        private static final int LOST_ALL = 6;
        final Connector connector;
        final Scheduler scheduler;
        final Set<Operation> operations = Collections.newSetFromMap(new IdentityHashMap<>());
        final Map<Integer, Operation> wire = new HashMap<>();
        final ArrayDeque<Delivery> deliveries = new ArrayDeque<>();
        int nextId = 1;
        Remote remote;
        boolean retryScheduled, delivering;

        Connection(Connector connector, Scheduler scheduler) {
            this.connector = Objects.requireNonNull(connector);
            this.scheduler = Objects.requireNonNull(scheduler);
        }

        private final class Remote implements IBinder.DeathRecipient {
            final IBinder binder;
            final INsdSubscription service;
            final INsdSubscription.IClient token = new INsdSubscription.IClient.Stub() {
                public void event(int id, int kind, int error, long scope, NsdServiceInfo info) {
                    received(Remote.this, id, kind, error, scope, info);
                }
            };
            boolean dead;
            Remote(IBinder binder) {
                this.binder = binder;
                service = INsdSubscription.Stub.asInterface(binder);
            }
            public void binderDied() { serviceDied(this); }
        }

        private Remote connect() throws RemoteException {
            synchronized (this) { if (remote != null && !remote.dead) return remote; }
            IBinder binder = connector.find();
            if (binder == null) throw new RemoteException("NSD subscription service is unavailable");
            Remote candidate = new Remote(binder);
            binder.linkToDeath(candidate, 0);
            Remote selected;
            synchronized (this) {
                if (!candidate.dead && binder.isBinderAlive() && remote == null) remote = candidate;
                selected = remote;
            }
            if (selected != candidate) unlink(candidate);
            if (selected == null) throw new RemoteException("NSD subscription owner died during connection");
            return selected;
        }

        @Override public Session start(NsdServiceInfo selector, Callback callback) {
            Operation operation = new Operation(NsdRegistrationContract.copyForTransport(selector),
                    Objects.requireNonNull(callback));
            boolean full;
            synchronized (this) {
                full = operations.size() >= MAX_OPERATIONS;
                if (!full) operations.add(operation);
            }
            if (full) { callback.failed(NsdManager.FAILURE_INTERNAL_ERROR); return () -> {}; }
            try { drive(operation, true); }
            catch (RuntimeException | Error failure) {
                // The service rejects identity/permission before admission. A
                // definite initial denial owns no request to cancel or retry.
                // Unknown failures may still follow an accepted transaction.
                boolean retained;
                synchronized (this) {
                    operation.callback = null;
                    operation.stopping = true;
                    if (operation.source == null || failure instanceof SecurityException
                            && !operation.established && !operation.enrolled) {
                        operations.remove(operation);
                        wire.remove(operation.id, operation);
                        operation.source = null;
                        operation.observed.clear();
                    }
                    retained = operations.contains(operation);
                }
                drain(); // Also releases the final idle remote death recipient.
                if (retained) scheduleRetry();
                throw failure;
            }
            return operation;
        }

        @Override public synchronized int retainedCount() {
            int count = 0;
            for (Operation operation : operations) if (operation.callback == null) count++;
            return count;
        }

        private final class Operation implements Session {
            final NsdServiceInfo selector;
            final Set<Long> observed = new HashSet<>();
            Callback callback;
            Remote source;
            int id;
            boolean established, enrolled, stopping, driving, driveAgain;
            Operation(NsdServiceInfo selector, Callback callback) {
                this.selector = selector; this.callback = callback;
            }
            public void stop() {
                synchronized (Connection.this) {
                    if (!operations.contains(this) || stopping) return;
                    stopping = true;
                    observed.clear();
                }
                drive(this, false);
            }
        }

        private void attach(Operation operation, Remote selected) {
            synchronized (this) {
                if (!operations.contains(operation) || selected.dead || selected != remote) return;
                if (operation.source != null) return;
                if (nextId <= 0) throw new IllegalStateException("NSD listener identifiers exhausted");
                operation.id = nextId++;
                operation.source = selected;
                operation.enrolled = false;
                wire.put(operation.id, operation);
            }
        }

        private void drive(Operation operation, boolean initial) {
            synchronized (this) {
                if (!operations.contains(operation)) return;
                if (operation.driving) { operation.driveAgain = true; return; }
                operation.driving = true;
            }
            boolean retry = false;
            try {
                Remote selected;
                synchronized (this) { selected = operation.source; }
                if (selected == null) {
                    try { selected = connect(); }
                    catch (RemoteException unavailable) {
                        synchronized (this) {
                            if (initial && !operation.established && !operation.stopping) {
                                terminal(operation, INsdSubscription.FAILED, NsdManager.FAILURE_INTERNAL_ERROR);
                            } else retry = true;
                        }
                        return;
                    }
                    attach(operation, selected);
                }
                int id;
                boolean enrolled;
                synchronized (this) {
                    if (!operations.contains(operation) || operation.source != selected || selected.dead) {
                        retry = operations.contains(operation);
                        return;
                    }
                    id = operation.id;
                    enrolled = operation.enrolled;
                }
                try {
                    if (!enrolled) {
                        // Ambiguous live-Binder failures retry this exact token
                        // and ID. The service treats the enrollment idempotently.
                        selected.service.register(selected.token, id, operation.selector);
                        synchronized (this) {
                            if (operation.source == selected && operation.id == id) operation.enrolled = true;
                        }
                    }
                    boolean stop;
                    synchronized (this) {
                        stop = operations.contains(operation) && operation.source == selected
                                && operation.id == id && operation.stopping;
                    }
                    if (stop) selected.service.unregister(selected.token, id);
                } catch (RemoteException unavailable) {
                    if (!selected.binder.isBinderAlive()) serviceDied(selected);
                    retry = true;
                } catch (ServiceSpecificException unavailable) {
                    if (unavailable.errorCode != INsdSubscription.ERROR_CLIENT_UNAVAILABLE) throw unavailable;
                    synchronized (this) {
                        if (!operation.established && !operation.stopping) {
                            terminal(operation, INsdSubscription.FAILED, NsdManager.FAILURE_INTERNAL_ERROR);
                        } else retry = true;
                    }
                }
            } catch (RuntimeException failure) {
                if (initial) throw failure;
                // An established subscription has no registration-failure or
                // unregister-failure callback. Retry the actual owner boundary;
                // do not manufacture either success or a new initial failure.
                retry = true;
            } finally {
                boolean again;
                synchronized (this) {
                    operation.driving = false;
                    again = operation.driveAgain && operations.contains(operation);
                    operation.driveAgain = false;
                }
                drain();
                if (retry) scheduleRetry();
                else if (again) drive(operation, false);
            }
        }

        private void scheduleRetry() {
            synchronized (this) {
                if (retryScheduled || operations.isEmpty()) return;
                retryScheduled = true;
            }
            scheduler.later(() -> {
                ArrayList<Operation> pending = new ArrayList<>();
                synchronized (Connection.this) {
                    retryScheduled = false;
                    for (Operation operation : operations) {
                        if (operation.source == null || !operation.enrolled || operation.stopping) pending.add(operation);
                    }
                }
                for (Operation operation : pending) drive(operation, false);
            }, 1000);
        }

        private void received(Remote source, int id, int kind, int error, long scope, NsdServiceInfo info) {
            if (kind < INsdSubscription.UPDATED || kind > INsdSubscription.READY) {
                throw new IllegalArgumentException("Unknown NSD subscription event");
            }
            synchronized (this) {
                Operation operation = wire.get(id);
                if (source.dead || operation == null || operation.source != source) return;
                switch (kind) {
                    case INsdSubscription.READY:
                        operation.established = true;
                        break;
                    case INsdSubscription.UPDATED:
                        if (scope <= 0 || info == null || operation.stopping) return;
                        operation.established = true;
                        append(new Delivery(operation, source, kind, error, scope,
                                NsdRegistrationContract.copyForTransport(info), null));
                        break;
                    case INsdSubscription.LOST:
                        if (scope <= 0 || operation.stopping) return;
                        append(new Delivery(operation, source, kind, error, scope, null, null));
                        break;
                    case INsdSubscription.FAILED:
                        if (!operation.established && !operation.stopping) terminal(operation, kind, error);
                        else {
                            // The new native owner may fail initial setup after
                            // an app's established subscription survived restart.
                            // The dead private request is not a new app request.
                            wire.remove(operation.id, operation);
                            operation.source = null;
                            operation.enrolled = false;
                            scheduleRetry();
                        }
                        break;
                    case INsdSubscription.UNREGISTERED:
                        if (operation.stopping) terminal(operation, kind, 0);
                        break;
                    default: throw new AssertionError(kind);
                }
            }
            drain();
        }

        private void serviceDied(Remote source) {
            synchronized (this) {
                if (source.dead) return;
                source.dead = true;
                if (remote == source) remote = null;
                for (Operation operation : new ArrayList<>(operations)) {
                    if (operation.source != source) continue;
                    wire.remove(operation.id, operation);
                    operation.source = null;
                    operation.enrolled = false;
                    if (!operation.established && !operation.stopping) {
                        terminal(operation, INsdSubscription.FAILED, NsdManager.FAILURE_INTERNAL_ERROR);
                    } else if (operation.callback == null) {
                        // The actual process death closes any abandoned native
                        // ownership; no application success is delivered.
                        operations.remove(operation);
                    } else if (!operation.stopping) {
                        append(new Delivery(operation, source, LOST_ALL, 0, 0, null, null));
                    }
                }
            }
            unlink(source);
            drain();
            scheduleRetry();
        }

        private void unlink(Remote source) {
            try { source.binder.unlinkToDeath(source, 0); }
            catch (RuntimeException alreadyDead) { /* Connection ownership already retired. */ }
        }

        /** Caller holds this monitor; terminal callback ownership moves to its delivery. */
        private void terminal(Operation operation, int kind, int error) {
            if (!operations.remove(operation)) return;
            wire.remove(operation.id, operation);
            Callback callback = operation.callback;
            operation.callback = null;
            operation.observed.clear();
            if (callback != null) append(new Delivery(operation, operation.source, kind, error, 0, null, callback));
        }

        private final class Delivery {
            final Operation operation;
            final Remote source;
            final int kind, error;
            final long scope;
            final NsdServiceInfo info;
            final Callback terminal;
            Delivery(Operation operation, Remote source, int kind, int error, long scope,
                    NsdServiceInfo info, Callback terminal) {
                this.operation = operation; this.source = source; this.kind = kind;
                this.error = error; this.scope = scope; this.info = info; this.terminal = terminal;
            }
        }

        private void append(Delivery delivery) {
            while (deliveries.size() >= MAX_DELIVERIES) {
                try { wait(100); }
                catch (InterruptedException ignored) { /* Logical stop owns cancellation. */ }
            }
            deliveries.addLast(delivery);
        }

        /** One bounded delivery queue preserves event/death order without callback locks. */
        private void drain() {
            synchronized (this) {
                if (delivering) return;
                delivering = true;
            }
            try {
                for (;;) {
                    Delivery delivery;
                    Callback callback;
                    int count = 1;
                    synchronized (this) {
                        delivery = deliveries.pollFirst();
                        notifyAll();
                        if (delivery == null) return;
                        Operation operation = delivery.operation;
                        callback = delivery.terminal != null ? delivery.terminal : operation.callback;
                        if (callback == null || delivery.terminal == null && operation.stopping) continue;
                        if (delivery.kind == INsdSubscription.UPDATED) {
                            if (operation.source != delivery.source || delivery.source.dead) continue;
                            operation.observed.add(delivery.scope);
                        } else if (delivery.kind == INsdSubscription.LOST) {
                            if (!operation.observed.remove(delivery.scope)) continue;
                        } else if (delivery.kind == LOST_ALL) {
                            count = operation.observed.size();
                            operation.observed.clear();
                        }
                    }
                    try {
                        switch (delivery.kind) {
                            case INsdSubscription.UPDATED: callback.updated(delivery.info); break;
                            case INsdSubscription.LOST: callback.lost(); break;
                            case LOST_ALL: for (int i = 0; i < count; i++) callback.lost(); break;
                            case INsdSubscription.FAILED: callback.failed(delivery.error); break;
                            case INsdSubscription.UNREGISTERED: callback.unregistered(); break;
                            default: throw new AssertionError(delivery.kind);
                        }
                    } catch (RuntimeException rejected) {
                        delivery.operation.stop();
                    }
                }
            } finally {
                boolean remaining;
                Remote idle = null;
                synchronized (this) {
                    delivering = false;
                    remaining = !deliveries.isEmpty();
                    if (operations.isEmpty() && remote != null) { idle = remote; remote = null; }
                }
                if (idle != null) unlink(idle);
                if (remaining) drain();
            }
        }
    }
}
