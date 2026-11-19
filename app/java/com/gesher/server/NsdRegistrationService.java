/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.server;

import android.Manifest;
import android.content.pm.PackageManager;
import android.net.nsd.AdvertisingRequest;
import android.net.nsd.NsdManager;
import android.net.nsd.NsdServiceInfo;
import android.os.IBinder;
import android.os.Process;
import android.os.RemoteException;
import android.os.ServiceSpecificException;

import com.gesher.net.INsdRegistration;
import com.gesher.net.NsdRegistrationContract;
import com.gesher.net.WindowsNsdAdvertiser;

import java.util.ArrayList;
import java.util.ArrayDeque;
import java.lang.ref.WeakReference;
import java.util.HashMap;
import java.util.HashSet;
import java.util.Map;
import java.util.Objects;
import java.util.Set;
import java.util.concurrent.Executor;
import java.util.concurrent.Executors;
import java.util.concurrent.ScheduledExecutorService;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicBoolean;
import java.io.FileDescriptor;
import java.io.PrintWriter;

/**
 * System-process ownership of application DNS-SD advertisements.
 *
 * <p>The callback token and the authenticated calling process are separate
 * lifetimes: a callback hosted elsewhere cannot keep a dead application's
 * advertisements alive. The backend owns copied records after logical withdrawal,
 * so pending goodbyes retain no application Binder, Context or executor. In
 * Services.createInProcess the whole service shares the application's process;
 * that collapsed profile cannot deliver a goodbye after its own process dies.
 */
public final class NsdRegistrationService extends INsdRegistration.Stub {
    private static final int MAX_REQUESTS = 200;
    private static final int MAX_CLIENTS = 1024, MAX_OWNED_IDS = 1024;
    private static final int MAX_PENDING_ADMISSIONS = 4096;
    private static final int MAX_PENDING_BACKEND_EVENTS = 4096;
    private final Object mLock = new Object();
    private final Map<IBinder, Client> mClients = new HashMap<>();
    // A retiring ID stays charged until the backend accepts withdrawal. The
    // retry set owns no Client, ProcessRecord, callback or Context reference.
    private final Set<Integer> mOwnedIds = new HashSet<>();
    private final Set<Integer> mWithdrawals = new HashSet<>();
    private int mPendingAdmissions;
    private int mPendingBackendEvents;
    private boolean mRetryScheduled;
    private final Authority mAuthority;
    private final WindowsNsdAdvertiser.Backend mBackend;
    private final Dispatcher mDispatcher;
    // Only the serial dispatcher allocates these identifiers. Never reuse an ID
    // while a backend can still hold a retired record or a completion callback.
    private int mNextId = 1;

    NsdRegistrationService(PackageManagerService pm, ActivityTaskManagerService atm,
            ActivityManagerService am, boolean isolatedServiceProcess) {
        this(new ProcessAuthority(pm, atm, am.callers(), isolatedServiceProcess),
                WindowsNsdAdvertiser.create(),
                dispatcher());
        // Services publishes this Binder only after both retirement hooks exist.
        am.onProcessDeath(this::processDied);
        pm.onPackageRemoval(this::packageRemoved);
        DeviceWork.add(this::nextDueElapsed);
    }

    interface Dispatcher extends Executor {
        void later(Runnable action, long delayMillis);
    }

    private static Dispatcher dispatcher() {
        ScheduledExecutorService serial = Executors.newSingleThreadScheduledExecutor(action -> {
                    Thread thread = new Thread(action, "nsd-registration");
                    thread.setDaemon(true);
                    return thread;
                });
        return new Dispatcher() {
            public void execute(Runnable action) { serial.execute(action); }
            public void later(Runnable action, long delayMillis) {
                serial.schedule(action, delayMillis, TimeUnit.MILLISECONDS);
            }
        };
    }

    /** Explicit scheduling/authority seams; the production constructor uses real services. */
    NsdRegistrationService(Authority authority, WindowsNsdAdvertiser.Backend backend,
            Dispatcher dispatcher) {
        mAuthority = Objects.requireNonNull(authority);
        mBackend = Objects.requireNonNull(backend);
        mDispatcher = Objects.requireNonNull(dispatcher);
    }

