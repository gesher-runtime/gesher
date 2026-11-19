/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package android.net.nsd;

import android.content.Context;
import android.net.Network;
import android.net.ConnectivityManager;
import android.net.NetworkRequest;
import android.os.Handler;
import android.os.HandlerThread;
import com.gesher.net.WindowsNsd;
import com.gesher.net.WindowsNsdResolver;
import com.gesher.net.NsdRegistrationClient;
import com.gesher.net.NsdRegistrationContract;
import com.gesher.net.NsdSubscriptionClient;

import java.io.IOException;
import java.util.IdentityHashMap;
import java.util.Map;
import java.util.Objects;
import java.util.concurrent.Executor;
import java.util.concurrent.RejectedExecutionException;

/**
 * Android 16 NSD browsing, registration and one-shot resolution over actual
 * Windows networks. Registration is owned by the system service process; raw
 * DNS packets preserve binary TXT. Continuing service-info subscriptions have
 * request-owned sockets in the system service process.
 */
public final class NsdManager {
    public static final String ACTION_NSD_STATE_CHANGED = "android.net.nsd.STATE_CHANGED";
    public static final String EXTRA_NSD_STATE = "nsd_state";
    public static final int NSD_STATE_DISABLED = 1;
    public static final int NSD_STATE_ENABLED = 2;

    public static final int PROTOCOL_DNS_SD = 0x0001;

    public static final int FAILURE_INTERNAL_ERROR = 0;
    public static final int FAILURE_ALREADY_ACTIVE = 3;
    public static final int FAILURE_MAX_LIMIT = 4;
    public static final int FAILURE_OPERATION_NOT_RUNNING = 5;
    public static final int FAILURE_BAD_PARAMETERS = 6;

    public interface DiscoveryListener {
        void onStartDiscoveryFailed(String serviceType, int errorCode);

        void onStopDiscoveryFailed(String serviceType, int errorCode);

        void onDiscoveryStarted(String serviceType);

        void onDiscoveryStopped(String serviceType);

        void onServiceFound(NsdServiceInfo serviceInfo);

        void onServiceLost(NsdServiceInfo serviceInfo);
    }

    public interface RegistrationListener {
        void onRegistrationFailed(NsdServiceInfo serviceInfo, int errorCode);

        void onUnregistrationFailed(NsdServiceInfo serviceInfo, int errorCode);

        void onServiceRegistered(NsdServiceInfo serviceInfo);

        void onServiceUnregistered(NsdServiceInfo serviceInfo);
    }

    public interface ResolveListener {
        void onResolveFailed(NsdServiceInfo serviceInfo, int errorCode);

        void onServiceResolved(NsdServiceInfo serviceInfo);

        default void onResolutionStopped(NsdServiceInfo serviceInfo) {
        }

        default void onStopResolutionFailed(NsdServiceInfo serviceInfo, int errorCode) {
        }
    }

    public interface ServiceInfoCallback {
        void onServiceInfoCallbackRegistrationFailed(int errorCode);

        void onServiceUpdated(NsdServiceInfo serviceInfo);

        void onServiceLost();

        void onServiceInfoCallbackUnregistered();
    }

    private final Executor mMainExecutor = Runnable::run;
    private final Executor mServiceExecutor;
    private final WindowsNsd.Factory mBackend;
    private final WindowsNsdResolver.Factory mResolverBackend;
    private final NsdRegistrationClient.Factory mRegistrationBackend;
    private final NsdSubscriptionClient.Factory mSubscriptionBackend;
    private final Context mContext;
    private final Map<DiscoveryListener, Discovery> mDiscoveries = new IdentityHashMap<>();
    // All listener identity maps use mDiscoveries as their common lock.
    private final Map<ResolveListener, Resolution> mResolutions = new IdentityHashMap<>();
    private final Map<RegistrationListener, Registration> mRegistrations = new IdentityHashMap<>();
    private final Map<ServiceInfoCallback, Subscription> mSubscriptions = new IdentityHashMap<>();

