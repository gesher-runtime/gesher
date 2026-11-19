/* Copyright (C) 2026 The Gesher Authors. SPDX-License-Identifier: Apache-2.0 */
package gesher.services;

import android.content.Context;
import android.content.ContextWrapper;
import android.net.ConnectivityManager;
import android.net.Network;
import android.net.NetworkRequest;
import android.net.nsd.AdvertisingRequest;
import android.net.nsd.DiscoveryRequest;
import android.net.nsd.NsdManager;
import android.net.nsd.NsdServiceInfo;
import com.gesher.net.WindowsNsd;

import java.lang.reflect.Constructor;
import java.util.ArrayDeque;
import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.Executor;
import java.util.concurrent.RejectedExecutionException;

/** Real production manager with an explicitly supplied transport; no fake default. */
public final class NsdContractTest {
    private static int checks;
    private static void check(boolean value, String message) {
        if (!value) throw new AssertionError(message);
        checks++;
    }
    private static void expect(Class<? extends Throwable> type, Runnable action) {
        try { action.run(); } catch (Throwable error) {
            check(type.isInstance(error), "Wrong exception " + error);
            return;
        }
        throw new AssertionError("Missing " + type.getSimpleName());
    }
    private static final class Queue implements Executor {
        final ArrayDeque<Runnable> tasks = new ArrayDeque<>();
        boolean reject;
        public void execute(Runnable task) {
            if (reject) throw new RejectedExecutionException("fixture");
            tasks.add(task);
        }
        void drain() { while (!tasks.isEmpty()) tasks.remove().run(); }
    }
    private static final class Request implements WindowsNsd.Session {
        final WindowsNsd.Callback callback;
        final WindowsNsd.Scope scope;
        final String type;
        final String subtype;
        boolean stop;
        int stopCalls;
        Request(String type, String subtype, WindowsNsd.Scope scope, WindowsNsd.Callback callback) {
            this.type = type; this.subtype = subtype; this.scope = scope; this.callback = callback;
        }
        public void stop() { stop = true; stopCalls++; }
    }
    private static final class Backend implements WindowsNsd.Factory {
        final List<Request> requests = new ArrayList<>();
        public int[] interfaces() { return new int[] {7, 11}; }
        public WindowsNsd.Session start(String type, String subtype, WindowsNsd.Scope scope, WindowsNsd.Callback callback) {
            Request request = new Request(type, subtype, scope, callback);
            requests.add(request);
            callback.started();
            return request;
        }
        Request last() { return requests.get(requests.size() - 1); }
    }
    private static class Listener implements NsdManager.DiscoveryListener {
        final List<String> events = new ArrayList<>();
        NsdServiceInfo found;
        NsdServiceInfo lost;
        public void onDiscoveryStarted(String type) { events.add("started " + type); }
        public void onDiscoveryStopped(String type) { events.add("stopped " + type); }
        public void onStartDiscoveryFailed(String type, int code) { events.add("failed " + code); }
        public void onStopDiscoveryFailed(String type, int code) { events.add("stopfailed " + code); }
        public void onServiceFound(NsdServiceInfo info) { found = info; events.add("found"); }
        public void onServiceLost(NsdServiceInfo info) { lost = info; events.add("lost"); }
    }
    private static final class EqualListener extends Listener {
        @Override public boolean equals(Object value) { return value instanceof EqualListener; }
        @Override public int hashCode() { return 1; }
    }
    private static final class Connectivity extends ConnectivityManager {
        NetworkCallback callback;
        RuntimeException rejection;
        int unregisters;
        Connectivity() { super(null); }
        @Override public void registerNetworkCallback(NetworkRequest request, NetworkCallback value) {
            if (rejection != null) throw rejection;
            callback = value;
        }
        @Override public void unregisterNetworkCallback(NetworkCallback value) {
            unregisters++;
            if (value != callback) throw new IllegalArgumentException("Unknown callback");
            callback = null;
        }
    }
    private static final class NetworkContext extends ContextWrapper {
        final Connectivity connectivity = new Connectivity();
        boolean denyNetworkState;
        NetworkContext() { super(null); }
        @Override public Object getSystemService(String name) { return connectivity; }
        @Override public String getSystemServiceName(Class<?> service) { return Context.CONNECTIVITY_SERVICE; }
        @Override public void enforcePermission(String permission, int pid, int uid, String message) {
            if (denyNetworkState && permission.equals("android.permission.ACCESS_NETWORK_STATE")) {
                throw new SecurityException("fixture network-state denial");
            }
        }
    }
    private static NsdManager manager(Queue service, Backend backend) throws Exception {
        Constructor<NsdManager> c = NsdManager.class.getDeclaredConstructor(Executor.class, WindowsNsd.Factory.class);
        c.setAccessible(true);
        return c.newInstance(service, backend);
    }
    private static void start(NsdManager manager, Executor executor, Listener listener) {
        manager.discoverServices("_gesher._tcp", 1, (Network) null, executor, listener);
    }
    public static void main(String[] args) throws Exception {
        if (args.length > 0) System.load(args[0]);
        Queue service = new Queue();
        Queue app = new Queue();
        Backend backend = new Backend();
        NsdManager manager = manager(service, backend);
        check(NsdManager.class.getMethod("discoverServices", DiscoveryRequest.class, Executor.class,
                NsdManager.DiscoveryListener.class).getReturnType() == void.class, "DiscoveryRequest descriptor");
        check(NsdManager.class.getMethod("discoverServices", String.class, int.class, NetworkRequest.class,
                Executor.class, NsdManager.DiscoveryListener.class).getReturnType() == void.class, "NetworkRequest descriptor");
        check(NsdManager.class.getMethod("registerService", AdvertisingRequest.class, Executor.class,
                NsdManager.RegistrationListener.class).getReturnType() == void.class, "AdvertisingRequest descriptor");
        Listener listener = new Listener();
        start(manager, app, listener);
        Request first = backend.last();
        check(listener.events.isEmpty(), "No synchronous application callback");
        check(first.scope.interfaces().length == 2, "Unbound scope retains all interfaces");
        service.drain();
        check(listener.events.isEmpty(), "Application executor owns execution");
        app.drain();
        check(listener.events.equals(List.of("started _gesher._tcp")), "Started type unchanged");
        expect(IllegalArgumentException.class, () -> start(manager, app, listener));
        first.callback.found("Owned.שלום", "_gesher._tcp.", 7, 12345000L, "_printer");
        first.callback.lost("Owned.שלום", "_gesher._tcp.", 7);
        service.drain(); app.drain();
        check(listener.events.equals(List.of("started _gesher._tcp", "found", "lost")), "Event order");
        check(listener.found.getServiceName().equals("Owned.שלום"), "Unicode instance unchanged");
        check(listener.found.getServiceType().equals("_gesher._tcp."), "Android found type suffix");
        check(listener.found.getNetwork().equals(new Network(107)), "Actual network association");
        check(listener.found.getInterfaceIndex() == 0, "Android uses Network instead of the local-only interface fallback");
        check(listener.found.getPort() == 0 && listener.found.getHost() == null,
                "Browse does not invent resolved port/address");
        check(listener.found.getAttributes().isEmpty(), "Browse does not invent TXT");
        check(listener.found.getExpirationTime().getEpochSecond() == 12345, "Observed SRV expiration retained");
        check(listener.found.getSubtypes().equals(java.util.Set.of("_printer")), "Observed query subtype retained");
        check(listener.lost != listener.found, "Separate event values");
        check(listener.lost.getExpirationTime() == null, "Unobserved expiration remains unknown");
        manager.stopServiceDiscovery(listener);
        check(first.stop, "Stop reaches backend");
        service.drain(); app.drain();
        check(!listener.events.contains("stopped _gesher._tcp"), "Stop waits for native completion");
        manager.stopServiceDiscovery(listener);
        check(first.stopCalls == 1, "Pending repeated stop coalesces without restarting transport cleanup");
        expect(IllegalArgumentException.class, () -> start(manager, app, listener));
        first.callback.stopped();
        manager.stopServiceDiscovery(listener);
        check(first.stopCalls == 1, "Stop coalesces while terminal dispatch is queued");
        service.drain();
        check(!listener.events.contains("stopped _gesher._tcp"), "Terminal still uses application executor");
        // Android removes the listener on its handler before submitting terminal delivery.
        start(manager, app, listener);
        app.drain(); service.drain(); app.drain();
        check(listener.events.get(listener.events.size() - 1).startsWith("started"), "Reuse after terminal submission");
        check(java.util.Collections.frequency(listener.events, "stopped _gesher._tcp") == 1,
                "Repeated stops deliver exactly one terminal callback");
        first.callback.found("stale", "_gesher._tcp.", 7); service.drain(); app.drain();
        check(!"stale".equals(listener.found.getServiceName()), "Old handle cannot target reused listener");
        backend.last().callback.failed(0); service.drain(); app.drain();
        expect(IllegalArgumentException.class, () -> manager.stopServiceDiscovery(listener));
        Listener reentrant = new Listener() {
            @Override public void onDiscoveryStopped(String type) {
                super.onDiscoveryStopped(type);
                start(manager, app, this);
            }
        };
        start(manager, app, reentrant); Request prior = backend.last();
        service.drain(); app.drain();
        prior.callback.stopped(); service.drain(); app.drain();
        check(backend.last() != prior, "Listener can be reused from terminal callback");
        service.drain(); app.drain();
        check(reentrant.events.equals(List.of("started _gesher._tcp", "stopped _gesher._tcp",
                "started _gesher._tcp")), "Reentrant reuse preserves submitted callback order");
        backend.last().callback.failed(0); service.drain(); app.drain();
        EqualListener a = new EqualListener(); EqualListener b = new EqualListener();
        start(manager, app, a); Request ar = backend.last();
        start(manager, app, b); Request br = backend.last(); service.drain(); app.drain();
        check(a.events.size() == 1 && b.events.size() == 1, "Identity keys ignore application equals");
        ar.callback.stopped(); br.callback.stopped(); service.drain(); app.drain();
        DiscoveryRequest request = new DiscoveryRequest.Builder(1, "_gesher._udp")
                .setSubtype("_printer").setNetwork(new Network(111)).build();
        manager.discoverServices(request, app, listener);
        check(backend.last().subtype.equals("_printer"), "Subtype forwarded");
        check(backend.last().scope.interfaces()[0] == 11, "Requested network restricts interface");
        check(backend.last().scope.interfaces().length == 1, "No all-interface fallback");
        backend.last().callback.stopped(); service.drain(); app.drain();
        manager.discoverServices("_gesher._tcp", 1, new Network(99999), app, listener);
        check(backend.last().scope.interfaces().length == 0, "Absent explicit network never leaks to others");
        backend.last().callback.stopped(); service.drain(); app.drain();
        start(manager, app, listener); Request rejected = backend.last(); app.reject = true;
        expect(RejectedExecutionException.class, service::drain);
        check(rejected.stop, "Rejected application executor cancels transport");
        app.reject = false;
        expect(IllegalArgumentException.class, () -> manager.stopServiceDiscovery(listener));
        expect(IllegalArgumentException.class, () -> manager.discoverServices("", 1, (Network) null, app, listener));
        expect(IllegalArgumentException.class, () -> manager.discoverServices("_gesher._tcp", 7, (Network) null, app, listener));
        expect(NullPointerException.class, () -> start(manager, null, listener));
        expect(NullPointerException.class, () -> start(manager, app, null));
        expect(NullPointerException.class, () -> manager.discoverServices((DiscoveryRequest) null, app, listener));
        expect(NullPointerException.class, () -> manager.discoverServices("_gesher._tcp", 1, (NetworkRequest) null, app, listener));
        expect(NullPointerException.class, () -> manager.stopServiceDiscovery(null));
        Listener defaultListener = new Listener();
        manager.discoverServices("_gesher._tcp", 1, defaultListener);
        check(defaultListener.events.isEmpty(), "Default callback is asynchronous");
        service.drain();
        check(defaultListener.events.size() == 1, "Default callback runs on service dispatcher");
        backend.last().callback.stopped(); service.drain();
        NetworkContext context = new NetworkContext();
        Constructor<NsdManager> contextual = NsdManager.class.getDeclaredConstructor(
                Context.class, Executor.class, WindowsNsd.Factory.class);
        contextual.setAccessible(true);
        NsdManager selectedManager = contextual.newInstance(context, service, backend);
        NetworkRequest selection = new NetworkRequest.Builder().build();
        RuntimeException quota = new IllegalArgumentException("callback quota");
        context.connectivity.rejection = quota;
        try {
            selectedManager.discoverServices("_gesher._tcp", 1, selection, app, listener);
            throw new AssertionError("Missing registration rejection");
        } catch (RuntimeException error) { check(error == quota, "Network callback registration preserves original exception"); }
        check(context.connectivity.unregisters == 0, "Failed registration never unregisters an unknown callback");
        context.connectivity.rejection = null;
        selectedManager.discoverServices("_gesher._tcp", 1, selection, app, listener);
        Request selected = backend.last();
        ConnectivityManager.NetworkCallback networkCallback = context.connectivity.callback;
        networkCallback.onAvailable(new Network(107));
        long beforeLoss = selected.scope.snapshot().get(7);
        networkCallback.onLost(new Network(107));
        networkCallback.onAvailable(new Network(107));
        check(selected.scope.snapshot().get(7) != beforeLoss, "Loss and return between polls retains new network generation");
        selected.callback.stopped(); service.drain(); app.drain();
        check(context.connectivity.unregisters == 1 && context.connectivity.callback == null,
                "Terminal discovery unregisters exactly its accepted network callback");
        Listener permissionReuse = new Listener();
        start(selectedManager, app, permissionReuse); Request ordinary = backend.last();
        context.denyNetworkState = true;
        expect(IllegalArgumentException.class, () -> selectedManager.discoverServices(
                "_gesher._tcp", 1, selection, app, permissionReuse));
        Listener denied = new Listener();
        expect(SecurityException.class, () -> selectedManager.discoverServices(
                "_gesher._tcp", 1, selection, app, denied));
        context.denyNetworkState = false;
        selectedManager.discoverServices("_gesher._tcp", 1, selection, app, denied);
        check(backend.last() != ordinary, "Permission denial does not reserve the new listener");
        backend.last().callback.stopped(); ordinary.callback.stopped(); service.drain(); app.drain();
        Backend cappedBackend = new Backend(); NsdManager capped = manager(service, cappedBackend);
        for (int n = 0; n < 200; n++) start(capped, app, new Listener());
        Listener excess = new Listener(); start(capped, app, excess);
        check(cappedBackend.requests.size() == 200, "Logical request limit does not admit another transport");
        expect(IllegalArgumentException.class, () -> start(capped, app, excess));
        service.drain(); app.drain();
        check(excess.events.equals(List.of("failed 4")), "Limit failure uses one asynchronous terminal callback");
        for (Request accepted : cappedBackend.requests) accepted.callback.stopped();
        service.drain(); app.drain();
        NsdServiceInfo info = new NsdServiceInfo(); info.setServiceName("owned"); info.setServiceType("_gesher._tcp"); info.setPort(42);
        // Registration now has a real system-owned transport. This browse-only
        // fixture retains argument validation; its former unavailable-backend
        // expectation is covered by the separate old-source negative control.
        // Positive registration/retirement belongs to the installed-client gate.
        NsdManager.RegistrationListener registration = new NsdManager.RegistrationListener() {
            public void onRegistrationFailed(NsdServiceInfo value, int code) {
                throw new AssertionError("Argument-only fixture reached registration backend");
            }
            public void onServiceRegistered(NsdServiceInfo value) { throw new AssertionError("Unexpected registration"); }
            public void onUnregistrationFailed(NsdServiceInfo value, int code) { throw new AssertionError("Unexpected unregistration"); }
            public void onServiceUnregistered(NsdServiceInfo value) { throw new AssertionError("Unexpected unregistration"); }
        };
        expect(NullPointerException.class, () -> manager.registerService((AdvertisingRequest) null, app, registration));
        expect(IllegalArgumentException.class, () -> manager.registerService(
                new AdvertisingRequest.Builder(info).setProtocolType(-1).build(), app, registration));
        System.out.println("NsdContractTest all passed: " + checks);
    }
}
