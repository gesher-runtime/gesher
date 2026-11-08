/* Copyright (C) 2026 The Gesher Authors. SPDX-License-Identifier: Apache-2.0 */
package gesher.services;

import android.net.Network;
import android.net.NetworkRequest;
import android.net.nsd.AdvertisingRequest;
import android.net.nsd.DiscoveryRequest;
import android.net.nsd.NsdManager;
import android.net.nsd.NsdServiceInfo;

import java.lang.reflect.Constructor;
import java.util.ArrayDeque;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Deque;
import java.util.List;
import java.util.concurrent.Executor;
import java.util.concurrent.RejectedExecutionException;

/** Real NSD manager/value classes; deterministic executors, no mDNS or device required. */
public final class NsdContractTest {
    private static int checks;

    private static void check(boolean condition, String message) {
        if (!condition) throw new AssertionError(message);
        checks++;
    }

    private static void expect(Class<? extends Throwable> type, Runnable call, String message) {
        try {
            call.run();
        } catch (Throwable error) {
            check(type.isInstance(error), message + ": " + error);
            return;
        }
        throw new AssertionError(message + ": no exception");
    }

    private static final class Queue implements Executor {
        final Deque<Runnable> tasks = new ArrayDeque<>();
        boolean reject;
        boolean running;

        @Override public void execute(Runnable task) {
            if (reject) throw new RejectedExecutionException("fixture rejection");
            tasks.addLast(task);
        }

        void runNext() { run(tasks.removeFirst()); }
        void runLast() { run(tasks.removeLast()); }
        void drain() { while (!tasks.isEmpty()) runNext(); }
        private void run(Runnable task) {
            running = true;
            try { task.run(); } finally { running = false; }
        }
    }

    private static class Discovery implements NsdManager.DiscoveryListener {
        final List<String> events = new ArrayList<>();

        @Override public void onDiscoveryStarted(String type) { events.add("start " + type); }
        @Override public void onDiscoveryStopped(String type) { events.add("stop " + type); }
        @Override public void onStartDiscoveryFailed(String type, int code) {
            throw new AssertionError("Unexpected discovery failure " + code);
        }
        @Override public void onStopDiscoveryFailed(String type, int code) {
            throw new AssertionError("Unexpected stop failure " + code);
        }
        @Override public void onServiceFound(NsdServiceInfo info) {
            throw new AssertionError("No mDNS backend must not invent a service");
        }
        @Override public void onServiceLost(NsdServiceInfo info) {
            throw new AssertionError("No mDNS backend must not invent a service");
        }
    }

    private static final class EqualDiscovery extends Discovery {
        @Override public boolean equals(Object other) { return other instanceof EqualDiscovery; }
        @Override public int hashCode() { return 1; }
    }

    private static final class Registration implements NsdManager.RegistrationListener {
        NsdServiceInfo info;
        int failures;
        int code = -1;

        @Override public void onRegistrationFailed(NsdServiceInfo value, int errorCode) {
            info = value;
            code = errorCode;
            failures++;
        }
        @Override public void onServiceRegistered(NsdServiceInfo info) {
            throw new AssertionError("No mDNS backend must not advertise success");
        }
        @Override public void onUnregistrationFailed(NsdServiceInfo info, int code) {
            throw new AssertionError("Unexpected unregistration failure");
        }
        @Override public void onServiceUnregistered(NsdServiceInfo info) {
            throw new AssertionError("Failed registration must not be unregistered");
        }
    }

    private static NsdManager manager(Executor defaultExecutor) throws Exception {
        // Isolate the host's main Looper while exercising the same production manager.
        Constructor<NsdManager> constructor = NsdManager.class.getDeclaredConstructor(Executor.class);
        constructor.setAccessible(true);
        return constructor.newInstance(defaultExecutor);
    }

    private static void start(NsdManager manager, String type, Executor executor,
            NsdManager.DiscoveryListener listener) {
        manager.discoverServices(type, NsdManager.PROTOCOL_DNS_SD, (Network) null, executor, listener);
    }