    private static final class Subscription {
        final Executor executor;
        NsdSubscriptionClient.Session session;
        boolean stopPending, stopSent, terminalDelivered;
        Subscription(Executor executor) { this.executor = executor; }
    }

    private static final class Registration {
        NsdServiceInfo original;
        Executor executor;
        NsdRegistrationClient.Session session;
        boolean stopPending;
        boolean draining;
        final java.util.ArrayDeque<RegistrationCommand> commands = new java.util.ArrayDeque<>();
        Registration(NsdServiceInfo original, Executor executor) {
            this.original = original; this.executor = executor;
        }
    }

    private static final class RegistrationCommand {
        final AdvertisingRequest update; // null denotes logical unregister
        final int otherActiveRequests;
        RegistrationCommand(AdvertisingRequest update, int otherActiveRequests) {
            this.update = update; this.otherActiveRequests = otherActiveRequests;
        }
    }

    private static final class Resolution {
        final NsdServiceInfo original;
        final Executor executor;
        WindowsNsdResolver.Session session;
        boolean stopPending;
        Resolution(NsdServiceInfo original, Executor executor) {
            this.original = original; this.executor = executor;
        }
    }

    private static final class CallbackThread {
        static final Executor EXECUTOR;
        static {
            HandlerThread thread = new HandlerThread("Gesher-NsdManager");
            thread.setDaemon(true);
            thread.start();
            Handler handler = new Handler(thread.getLooper());
            EXECUTOR = command -> {
                if (!handler.post(command)) throw new RejectedExecutionException("NSD callback thread stopped");
            };
        }
    }
    private static final class Discovery {
        final String serviceType;
        final Executor executor;
        WindowsNsd.Session session;
        boolean stopPending;
        ConnectivityManager connectivity;
        ConnectivityManager.NetworkCallback networks;
        boolean networksRegistered;
        long networkGeneration;
        final Map<Integer, Long> selected = new java.util.HashMap<>();
        Discovery(String serviceType, Executor executor) {
            this.serviceType = serviceType;
            this.executor = executor;
        }
    }

    public NsdManager(Context context) {
        if (context != null) {
            context.enforcePermission("android.permission.INTERNET",
                    android.os.Process.myPid(), android.os.Process.myUid(), "NsdService");
            Context app = context.getApplicationContext();
            mContext = app != null ? app : context;
        } else mContext = null;
        WindowsNsd.enforceInternetPermission();
        mServiceExecutor = CallbackThread.EXECUTOR;
        mBackend = WindowsNsd.factory();
        mResolverBackend = WindowsNsdResolver.factory();
        mRegistrationBackend = NsdRegistrationClient.factory();
        mSubscriptionBackend = NsdSubscriptionClient.factory();
    }

    // Explicit fixture injection; ordinary construction always selects WindowsNsd.
    private NsdManager(Executor serviceExecutor, WindowsNsd.Factory backend) {
        this(null, serviceExecutor, backend);
    }

    private NsdManager(Context context, Executor serviceExecutor, WindowsNsd.Factory backend) {
        this(context, serviceExecutor, backend, WindowsNsdResolver.factory());
    }

    private NsdManager(Context context, Executor serviceExecutor, WindowsNsd.Factory backend,
            WindowsNsdResolver.Factory resolverBackend) {
        this(context, serviceExecutor, backend, resolverBackend, NsdRegistrationClient.factory());
    }

    private NsdManager(Context context, Executor serviceExecutor, WindowsNsd.Factory backend,
            WindowsNsdResolver.Factory resolverBackend,
            NsdRegistrationClient.Factory registrationBackend) {
        this(context, serviceExecutor, backend, resolverBackend, registrationBackend,
                NsdSubscriptionClient.factory());
    }

