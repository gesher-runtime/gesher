/* Copyright (C) 2026 The Gesher Authors. SPDX-License-Identifier: Apache-2.0 */
package com.gesher.server;

import android.net.nsd.NsdManager;
import android.net.nsd.NsdServiceInfo;
import android.os.IBinder;
import android.os.Process;
import android.os.RemoteException;
import android.os.ServiceSpecificException;
import com.gesher.net.INsdSubscription;
import com.gesher.net.NsdRegistrationContract;
import com.gesher.net.WindowsNsdSubscriber;
import java.io.FileDescriptor;
import java.io.PrintWriter;
import java.lang.ref.WeakReference;
import java.util.ArrayDeque;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.HashSet;
import java.util.Map;
import java.util.Objects;
import java.util.Set;
import java.util.concurrent.Executors;
import java.util.concurrent.ScheduledExecutorService;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicBoolean;

/** Real service-process ownership of continuing application service-info subscriptions. */
public final class NsdSubscriptionService extends INsdSubscription.Stub {
    private static final int MAX_CLIENTS = 1024, MAX_OWNED_IDS = 1024;
    private static final int MAX_PENDING_ADMISSIONS = 4096;
    private static final int MAX_BACKEND_EVENTS = 4096, MAX_REQUEST_EVENTS = 128;
    private final Object lock = new Object();
    private final Map<IBinder, Client> clients = new HashMap<>();
    private final Set<Integer> ownedIds = new HashSet<>(), withdrawals = new HashSet<>();
    private final NsdRegistrationService.Authority authority;
    private final WindowsNsdSubscriber.Backend backend;
    private final NsdRegistrationService.Dispatcher dispatcher;
    private final ThreadLocal<Boolean> dispatching = ThreadLocal.withInitial(() -> false);
    private int nextId = 1, pendingAdmissions, queuedEvents;
    private boolean retryScheduled;

    NsdSubscriptionService(PackageManagerService pm, ActivityTaskManagerService atm,
            ActivityManagerService am, boolean isolatedServiceProcess) {
        this(new NsdRegistrationService.ProcessAuthority(pm, atm, am.callers(), isolatedServiceProcess),
                WindowsNsdSubscriber.create(), serialDispatcher());
        am.onProcessDeath(this::processDied);
        pm.onPackageRemoval(this::packageRemoved);
        DeviceWork.add(this::nextDueElapsed);
    }

    /** Explicit host seams; production always uses the real process authority and native backend. */
    NsdSubscriptionService(NsdRegistrationService.Authority authority,
            WindowsNsdSubscriber.Backend backend, NsdRegistrationService.Dispatcher dispatcher) {
        this.authority = Objects.requireNonNull(authority);
        this.backend = Objects.requireNonNull(backend);
        this.dispatcher = Objects.requireNonNull(dispatcher);
    }

    private static NsdRegistrationService.Dispatcher serialDispatcher() {
        ScheduledExecutorService executor = Executors.newSingleThreadScheduledExecutor(action -> {
            Thread thread = new Thread(action, "nsd-subscription-service");
            thread.setDaemon(true);
            return thread;
        });
        return new NsdRegistrationService.Dispatcher() {
            public void execute(Runnable action) { executor.execute(action); }
            public void later(Runnable action, long delay) {
                executor.schedule(action, delay, TimeUnit.MILLISECONDS);
            }
        };
    }

    private Runnable onDispatcher(Runnable action) {
        return () -> {
            boolean prior = dispatching.get();
            dispatching.set(true);
            try { action.run(); }
            finally { dispatching.set(prior); }
        };
    }

    private final class Client implements IBinder.DeathRecipient {
        final IClient callback;
        final IBinder token;
        final NsdRegistrationService.Owner owner;
        final Map<Integer, Request> requests = new HashMap<>();
        final Map<Integer, Integer> pendingStarts = new HashMap<>();
        final Set<Integer> pendingStops = new HashSet<>();
        final AtomicBoolean linked = new AtomicBoolean();
        int pending; // includes the acquire-to-enrollment interval, always guarded by lock
        volatile boolean revoked;

