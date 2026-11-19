/* Copyright (C) 2026 The Gesher Authors. SPDX-License-Identifier: Apache-2.0 */
package com.gesher.nsdregistration;

import android.Manifest;
import android.app.Activity;
import android.content.pm.PackageManager;
import android.net.ConnectivityManager;
import android.net.LinkAddress;
import android.net.LinkProperties;
import android.net.Network;
import android.net.nsd.NsdManager;
import android.net.nsd.NsdServiceInfo;
import android.os.Bundle;
import android.os.Process;
import org.json.JSONObject;

import java.io.BufferedReader;
import java.io.InputStreamReader;
import java.net.InetAddress;
import java.net.Inet6Address;
import java.net.InetSocketAddress;
import java.net.ServerSocket;
import java.net.Socket;
import java.net.SocketException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.StandardCopyOption;
import java.util.Arrays;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.atomic.AtomicReference;
import java.util.function.BooleanSupplier;

/** Ordinary installed application. No service, transport, permission or identity seam. */
public final class RegistrationActivity extends Activity {
    private final AtomicReference<Throwable> failure = new AtomicReference<>();
    private final ExecutorService callbacks = Executors.newSingleThreadExecutor(r ->
            new Thread(r, "nsd-fixture-callback"));
    private Path control;
    private String type, instance, mode, nonce, interfaceName;
    private InetAddress address;
    private Network network;
    private NsdManager manager;
    private volatile Slot primary, sibling;
    private volatile boolean finished, destroyed;
    private int commands, checks;

    private void check(boolean condition, String why) {
        if (!condition) throw new AssertionError(why);
        synchronized (this) { checks++; }
    }

    @Override public void onCreate(Bundle state) {
        super.onCreate(state);
        Thread worker = new Thread(() -> {
            try { run(); }
            catch (Throwable error) { fail(error); }
        }, "nsd-fixture-control");
        worker.setDaemon(true);
        worker.start();
    }

    private void run() throws Exception {
        control = Path.of(required("control"));
        type = required("type"); instance = required("instance"); mode = required("mode");
        check(type.matches("_gr[0-9a-f]{12}\\._tcp"), "unique owned service type");
        check(mode.equals("public") || mode.equals("reuse") || mode.equals("death") || mode.equals("uninstall")
                || mode.equals("conflict"), "explicit fixture mode");
        nonce = type.substring(3, 15);
        address = InetAddress.getByName(required("address").split("%", 2)[0]);
        if (address instanceof Inet6Address && address.isLinkLocalAddress()) {
            int actualIndex = getIntent().getIntExtra("interfaceIndex", 0);
            check(actualIndex > 0, "actual IPv6 interface index for the link-local listener");
            address = Inet6Address.getByAddress(null, address.getAddress(), actualIndex);
        }
        check(checkSelfPermission(Manifest.permission.INTERNET) == PackageManager.PERMISSION_GRANTED,
                "real manifest INTERNET grant");
        check(checkSelfPermission(Manifest.permission.ACCESS_NETWORK_STATE)
                == PackageManager.PERMISSION_GRANTED, "real interface-query permission");
        check(Process.myUid() >= 10000, "ordinary assigned application UID");
        check(getApplicationInfo().sourceDir.replace('\\', '/').contains("/data/app/"), "installed code path");
        ConnectivityManager connectivity = getSystemService(ConnectivityManager.class);
        for (Network candidate : connectivity.getAllNetworks()) {
            LinkProperties properties = connectivity.getLinkProperties(candidate);
            if (properties == null) continue;
            for (LinkAddress local : properties.getLinkAddresses()) {
                if (Arrays.equals(local.getAddress().getAddress(), address.getAddress())) {
                    check(network == null || network.equals(candidate), "unambiguous actual Network");
                    network = candidate; interfaceName = properties.getInterfaceName();
                }
            }
        }
        check(network != null && interfaceName != null, "peer address belongs to an actual Network");
        manager = getSystemService(NsdManager.class);
        check(manager != null, "real Context NSD manager");
        check(Class.forName("com.gesher.net.NsdRegistrationClient").getClassLoader()
                == NsdManager.class.getClassLoader(), "same genuine boot provider graph");
        primary = register(instance, 1, null);
        if (mode.equals("reuse")) sibling = register(instance + "-sibling", 1, null);
        saveState();
        write("ready.json", snapshot());
        long deadline = System.nanoTime() + 180_000_000_000L;
        while (!finished && !destroyed) {
            Throwable error = failure.get();
            if (error != null) throw new AssertionError("asynchronous fixture failure", error);
            if (System.nanoTime() >= deadline) throw new AssertionError("180-second fixture deadline");
            Path next = control.resolve("app-command");
            if (!Files.exists(next)) { Thread.sleep(20); continue; }
            String[] command = Files.readString(next, StandardCharsets.UTF_8).trim().split(" ", 2);
            if (command.length != 2) throw new AssertionError("fixture command framing");
            int serial = Integer.parseInt(command[0]);
            if (serial <= commands) { Thread.sleep(20); continue; }
            switch (command[1]) {
                case "reuse": {
                    check(mode.equals("reuse") && primary != null && primary.epoch == 1,
                            "single immediate-reuse transition");
                    Slot old = primary;
                    Slot replacement = new Slot(old.name, 2, old.listener);
                    unregister(old);
                    // Keep the old TCP port reserved until the replacement is bound.
                    // The new Binder request is made immediately after the terminal callback.
                    register(replacement);
                    check(replacement.port != old.port, "replacement owns a different real listener");
                    primary = replacement;
                    old.close();
                    check(old.listener.registered == 2 && old.listener.unregistered == 1,
                            "same listener identity reused exactly once");
                    break;
                }
                case "withdraw-main": {
                    Slot old = primary; check(old != null, "main owner exists");
                    unregister(old); old.close(); primary = null;
                    check(sibling != null && sibling.active, "shared default host still has a service owner");
                    break;
                }
                case "finish": {
                    if (primary != null) { unregister(primary); primary.close(); primary = null; }
                    if (sibling != null) { unregister(sibling); sibling.close(); sibling = null; }
                    check(localManagerOwners() == 0, "terminal manager listener map empty");
                    finished = true;
                    break;
                }
                default: throw new AssertionError("unknown fixture command");
            }
            commands = serial;
            saveState();
            write("app-ack.json", new JSONObject().put("serial", serial).put("command", command[1]));
        }
        if (finished) {
            callbacks.shutdown();
            System.out.println("NSD_REGISTRATION_APP_FINISHED checks=" + checks + " pid=" + Process.myPid());
            System.out.flush();
            runOnUiThread(this::finish);
        }
    }