    private NsdManager(Context context, Executor serviceExecutor, WindowsNsd.Factory backend,
            WindowsNsdResolver.Factory resolverBackend,
            NsdRegistrationClient.Factory registrationBackend,
            NsdSubscriptionClient.Factory subscriptionBackend) {
        mContext = context;
        mServiceExecutor = Objects.requireNonNull(serviceExecutor);
        mBackend = Objects.requireNonNull(backend);
        mResolverBackend = Objects.requireNonNull(resolverBackend);
        mRegistrationBackend = Objects.requireNonNull(registrationBackend);
        mSubscriptionBackend = Objects.requireNonNull(subscriptionBackend);
    }

    // --- registering ---------------------------------------------------------------

    public void registerService(NsdServiceInfo serviceInfo, int protocolType,
            RegistrationListener listener) {
        registerService(serviceInfo, protocolType, mMainExecutor, listener);
    }

    public void registerService(NsdServiceInfo serviceInfo, int protocolType,
            Executor executor, RegistrationListener listener) {
        NsdRegistrationContract.checkServiceInfoForRegistration(serviceInfo);
        NsdRegistrationContract.checkProtocol(protocolType);
        AdvertisingRequest.Builder builder = new AdvertisingRequest.Builder(serviceInfo, protocolType);
        synchronized (mDiscoveries) {
            Registration existing = mRegistrations.get(listener);
            if (existing != null && NsdRegistrationContract.isSubtypeUpdate(
                    existing.original, serviceInfo)) {
                builder.setFlags(AdvertisingRequest.NSD_ADVERTISING_UPDATE_ONLY);
            }
        }
        registerService(builder.build(), executor, listener);
    }

    /** Additional pinned-source profile: preserve every supplied advertising option. */
    public void registerService(AdvertisingRequest request, Executor executor,
            RegistrationListener listener) {
        Objects.requireNonNull(request, "AdvertisingRequest cannot be null");
        NsdServiceInfo original = request.getServiceInfo();
        NsdRegistrationContract.checkServiceInfoForRegistration(original);
        NsdRegistrationContract.checkProtocol(request.getProtocolType());
        Objects.requireNonNull(listener, "listener cannot be null");
        // The r4 manager does not add an explicit executor-null check here.
        // Preserve original request identity for failure/unregister callbacks;
        // the real Binder-equivalent boundary owns the independent byte snapshot.
        AdvertisingRequest owned = new AdvertisingRequest.Builder(
                NsdRegistrationContract.copyForTransport(original), request.getProtocolType())
                .setFlags(request.getFlags()).setTtl(request.getTtl()).build();
        boolean update = (request.getFlags() & AdvertisingRequest.NSD_ADVERTISING_UPDATE_ONLY) != 0;
        Registration registration;
        int other;
        synchronized (mDiscoveries) {
            registration = mRegistrations.get(listener);
            if (update) {
                if (registration == null) throw new IllegalArgumentException("listener not registered");
                registration.original = original;
                registration.executor = executor;
            } else {
                if (registration != null || mDiscoveries.containsKey(listener)
                        || mResolutions.containsKey(listener) || mSubscriptions.containsKey(listener)) {
                    throw new IllegalArgumentException("listener already in use");
                }
                registration = new Registration(original, executor);
                mRegistrations.put(listener, registration);
            }
            // The service owns the registration/retained-update portion of the
            // quota; it must not trust this caller to under-report those entries.
            other = mDiscoveries.size() + mResolutions.size() + mSubscriptions.size()
                    + mSubscriptionBackend.retainedCount();
            if (update) registration.commands.addLast(new RegistrationCommand(owned, other));
        }
        if (update) {
            drainRegistration(registration);
            return;
        }
        final Registration record = registration;
        try {
            NsdRegistrationClient.Session session = mRegistrationBackend.start(owned, other,
                    new NsdRegistrationClient.Callback() {
                        public void registered(NsdServiceInfo result) {
                            registrationEvent(listener, record, false,
                                    (value, sink) -> sink.onServiceRegistered(result));
                        }
                        public void failed(int error, boolean retained) {
                            registrationEvent(listener, record, true,
                                    (value, sink) -> sink.onRegistrationFailed(value, error));
                        }
                        public void unregistered() {
                            registrationEvent(listener, record, true,
                                    (value, sink) -> sink.onServiceUnregistered(value));
                        }
                        public void unregisterFailed(int error) {
                            registrationEvent(listener, record, true,
                                    (value, sink) -> sink.onUnregistrationFailed(value, error));
                        }
                    });
            synchronized (mDiscoveries) { record.session = session; }
            drainRegistration(record);
        } catch (RuntimeException | Error failure) {
            synchronized (mDiscoveries) {
                if (mRegistrations.get(listener) == record) mRegistrations.remove(listener);
                record.commands.clear();
            }
            throw failure;
        }
    }