    static final class Owner {
        final int pid;
        final int uid;
        final ApkPackage pkg;
        final ActivityTaskManagerService.ProcessRecord process;

        Owner(int pid, int uid, ApkPackage pkg, ActivityTaskManagerService.ProcessRecord process) {
            this.pid = pid;
            this.uid = uid;
            this.pkg = pkg;
            this.process = process;
        }

        boolean same(Owner other) {
            return pid == other.pid && uid == other.uid && pkg == other.pkg
                    && process == other.process;
        }
    }

    interface Authority {
        Owner capture();
        boolean current(Owner owner);
        default boolean canDump() { return false; }
    }

    static final class ProcessAuthority implements Authority {
        private final PackageManagerService mPm;
        private final ActivityTaskManagerService mAtm;
        private final CallerIdentity mCallers;
        private final boolean mIsolatedServiceProcess;

        ProcessAuthority(PackageManagerService pm, ActivityTaskManagerService atm,
                CallerIdentity callers, boolean isolatedServiceProcess) {
            mPm = pm;
            mAtm = atm;
            mCallers = callers;
            mIsolatedServiceProcess = isolatedServiceProcess;
        }

        @Override public boolean canDump() { return mCallers.isSystem(); }

        @Override public Owner capture() {
            int pid = mCallers.callerPid();
            int uid = mCallers.callerUid();
            synchronized (mAtm) {
                ActivityTaskManagerService.ProcessRecord process = mAtm.callerProcess(pid);
                String name = mCallers.packageForPid(pid);
                ApkPackage pkg = name == null ? null : mPm.get(name);
                Owner owner = new Owner(pid, uid, pkg, process);
                if (!current(owner)) {
                    throw new SecurityException("Unrecognized or retired NSD application owner");
                }
                return owner;
            }
        }

        @Override public boolean current(Owner owner) {
            synchronized (mAtm) {
                if (owner.process == null) {
                    // Existing PermissionPolicy permits the actual local service
                    // process. Unknown remote shell/system-looking PIDs get no
                    // ownership merely from their UID or a callback token.
                    return mIsolatedServiceProcess && owner.pkg == null
                            && owner.pid == Process.myPid()
                            && mAtm.callerProcess(owner.pid) == null
                            && mCallers.packageForPid(owner.pid) == null
                            && mPm.permissions().checkProcessPermission(
                                    Manifest.permission.INTERNET, owner.pid, owner.uid)
                                    == PackageManager.PERMISSION_GRANTED;
                }
                ActivityTaskManagerService.ProcessRecord process = owner.process;
                return owner.pkg != null && process.pid == owner.pid && process.uid == owner.uid
                        && owner.pkg.uid() == owner.uid
                        && mAtm.callerProcess(owner.pid) == process
                        && !process.exiting && !process.dead && !process.pidSuperseded
                        && process.packageCode == owner.pkg
                        && owner.pkg.packageName.equals(process.packageName)
                        && mPm.get(owner.pkg.packageName) == owner.pkg
                        && !mPm.isRemovalPending(owner.pkg.packageName)
                        && (process.enabledProcessLifetime == null
                                || process.enabledProcessLifetime.getAsBoolean())
                        && mPm.permissions().checkProcessPermission(
                                Manifest.permission.INTERNET, owner.pid, owner.uid)
                                == PackageManager.PERMISSION_GRANTED;
            }
        }
    }

    private final class Client implements IBinder.DeathRecipient {
        final IClient callback;
        final IBinder token;
        final Owner owner;
        final Map<Integer, Request> requests = new HashMap<>(); // guarded by mLock
        final AtomicBoolean linked = new AtomicBoolean();
        volatile boolean revoked;
        int pending; // admitted dispatcher jobs, guarded by mLock
        final Map<Integer, Integer> pendingRegistrations = new HashMap<>();
        final Set<Integer> pendingStops = new HashSet<>();
        final Set<Integer> pendingRejections = new HashSet<>();