        Client(IClient callback, NsdRegistrationService.Owner owner) {
            this.callback = callback;
            token = Objects.requireNonNull(callback.asBinder(), "Missing callback Binder");
            this.owner = owner;
        }
        public void binderDied() { retire(this); }
    }

    private static final class Request {
        final Client client;
        final int clientId, backendId;
        final NsdServiceInfo selector;
        final ArrayDeque<Event> events = new ArrayDeque<>();
        boolean submitted, ready, drainScheduled;
        volatile boolean stopping;
        Request(Client client, int clientId, int backendId, NsdServiceInfo selector) {
            this.client = client;
            this.clientId = clientId;
            this.backendId = backendId;
            this.selector = selector;
        }
    }

    private static final class Event {
        final int kind, error;
        final long scope;
        final NsdServiceInfo info;
        Event(int kind, int error, long scope, NsdServiceInfo info) {
            this.kind = kind; this.error = error; this.scope = scope; this.info = info;
        }
    }
    private static final class CapacityExceeded extends RuntimeException {}

    private static void arguments(IClient callback, int id) {
        Objects.requireNonNull(callback, "Missing NSD callback");
        Objects.requireNonNull(callback.asBinder(), "Missing NSD callback Binder");
        if (id <= 0) throw new IllegalArgumentException("Invalid NSD request identifier");
    }

    @Override public void register(IClient callback, int id, NsdServiceInfo selector) {
        arguments(callback, id);
        Objects.requireNonNull(selector, "Missing service selector");
        if (selector.getServiceName() == null || selector.getServiceName().isEmpty()
                || selector.getServiceType() == null || selector.getServiceType().isEmpty()) {
            throw new IllegalArgumentException("Missing service name or type");
        }
        NsdRegistrationService.Owner owner = authority.capture();
        boolean full;
        synchronized (lock) {
            full = pendingAdmissions >= MAX_PENDING_ADMISSIONS;
            if (!full) pendingAdmissions++;
        }
        if (full) { reject(callback, owner, id); return; }
        Client acquired = null;
        boolean transferred = false;
        try {
            NsdServiceInfo copy = NsdRegistrationContract.copyForTransport(selector);
            try { acquired = acquire(callback, owner); }
            catch (CapacityExceeded exhausted) { reject(callback, owner, id); return; }
            catch (RemoteException unavailable) {
                throw new ServiceSpecificException(ERROR_CLIENT_UNAVAILABLE,
                        "NSD callback enrollment is unavailable");
            }
            Client client = acquired;
            synchronized (lock) { client.pendingStarts.merge(id, 1, Integer::sum); }
            transferred = true;
            enqueue(client, id, true, () -> registerOnDispatcher(client, id, copy));
        } finally {
            if (!transferred) {
                synchronized (lock) {
                    pendingAdmissions--;
                    if (acquired != null) acquired.pending--;
                }
                if (acquired != null) releaseIdle(acquired);
            }
        }
    }

    private void reject(IClient callback, NsdRegistrationService.Owner owner, int id) {
        Client known;
        synchronized (lock) {
            known = clients.get(callback.asBinder());
            if (known != null) {
                if (!known.owner.same(owner) || known.revoked) {
                    throw new SecurityException("Different or retired NSD subscription owner");
                }
                // A duplicate private enrollment retry must not terminate an
                // already accepted request because another intake queue is full.
                if (known.requests.containsKey(id) || known.pendingStarts.containsKey(id)) return;
            }
        }
        if (!authority.current(owner)) { if (known != null) retire(known); return; }
        try { callback.event(id, FAILED, NsdManager.FAILURE_INTERNAL_ERROR, 0, null); }
        catch (RemoteException | RuntimeException unavailable) { if (known != null) retire(known); }
    }