    private interface RegistrationEvent {
        void deliver(NsdServiceInfo original, RegistrationListener listener);
    }

    private void registrationEvent(RegistrationListener listener, Registration registration,
            boolean terminal, RegistrationEvent event) {
        mServiceExecutor.execute(() -> {
            NsdServiceInfo original;
            Executor executor;
            synchronized (mDiscoveries) {
                if (mRegistrations.get(listener) != registration) return;
                original = registration.original;
                executor = registration.executor;
                if (terminal) {
                    mRegistrations.remove(listener);
                    registration.commands.clear();
                }
            }
            try {
                executor.execute(() -> event.deliver(original, listener));
            } catch (RejectedExecutionException rejected) {
                if (!terminal) {
                    synchronized (mDiscoveries) {
                        if (mRegistrations.get(listener) == registration) mRegistrations.remove(listener);
                        registration.commands.clear();
                        if (!registration.stopPending) {
                            registration.stopPending = true;
                            registration.commands.add(new RegistrationCommand(null, 0));
                        }
                    }
                    drainRegistration(registration);
                }
                throw rejected;
            }
        });
    }

    // A synchronous callback can enqueue an update/unregister before start()
    // returns its session. One drain preserves submission order across that
    // handoff without invoking the backend while holding a listener-map lock.
    private void drainRegistration(Registration registration) {
        synchronized (mDiscoveries) {
            if (registration.session == null || registration.draining) return;
            registration.draining = true;
        }
        try {
            for (;;) {
                RegistrationCommand command;
                synchronized (mDiscoveries) {
                    command = registration.commands.pollFirst();
                    if (command == null) {
                        registration.draining = false;
                        return;
                    }
                }
                if (command.update == null) registration.session.stop();
                else registration.session.update(command.update, command.otherActiveRequests);
            }
        } catch (RuntimeException | Error failure) {
            synchronized (mDiscoveries) { registration.draining = false; }
            throw failure;
        }
    }

    public void unregisterService(RegistrationListener listener) {
        Objects.requireNonNull(listener, "listener cannot be null");
        Registration registration;
        synchronized (mDiscoveries) {
            registration = mRegistrations.get(listener);
            if (registration == null) throw new IllegalArgumentException("listener not registered");
            if (!registration.stopPending) {
                registration.stopPending = true;
                registration.commands.addLast(new RegistrationCommand(null, 0));
            }
        }
        drainRegistration(registration);
    }

    // --- discovering ---------------------------------------------------------------

    public void discoverServices(String serviceType, int protocolType,
            DiscoveryListener listener) {
        discoverServices(serviceType, protocolType, (Network) null, mMainExecutor, listener);
    }

    public void discoverServices(String serviceType, int protocolType, Network network,
            Executor executor, DiscoveryListener listener) {
        startDiscovery(serviceType, null, protocolType, network, null, executor, listener);
    }

    public void discoverServices(DiscoveryRequest request, Executor executor,
            DiscoveryListener listener) {
        Objects.requireNonNull(request, "DiscoveryRequest cannot be null");
        startDiscovery(request.getServiceType(), request.getSubtype(), request.getProtocolType(),
                request.getNetwork(), null, executor, listener);
    }

    public void discoverServices(String serviceType, int protocolType, NetworkRequest request,
            Executor executor, DiscoveryListener listener) {
        Objects.requireNonNull(request, "NetworkRequest cannot be null");
        startDiscovery(serviceType, null, protocolType, null, request, executor, listener);
    }

