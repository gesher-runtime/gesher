/* Copyright (C) 2026 The Gesher Authors. SPDX-License-Identifier: Apache-2.0 */
package gesher.services;

import android.net.Network;
import android.net.nsd.DiscoveryRequest;
import android.net.nsd.NsdManager;
import android.net.nsd.NsdServiceInfo;
import com.gesher.net.WindowsNetworks;
import com.gesher.net.WindowsNsd;
import java.lang.reflect.Method;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.StandardCopyOption;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicReference;
import java.util.function.BooleanSupplier;
import java.util.Arrays;

/** Required Windows production gate with an independent raw-wire fixture peer. */
public final class NsdBrowseWindowsTest {
    private static int checks;
    private static int command;
    private static final AtomicReference<String> failure = new AtomicReference<>();
    private static Listener diagnosticListener;
    private static void check(boolean ok, String text) {
        if (!ok) throw new AssertionError(text);
        checks++;
    }
    private static void await(BooleanSupplier ready, String text) throws Exception {
        await(ready, text, 15000);
    }
    private static void await(BooleanSupplier ready, String text, long timeoutMillis) throws Exception {
        long deadline = System.nanoTime() + timeoutMillis * 1_000_000L;
        while (!ready.getAsBoolean()) {
            if (failure.get() != null) throw new AssertionError(failure.get() + " " + diagnostics());
            if (System.nanoTime() >= deadline) throw new AssertionError("Timeout: " + text + " " + diagnostics());
            Thread.sleep(10);
        }
        check(failure.get() == null, text);
    }
    private static final class Listener implements NsdManager.DiscoveryListener {
        final AtomicInteger started = new AtomicInteger(), found = new AtomicInteger();
        final AtomicInteger lost = new AtomicInteger(), stopped = new AtomicInteger();
        volatile NsdServiceInfo info;
        public void onDiscoveryStarted(String type) { started.incrementAndGet(); }
        public void onDiscoveryStopped(String type) { stopped.incrementAndGet(); }
        public void onStartDiscoveryFailed(String type, int error) { failure.set("Windows browse failed " + error + " for " + type); }
        public void onStopDiscoveryFailed(String type, int error) { failure.set("Windows cancel failed " + error); }
        public void onServiceFound(NsdServiceInfo value) { info = value; found.incrementAndGet(); }
        public void onServiceLost(NsdServiceInfo value) { info = value; lost.incrementAndGet(); }
    }
    private static void peer(Path control, String action) throws Exception {
        String serial = Integer.toString(++command);
        Path temporary = control.resolve("command-new");
        Files.write(temporary, (serial + " " + action).getBytes(StandardCharsets.UTF_8));
        Files.move(temporary, control.resolve("command"), StandardCopyOption.REPLACE_EXISTING);
        await(() -> {
            try { return Files.readString(control.resolve("ack")).trim().equals(serial); }
            catch (Exception ignored) { return false; }
        }, "peer " + action);
    }
    private static int active() {
        try {
            Method method = WindowsNsd.class.getDeclaredMethod("nativeActive");
            method.setAccessible(true);
            return (Integer) method.invoke(null);
        } catch (Exception e) { throw new AssertionError(e); }
    }
    private static void stop(NsdManager manager, Listener listener) throws Exception {
        manager.stopServiceDiscovery(listener);
        await(() -> listener.stopped.get() == 1, "cancellation completion");
        await(() -> active() == 0, "native handle retirement");
    }
    private static long baseGoodbyes() throws Exception {
        Method method = WindowsNsd.class.getDeclaredMethod("nativeBaseGoodbyes");
        method.setAccessible(true);
        return (Long) method.invoke(null);
    }
    private static String diagnostics() {
        try {
            Method method = WindowsNsd.class.getDeclaredMethod("nativeDiagnostics");
            method.setAccessible(true);
            Listener current = diagnosticListener;
            return "native[callbacks,records,membership,baseGoodbyes,errors,lastStatus]="
                    + Arrays.toString((long[]) method.invoke(null)) + " active=" + active()
                    + (current == null ? "" : " started=" + current.started.get()
                    + " found=" + current.found.get() + " lost=" + current.lost.get()
                    + " stopped=" + current.stopped.get());
        } catch (Exception error) { return "diagnostics unavailable: " + error; }
    }
    private static void longObservation(Listener listener, String name) throws Exception {
        check(listener.info.getServiceName().equals(name), "Fresh owned long-lived observation");
        Method method = WindowsNsd.class.getDeclaredMethod("nativeLastPtrTtl");
        method.setAccessible(true);
        check((Long) method.invoke(null) >= 60, "Native positive PTR lifetime exceeds the entire goodbye wait");
        if (listener.info.getExpirationTime() != null) {
            check(listener.info.getExpirationTime().toEpochMilli() >= System.currentTimeMillis() + 60000,
                    "Known SRV lifetime also exceeds the goodbye wait");
        }
    }
    public static void main(String[] args) throws Exception {
        if (args.length != 5) throw new IllegalArgumentException("type instance interface control family");
        System.loadLibrary("android_runtime");
        String type = args[0], instance = args[1];
        int index = Integer.parseInt(args[2]), family = Integer.parseInt(args[4]);
        Path control = Path.of(args[3]);
        if (!type.startsWith("_gs")) throw new IllegalArgumentException("Owned fixture type required");
        NsdManager manager = new NsdManager(null);
        check(active() == 0, "Initially no requests");
        Listener browse = new Listener();
        diagnosticListener = browse;
        manager.discoverServices(type, 1, new Network(100 + index), Runnable::run, browse);
        await(() -> browse.started.get() == 1, "Windows accepted browse");
        check(active() == 1, "One native request for explicit network");
        peer(control, "announce");
        await(() -> browse.found.get() == 1, "independent IPv" + family + " peer");
        NsdServiceInfo observed = browse.info;
        check(observed.getServiceName().equals(instance), "Unicode/dot/backslash identity: " + observed.getServiceName());
        check(observed.getServiceType().equals(type + "."), "Android found type");
        check(observed.getNetwork().equals(new Network(100 + index)), "Observed network");
        check(observed.getInterfaceIndex() == 0, "Network result has no local-only interface fallback");
        check(observed.getPort() == 0 && observed.getHost() == null && observed.getAttributes().isEmpty(),
                "PTR discovery does not invent resolved data");
        if (observed.getExpirationTime() == null) {
            System.out.println("Windows DNS result has no matching SRV; expiration remains unknown");
        } else {
            long expiration = observed.getExpirationTime().toEpochMilli();
            check(expiration >= System.currentTimeMillis() + 2500 && expiration <= System.currentTimeMillis() + 6000,
                    "Expiration uses the peer's observed SRV lifetime");
        }
        Thread.sleep(6500);
        check(browse.found.get() == 1 && browse.lost.get() == 0, "Wire refresh preserves one observation");
        peer(control, "silent");
        await(() -> browse.lost.get() == 1, "PTR TTL expiry");
        check(browse.info.getServiceName().equals(instance), "Lost identity");
        peer(control, "announce");
        await(() -> browse.found.get() == 2, "reappearance after expiry");
        stop(manager, browse);
        int count = browse.found.get();
        peer(control, "announce"); Thread.sleep(300);
        check(browse.found.get() == count && browse.stopped.get() == 1, "No events after acknowledged stop");
        String longType = type.replace("_gs", "_gl");
        Listener longBrowse = new Listener();
        diagnosticListener = longBrowse;
        manager.discoverServices(longType, 1, new Network(100 + index), Runnable::run, longBrowse);
        await(() -> longBrowse.started.get() == 1, "long-lived browse accepted");
        peer(control, "announce-long");
        await(() -> longBrowse.found.get() == 1, "fresh long-lived peer");
        longObservation(longBrowse, instance + "-long");
        peer(control, "goodbye");
        await(() -> longBrowse.lost.get() == 1, "goodbye causes loss before positive TTL expiry", 3000);
        stop(manager, longBrowse);
        String subtypeType = type.replace("_gs", "_gu");
        Listener subtype = new Listener();
        diagnosticListener = subtype;
        manager.discoverServices(new DiscoveryRequest.Builder(1, subtypeType).setSubtype("_printer")
                .setNetwork(new Network(100 + index)).build(), Runnable::run, subtype);
        await(() -> subtype.started.get() == 1, "subtype browse accepted");
        check(active() == 2, "Subtype membership and base withdrawal have separate owned requests");
        peer(control, "announce-base-only-long");
        Thread.sleep(1200);
        check(subtype.found.get() == 0 && subtype.lost.get() == 0,
                "Base PTR and SRV records do not invent the requested subtype membership");
        System.out.println("Subtype before membership announcement: " + diagnostics());
        peer(control, "announce-subtype-long");
        await(() -> subtype.found.get() == 1, "subtype PTR browse");
        check(subtype.info.getServiceType().equals(subtypeType + "."), "Subtype result base type");
        check(subtype.info.getSubtypes().contains("_printer"), "Actual queried subtype retained");
        longObservation(subtype, instance + "-sub-long");
        long beforeGoodbye = baseGoodbyes();
        peer(control, "base-goodbye");
        await(() -> {
            try { return baseGoodbyes() > beforeGoodbye && subtype.lost.get() == 1; }
            catch (Exception error) { throw new AssertionError(error); }
        }, "received base-only goodbye causes loss before positive TTL expiry", 3000);
        stop(manager, subtype);
        peer(control, "announce-subtype-long");
        Listener absent = new Listener();
        diagnosticListener = absent;
        manager.discoverServices(subtypeType, 1, new Network(Integer.MAX_VALUE), Runnable::run, absent);
        await(() -> absent.started.get() == 1, "Absent network observation");
        Thread.sleep(1200);
        check(absent.found.get() == 0 && active() == 0, "No fallback to another interface");
        stop(manager, absent);
        peer(control, "goodbye");
        for (int n = 0; n < 50; n++) {
            Listener race = new Listener();
            diagnosticListener = race;
            manager.discoverServices(type, 1, new Network(100 + index), Runnable::run, race);
            manager.stopServiceDiscovery(race);
            await(() -> race.stopped.get() == 1, "immediate cancel " + n);
            await(() -> active() == 0, "No retained request or entered callback after race " + n);
        }
        boolean prior = WindowsNetworks.internetAllowed();
        try {
            WindowsNetworks.setInternetAllowed(false);
            try { new NsdManager(null); throw new AssertionError("Missing INTERNET bypassed"); }
            catch (SecurityException expected) { checks++; }
            check(active() == 0, "Denied operation creates no native requests");
        } finally { WindowsNetworks.setInternetAllowed(prior); }
        peer(control, "quit");
        System.out.println("NsdBrowseWindowsTest all passed: " + checks + " family=" + family
                + " production=DnsStartMulticastQuery peer=independent-wire nativeActive=" + active());
    }
}