    private int localManagerOwners() throws Exception {
        // Read-only ownership observation; never replaces the application manager/backend.
        var lock = NsdManager.class.getDeclaredField("mDiscoveries"); lock.setAccessible(true);
        var field = NsdManager.class.getDeclaredField("mRegistrations"); field.setAccessible(true);
        synchronized (lock.get(manager)) { return ((java.util.Map<?, ?>) field.get(manager)).size(); }
    }

    private String required(String key) {
        String value = getIntent().getStringExtra(key);
        if (value == null || value.isEmpty()) throw new IllegalArgumentException("missing fixture " + key);
        return value;
    }

    private Slot register(String name, int epoch, Listener reused) throws Exception {
        return register(new Slot(name, epoch, reused == null ? new Listener() : reused));
    }
    private Slot register(Slot slot) throws Exception {
        check(slot.attributeHash.equals(attributeHash(slot.info)), "exact prepared TXT bytes immediately before registration");
        check(slot.info.getHostname() == null && slot.info.getHostAddresses().isEmpty(), "default host ownership before registration");
        slot.listener.current = slot;
        int before = slot.listener.registered;
        manager.registerService(slot.info, NsdManager.PROTOCOL_DNS_SD, callbacks, slot.listener);
        await(() -> slot.listener.registered == before + 1, "registration callback");
        check(slot.active && slot.name != null, "logical registration completion");
        return slot;
    }

    private void unregister(Slot slot) throws Exception {
        int before = slot.listener.unregistered;
        manager.unregisterService(slot.listener);
        await(() -> slot.listener.unregistered == before + 1, "logical unregister callback");
        check(!slot.active, "registration logically withdrawn");
        try { manager.unregisterService(slot.listener); throw new AssertionError("terminal listener retained"); }
        catch (IllegalArgumentException expected) { check(true, "terminal listener identity released"); }
    }

    private void await(BooleanSupplier done, String why) throws Exception {
        long until = System.nanoTime() + 20_000_000_000L;
        while (!done.getAsBoolean()) {
            Throwable error = failure.get();
            if (error != null) throw new AssertionError(why, error);
            if (System.nanoTime() >= until) throw new AssertionError("fixture deadline: " + why);
            Thread.sleep(5);
        }
    }