    private void dispatch(DiscoveryListener listener, Discovery discovery, boolean terminal,
            Runnable callback) {
        mServiceExecutor.execute(() -> {
            synchronized (mDiscoveries) {
                if (mDiscoveries.get(listener) != discovery) return;
                if (terminal) mDiscoveries.remove(listener);
            }
            if (terminal) detachNetworks(discovery);
            try {
                // AOSP submits callbacks in handler order. An application-supplied
                // executor still controls their execution order and concurrency.
                discovery.executor.execute(callback);
            } catch (RejectedExecutionException error) {
                synchronized (mDiscoveries) {
                    if (mDiscoveries.get(listener) == discovery) mDiscoveries.remove(listener);
                    discovery.stopPending = true;
                    if (discovery.session != null) discovery.session.stop();
                }
                if (!terminal) detachNetworks(discovery);
                throw error;
            }
        });
    }

    private static void detachNetworks(Discovery discovery) {
        ConnectivityManager connectivity;
        ConnectivityManager.NetworkCallback networks;
        synchronized (discovery) {
            connectivity = discovery.connectivity;
            networks = discovery.networksRegistered ? discovery.networks : null;
            discovery.networksRegistered = false;
            discovery.networks = null;
            discovery.connectivity = null;
        }
        if (networks != null) connectivity.unregisterNetworkCallback(networks);
    }

