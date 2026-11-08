/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package android.net.nsd;

import android.content.Context;
import android.net.Network;
import android.net.NetworkRequest;
import android.os.Handler;
import android.os.Looper;

import java.util.IdentityHashMap;
import java.util.Map;
import java.util.Objects;
import java.util.concurrent.Executor;
import java.util.concurrent.RejectedExecutionException;

/**
 * NsdManager (network service discovery, DNS-SD over mDNS) with the Connectivity
 * module's public API, which framework.jar compiles against and does not contain: an app
 * that names a listener class (NsdManager.RegistrationListener) failed to load without
 * it. Gesher has no mDNS responder (docs/network.md): a discovery starts and finds
 * nothing, and registering or resolving a service fails with
 * {@link #FAILURE_INTERNAL_ERROR}, each through the listener as the module reports it,
 * on the executor given or the main thread. A discovery listener is in use from its
 * start until it is stopped; reusing it throws, as on a device. Unregistering a
 * registration, resolution or callback that failed does nothing (apps unregister in
 * onDestroy whether or not it succeeded, which on a device it usually has).
 *
 * <p>Registered as {@code servicediscovery} by
 * {@link android.net.ConnectivityFrameworkInitializer}.
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

    private final Executor mMainExecutor;
    // AOSP keys listeners by identity, not by application-defined equals/hashCode.
    private final Map<DiscoveryListener, Discovery> mDiscoveries = new IdentityHashMap<>();

    private static final class Discovery {
        final String serviceType;
        final Executor executor;
        // Protected by mDiscoveries. Keep the listener reserved until its stop callback.
        boolean stopPending;
        boolean stopped;

        Discovery(String serviceType, Executor executor) {
            this.serviceType = serviceType;
            this.executor = executor;
        }
    }

    public NsdManager(Context context) {
        this(new Handler(Looper.getMainLooper())::post);
    }

    private NsdManager(Executor mainExecutor) {
        mMainExecutor = Objects.requireNonNull(mainExecutor);
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
        executor.execute(() -> listener.onRegistrationFailed(info, FAILURE_INTERNAL_ERROR));
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
        startDiscovery(serviceType, protocolType, executor, listener);
    }

    public void discoverServices(DiscoveryRequest discoveryRequest, Executor executor,
            DiscoveryListener listener) {
        Objects.requireNonNull(discoveryRequest, "DiscoveryRequest cannot be null");
        startDiscovery(discoveryRequest.getServiceType(), discoveryRequest.getProtocolType(),
                executor, listener);
    }

    public void discoverServices(String serviceType, int protocolType, NetworkRequest networkRequest,
            Executor executor, DiscoveryListener listener) {
        Objects.requireNonNull(networkRequest, "NetworkRequest cannot be null");
        // As with the Network overload, the absent mDNS backend produces no found/lost
        // events. Do not register a Windows network watcher with nothing to discover.
        startDiscovery(serviceType, protocolType, executor, listener);
    }

    private void startDiscovery(String serviceType, int protocolType, Executor executor,
            DiscoveryListener listener) {
        if (serviceType == null || serviceType.isEmpty()) {
            throw new IllegalArgumentException("Service type cannot be empty");
        }
        checkProtocol(protocolType);
        Objects.requireNonNull(executor, "executor cannot be null");
        Objects.requireNonNull(listener, "listener cannot be null");
        Discovery discovery = new Discovery(serviceType, executor);
        synchronized (mDiscoveries) {
            if (mDiscoveries.containsKey(listener)) {
                throw new IllegalArgumentException("listener already in use");
            }
            mDiscoveries.put(listener, discovery);
        }
        try {
            executor.execute(() -> {
                synchronized (mDiscoveries) {
                    // A non-FIFO executor may run the terminal callback first. Never
                    // resurrect that request, including after this listener is reused.
                    if (discovery.stopped) return;
                }
                listener.onDiscoveryStarted(serviceType);
            });
        } catch (RejectedExecutionException e) {
            synchronized (mDiscoveries) {
                if (mDiscoveries.get(listener) == discovery) mDiscoveries.remove(listener);
                discovery.stopped = true;
            }
            throw e;
        }
    }

    public void stopServiceDiscovery(DiscoveryListener listener) {
        Objects.requireNonNull(listener, "listener cannot be null");
        Discovery discovery;
        synchronized (mDiscoveries) {
            discovery = mDiscoveries.get(listener);
            if (discovery == null || discovery.stopPending) {
                throw new IllegalArgumentException("listener not registered");
            }
            discovery.stopPending = true;
        }
        try {
            discovery.executor.execute(() -> {
                synchronized (mDiscoveries) {
                    if (discovery.stopped) return;
                    discovery.stopped = true;
                    if (mDiscoveries.get(listener) == discovery) mDiscoveries.remove(listener);
                }
                listener.onDiscoveryStopped(discovery.serviceType);
            });
        } catch (RejectedExecutionException e) {
            synchronized (mDiscoveries) {
                // Submission failed: leave an active request that can be stopped again.
                if (!discovery.stopped) discovery.stopPending = false;
            }
            throw e;
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
        executor.execute(() -> listener.onResolveFailed(info, FAILURE_INTERNAL_ERROR));
    }

    public void stopServiceResolution(ResolveListener listener) {
        Objects.requireNonNull(listener, "listener cannot be null");
    }

    public void registerServiceInfoCallback(NsdServiceInfo serviceInfo, Executor executor,
            ServiceInfoCallback listener) {
        checkServiceInfo(serviceInfo);
        Objects.requireNonNull(executor, "executor cannot be null");
        Objects.requireNonNull(listener, "listener cannot be null");
        executor.execute(() -> listener.onServiceInfoCallbackRegistrationFailed(
                FAILURE_INTERNAL_ERROR));
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