    private final class Listener implements NsdManager.RegistrationListener {
        volatile Slot current;
        volatile int registered, unregistered;
        private void executor() {
            check(Thread.currentThread().getName().equals("nsd-fixture-callback"), "selected callback executor");
        }
        public void onServiceRegistered(NsdServiceInfo value) {
            try {
                executor(); Slot slot = current;
                check(slot != null && !slot.active, "one success for current request");
                check(value.getServiceName() != null && value.getServiceType() == null
                        && value.getPort() == 0 && value.getAttributes().isEmpty(),
                        "pinned reduced callback value");
                slot.name = value.getServiceName(); slot.active = true; registered++;
            } catch (Throwable error) { fail(error); }
        }
        public void onServiceUnregistered(NsdServiceInfo value) {
            try {
                executor(); Slot slot = current;
                check(slot != null && slot.active && value == slot.info, "unregister returns original request");
                slot.active = false; unregistered++;
            } catch (Throwable error) { fail(error); }
        }
        public void onRegistrationFailed(NsdServiceInfo value, int error) {
            fail(new AssertionError("real registration failed: " + error));
        }
        public void onUnregistrationFailed(NsdServiceInfo value, int error) {
            fail(new AssertionError("real unregister failed: " + error));
        }
    }

    private final class Slot implements AutoCloseable {
        final int epoch, port;
        final NsdServiceInfo info;
        final Listener listener;
        final ServerSocket server;
        final String attributeHash;
        volatile String name;
        volatile boolean active;
        Slot(String requested, int epoch, Listener listener) throws Exception {
            this.epoch = epoch; this.listener = listener;
            server = new ServerSocket();
            server.bind(new InetSocketAddress(address, 0));
            port = server.getLocalPort();
            info = mode.equals("public") ? new NsdServiceInfo() : resolveSeed(epoch);
            if (!mode.equals("public")) {
                // Explicit additional pinned-source value preparation: SDK36 has
                // getHostname but no public setter. The genuine method is invoked
                // once to release the seed peer's hostname ownership.
                NsdServiceInfo.class.getMethod("setHostname", String.class).invoke(info, new Object[] {null});
                info.setHostAddresses(java.util.Collections.emptyList());
                check(info.getHostname() == null && info.getHostAddresses().isEmpty()
                        && network.equals(info.getNetwork()), "seed host cleared; actual Network retained");
            } else {
                info.setAttribute("flag", (String) null); info.setAttribute("empty", "");
                info.setAttribute("public", "héllo\u0000");
            }
            info.setServiceName(requested); info.setServiceType(type);
            info.setPort(port); info.setNetwork(network);
            check(info.getAttributes().containsKey("flag") && info.getAttributes().get("flag") == null,
                    "resolved flag preserves null");
            check(info.getAttributes().containsKey("empty") && info.getAttributes().get("empty").length == 0,
                    "resolved empty value remains distinct");
            if (!mode.equals("public")) {
                byte[] binary = new byte[205];
                binary[0] = 0; binary[1] = (byte) 255; binary[2] = (byte) 254;
                binary[3] = '='; binary[4] = (byte) 128;
                for (int i = 0; i < 200; i++) binary[i + 5] = (byte) i;
                check(Arrays.equals(info.getAttributes().get("bin"), binary), "resolved opaque binary bytes");
                check(Arrays.equals(info.getAttributes().get("epoch"),
                        Integer.toString(epoch).getBytes(StandardCharsets.US_ASCII)), "resolved epoch bytes");
                for (int i = 0; i < 4; i++) {
                    byte[] fill = new byte[230]; Arrays.fill(fill, (byte) (65 + i));
                    check(Arrays.equals(info.getAttributes().get("p" + i), fill), "resolved full TXT padding");
                }
                byte[] tail = new byte[127];
                for (int i = 0; i < tail.length; i++) tail[i] = (byte) (255 - i);
                check(Arrays.equals(info.getAttributes().get("tail"), tail), "resolved opaque tail bytes");
                int policy = 0;
                for (var entry : info.getAttributes().entrySet()) {
                    policy += entry.getKey().length() + 2 + (entry.getValue() == null ? 0 : entry.getValue().length);
                }
                check(policy == 1300, "full allowed TXT attribute policy bytes");
            } else {
                check(Arrays.equals(info.getAttributes().get("public"), "héllo\u0000".getBytes(StandardCharsets.UTF_8)),
                        "public String registration preserves UTF-8 and embedded NUL");
            }
            attributeHash = attributeHash(info);
            Thread acceptor = new Thread(() -> {
                while (!server.isClosed()) {
                    try (Socket peer = server.accept()) {
                        peer.setSoTimeout(3000);
                        String request = new BufferedReader(new InputStreamReader(
                                peer.getInputStream(), StandardCharsets.US_ASCII)).readLine();
                        if (!("NSD_CONNECT " + nonce).equals(request)) continue;
                        peer.getOutputStream().write(("NSD_REGISTERED " + nonce + " " + epoch + "\n")
                                .getBytes(StandardCharsets.US_ASCII));
                    } catch (SocketException closed) { if (!server.isClosed()) fail(closed); }
                    catch (Throwable error) { fail(error); }
                }
            }, "nsd-fixture-listener-" + port);
            acceptor.setDaemon(true); acceptor.start();
        }
        JSONObject json() throws Exception {
            return new JSONObject().put("name", name).put("requestedName", info.getServiceName())
                    .put("epoch", epoch).put("port", port).put("active", active)
                    .put("preparation", mode.equals("public") ? "sdk36-public-string" : "resolved-binary-plus-pinned-setHostname")
                    .put("attributesCanonicalSha256", attributeHash)
                    .put("registeredCallbacks", listener.registered).put("unregisteredCallbacks", listener.unregistered);
        }
        public void close() throws Exception { server.close(); check(server.isClosed(), "TCP listener closed"); }
    }

