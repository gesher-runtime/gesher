/* Copyright (C) 2026 The Gesher Authors. SPDX-License-Identifier: Apache-2.0 */
package gesher.nsd;

import android.content.Context;
import android.net.Network;
import android.net.nsd.NsdManager;
import android.net.nsd.NsdServiceInfo;
import android.os.Looper;
import java.net.ServerSocket;
import java.util.UUID;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicReference;

/** Public SDK calls under app_process/system-context shell identity; not APK permission qualification. */
public final class NsdAndroidReference {
    private static int checks;
    private static void check(boolean condition, String message) {
        if (!condition) throw new AssertionError(message);
        checks++;
        System.out.println("NSD_REFERENCE_CHECK " + checks + " " + message);
    }
    private static void waitFor(CountDownLatch signal, String name) throws Exception {
        check(signal.await(30, TimeUnit.SECONDS), "callback " + name);
    }
    private static void event(String method, String type, NsdServiceInfo info) {
        System.out.println("NSD_REFERENCE_EVENT " + method + " thread=" + Thread.currentThread().getName()
                + " type=" + type + " info=" + info);
    }
    public static void main(String[] args) {
        try { run(); System.out.println("NsdAndroidReference all passed: " + checks); System.exit(0); }
        catch (Throwable error) { error.printStackTrace(); System.exit(1); }
    }
    private static void run() throws Exception {
        Looper.prepareMainLooper();
        Class<?> activityThread = Class.forName("android.app.ActivityThread");
        Object thread = activityThread.getMethod("systemMain").invoke(null);
        Context context = (Context) activityThread.getMethod("getSystemContext").invoke(thread);
        NsdManager manager = context.getSystemService(NsdManager.class);
        check(manager != null, "real system NSD service");
        String nonce = UUID.randomUUID().toString().replace("-", "").substring(0, 10);
        String type = "_gs" + nonce + "._tcp";
        String instance = "Owned.שלום\\" + nonce;
        ExecutorService executor = Executors.newSingleThreadExecutor(r -> new Thread(r, "nsd-oracle-callback"));
        CountDownLatch started = new CountDownLatch(1), found = new CountDownLatch(1), lost = new CountDownLatch(1);
        CountDownLatch stopped = new CountDownLatch(1), registered = new CountDownLatch(1), unregistered = new CountDownLatch(1);
        AtomicReference<NsdServiceInfo> observation = new AtomicReference<>();
        AtomicReference<String> error = new AtomicReference<>();
        NsdManager.DiscoveryListener listener = new NsdManager.DiscoveryListener() {
            public void onDiscoveryStarted(String t) { event("started", t, null); started.countDown(); }
            public void onStartDiscoveryFailed(String t, int e) { error.set("start " + e); started.countDown(); }
            public void onDiscoveryStopped(String t) { event("stopped", t, null); stopped.countDown(); }
            public void onStopDiscoveryFailed(String t, int e) { error.set("stop " + e); stopped.countDown(); }
            public void onServiceFound(NsdServiceInfo info) {
                if (!info.getServiceName().equals(instance)) return;
                event("found", info.getServiceType(), info); observation.set(info); found.countDown();
            }
            public void onServiceLost(NsdServiceInfo info) {
                if (!info.getServiceName().equals(instance)) return;
                event("lost", info.getServiceType(), info); lost.countDown();
            }
        };
        NsdManager.RegistrationListener registration = new NsdManager.RegistrationListener() {
            public void onRegistrationFailed(NsdServiceInfo info, int e) { error.set("register " + e); registered.countDown(); }
            public void onServiceRegistered(NsdServiceInfo info) { event("registered", info.getServiceType(), info); registered.countDown(); }
            public void onUnregistrationFailed(NsdServiceInfo info, int e) { error.set("unregister " + e); unregistered.countDown(); }
            public void onServiceUnregistered(NsdServiceInfo info) { event("unregistered", info.getServiceType(), info); unregistered.countDown(); }
        };
        try (ServerSocket server = new ServerSocket(0)) {
            manager.discoverServices(type, NsdManager.PROTOCOL_DNS_SD, (Network) null, executor, listener);
            waitFor(started, "started"); check(error.get() == null, "real browse accepted");
            try {
                manager.discoverServices(type, 1, (Network) null, executor, listener);
                throw new AssertionError("Duplicate listener accepted");
            } catch (IllegalArgumentException expected) { check(true, "duplicate listener rejected"); }
            NsdServiceInfo service = new NsdServiceInfo();
            service.setServiceName(instance); service.setServiceType(type); service.setPort(server.getLocalPort());
            service.setAttribute("nul", "before" + (char) 0 + "after");
            manager.registerService(service, 1, executor, registration);
            waitFor(registered, "registered"); check(error.get() == null, "real owned registration accepted");
            waitFor(found, "owned found"); check(error.get() == null, "owned observation accepted");
            NsdServiceInfo info = observation.get();
            check(info.getServiceName().equals(instance), "Unicode dot backslash instance retained");
            check(info.getServiceType().equals(type + "."), "found type has trailing dot");
            check(info.getNetwork() != null, "found network present");
            check(info.getPort() == 0 && info.getHost() == null, "browse is unresolved");
            check(info.getAttributes().isEmpty(), "PTR discovery does not expose TXT");
            manager.unregisterService(registration);
            waitFor(unregistered, "unregistered"); check(error.get() == null, "real unregistration accepted");
            waitFor(lost, "owned lost");
            manager.stopServiceDiscovery(listener);
            waitFor(stopped, "stopped"); check(error.get() == null, "real stop acknowledged");
            try { manager.stopServiceDiscovery(listener); throw new AssertionError("Unknown listener accepted"); }
            catch (IllegalArgumentException expected) { check(true, "terminal listener removed"); }
        } finally { executor.shutdownNow(); }
    }
}
