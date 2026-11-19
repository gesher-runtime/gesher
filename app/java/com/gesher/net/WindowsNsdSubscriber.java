/* Copyright (C) 2026 The Gesher Authors. SPDX-License-Identifier: Apache-2.0 */
package com.gesher.net;

import android.net.Network;
import android.net.nsd.NsdManager;
import android.net.nsd.NsdServiceInfo;
import android.os.SystemClock;
import com.gesher.net.mdns.MdnsSubscriptionState;
import java.io.IOException;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.HashMap;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.Objects;
import java.util.concurrent.Semaphore;
import java.util.function.LongSupplier;

/** Continuing named DNS-SD observation through real request-owned Windows sockets. */
public final class WindowsNsdSubscriber {
    private WindowsNsdSubscriber() {}
    private static final int MAX_SCOPES = 64;
    private static final Semaphore WORKERS = new Semaphore(200);

    public interface Callback {
        /** Private ownership progress after genuine transport/scope initialization. */
        void ready();
        void updated(long scopeId, NsdServiceInfo service);
        void lost(long scopeId);
        void failed(int error);
    }

    /** IDs belong to the service owner; withdrawal drops application references immediately. */
    public interface Backend {
        void register(int id, NsdServiceInfo selector, Callback callback);
        void unregister(int id, Runnable completion);
        void withdraw(int id);
        long nextDueElapsed();
        int[] nativeState();
    }

    public static Backend create() {
        return new Owner(WindowsNsdResolver.subscriptionDrivers(), SystemClock::elapsedRealtime);
    }

    // Explicit test seam. Ordinary construction never selects a fixture driver.
    static Backend withDriver(WindowsNsdResolver.DriverFactory factory, LongSupplier clock) {
        return new Owner(factory, clock);
    }

    private static final class Owner implements Backend {
        private final Map<Integer, Subscriber> subscriptions = new HashMap<>();
        private final WindowsNsdResolver.DriverFactory factory;
        private final LongSupplier clock;

        Owner(WindowsNsdResolver.DriverFactory factory, LongSupplier clock) {
            this.factory = Objects.requireNonNull(factory);
            this.clock = Objects.requireNonNull(clock);
        }

        public void register(int id, NsdServiceInfo selector, Callback callback) {
            Objects.requireNonNull(selector);
            Objects.requireNonNull(callback);
            if (MdnsSubscriptionState.baseType(selector.getServiceType()) == null) {
                callback.failed(NsdManager.FAILURE_BAD_PARAMETERS);
                return;
            }
            Subscriber value;
            synchronized (this) {
                if (subscriptions.containsKey(id)) throw new IllegalArgumentException("Duplicate subscription ID");
                if (!WORKERS.tryAcquire()) {
                    callback.failed(NsdManager.FAILURE_INTERNAL_ERROR);
                    return;
                }
                try {
                    value = new Subscriber(this, factory, clock, id, selector, callback);
                    subscriptions.put(id, value);
                } catch (RuntimeException | Error failure) {
                    WORKERS.release();
                    throw failure;
                }
            }
            try {
                Thread thread = new Thread(value, "nsd-subscription-" + id);
                thread.setDaemon(true);
                thread.start();
            } catch (RuntimeException | Error failure) {
                synchronized (this) { subscriptions.remove(id, value); }
                WORKERS.release();
                value.initialFailure();
                value.completeWithoutWorker();
            }
        }

        public void unregister(int id, Runnable completion) {
            Subscriber value;
            synchronized (this) { value = subscriptions.get(id); }
            if (value == null) completion.run();
            else value.stop(completion, false);
        }

        public void withdraw(int id) {
            Subscriber value;
            synchronized (this) { value = subscriptions.get(id); }
            if (value != null) value.stop(null, true);
        }

        public synchronized long nextDueElapsed() {
            long next = Long.MAX_VALUE;
            for (Subscriber value : subscriptions.values()) next = Math.min(next, value.nextDue);
            return next;
        }

        public int[] nativeState() { return WindowsNsdResolver.subscriptionNativeState(); }

        void finished(Subscriber value) {
            synchronized (this) { subscriptions.remove(value.id, value); }
            WORKERS.release();
        }
    }

    private static final class Observation {
        final long id, lifetime;
        final MdnsSubscriptionState state;
        long retrySendAt;
        boolean progress = true;

        Observation(long id, long lifetime, MdnsSubscriptionState state) {
            this.id = id;
            this.lifetime = lifetime;
            this.state = state;
        }
    }