    private NsdServiceInfo resolveSeed(int epoch) throws Exception {
        // Ordinary SDK36 obtains arbitrary byte-valued attributes from a resolved
        // service. The owned independent peer supplies the exact 1300-byte set.
        // Resolution uses public descriptors and no TXT-map mutation. The caller
        // explicitly records its separate hidden hostname-clearing preparation.
        NsdServiceInfo seed = new NsdServiceInfo();
        seed.setServiceName("Seed-" + epoch); seed.setServiceType("_gs" + nonce + "._tcp");
        seed.setNetwork(network);
        AtomicReference<NsdServiceInfo> answer = new AtomicReference<>();
        manager.resolveService(seed, callbacks, new NsdManager.ResolveListener() {
            public void onServiceResolved(NsdServiceInfo value) { answer.set(value); }
            public void onResolveFailed(NsdServiceInfo value, int error) {
                fail(new AssertionError("independent binary seed resolution failed: " + error));
            }
        });
        await(() -> answer.get() != null, "independent binary seed resolved");
        check(network.equals(answer.get().getNetwork()), "seed was resolved on the actual selected Network");
        return answer.get();
    }

    private static String attributeHash(NsdServiceInfo value) throws Exception {
        java.io.ByteArrayOutputStream bytes = new java.io.ByteArrayOutputStream();
        java.io.DataOutputStream output = new java.io.DataOutputStream(bytes);
        for (var entry : new java.util.TreeMap<>(value.getAttributes()).entrySet()) {
            byte[] key = entry.getKey().getBytes(StandardCharsets.UTF_8), raw = entry.getValue();
            output.writeInt(key.length); output.write(key); output.writeInt(raw == null ? -1 : raw.length);
            if (raw != null) output.write(raw);
        }
        byte[] digest = java.security.MessageDigest.getInstance("SHA-256").digest(bytes.toByteArray());
        StringBuilder result = new StringBuilder();
        for (byte b : digest) result.append(String.format(java.util.Locale.ROOT, "%02x", b & 255));
        return result.toString();
    }

    private synchronized JSONObject snapshot() throws Exception {
        JSONObject result = new JSONObject().put("pid", Process.myPid()).put("uid", Process.myUid())
                .put("package", getPackageName()).put("sourceDir", getApplicationInfo().sourceDir)
                .put("mode", mode).put("type", type).put("networkHandle", network == null ? 0 : network.getNetworkHandle())
                .put("network", network == null ? JSONObject.NULL : network.toString())
                .put("interfaceName", interfaceName).put("address", address == null ? "" : address.getHostAddress())
                .put("checks", checks).put("finished", finished).put("commands", commands);
        result.put("primary", primary == null ? JSONObject.NULL : primary.json());
        result.put("sibling", sibling == null ? JSONObject.NULL : sibling.json());
        if (failure.get() != null) result.put("error", failure.get().toString());
        return result;
    }
    private void saveState() throws Exception { write("app-state.json", snapshot()); }
    private synchronized void write(String file, JSONObject value) throws Exception {
        Files.createDirectories(control);
        Path temporary = control.resolve(file + ".new");
        Files.writeString(temporary, value.toString() + "\n", StandardCharsets.UTF_8);
        Files.move(temporary, control.resolve(file), StandardCopyOption.REPLACE_EXISTING);
    }
    private void fail(Throwable error) {
        if (!failure.compareAndSet(null, error)) return;
        error.printStackTrace(System.err); System.err.flush();
        try { if (control != null) saveState(); } catch (Throwable reportError) { reportError.printStackTrace(System.err); }
    }
    @Override public void onDestroy() {
        destroyed = true;
        // Death/uninstall cases deliberately leave registration cleanup to the real
        // system owner. These calls close only the application's actual TCP listeners.
        try { if (primary != null) primary.close(); } catch (Exception error) { fail(error); }
        try { if (sibling != null) sibling.close(); } catch (Exception error) { fail(error); }
        callbacks.shutdownNow();
        super.onDestroy();
    }
}