        Client(IClient callback, Owner owner) {
            this.callback = callback;
            token = Objects.requireNonNull(callback.asBinder(), "Missing NSD callback Binder");
            this.owner = owner;
        }

        @Override public void binderDied() { retire(this); }
    }

    private static final class Request {
        final Client client;
        final int clientId;
        final int backendId;
        NsdServiceInfo metadata;
        boolean submitted;
        volatile boolean stopping;
        volatile boolean callbacksEnabled = true;
        final ArrayDeque<BackendEvent> events = new ArrayDeque<>(); // mLock
        boolean eventScheduled, eventOverflow; // mLock

        Request(Client client, int clientId, int backendId, NsdServiceInfo metadata) {
            this.client = client;
            this.clientId = clientId;
            this.backendId = backendId;
            this.metadata = metadata;
        }
    }

    private static final class BackendEvent {
        final String name, hostname;
        final boolean registered, retained;
        final int error;
        BackendEvent(String name, String hostname, boolean registered, int error, boolean retained) {
            this.name = name; this.hostname = hostname; this.registered = registered;
            this.error = error; this.retained = retained;
        }
    }

    private static final class CapacityExceeded extends RuntimeException {}

    @Override public void register(IClient callback, int requestId, AdvertisingRequest request,
            int otherActiveRequests) throws RemoteException {
        checkArguments(callback, requestId);
        Objects.requireNonNull(request, "Missing advertising request");
        NsdRegistrationContract.checkServiceInfoForRegistration(request.getServiceInfo());
        NsdRegistrationContract.checkProtocol(request.getProtocolType());
        if (otherActiveRequests < 0) throw new IllegalArgumentException("Negative request count");
        Owner owner = mAuthority.capture();
        boolean full;
        synchronized (mLock) {
            full = mPendingAdmissions >= MAX_PENDING_ADMISSIONS;
            if (!full) mPendingAdmissions++;
        }
        if (full) { rejectIntake(callback, owner, requestId); return; }
        boolean transferred = false;
        try {
            // Binder may optimize a local call. Preserve initial value ownership,
            // including opaque TXT and the key omitted by the r4 copy constructor.
            AdvertisingRequest snapshot = new AdvertisingRequest.Builder(
                    NsdRegistrationContract.copyForTransport(request.getServiceInfo()),
                    request.getProtocolType()).setFlags(request.getFlags()).setTtl(request.getTtl())
                    .build();
            Client client;
            try { client = acquire(callback, owner); }
            catch (CapacityExceeded exhausted) { rejectIntake(callback, owner, requestId); return; }
            catch (RemoteException unavailable) {
                // RemoteException is not a supported Binder exception-reply
                // category. Use a named private code which the actual client
                // bridge converts to the asynchronous NSD error contract.
                throw new ServiceSpecificException(INsdRegistration.ERROR_CLIENT_UNAVAILABLE,
                        "NSD callback enrollment is unavailable");
            }
            synchronized (mLock) { client.pendingRegistrations.merge(requestId, 1, Integer::sum); }
            transferred = true;
            enqueue(client, requestId, true,
                    () -> registerOnDispatcher(client, requestId, snapshot, otherActiveRequests));
        } finally {
            if (!transferred) synchronized (mLock) { mPendingAdmissions--; }
        }
    }