    private static void descriptors() throws Exception {
        check(NsdManager.class.getMethod("discoverServices", DiscoveryRequest.class, Executor.class,
                NsdManager.DiscoveryListener.class).getReturnType() == void.class,
                "DiscoveryRequest overload does not link");
        check(NsdManager.class.getMethod("discoverServices", String.class, int.class,
                NetworkRequest.class, Executor.class, NsdManager.DiscoveryListener.class)
                .getReturnType() == void.class, "NetworkRequest overload does not link");
        check(NsdManager.class.getMethod("registerService", AdvertisingRequest.class, Executor.class,
                NsdManager.RegistrationListener.class).getReturnType() == void.class,
                "AdvertisingRequest overload does not link");
    }

    private static void executorAndOverloads() throws Exception {
        Queue main = new Queue();
        Queue executor = new Queue();
        NsdManager manager = manager(main);
        Discovery listener = new Discovery() {
            @Override public void onDiscoveryStarted(String type) {
                check(executor.running, "Start switched away from caller's executor");
                super.onDiscoveryStarted(type);
            }
            @Override public void onDiscoveryStopped(String type) {
                check(executor.running, "Stop switched away from caller's executor");
                super.onDiscoveryStopped(type);
            }
        };
        DiscoveryRequest request = new DiscoveryRequest.Builder("_ipp._tcp")
                .setSubtype("_printer").build();
        manager.discoverServices(request, executor, listener);
        check(listener.events.isEmpty() && executor.tasks.size() == 1,
                "Start did not dispatch through executor");
        executor.runNext();
        manager.stopServiceDiscovery(listener);
        check(main.tasks.isEmpty() && executor.tasks.size() == 1,
                "Stop ignored caller's executor");
        executor.runNext();
        check(listener.events.equals(Arrays.asList("start _ipp._tcp", "stop _ipp._tcp")),
                "DiscoveryRequest completion used the wrong service type");

        NetworkRequest networks = new NetworkRequest.Builder().build();
        manager.discoverServices("_http._tcp", NsdManager.PROTOCOL_DNS_SD,
                networks, executor, listener);
        executor.runNext();
        manager.stopServiceDiscovery(listener);
        executor.runNext();
        check(listener.events.subList(2, 4).equals(
                Arrays.asList("start _http._tcp", "stop _http._tcp")),
                "NetworkRequest overload lost its callbacks");

        Discovery legacy = new Discovery();
        manager.discoverServices("_legacy._tcp", NsdManager.PROTOCOL_DNS_SD, legacy);
        check(main.tasks.size() == 1 && legacy.events.isEmpty(), "Legacy start lost default executor");
        main.runNext();
        manager.stopServiceDiscovery(legacy);
        check(main.tasks.size() == 1, "Legacy stop lost default executor");
        main.runNext();
        check(legacy.events.size() == 2, "Legacy discovery did not finish");
    }

    private static void identityAndLifecycle() throws Exception {
        Queue executor = new Queue();
        NsdManager manager = manager(Runnable::run);
        Discovery first = new EqualDiscovery();
        Discovery second = new EqualDiscovery();
        start(manager, "_one._tcp", executor, first);
        start(manager, "_two._tcp", executor, second);
        executor.drain();
        manager.stopServiceDiscovery(first);
        expect(IllegalArgumentException.class,
                () -> start(manager, "_duplicate._tcp", executor, first),
                "Listener reused before terminal callback");
        manager.stopServiceDiscovery(second);
        executor.drain();
        check(first.events.equals(Arrays.asList("start _one._tcp", "stop _one._tcp")),
                "Application equals collapsed distinct listeners");
        check(second.events.equals(Arrays.asList("start _two._tcp", "stop _two._tcp")),
                "Stopping one listener removed another equal listener");
        expect(IllegalArgumentException.class, () -> manager.stopServiceDiscovery(first),
                "Inactive discovery accepted another stop");

        Discovery reordered = new Discovery();
        start(manager, "_old._tcp", executor, reordered);
        manager.stopServiceDiscovery(reordered);
        executor.runLast(); // The supplied executor need not be FIFO.
        start(manager, "_new._tcp", Runnable::run, reordered);
        executor.drain();
        manager.stopServiceDiscovery(reordered);
        check(reordered.events.equals(Arrays.asList("stop _old._tcp", "start _new._tcp",
                "stop _new._tcp")), "A stale start resurrected a stopped/reused listener");

        Discovery reentrant = new Discovery() {
            @Override public void onDiscoveryStopped(String type) {
                super.onDiscoveryStopped(type);
                if (type.equals("_first._tcp")) start(manager, "_again._tcp", Runnable::run, this);
            }
        };
        start(manager, "_first._tcp", Runnable::run, reentrant);
        manager.stopServiceDiscovery(reentrant);
        manager.stopServiceDiscovery(reentrant);
        check(reentrant.events.equals(Arrays.asList("start _first._tcp", "stop _first._tcp",
                "start _again._tcp", "stop _again._tcp")),
                "Listener cannot be reused from its terminal callback");
    }