    /** A single worker owns every operation on its live or retiring native handle. */
    private static final class Subscriber implements Runnable {
        final Owner owner;
        final int id;
        private final WindowsNsdResolver.DriverFactory factory;
        private final LongSupplier clock;
        private final String name, type;
        private final int requestedNetwork, requestedInterface;
        private final Map<Long, Observation> observations = new LinkedHashMap<>();
        private long nextScopeId = 1;
        private Callback callback;
        private Runnable completion;
        private volatile boolean stopping;
        volatile long nextDue;
        private boolean established;
        private WindowsNsdResolver.Driver driver;
        private WindowsNsdResolver.Interface[] configured;

        Subscriber(Owner owner, WindowsNsdResolver.DriverFactory factory, LongSupplier clock,
                int id, NsdServiceInfo selector, Callback callback) {
            this.owner = owner;
            this.id = id;
            this.factory = factory;
            this.clock = clock;
            name = selector.getServiceName();
            type = selector.getServiceType();
            Network network = selector.getNetwork();
            requestedNetwork = network == null ? 0 : network.netId;
            requestedInterface = network == null ? selector.getInterfaceIndex() : 0;
            this.callback = callback;
        }

        synchronized void stop(Runnable done, boolean revoke) {
            if (revoke) completion = null;
            else if (!stopping) completion = Objects.requireNonNull(done);
            stopping = true;
            callback = null;
            notifyAll();
        }

        void initialFailure() {
            Callback target;
            synchronized (this) {
                target = stopping ? null : callback;
                callback = null;
            }
            if (target != null) target.failed(NsdManager.FAILURE_INTERNAL_ERROR);
        }

        void completeWithoutWorker() {
            Runnable done;
            synchronized (this) { done = completion; completion = null; }
            if (done != null) done.run();
        }

        private Callback callback() {
            synchronized (this) { return stopping ? null : callback; }
        }

        private void emit(Observation observation, MdnsSubscriptionState.Event event) {
            Callback target = callback();
            if (target == null || event == null) return;
            if (event.lost) target.lost(observation.id);
            else target.updated(observation.id, event.update);
        }

        private void emit(Observation observation, List<MdnsSubscriptionState.Event> events) {
            for (MdnsSubscriptionState.Event event : events) emit(observation, event);
        }

        private boolean selected(WindowsNsdResolver.Interface scope) {
            return requestedNetwork != 0 ? scope.netId == requestedNetwork
                    : requestedInterface == 0 || scope.index == requestedInterface;
        }

        private boolean reconcile(long now) throws IOException {
            WindowsNsdResolver.SubscriptionScope[] snapshot = driver.subscriptionInterfaces();
            if (snapshot == null) return false;
            if (snapshot.length > MAX_SCOPES) throw new IOException("DNS-SD scope budget exceeded");
            List<WindowsNsdResolver.Interface> interfaces = new ArrayList<>();
            Map<Long, WindowsNsdResolver.SubscriptionScope> selected = new LinkedHashMap<>();
            long generation = driver.generation();
            for (WindowsNsdResolver.SubscriptionScope row : snapshot) {
                WindowsNsdResolver.Interface scope = row.scope;
                if (scope.generation != generation) return false;
                if (row.lifetime <= 0) return false; // unknown continuity is never a fabricated absence
                if (!selected(scope)) continue;
                if (selected.put(scope.luid, row) != null) {
                    throw new IOException("Duplicate native DNS-SD scope");
                }
                interfaces.add(scope);
            }
            WindowsNsdResolver.Interface[] current = interfaces.toArray(new WindowsNsdResolver.Interface[0]);
            if (!Arrays.equals(configured, current)) {
                if (!driver.configure(current)) return false;
                if (driver.generation() != generation) return false;
            }
            for (Map.Entry<Long, Observation> entry : new ArrayList<>(observations.entrySet())) {
                Observation prior = entry.getValue();
                WindowsNsdResolver.SubscriptionScope row = selected.get(entry.getKey());
                WindowsNsdResolver.Interface old = prior.state.scope();
                if (row == null || row.lifetime != prior.lifetime || row.scope.index != old.index
                        || row.scope.netId != old.netId) {
                    observations.remove(entry.getKey());
                    emit(prior, prior.state.retire(now));
                } else prior.state.rebind(row.scope);
            }
            for (Map.Entry<Long, WindowsNsdResolver.SubscriptionScope> entry : selected.entrySet()) {
                if (observations.containsKey(entry.getKey())) continue;
                if (nextScopeId <= 0) throw new IOException("DNS-SD scope identifiers exhausted");
                WindowsNsdResolver.SubscriptionScope row = entry.getValue();
                Observation added = new Observation(nextScopeId++, row.lifetime,
                        new MdnsSubscriptionState(name, type, row.scope, now));
                observations.put(entry.getKey(), added);
            }
            configured = current;
            if (!established) {
                established = true;
                Callback target = callback();
                if (target != null) target.ready();
            }
            return true;
        }