    private void rejectIntake(IClient callback, Owner owner, int id) {
        Client client;
        boolean ordered = false;
        synchronized (mLock) {
            client = mClients.get(callback.asBinder());
            if (client != null) {
                if (!client.owner.same(owner) || client.revoked) {
                    throw new SecurityException("Different or retired NSD client owner");
                }
                ordered = client.requests.containsKey(id) || client.pendingRegistrations.containsKey(id);
                if (ordered) {
                    if (!client.pendingRejections.add(id)) return;
                    client.pending++;
                }
            }
        }
        if (ordered) {
            Client accepted = client;
            // One reserved terminal entry per already-known ID; it follows all
            // accepted registration/update work without consuming a data slot.
            mDispatcher.execute(() -> {
                try {
                    Request retained;
                    synchronized (mLock) { retained = accepted.requests.get(id); }
                    failAdmission(accepted, id, retained, NsdManager.FAILURE_INTERNAL_ERROR);
                } finally {
                    synchronized (mLock) { accepted.pendingRejections.remove(id); accepted.pending--; }
                    releaseIdle(accepted);
                }
            });
            return;
        }
        if (!mAuthority.current(owner)) { if (client != null) retire(client); return; }
        // Deliver real internal exhaustion through the existing typed callback.
        // No checked-exception serialization or synchronous app failure stands
        // in for the Android asynchronous result.
        try { callback.event(id, FAILED, NsdManager.FAILURE_INTERNAL_ERROR, false, null); }
        catch (RemoteException | RuntimeException unavailable) { if (client != null) retire(client); }
    }

    @Override public void unregister(IClient callback, int requestId) {
        checkArguments(callback, requestId);
        Owner owner = mAuthority.capture();
        Client client;
        synchronized (mLock) {
            client = mClients.get(callback.asBinder());
            if (client == null || client.revoked) return;
            if (!client.owner.same(owner)) throw new SecurityException("Different NSD client owner");
            Request request = client.requests.get(requestId);
            if (request == null && !client.pendingRegistrations.containsKey(requestId)
                    || request != null && request.stopping
                    || !client.pendingStops.add(requestId)) return;
            client.pending++;
        }
        enqueue(client, requestId, false, () -> unregisterOnDispatcher(client, requestId));
    }

    private static void checkArguments(IClient callback, int id) {
        Objects.requireNonNull(callback, "Missing NSD callback");
        if (id <= 0) throw new IllegalArgumentException("Invalid NSD request identifier");
    }

    private Client acquire(IClient callback, Owner owner) throws RemoteException {
        IBinder token = Objects.requireNonNull(callback.asBinder(), "Missing NSD callback Binder");
        synchronized (mLock) {
            Client existing = mClients.get(token);
            if (existing != null) {
                if (!existing.owner.same(owner) || existing.revoked
                        || !mAuthority.current(existing.owner)) {
                    throw new SecurityException("Different or retired NSD client owner");
                }
                existing.pending++;
                return existing;
            }
            if (mClients.size() >= MAX_CLIENTS) throw new CapacityExceeded();
        }
        Client candidate = new Client(callback, owner);
        // Link before publishing. A death callback during link marks this object
        // revoked even though it has not entered mClients yet.
        token.linkToDeath(candidate, 0);
        candidate.linked.set(true);
        Client accepted = null;
        try {
            synchronized (mLock) {
                if (candidate.revoked || !token.isBinderAlive() || !mAuthority.current(owner)) {
                    throw new RemoteException("NSD client retired during registration");
                }
                Client existing = mClients.get(token);
                if (existing == null) {
                    if (mClients.size() >= MAX_CLIENTS) {
                        throw new CapacityExceeded();
                    }
                    mClients.put(token, candidate);
                    accepted = candidate;
                } else {
                    if (!existing.owner.same(owner) || existing.revoked
                            || !mAuthority.current(existing.owner)) {
                        throw new SecurityException("Different or retired NSD client owner");
                    }
                    accepted = existing;
                }
                accepted.pending++;
            }
            return accepted;
        } finally {
            if (accepted != candidate) unlink(candidate);
        }
    }

    private void enqueue(Client client, int id, boolean admission, Runnable action) {
        AtomicBoolean finished = new AtomicBoolean();
        Runnable finish = () -> {
            if (!finished.compareAndSet(false, true)) return;
            synchronized (mLock) {
                client.pending--;
                if (admission) {
                    mPendingAdmissions--;
                    int count = client.pendingRegistrations.get(id);
                    if (count == 1) {
                        client.pendingRegistrations.remove(id);
                    }
                    else client.pendingRegistrations.put(id, count - 1);
                } else client.pendingStops.remove(id);
            }
            releaseIdle(client);
        };
        try {
            mDispatcher.execute(() -> {
                try { action.run(); }
                finally { finish.run(); }
            });
        } catch (RuntimeException failure) {
            finish.run();
            throw failure;
        }
    }