    private Client acquire(IClient callback, NsdRegistrationService.Owner owner)
            throws RemoteException {
        IBinder token = callback.asBinder();
        synchronized (lock) {
            Client existing = clients.get(token);
            if (existing != null) {
                if (!existing.owner.same(owner) || existing.revoked) {
                    throw new SecurityException("Different or retired NSD subscription owner");
                }
                existing.pending++;
                return existing;
            }
            if (clients.size() >= MAX_CLIENTS) throw new CapacityExceeded();
        }
        Client candidate = new Client(callback, owner);
        token.linkToDeath(candidate, 0);
        candidate.linked.set(true);
        Client accepted = null;
        try {
            if (!authority.current(owner)) throw new RemoteException("NSD owner retired during enrollment");
            synchronized (lock) {
                if (candidate.revoked || !token.isBinderAlive()) {
                    throw new RemoteException("NSD callback retired during enrollment");
                }
                Client existing = clients.get(token);
                if (existing == null) {
                    if (clients.size() >= MAX_CLIENTS) throw new CapacityExceeded();
                    clients.put(token, candidate);
                    accepted = candidate;
                } else {
                    if (!existing.owner.same(owner) || existing.revoked) {
                        throw new SecurityException("Different or retired NSD subscription owner");
                    }
                    accepted = existing;
                }
                // This charge is taken while the client is published. releaseIdle
                // cannot remove it before the pending request enters its map.
                accepted.pending++;
            }
            return accepted;
        } finally { if (accepted != candidate) unlink(candidate); }
    }

    private void enqueue(Client client, int id, boolean start, Runnable action) {
        AtomicBoolean finished = new AtomicBoolean();
        Runnable finish = () -> {
            if (!finished.compareAndSet(false, true)) return;
            synchronized (lock) {
                client.pending--;
                if (start) {
                    pendingAdmissions--;
                    int count = client.pendingStarts.get(id);
                    if (count == 1) client.pendingStarts.remove(id);
                    else client.pendingStarts.put(id, count - 1);
                } else client.pendingStops.remove(id);
            }
            releaseIdle(client);
        };
        try {
            dispatcher.execute(onDispatcher(() -> {
                try { action.run(); }
                finally { finish.run(); }
            }));
        } catch (RuntimeException | Error failure) {
            finish.run();
            throw failure;
        }
    }

    private boolean current(Client client) {
        if (client.revoked || !authority.current(client.owner)) {
            retire(client);
            return false;
        }
        synchronized (lock) { return clients.get(client.token) == client; }
    }
    private boolean current(Request request) {
        if (!current(request.client)) return false;
        synchronized (lock) { return request.client.requests.get(request.clientId) == request; }
    }

    private void registerOnDispatcher(Client client, int id, NsdServiceInfo selector) {
        if (!current(client)) return;
        Request request;
        Event immediate = null;
        boolean create = false;
        synchronized (lock) {
            request = client.requests.get(id);
            if (request != null) {
                if (request.ready && !request.stopping) immediate = new Event(READY, 0, 0, null);
            } else if (ownedIds.size() >= MAX_OWNED_IDS || nextId <= 0) {
                immediate = new Event(FAILED, NsdManager.FAILURE_INTERNAL_ERROR, 0, null);
            } else {
                request = new Request(client, id, nextId++, selector);
                request.stopping = client.pendingStops.contains(id);
                client.requests.put(id, request);
                ownedIds.add(request.backendId);
                create = true;
            }
        }
        // Never call authority or a remote Binder while holding the intake lock.
        if (immediate != null) emit(client, id, immediate);
        if (!create) return; // Same connection/request ID is an idempotent transport retry.
        if (!current(request)) return;
        Request active = request;
        active.submitted = true;
        WeakReference<Request> weak = new WeakReference<>(active);
        try {
            backend.register(active.backendId, selector, new WindowsNsdSubscriber.Callback() {
                private void post(Event event) {
                    Request target = weak.get();
                    if (target != null) postBackend(target, event);
                }
                public void ready() { post(new Event(READY, 0, 0, null)); }
                public void updated(long scope, NsdServiceInfo info) {
                    post(new Event(UPDATED, 0, scope, NsdRegistrationContract.copyForTransport(info)));
                }
                public void lost(long scope) { post(new Event(LOST, 0, scope, null)); }
                public void failed(int error) { post(new Event(FAILED, error, 0, null)); }
            });
        } catch (RuntimeException | LinkageError unavailable) {
            handle(active, new Event(FAILED, NsdManager.FAILURE_INTERNAL_ERROR, 0, null));
        }
    }

    private void clearEvents(Request request) {
        queuedEvents -= request.events.size();
        request.events.clear();
        lock.notifyAll();
    }

