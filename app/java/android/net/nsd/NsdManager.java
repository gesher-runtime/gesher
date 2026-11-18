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

import java.io.IOException;
import java.util.IdentityHashMap;
import java.util.Map;
import java.util.Objects;
import java.util.concurrent.Executor;
import java.util.concurrent.RejectedExecutionException;

/**
 * Android 16 NSD API backed by real Windows DNS-SD browsing. Registration,
 * resolution and service-info subscriptions remain explicitly unavailable:
 * Windows' string-only TXT API cannot represent Android's arbitrary bytes.
 * See docs/research/nsd-windows-browse-2026-11-18.md.
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
    private final Context mContext;
    private final Map<DiscoveryListener, Discovery> mDiscoveries = new IdentityHashMap<>();

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
    }

    // Explicit fixture injection; ordinary construction always selects WindowsNsd.
    private NsdManager(Executor serviceExecutor, WindowsNsd.Factory backend) {
        this(null, serviceExecutor, backend);
    }

    private NsdManager(Context context, Executor serviceExecutor, WindowsNsd.Factory backend) {
        mContext = context;
        mServiceExecutor = Objects.requireNonNull(serviceExecutor);
        mBackend = Objects.requireNonNull(backend);
    }

    // --- registering ---------------------------------------------------------------

    public void registerService(NsdServiceInfo serviceInfo, int protocolType,
            RegistrationListener listener) {
        registerService(serviceInfo, protocolType, mMainExecutor, listener);
    }

    public void registerService(NsdServiceInfo serviceInfo, int protocolType,
            Executor executor, RegistrationListener listener) {
        checkServiceInfo(serviceInfo);
        checkProtocol(protocolType);
        Objects.requireNonNull(executor, "executor cannot be null");
        Objects.requireNonNull(listener, "listener cannot be null");
        NsdServiceInfo info = new NsdServiceInfo(serviceInfo);
        mServiceExecutor.execute(() -> executor.execute(() ->
                listener.onRegistrationFailed(info, FAILURE_INTERNAL_ERROR)));
    }

    public void registerService(AdvertisingRequest advertisingRequest, Executor executor,
            RegistrationListener listener) {
        Objects.requireNonNull(advertisingRequest, "AdvertisingRequest cannot be null");
        registerService(advertisingRequest.getServiceInfo(), advertisingRequest.getProtocolType(),
                executor, listener);
    }

    public void unregisterService(RegistrationListener listener) {
        Objects.requireNonNull(listener, "listener cannot be null");
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
            if (mDiscoveries.containsKey(listener)) throw new IllegalArgumentException("listener already in use");
        }
        if (request != null && mContext != null) {
            mContext.enforcePermission("android.permission.ACCESS_NETWORK_STATE",
                    android.os.Process.myPid(), android.os.Process.myUid(), "NsdService");
        }
        Discovery discovery = new Discovery(serviceType, executor);
        boolean overLimit;
        synchronized (mDiscoveries) {
            // Another thread may have registered it during permission checking.
            if (mDiscoveries.containsKey(listener)) throw new IllegalArgumentException("listener already in use");
            overLimit = mDiscoveries.size() >= 200;
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
        NsdServiceInfo info = new NsdServiceInfo(serviceInfo);
        mServiceExecutor.execute(() -> executor.execute(() ->
                listener.onResolveFailed(info, FAILURE_INTERNAL_ERROR)));
    }

    public void stopServiceResolution(ResolveListener listener) {
        Objects.requireNonNull(listener, "listener cannot be null");
    }

    public void registerServiceInfoCallback(NsdServiceInfo serviceInfo, Executor executor,
            ServiceInfoCallback listener) {
        checkServiceInfo(serviceInfo);
        Objects.requireNonNull(executor, "executor cannot be null");
        Objects.requireNonNull(listener, "listener cannot be null");
        mServiceExecutor.execute(() -> executor.execute(() ->
                listener.onServiceInfoCallbackRegistrationFailed(FAILURE_INTERNAL_ERROR)));
    }

    public void unregisterServiceInfoCallback(ServiceInfoCallback listener) {
        Objects.requireNonNull(listener, "listener cannot be null");
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