    private boolean current(Client client) {
        if (client.revoked || !mAuthority.current(client.owner)) {
            retire(client);
            return false;
        }
        synchronized (mLock) { return mClients.get(client.token) == client; }
    }

    private boolean current(Request request) {
        if (!current(request.client)) return false;
        synchronized (mLock) {
            return request.client.requests.get(request.clientId) == request;
        }
    }

    private void registerOnDispatcher(Client client, int clientId, AdvertisingRequest request,
            int other) {
        if (!current(client)) return;
        boolean update = (request.getFlags() & AdvertisingRequest.NSD_ADVERTISING_UPDATE_ONLY) != 0;
        Request existing;
        boolean full;
        synchronized (mLock) {
            existing = client.requests.get(clientId);
            // Independent of the caller-supplied count, real registrations can
            // never exceed the client limit, including retained failed updates.
            full = client.requests.size() >= MAX_REQUESTS
                    || (long) client.requests.size() + other >= MAX_REQUESTS
                    || existing == null && mOwnedIds.size() >= MAX_OWNED_IDS;
        }
        if (full) {
            failAdmission(client, clientId, existing, NsdManager.FAILURE_MAX_LIMIT);
            return;
        }
        if (update != (existing != null)) {
            failAdmission(client, clientId, existing, NsdManager.FAILURE_INTERNAL_ERROR);
            return;
        }
        NsdRegistrationContract.Prepared prepared;
        try { prepared = NsdRegistrationContract.normalize(request, client.owner.uid); }
        catch (NsdRegistrationContract.Failure failure) {
            failAdmission(client, clientId, existing, failure.error);
            return;
        }
        if (!current(client)) return;
        if (update) {
            if (!current(existing) || existing.stopping || !existing.callbacksEnabled) return;
            existing.metadata = prepared.service;
            try { mBackend.update(existing.backendId, prepared.service, prepared.options); }
            catch (RuntimeException | LinkageError failure) {
                failed(existing, NsdManager.FAILURE_INTERNAL_ERROR, true);
            }
            return;
        }
        if (mNextId <= 0) {
            failAdmission(client, clientId, null, NsdManager.FAILURE_INTERNAL_ERROR);
            return;
        }
        Request added = new Request(client, clientId, mNextId++, prepared.service);
        synchronized (mLock) {
            if (client.revoked || mClients.get(client.token) != client) return;
            mOwnedIds.add(added.backendId);
            client.requests.put(clientId, added);
        }
        if (!current(added)) return;
        added.submitted = true;
        // The backend may still be retiring after the service revokes this
        // client. Its callback bridge must not prolong application ownership.
        WeakReference<Request> reference = new WeakReference<>(added);
        try {
            mBackend.register(added.backendId, prepared.service, prepared.options, client.owner.uid,
                    new WindowsNsdAdvertiser.Callback() {
                        @Override public void registered(NsdServiceInfo info) {
                            Request target = reference.get();
                            if (target == null || target.client.revoked || !target.callbacksEnabled) return;
                            postBackendEvent(target, new BackendEvent(info.getServiceName(),
                                    info.getHostname(), true, 0, false));
                        }
                        @Override public void failed(int error, boolean retained) {
                            Request target = reference.get();
                            if (target == null || target.client.revoked || !target.callbacksEnabled) return;
                            postBackendEvent(target, new BackendEvent(null, null, false, error, retained));
                        }
                    });
        } catch (RuntimeException | LinkageError failure) {
            failed(added, NsdManager.FAILURE_INTERNAL_ERROR, false);
        }
    }

    private void clearBackendEvents(Request request) {
        mPendingBackendEvents -= request.events.size();
        request.events.clear();
    }