    private void postBackend(Request request, Event event) {
        // Inline callbacks from construction/rejection cannot wait on the very
        // dispatcher needed to drain backpressure. They already have serial ownership.
        if (dispatching.get()) { handle(request, event); return; }
        boolean schedule = false;
        synchronized (lock) {
            while (!request.client.revoked && !request.stopping
                    && request.client.requests.get(request.clientId) == request
                    && (queuedEvents >= MAX_BACKEND_EVENTS || request.events.size() >= MAX_REQUEST_EVENTS)) {
                try { lock.wait(100); }
                catch (InterruptedException ignored) { /* Ownership/stop is the cancellation signal. */ }
            }
            if (request.client.revoked || request.stopping
                    || request.client.requests.get(request.clientId) != request) return;
            request.events.addLast(event);
            queuedEvents++;
            if (!request.drainScheduled) { request.drainScheduled = true; schedule = true; }
        }
        if (schedule) scheduleDrain(request);
    }

    private void scheduleDrain(Request request) {
        try { dispatcher.execute(onDispatcher(() -> drainEvents(request))); }
        catch (RuntimeException rejected) {
            // The production executor is never shut down. A boundary rejection
            // still has to release producer backpressure and retire ownership.
            retire(request.client);
            throw rejected;
        }
    }

    private void drainEvents(Request request) {
        for (int count = 0; count < 32; count++) {
            Event event;
            synchronized (lock) {
                event = request.events.pollFirst();
                if (event == null) { request.drainScheduled = false; return; }
                queuedEvents--;
                lock.notifyAll();
            }
            handle(request, event);
        }
        synchronized (lock) {
            if (request.events.isEmpty()) { request.drainScheduled = false; return; }
        }
        scheduleDrain(request);
    }

    private void handle(Request request, Event event) {
        if (!current(request) || request.stopping) return;
        if (event.kind == READY) request.ready = true;
        if (event.kind == FAILED) {
            // The subscriber reports failure only during actual initial setup.
            // Established transport interruption uses lost/recovery, not this API error.
            if (request.ready) return;
            synchronized (lock) {
                request.client.requests.remove(request.clientId, request);
                clearEvents(request);
            }
            withdraw(request);
        }
        emit(request.client, request.clientId, event);
        if (event.kind == FAILED) releaseIdle(request.client);
    }

    @Override public void unregister(IClient callback, int id) {
        arguments(callback, id);
        NsdRegistrationService.Owner owner = authority.capture();
        Client client;
        synchronized (lock) {
            client = clients.get(callback.asBinder());
            if (client == null || client.revoked) return;
            if (!client.owner.same(owner)) throw new SecurityException("Different NSD subscription owner");
            Request request = client.requests.get(id);
            if (request == null && !client.pendingStarts.containsKey(id)
                    || request != null && request.stopping
                    || !client.pendingStops.add(id)) return;
            if (request != null) {
                request.stopping = true;
                clearEvents(request);
            }
            client.pending++;
        }
        enqueue(client, id, false, () -> stopOnDispatcher(client, id));
    }

    private void stopOnDispatcher(Client client, int id) {
        if (!current(client)) return;
        Request request;
        synchronized (lock) {
            request = client.requests.get(id);
            if (request == null) return;
            request.stopping = true;
            clearEvents(request);
        }
        WeakReference<Request> weak = new WeakReference<>(request);
        try {
            backend.unregister(request.backendId, () -> dispatcher.execute(onDispatcher(() -> {
                Request target = weak.get();
                if (target == null || !current(target)) return;
                synchronized (lock) {
                    target.client.requests.remove(target.clientId, target);
                    ownedIds.remove(target.backendId);
                    clearEvents(target);
                }
                target.submitted = false;
                emit(target.client, target.clientId, new Event(UNREGISTERED, 0, 0, null));
                releaseIdle(target.client);
            })));
        } catch (RuntimeException | LinkageError retry) {
            // A still-owned stop has no API failure callback. Keep exactly one
            // live completion and retry the actual cleanup, with bounded spacing.
            dispatcher.later(onDispatcher(() -> stopOnDispatcher(client, id)), 1000);
        }
    }

    private void emit(Client client, int id, Event event) {
        if (client.revoked || !authority.current(client.owner)) { retire(client); return; }
        try { client.callback.event(id, event.kind, event.error, event.scope, event.info); }
        catch (RemoteException | RuntimeException unavailable) { retire(client); }
    }