    private void startDiscovery(String serviceType, String subtype, int protocolType,
            Network network, NetworkRequest request, Executor executor, DiscoveryListener listener) {
        if (serviceType == null || serviceType.isEmpty()) {
            throw new IllegalArgumentException("Service type cannot be empty");
        }
        checkProtocol(protocolType);
        Objects.requireNonNull(executor, "executor cannot be null");
        Objects.requireNonNull(listener, "listener cannot be null");
        synchronized (mDiscoveries) {
            // API36 rejects an already-used listener before the NetworkRequest
            // tracker reaches ConnectivityManager's permission check. Do not
            // reserve a new listener if that permission check then fails.
            if (mDiscoveries.containsKey(listener) || mResolutions.containsKey(listener)
                    || mRegistrations.containsKey(listener) || mSubscriptions.containsKey(listener)) {
                throw new IllegalArgumentException("listener already in use");
            }
        }
        if (request != null && mContext != null) {
            mContext.enforcePermission("android.permission.ACCESS_NETWORK_STATE",
                    android.os.Process.myPid(), android.os.Process.myUid(), "NsdService");
        }
        Discovery discovery = new Discovery(serviceType, executor);
        boolean overLimit;
        synchronized (mDiscoveries) {
            // Another thread may have registered it during permission checking.
            if (mDiscoveries.containsKey(listener) || mResolutions.containsKey(listener)
                    || mRegistrations.containsKey(listener) || mSubscriptions.containsKey(listener)) {
                throw new IllegalArgumentException("listener already in use");
            }
            // The service applies discovery's quota to the whole active client
            // request map. The Java one-shot resolve path has no such admission
            // check, but its active requests still count when discovery starts.
            overLimit = mDiscoveries.size() + mResolutions.size() + mRegistrations.size()
                    + mSubscriptions.size() + mSubscriptionBackend.retainedCount()
                    + mRegistrationBackend.retainedCount() >= 200;
            mDiscoveries.put(listener, discovery);
        }
        if (overLimit) {
            dispatch(listener, discovery, true, () ->
                    listener.onStartDiscoveryFailed(serviceType, FAILURE_MAX_LIMIT));
            return;
        }
        try {
            WindowsNsd.Scope scope;
            if (request != null) {
                if (mContext == null) {
                    dispatch(listener, discovery, true, () -> listener.onStartDiscoveryFailed(
                            serviceType, FAILURE_INTERNAL_ERROR));
                    return;
                }
                discovery.connectivity = mContext.getSystemService(ConnectivityManager.class);
                discovery.networks = new ConnectivityManager.NetworkCallback() {
                    @Override public void onAvailable(Network value) {
                        synchronized (discovery.selected) {
                            int index = value.netId - 100;
                            if (!discovery.selected.containsKey(index)) {
                                discovery.selected.put(index, ++discovery.networkGeneration);
                            }
                        }
                    }
                    @Override public void onLost(Network value) {
                        synchronized (discovery.selected) { discovery.selected.remove(value.netId - 100); }
                    }
                };
                discovery.connectivity.registerNetworkCallback(request, discovery.networks);
                discovery.networksRegistered = true;
                scope = new WindowsNsd.Scope() {
                    @Override public int[] interfaces() {
                        synchronized (discovery.selected) {
                            int[] indices = new int[discovery.selected.size()];
                            int i = 0;
                            for (int index : discovery.selected.keySet()) indices[i++] = index;
                            return indices;
                        }
                    }
                    @Override public Map<Integer, Long> snapshot() {
                        synchronized (discovery.selected) {
                            return new java.util.HashMap<>(discovery.selected);
                        }
                    }
                };
            } else if (network != null) {
                int index = network.netId - 100;
                scope = () -> {
                    if (network.netId <= 100 || index <= 0) throw new IOException("Invalid network");
                    for (int present : mBackend.interfaces()) if (present == index) return new int[] {index};
                    // Never send a network-bound request over a different adapter.
                    return new int[0];
                };
            } else scope = mBackend::interfaces;

            WindowsNsd.Session session = mBackend.start(serviceType, subtype, scope,
                    new WindowsNsd.Callback() {
                        public void started() {
                            dispatch(listener, discovery, false, () -> listener.onDiscoveryStarted(serviceType));
                        }
                        private NsdServiceInfo info(String name, String type, int index, long expiry, String subtype) {
                            NsdServiceInfo info = new NsdServiceInfo();
                            info.setServiceName(name);
                            info.setServiceType(type);
                            info.setNetwork(new Network(index + 100));
                            if (expiry >= 0) info.setExpirationTime(java.time.Instant.ofEpochMilli(expiry));
                            if (subtype != null) info.setSubtypes(java.util.Set.of(subtype));
                            return info;
                        }
                        public void found(String name, String type, int index) { found(name, type, index, -1, null); }
                        public void found(String name, String type, int index, long expiry, String subtype) {
                            NsdServiceInfo value = info(name, type, index, expiry, subtype);
                            dispatch(listener, discovery, false, () -> listener.onServiceFound(value));
                        }
                        public void lost(String name, String type, int index) { lost(name, type, index, -1, null); }
                        public void lost(String name, String type, int index, long expiry, String subtype) {
                            NsdServiceInfo value = info(name, type, index, expiry, subtype);
                            dispatch(listener, discovery, false, () -> listener.onServiceLost(value));
                        }
                        public void stopped() {
                            dispatch(listener, discovery, true, () -> listener.onDiscoveryStopped(serviceType));
                        }
                        public void stopFailed(int error) {
                            dispatch(listener, discovery, true, () -> listener.onStopDiscoveryFailed(serviceType, error));
                        }
                        public void failed(int error) {
                            dispatch(listener, discovery, true, () -> listener.onStartDiscoveryFailed(serviceType, error));
                        }
                    });
            synchronized (mDiscoveries) {
                discovery.session = session;
                if (discovery.stopPending) session.stop();
            }
        } catch (RuntimeException | Error error) {
            synchronized (mDiscoveries) {
                if (mDiscoveries.get(listener) == discovery) mDiscoveries.remove(listener);
                if (discovery.session != null) discovery.session.stop();
            }
            try { detachNetworks(discovery); }
            catch (RuntimeException | Error cleanup) { error.addSuppressed(cleanup); }
            throw error;
        }
    }