    private void postBackendEvent(Request request, BackendEvent event) {
        boolean schedule = false;
        synchronized (mLock) {
            if (request.client.revoked || !request.callbacksEnabled || request.eventOverflow
                    || request.client.requests.get(request.clientId) != request) return;
            if (mPendingBackendEvents >= MAX_PENDING_BACKEND_EVENTS) {
                // Reserve one terminal notification in this already-owned ID.
                // Exhaustion becomes an honest asynchronous failure and cleanup.
                clearBackendEvents(request);
                request.eventOverflow = true;
            } else {
                request.events.add(event);
                mPendingBackendEvents++;
            }
            if (!request.eventScheduled) { request.eventScheduled = true; schedule = true; }
        }
        if (schedule) mDispatcher.execute(() -> drainBackendEvents(request));
    }

    private void drainBackendEvents(Request request) {
        ArrayList<BackendEvent> events;
        boolean overflow;
        synchronized (mLock) {
            events = new ArrayList<>(request.events);
            clearBackendEvents(request);
            overflow = request.eventOverflow;
            request.eventScheduled = false;
        }
        if (overflow) { failed(request, NsdManager.FAILURE_INTERNAL_ERROR, false); return; }
        for (BackendEvent event : events) {
            if (event.registered) registered(request, event.name, event.hostname);
            else failed(request, event.error, event.retained);
        }
    }

    private void failAdmission(Client client, int id, Request retained, int error) {
        if (!current(client)) return;
        if (retained != null) retained.callbacksEnabled = false;
        emit(client, id, FAILED, error, retained != null, null);
    }

    private void registered(Request request, String name, String hostname) {
        if (!current(request) || request.stopping || !request.callbacksEnabled) return;
        NsdServiceInfo result = new NsdServiceInfo(name, null);
        result.setHostname(hostname);
        emit(request.client, request.clientId, REGISTERED, 0, false, result);
    }

    private void failed(Request request, int error, boolean retained) {
        if (!current(request) || request.stopping || !request.callbacksEnabled) return;
        request.callbacksEnabled = false;
        synchronized (mLock) { clearBackendEvents(request); }
        if (!retained) {
            synchronized (mLock) { request.client.requests.remove(request.clientId, request); }
            withdraw(request);
        }
        emit(request.client, request.clientId, FAILED, error, retained, null);
        releaseIdle(request.client);
    }

    private void unregisterOnDispatcher(Client client, int id) {
        if (!current(client)) return;
        Request request;
        synchronized (mLock) { request = client.requests.get(id); }
        if (request == null || request.stopping) return;
        request.stopping = true;
        WeakReference<Request> reference = new WeakReference<>(request);
        try {
            mBackend.unregister(request.backendId, () -> mDispatcher.execute(() -> {
                Request target = reference.get();
                if (target == null || !current(target)) return;
                Client active = target.client;
                synchronized (mLock) {
                    active.requests.remove(target.clientId, target);
                    mOwnedIds.remove(target.backendId);
                }
                target.submitted = false;
                target.callbacksEnabled = false;
                // Logical withdrawal is complete. The backend now retains only
                // independent record values for any genuine pending goodbyes.
                releaseIdle(active);
                emit(active, target.clientId, UNREGISTERED, 0, false, null);
            }));
        } catch (RuntimeException | LinkageError failure) {
            request.stopping = false;
            request.callbacksEnabled = false;
            // Keep ownership for death/uninstall cleanup if withdrawal failed.
            emit(client, id, UNREGISTER_FAILED, NsdManager.FAILURE_INTERNAL_ERROR, true, null);
        }
    }

    private void emit(Client client, int id, int event, int error, boolean retained,
            NsdServiceInfo info) {
        if (client.revoked || !mAuthority.current(client.owner)) {
            retire(client);
            return;
        }
        try { client.callback.event(id, event, error, retained, info); }
        catch (RemoteException | RuntimeException unavailable) { retire(client); }
    }

    private void releaseIdle(Client client) {
        boolean idle;
        synchronized (mLock) {
            idle = client.pending == 0 && client.requests.isEmpty()
                    && mClients.remove(client.token, client);
        }
        if (idle) unlink(client);
    }

    private void unlink(Client client) {
        if (!client.linked.compareAndSet(true, false)) return;
        try { client.token.unlinkToDeath(client, 0); }
        catch (RuntimeException alreadyDead) { /* The client is no longer published. */ }
    }