    private void releaseIdle(Client client) {
        boolean idle;
        synchronized (lock) {
            idle = client.pending == 0 && client.requests.isEmpty()
                    && clients.remove(client.token, client);
        }
        if (idle) unlink(client);
    }
    private void unlink(Client client) {
        if (!client.linked.compareAndSet(true, false)) return;
        try { client.token.unlinkToDeath(client, 0); }
        catch (RuntimeException alreadyDead) { /* No published client remains. */ }
    }

    private void withdraw(Request request) {
        boolean pending;
        synchronized (lock) {
            pending = request.submitted;
            request.submitted = false;
            if (pending) withdrawals.add(request.backendId);
            else if (!withdrawals.contains(request.backendId)) ownedIds.remove(request.backendId);
        }
        if (pending) attemptWithdrawal(request.backendId);
        scheduleWithdrawal();
    }
    private void attemptWithdrawal(int id) {
        try { backend.withdraw(id); }
        catch (RuntimeException | LinkageError retry) { return; }
        synchronized (lock) { withdrawals.remove(id); ownedIds.remove(id); }
    }
    private void scheduleWithdrawal() {
        synchronized (lock) {
            if (withdrawals.isEmpty() || retryScheduled) return;
            retryScheduled = true;
        }
        dispatcher.later(onDispatcher(() -> {
            ArrayList<Integer> ids;
            synchronized (lock) { retryScheduled = false; ids = new ArrayList<>(withdrawals); }
            for (int id : ids) attemptWithdrawal(id);
            scheduleWithdrawal();
        }), 1000);
    }

    private void retire(Client client) {
        ArrayList<Request> retired;
        synchronized (lock) {
            if (client.revoked) return;
            client.revoked = true;
            clients.remove(client.token, client);
            retired = new ArrayList<>(client.requests.values());
            for (Request request : retired) { request.stopping = true; clearEvents(request); }
            client.requests.clear();
            lock.notifyAll();
        }
        unlink(client);
        if (!retired.isEmpty()) dispatcher.execute(onDispatcher(() -> {
            for (Request request : retired) withdraw(request);
        }));
    }

    private long nextDueElapsed() {
        synchronized (lock) { if (pendingAdmissions != 0 || !withdrawals.isEmpty()) return 0; }
        return backend.nextDueElapsed();
    }

    void processDied(ActivityTaskManagerService.ProcessRecord process) {
        ArrayList<Client> snapshot;
        synchronized (lock) { snapshot = new ArrayList<>(clients.values()); }
        for (Client client : snapshot) if (client.owner.process == process) retire(client);
    }
    void packageRemoved(String packageName) {
        ArrayList<Client> snapshot;
        synchronized (lock) { snapshot = new ArrayList<>(clients.values()); }
        for (Client client : snapshot) if (client.owner.pkg != null
                && packageName.equals(client.owner.pkg.packageName)) retire(client);
    }

    @Override protected void dump(FileDescriptor descriptor, PrintWriter writer, String[] args) {
        if (!authority.canDump()) throw new SecurityException("NSD diagnostics require system control-plane identity");
        int clientCount, requests = 0, retiring;
        synchronized (lock) {
            clientCount = clients.size();
            for (Client client : clients.values()) requests += client.requests.size();
            retiring = withdrawals.size();
        }
        try {
            int[] state = backend.nativeState();
            if (state == null || state.length != 6) throw new IllegalStateException("Native counter schema mismatch");
            writer.println("NSD_SUBSCRIPTION_STATE pid=" + Process.myPid() + " clients=" + clientCount
                    + " requests=" + requests + " native_requests=" + state[0] + " sockets=" + state[1]
                    + " watches=" + state[2] + " callbacks=" + state[3] + " polled_requests=" + state[4]
                    + " sent_datagrams=" + state[5] + " pending_withdrawals=" + retiring);
        } catch (RuntimeException | LinkageError unavailable) {
            writer.println("NSD_SUBSCRIPTION_NATIVE_UNAVAILABLE pid=" + Process.myPid()
                    + " error=" + unavailable.getClass().getName());
        }
    }
}