    public void stopServiceDiscovery(DiscoveryListener listener) {
        Objects.requireNonNull(listener, "listener cannot be null");
        synchronized (mDiscoveries) {
            Discovery discovery = mDiscoveries.get(listener);
            if (discovery == null) throw new IllegalArgumentException("listener not registered");
            // Registration remains live until the terminal event is dispatched.
            if (discovery.stopPending) return;
            discovery.stopPending = true;
            if (discovery.session != null) discovery.session.stop();
        }
    }

    // --- resolving -----------------------------------------------------------------

    @Deprecated
    public void resolveService(NsdServiceInfo serviceInfo, ResolveListener listener) {
        resolveService(serviceInfo, mMainExecutor, listener);
    }

    public void resolveService(NsdServiceInfo serviceInfo, Executor executor,
            ResolveListener listener) {
        checkServiceInfo(serviceInfo);
        Objects.requireNonNull(executor, "executor cannot be null");
        Objects.requireNonNull(listener, "listener cannot be null");
        Resolution resolution = new Resolution(serviceInfo, executor);
        synchronized (mDiscoveries) {
            if (mDiscoveries.containsKey(listener) || mResolutions.containsKey(listener)
                    || mRegistrations.containsKey(listener) || mSubscriptions.containsKey(listener)) {
                throw new IllegalArgumentException("listener already in use");
            }
            mResolutions.put(listener, resolution);
        }
        try {
            WindowsNsdResolver.Session session = mResolverBackend.start(serviceInfo,
                    new WindowsNsdResolver.Callback() {
                        public void resolved(NsdServiceInfo result) {
                            dispatchResolution(listener, resolution, () -> listener.onServiceResolved(result));
                        }
                        public void stopped() {
                            dispatchResolution(listener, resolution, () -> listener.onResolutionStopped(resolution.original));
                        }
                        public void failed(int error) {
                            dispatchResolution(listener, resolution, () -> listener.onResolveFailed(resolution.original, error));
                        }
                        public void stopFailed(int error) {
                            dispatchResolution(listener, resolution, () -> listener.onStopResolutionFailed(resolution.original, error));
                        }
                    });
            synchronized (mDiscoveries) {
                resolution.session = session;
                if (resolution.stopPending) session.stop();
            }
        } catch (RuntimeException | Error error) {
            synchronized (mDiscoveries) {
                if (mResolutions.get(listener) == resolution) mResolutions.remove(listener);
                if (resolution.session != null) resolution.session.stop();
            }
            throw error;
        }
    }

    private void dispatchResolution(ResolveListener listener, Resolution resolution, Runnable callback) {
        mServiceExecutor.execute(() -> {
            synchronized (mDiscoveries) {
                if (mResolutions.get(listener) != resolution) return;
                mResolutions.remove(listener);
            }
            // The backend has already released its resources. Listener removal
            // precedes executor submission, allowing reuse from terminal callbacks.
            resolution.executor.execute(callback);
        });
    }

    public void stopServiceResolution(ResolveListener listener) {
        Objects.requireNonNull(listener, "listener cannot be null");
        synchronized (mDiscoveries) {
            Resolution resolution = mResolutions.get(listener);
            if (resolution == null) throw new IllegalArgumentException("listener not registered");
            // Coalesce cancellation while the listener is still registered.
            if (resolution.stopPending) return;
            resolution.stopPending = true;
            if (resolution.session != null) resolution.session.stop();
        }
    }