    private void withdraw(Request request) {
        boolean added = false;
        synchronized (mLock) {
            if (request.submitted) {
                added = mWithdrawals.add(request.backendId);
                request.submitted = false;
            } else if (!mWithdrawals.contains(request.backendId)) {
                mOwnedIds.remove(request.backendId);
            }
        }
        if (added) attemptWithdrawal(request.backendId);
        scheduleWithdrawalRetry();
    }

    private void attemptWithdrawal(int id) {
        try { mBackend.withdraw(id); }
        catch (RuntimeException | LinkageError unavailable) { return; }
        synchronized (mLock) { mWithdrawals.remove(id); mOwnedIds.remove(id); }
    }

    private void drainWithdrawals() {
        ArrayList<Integer> ids;
        synchronized (mLock) { ids = new ArrayList<>(mWithdrawals); }
        for (int id : ids) attemptWithdrawal(id);
        scheduleWithdrawalRetry();
    }

    private void scheduleWithdrawalRetry() {
        boolean pending;
        synchronized (mLock) { pending = !mWithdrawals.isEmpty(); }
        if (pending && !mRetryScheduled) {
            mRetryScheduled = true;
            mDispatcher.later(() -> {
                mRetryScheduled = false;
                drainWithdrawals();
            }, 1000);
        }
    }

    private long nextDueElapsed() {
        synchronized (mLock) {
            if (mPendingAdmissions != 0 || !mWithdrawals.isEmpty()) return 0;
        }
        return mBackend.nextDueElapsed();
    }

    private void retire(Client client) {
        ArrayList<Request> retired;
        synchronized (mLock) {
            if (client.revoked) return;
            client.revoked = true;
            mClients.remove(client.token, client);
            retired = new ArrayList<>(client.requests.values());
            for (Request request : retired) clearBackendEvents(request);
            client.requests.clear();
        }
        unlink(client);
        // Queue even when a register call is currently submitting the request.
        // submitted is read on the same dispatcher after that call finishes.
        if (!retired.isEmpty()) mDispatcher.execute(() -> {
            for (Request request : retired) withdraw(request);
        });
    }

    @Override protected void dump(FileDescriptor fd, PrintWriter writer, String[] args) {
        if (!mAuthority.canDump()) throw new SecurityException("NSD diagnostics require system control-plane identity");
        int clients;
        int requests = 0;
        int withdrawals;
        synchronized (mLock) {
            clients = mClients.size();
            for (Client client : mClients.values()) requests += client.requests.size();
            withdrawals = mWithdrawals.size();
        }
        try {
            int[] nativeState = mBackend.nativeState();
            if (nativeState == null || nativeState.length != 6) {
                throw new IllegalStateException("Unexpected native NSD counter schema");
            }
            writer.println("NSD_REGISTRATION_STATE pid=" + Process.myPid() + " clients=" + clients
                    + " requests=" + requests + " native_requests=" + nativeState[0]
                    + " sockets=" + nativeState[1] + " watches=" + nativeState[2]
                    + " callbacks=" + nativeState[3] + " polled_requests=" + nativeState[4]
                    + " sent_datagrams=" + nativeState[5]
                    + " pending_withdrawals=" + withdrawals);
        } catch (RuntimeException | LinkageError unavailable) {
            writer.println("NSD_REGISTRATION_NATIVE_UNAVAILABLE pid=" + Process.myPid()
                    + " error=" + unavailable.getClass().getName());
        }
    }

    void processDied(ActivityTaskManagerService.ProcessRecord process) {
        ArrayList<Client> clients;
        synchronized (mLock) { clients = new ArrayList<>(mClients.values()); }
        for (Client client : clients) if (client.owner.process == process) retire(client);
    }

    void packageRemoved(String packageName) {
        ArrayList<Client> clients;
        synchronized (mLock) { clients = new ArrayList<>(mClients.values()); }
        for (Client client : clients) {
            if (client.owner.pkg != null && packageName.equals(client.owner.pkg.packageName)) {
                retire(client);
            }
        }
    }
}