    private static void rejectedExecutorsAndValidation() throws Exception {
        Queue executor = new Queue();
        NsdManager manager = manager(Runnable::run);
        Discovery listener = new Discovery();
        executor.reject = true;
        expect(RejectedExecutionException.class,
                () -> start(manager, "_rejected._tcp", executor, listener),
                "Rejected start was hidden");
        executor.reject = false;
        start(manager, "_retry._tcp", executor, listener);
        executor.runNext();
        expect(IllegalArgumentException.class,
                () -> start(manager, "_duplicate._tcp", executor, listener),
                "Active listener accepted twice");
        executor.reject = true;
        expect(RejectedExecutionException.class, () -> manager.stopServiceDiscovery(listener),
                "Rejected stop was hidden");
        executor.reject = false;
        manager.stopServiceDiscovery(listener);
        executor.runNext();
        check(listener.events.equals(Arrays.asList("start _retry._tcp", "stop _retry._tcp")),
                "Executor rejection leaked or prematurely removed listener state");
        expect(NullPointerException.class,
                () -> manager.discoverServices((DiscoveryRequest) null, executor, listener),
                "Null DiscoveryRequest accepted");
        expect(NullPointerException.class,
                () -> manager.discoverServices("_test._tcp", NsdManager.PROTOCOL_DNS_SD,
                        (NetworkRequest) null, executor, listener), "Null NetworkRequest accepted");
        expect(IllegalArgumentException.class,
                () -> start(manager, "", executor, listener), "Empty service type accepted");
        expect(IllegalArgumentException.class,
                () -> manager.discoverServices("_test._tcp", -1, (Network) null, executor, listener),
                "Unsupported discovery protocol accepted");
        expect(NullPointerException.class,
                () -> start(manager, "_test._tcp", null, listener), "Null executor accepted");
        expect(NullPointerException.class,
                () -> start(manager, "_test._tcp", executor, null), "Null listener accepted");
        check(executor.tasks.isEmpty(), "Invalid requests dispatched callbacks");
    }

    private static void advertising() throws Exception {
        Queue executor = new Queue();
        NsdManager manager = manager(Runnable::run);
        NsdServiceInfo info = new NsdServiceInfo();
        info.setServiceName("printer");
        info.setServiceType("_ipp._tcp");
        info.setPort(631);
        Registration listener = new Registration();
        manager.registerService(new AdvertisingRequest.Builder(info)
                .setFlags(AdvertisingRequest.FLAG_SKIP_PROBING).build(), executor, listener);
        info.setServiceName("mutated after submission");
        check(listener.failures == 0 && executor.tasks.size() == 1,
                "Advertising bypassed caller's executor");
        executor.runNext();
        check(listener.failures == 1 && listener.code == NsdManager.FAILURE_INTERNAL_ERROR,
                "Unsupported advertising did not complete with one error");
        check(listener.info != info && "printer".equals(listener.info.getServiceName())
                && listener.info.getPort() == 631,
                "Error callback lost its submitted service snapshot");
        manager.unregisterService(listener); // Existing cleanup tolerance after failure.
        check(executor.tasks.isEmpty(), "Failed advertising generated more terminal callbacks");
        expect(NullPointerException.class,
                () -> manager.registerService((AdvertisingRequest) null, executor, listener),
                "Null AdvertisingRequest accepted");
        expect(IllegalArgumentException.class,
                () -> manager.registerService(new AdvertisingRequest.Builder(info)
                        .setProtocolType(-1).build(), executor, listener),
                "Unsupported advertising protocol accepted");
    }

    public static void main(String[] args) throws Exception {
        descriptors();
        executorAndOverloads();
        identityAndLifecycle();
        rejectedExecutorsAndValidation();
        advertising();
        System.out.println("NsdContractTest all passed: " + checks);
    }
}