    public void registerServiceInfoCallback(NsdServiceInfo serviceInfo, Executor executor,
            ServiceInfoCallback listener) {
        checkServiceInfo(serviceInfo);
        Objects.requireNonNull(executor, "executor cannot be null");
        Objects.requireNonNull(listener, "listener cannot be null");
        NsdServiceInfo selector = NsdRegistrationContract.copyForTransport(serviceInfo);
        Subscription subscription = new Subscription(executor);
        synchronized (mDiscoveries) {
            if (mSubscriptions.containsKey(listener) || mDiscoveries.containsKey(listener)
                    || mRegistrations.containsKey(listener) || mResolutions.containsKey(listener)) {
                throw new IllegalArgumentException("listener already in use");
            }
            // The pinned service has no separate subscription admission quota.
            // Its active entry still counts for discovery/registration admission.
            mSubscriptions.put(listener, subscription);
        }
        try {
            NsdSubscriptionClient.Session session = mSubscriptionBackend.start(selector,
                    new NsdSubscriptionClient.Callback() {
                        public void updated(NsdServiceInfo info) {
                            subscriptionEvent(listener, subscription, false, () -> listener.onServiceUpdated(info));
                        }
                        public void lost() {
                            subscriptionEvent(listener, subscription, false, listener::onServiceLost);
                        }
                        public void failed(int error) {
                            subscriptionEvent(listener, subscription, true,
                                    () -> listener.onServiceInfoCallbackRegistrationFailed(error));
                        }
                        public void unregistered() {
                            subscriptionEvent(listener, subscription, true,
                                    listener::onServiceInfoCallbackUnregistered);
                        }
                    });
            synchronized (mDiscoveries) { subscription.session = session; }
            stopSubscription(subscription);
        } catch (RuntimeException | Error failure) {
            retireSubscription(listener, subscription);
            throw failure;
        }
    }

    public void unregisterServiceInfoCallback(ServiceInfoCallback listener) {
        Objects.requireNonNull(listener, "listener cannot be null");
        Subscription subscription;
        synchronized (mDiscoveries) {
            subscription = mSubscriptions.get(listener);
            if (subscription == null) throw new IllegalArgumentException("listener not registered");
            subscription.stopPending = true;
        }
        stopSubscription(subscription);
    }

    private void stopSubscription(Subscription subscription) {
        NsdSubscriptionClient.Session session;
        synchronized (mDiscoveries) {
            if (!subscription.stopPending || subscription.stopSent || subscription.session == null) return;
            subscription.stopSent = true;
            session = subscription.session;
        }
        session.stop();
    }

    private void retireSubscription(ServiceInfoCallback listener, Subscription subscription) {
        synchronized (mDiscoveries) {
            mSubscriptions.remove(listener, subscription);
            subscription.stopPending = true;
        }
        stopSubscription(subscription);
    }

    private void subscriptionEvent(ServiceInfoCallback listener, Subscription subscription,
            boolean terminal, Runnable callback) {
        try {
            mServiceExecutor.execute(() -> {
                synchronized (mDiscoveries) {
                    if (mSubscriptions.get(listener) != subscription || !terminal && subscription.stopPending) return;
                    if (terminal) mSubscriptions.remove(listener);
                }
                try {
                    subscription.executor.execute(() -> {
                        synchronized (mDiscoveries) {
                            // Recheck at execution, not merely submission: an app
                            // executor can hold an update across cancel and reuse.
                            if (!terminal && (subscription.stopPending
                                    || mSubscriptions.get(listener) != subscription)) return;
                            if (terminal) {
                                if (subscription.terminalDelivered) return;
                                subscription.terminalDelivered = true;
                            }
                        }
                        callback.run();
                    });
                } catch (RejectedExecutionException rejected) {
                    if (!terminal) retireSubscription(listener, subscription);
                    throw rejected;
                }
            });
        } catch (RejectedExecutionException rejected) {
            retireSubscription(listener, subscription);
            throw rejected;
        }
    }

    // --- checks ----------------------------------------------------------------------

    private static void checkServiceInfo(NsdServiceInfo serviceInfo) {
        Objects.requireNonNull(serviceInfo, "NsdServiceInfo cannot be null");
        if (serviceInfo.getServiceName() == null || serviceInfo.getServiceName().isEmpty()) {
            throw new IllegalArgumentException("Service name cannot be empty");
        }
        if (serviceInfo.getServiceType() == null || serviceInfo.getServiceType().isEmpty()) {
            throw new IllegalArgumentException("Service type cannot be empty");
        }
    }

    private static void checkProtocol(int protocolType) {
        if (protocolType != PROTOCOL_DNS_SD) {
            throw new IllegalArgumentException("Unsupported protocol");
        }
    }
}