        private void retireObservations(long now) {
            for (Observation observation : observations.values()) {
                emit(observation, observation.state.retire(now));
            }
            observations.clear();
            configured = null;
        }

        /** No driver method other than close is called once retirement starts. */
        private void retireDriver() {
            if (driver == null) return;
            for (;;) {
                try { if (driver.close()) { driver = null; return; } }
                catch (RuntimeException | LinkageError retry) { /* Still owns the native handle. */ }
                nextDue = clock.getAsLong() + 1000;
                pause(1000, false);
            }
        }

        private void pause(long millis, boolean allowStopWake) {
            long end = System.nanoTime() + millis * 1_000_000;
            synchronized (this) {
                for (;;) {
                    if (allowStopWake && stopping) return;
                    long remaining = end - System.nanoTime();
                    if (remaining <= 0) return;
                    try { wait(Math.max(1, remaining / 1_000_000)); }
                    catch (InterruptedException ignored) {
                        // A live native handle still requires the ordinary retirement path.
                    }
                }
            }
        }

        @Override public void run() {
            long nextSnapshot = 0;
            try {
                while (!stopping) {
                    long now = clock.getAsLong();
                    try {
                        // Observed record TTL is independent of a currently
                        // unknown interface snapshot. Retrying continuity must
                        // not extend a service's already known expiration.
                        for (Observation observation : observations.values()) {
                            emit(observation, observation.state.expire(now));
                        }
                        if (driver == null) {
                            driver = factory.open();
                            configured = null;
                            nextSnapshot = 0;
                        }
                        boolean stale = configured == null || configured.length != 0
                                && driver.generation() != configured[0].generation;
                        if (stale || now >= nextSnapshot) {
                            if (!reconcile(now)) {
                                nextDue = now + 100;
                                pause(100, true);
                                continue;
                            }
                            nextSnapshot = now + 1000;
                        }
                        nextDue = nextSnapshot;
                        for (Observation observation : observations.values()) {
                            nextDue = Math.min(nextDue, observation.state.nextDue(now));
                            if (now < observation.retrySendAt) continue;
                            MdnsSubscriptionState.Query query = observation.state.query(now, observation.progress);
                            observation.progress = false;
                            if (query == null) continue;
                            try {
                                if (driver.send(observation.state.scope().index, query.bytes)) {
                                    observation.state.sent(query, now);
                                } else {
                                    observation.progress = true;
                                    observation.retrySendAt = now + 1000;
                                }
                            } catch (IOException unavailable) {
                                // A send failure says nothing about whether the observed
                                // service departed. Keep receiving and allow actual expiry.
                                observation.progress = true;
                                observation.retrySendAt = now + 1000;
                            }
                        }
                        if (stopping) break;
                        WindowsNsdResolver.Packet packet = driver.poll(200);
                        if (packet == null) continue;
                        long receivedAt = clock.getAsLong();
                        if (driver.generation() != packet.generation) continue;
                        for (Observation observation : observations.values()) {
                            if (observation.state.scope().index != packet.index) continue;
                            emit(observation, observation.state.accept(packet, receivedAt));
                            observation.progress = true;
                            break;
                        }
                    } catch (IOException | RuntimeException | LinkageError unavailable) {
                        if (!established) {
                            initialFailure();
                            return;
                        }
                        // The actual transport is being retired; previously complete
                        // scopes receive the matching loss before a fresh connection.
                        retireObservations(now);
                        retireDriver();
                        nextDue = now + 1000;
                        pause(1000, true);
                    }
                }
            } finally {
                synchronized (this) { callback = null; }
                observations.clear();
                retireDriver();
                owner.finished(this);
                Runnable done;
                synchronized (this) { done = completion; completion = null; }
                if (done != null) done.run();
            }
        }
    }
}
