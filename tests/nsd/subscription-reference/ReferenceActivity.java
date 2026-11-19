/* Copyright (C) 2026 The Gesher Authors. SPDX-License-Identifier: Apache-2.0 */
package gesher.qa.nsdsubscription;

import android.app.Activity;
import android.content.Context;
import android.net.ConnectivityManager;
import android.net.LinkProperties;
import android.net.Network;
import android.net.nsd.NsdManager;
import android.net.nsd.NsdServiceInfo;
import android.os.Binder;
import android.os.Build;
import android.os.Bundle;
import android.os.Process;
import android.os.SystemClock;
import java.io.ByteArrayOutputStream;
import java.io.DataOutputStream;
import java.net.DatagramPacket;
import java.net.Inet4Address;
import java.net.InetAddress;
import java.net.InetSocketAddress;
import java.net.MulticastSocket;
import java.net.NetworkInterface;
import java.net.ServerSocket;
import java.net.Socket;
import java.net.SocketTimeoutException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collections;
import java.util.List;
import java.util.concurrent.BlockingQueue;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.LinkedBlockingQueue;
import java.util.concurrent.TimeUnit;
import org.json.JSONArray;
import org.json.JSONObject;

/** SDK-only ordinary APK. Its owned valid multicast peer does not use NsdManager. */
public final class ReferenceActivity extends Activity {
    private final List<String> checks = new ArrayList<>();
    private void check(boolean condition, String message) {
        if (!condition) throw new AssertionError(message);
        checks.add(message);
    }
    @Override public void onCreate(Bundle state) {
        super.onCreate(state);
        Context app = getApplicationContext(); String type = getIntent().getStringExtra("type");
        new Thread(() -> {
            JSONObject result = new JSONObject(); long begin = SystemClock.elapsedRealtime();
            try {
                result.put("sdk", Build.VERSION.SDK_INT).put("targetSdk", app.getApplicationInfo().targetSdkVersion)
                        .put("uid", Process.myUid()).put("callingUidOnWorker", Binder.getCallingUid())
                        .put("pid", Process.myPid()).put("package", app.getPackageName())
                        .put("internetSelfCheck", app.checkSelfPermission("android.permission.INTERNET"))
                        .put("networkStateSelfCheck", app.checkSelfPermission("android.permission.ACCESS_NETWORK_STATE"));
                check(Process.myUid() >= 10000 && Binder.getCallingUid() == Process.myUid(), "ordinary application identity");
                check(Build.VERSION.SDK_INT == 36 && app.getApplicationInfo().targetSdkVersion == 36, "exact SDK36 and target36 profile");
                if (type == null || !type.matches("_gr[0-9a-f]{12}\\._tcp")) throw new IllegalArgumentException("owned random type required");
                observe(app, type, result); result.put("passed", true);
            } catch (Throwable error) {
                try { result.put("passed", false).put("errorClass", error.getClass().getName()).put("error", String.valueOf(error.getMessage())); }
                catch (Exception ignored) { }
                android.util.Log.e("NSD_SUBSCRIPTION_REFERENCE", "Owned reference failed", error);
            } finally {
                try {
                    result.put("checks", new JSONArray(checks)).put("checkCount", checks.size())
                            .put("elapsedMillis", SystemClock.elapsedRealtime() - begin)
                            .put("stockAndroidReference", true).put("candidateWindowsExecution", false);
                    Files.writeString(new java.io.File(app.getFilesDir(), "result.json").toPath(), result.toString());
                    android.util.Log.i("NSD_SUBSCRIPTION_REFERENCE", result.toString());
                } catch (Exception error) { android.util.Log.e("NSD_SUBSCRIPTION_REFERENCE", "Cannot persist observation", error); }
                runOnUiThread(this::finish);
            }
        }, "owned-nsd-subscription-reference").start();
    }

    private static final class Event {
        final String kind, thread; final int error; final NsdServiceInfo info;
        Event(String kind, int error, NsdServiceInfo info) {
            this.kind = kind; this.error = error; this.info = info; thread = Thread.currentThread().getName();
        }
    }
    private final class Listener implements NsdManager.ServiceInfoCallback {
        final BlockingQueue<Event> events = new LinkedBlockingQueue<>();
        public void onServiceInfoCallbackRegistrationFailed(int error) { events.add(new Event("failed", error, null)); }
        public void onServiceUpdated(NsdServiceInfo info) { events.add(new Event("updated", 0, info)); }
        public void onServiceLost() { events.add(new Event("lost", 0, null)); }
        public void onServiceInfoCallbackUnregistered() { events.add(new Event("unregistered", 0, null)); }
        Event next(String kind) throws Exception {
            Event event = events.poll(5, TimeUnit.SECONDS);
            check(event != null && event.kind.equals(kind), "callback " + kind);
            check(event.thread.equals("owned-nsd-callback"), "application executor delivers " + kind);
            return event;
        }
    }
    private interface Operation { void run(); }
    private void rejects(Class<? extends Throwable> type, Operation action, String label) {
        try { action.run(); } catch (Throwable error) { check(type.isInstance(error), label); return; }
        throw new AssertionError(label + ": no exception");
    }
    private static NsdServiceInfo selector(String instance, String type, Network network) {
        NsdServiceInfo value = new NsdServiceInfo(); value.setServiceName(instance);
        value.setServiceType(type); value.setNetwork(network); return value;
    }
    private void observe(Context app, String type, JSONObject result) throws Exception {
        NsdManager manager = app.getSystemService(NsdManager.class); check(manager != null, "actual platform manager");
        result.put("managerClassLoader", String.valueOf(NsdManager.class.getClassLoader()));
        ConnectivityManager connectivity = app.getSystemService(ConnectivityManager.class);
        Network network = connectivity.getActiveNetwork(); check(network != null, "actual active Network");
        LinkProperties properties = connectivity.getLinkProperties(network);
        NetworkInterface iface = properties == null ? null : NetworkInterface.getByName(properties.getInterfaceName());
        check(iface != null && iface.isUp(), "actual selected network interface");
        InetAddress address = null;
        for (InetAddress candidate : Collections.list(iface.getInetAddresses())) {
            if (candidate instanceof Inet4Address && !candidate.isLoopbackAddress()) { address = candidate; break; }
        }
        check(address != null, "actual selected IPv4 address");
        result.put("network", network.toString()).put("interface", iface.getName()).put("interfaceIndex", iface.getIndex());
        ExecutorService executor = Executors.newSingleThreadExecutor(action -> new Thread(action, "owned-nsd-callback"));
        Listener listener = new Listener(); boolean registered = false;
        String instance = "Owned.שלום\\😀";
        try (Peer peer = new Peer(network, iface, address, type, instance)) {
            rejects(NullPointerException.class, () -> manager.registerServiceInfoCallback(null, executor, listener), "null selector rejected synchronously");
            rejects(IllegalArgumentException.class, () -> manager.unregisterServiceInfoCallback(listener), "unknown listener rejected synchronously");
            NsdServiceInfo selector = selector(instance, type, network);
            manager.registerServiceInfoCallback(selector, executor, listener); registered = true;
            rejects(IllegalArgumentException.class, () -> manager.registerServiceInfoCallback(selector, executor, listener), "duplicate callback identity rejected");
            peer.send(); Event first = listener.next("updated");
            check(first.info.getServiceName().equals(instance) && first.info.getServiceType().equals(type), "name and bare subscription type preserved");
            check(network.equals(first.info.getNetwork()) && first.info.getHostAddresses().contains(address), "actual Network and address preserved");
            check(Arrays.equals(first.info.getAttributes().get("binary"), Peer.binary(1)), "opaque TXT preserved");
            check(first.info.getAttributes().containsKey("flag") && first.info.getAttributes().get("flag") == null
                    && first.info.getAttributes().get("empty").length == 0, "key-only and empty TXT are distinct");
            try (Socket socket = new Socket()) {
                socket.connect(new InetSocketAddress(first.info.getHost(), first.info.getPort()), 3000); socket.setSoTimeout(3000);
                check(socket.getInputStream().read() == 'O', "returned TCP endpoint belongs to the owned peer");
            }
            peer.send(); check(listener.events.poll(250, TimeUnit.MILLISECONDS) == null, "identical record refresh is silent");
            peer.update(2); Event update = listener.next("updated");
            check(Arrays.equals(update.info.getAttributes().get("binary"), Peer.binary(2)), "changed binary TXT delivered");
            peer.goodbye(); listener.next("lost"); peer.update(3); Event reappeared = listener.next("updated");
            check(Arrays.equals(reappeared.info.getAttributes().get("binary"), Peer.binary(3)), "same subscription observes reappearance");
            manager.unregisterServiceInfoCallback(listener); listener.next("unregistered"); registered = false;
            peer.update(4); check(listener.events.poll(250, TimeUnit.MILLISECONDS) == null, "no updates after unregistration completion");
            manager.registerServiceInfoCallback(selector, executor, listener); registered = true;
            peer.send(); listener.next("updated"); manager.unregisterServiceInfoCallback(listener); listener.next("unregistered"); registered = false;
            check(listener.events.isEmpty(), "listener reusable with one terminal callback per lifetime");
            Listener invalid = new Listener(); manager.registerServiceInfoCallback(selector(instance, "owned", network), executor, invalid);
            check(invalid.next("failed").error == NsdManager.FAILURE_BAD_PARAMETERS, "invalid service type fails asynchronously with BAD_PARAMETERS");
            result.put("ownedQueries", peer.queries).put("sentPackets", peer.sent).put("tcpConnections", peer.connections);
            check(peer.error == null, "owned peer remained healthy");
        } finally {
            if (registered) try { manager.unregisterServiceInfoCallback(listener); listener.next("unregistered"); } catch (Exception ignored) { }
            executor.shutdown(); check(executor.awaitTermination(3, TimeUnit.SECONDS), "callback executor retired");
        }
    }

    private final class Peer implements AutoCloseable {
        final MulticastSocket udp; final ServerSocket tcp; final InetAddress group, address;
        final byte[] owner, host, base; final Thread worker, endpoint;
        volatile byte[] wire; volatile boolean running = true, answering = true;
        volatile Throwable error; volatile int queries, sent, connections;
        Peer(Network network, NetworkInterface iface, InetAddress address, String type, String instance) throws Exception {
            this.address = address; group = InetAddress.getByName("224.0.0.251");
            base = name(type.substring(0, type.indexOf('.')), "_tcp", "local");
            owner = name(instance, type.substring(0, type.indexOf('.')), "_tcp", "local"); host = name("h" + type.substring(1, 15), "local");
            tcp = new ServerSocket(); tcp.bind(new InetSocketAddress(address, 0)); tcp.setSoTimeout(200);
            MulticastSocket created = null;
            try {
                created = new MulticastSocket(null); created.setReuseAddress(true); created.bind(new InetSocketAddress(5353));
                network.bindSocket(created); created.setNetworkInterface(iface); created.setTimeToLive(255);
                created.setLoopbackMode(false); created.setSoTimeout(200); created.joinGroup(new InetSocketAddress(group, 5353), iface);
            } catch (Throwable failure) { if (created != null) created.close(); tcp.close(); throw failure; }
            udp = created; wire = packet(1);
            worker = new Thread(() -> {
                try { while (running) {
                    DatagramPacket incoming = new DatagramPacket(new byte[65535], 65535);
                    try { udp.receive(incoming); } catch (SocketTimeoutException pending) { continue; }
                    byte[] bytes = incoming.getData(); int length = incoming.getLength();
                    if (length < 12 || (bytes[2] & 0xf8) != 0 || (bytes[3] & 15) != 0
                            || !contains(bytes, length, owner) && !contains(bytes, length, host)) continue;
                    queries++; if (answering) send();
                } } catch (Throwable failure) { if (running) error = failure; }
            }, "owned-nsd-peer");
            endpoint = new Thread(() -> {
                try { while (running) {
                    Socket socket; try { socket = tcp.accept(); } catch (SocketTimeoutException pending) { continue; }
                    try (Socket client = socket) { client.getOutputStream().write('O'); connections++; }
                } } catch (Throwable failure) { if (running) error = failure; }
            }, "owned-nsd-endpoint"); worker.start(); endpoint.start();
        }
        synchronized void send() throws Exception { byte[] current = wire; udp.send(new DatagramPacket(current, current.length, group, 5353)); sent++; }
        synchronized void update(int value) throws Exception { wire = packet(value); answering = true; send(); }
        synchronized void goodbye() throws Exception {
            answering = false; ByteArrayOutputStream bytes = new ByteArrayOutputStream(); DataOutputStream out = new DataOutputStream(bytes);
            header(out, 1); rr(out, base, 12, owner, 0, 1); byte[] value = bytes.toByteArray(); udp.send(new DatagramPacket(value, value.length, group, 5353)); sent++;
        }
        byte[] packet(int value) throws Exception {
            ByteArrayOutputStream srv = new ByteArrayOutputStream(); DataOutputStream data = new DataOutputStream(srv);
            data.writeShort(0); data.writeShort(0); data.writeShort(tcp.getLocalPort()); data.write(host);
            ByteArrayOutputStream txt = new ByteArrayOutputStream();
            for (byte[] entry : List.of("flag".getBytes(StandardCharsets.US_ASCII), "empty=".getBytes(StandardCharsets.US_ASCII),
                    join("binary=".getBytes(StandardCharsets.US_ASCII), binary(value)))) { txt.write(entry.length); txt.write(entry); }
            ByteArrayOutputStream bytes = new ByteArrayOutputStream(); DataOutputStream out = new DataOutputStream(bytes);
            header(out, 3); rr(out, owner, 33, srv.toByteArray(), 120, 0x8001); rr(out, owner, 16, txt.toByteArray(), 120, 0x8001);
            rr(out, host, 1, address.getAddress(), 120, 0x8001); return bytes.toByteArray();
        }
        static byte[] binary(int value) { return new byte[]{0, (byte)255, (byte)254, '=', (byte)128, (byte)value}; }
        public void close() throws Exception {
            running = false; udp.close(); tcp.close(); worker.join(3000); endpoint.join(3000);
            check(!worker.isAlive() && !endpoint.isAlive(), "owned multicast/TCP sockets and workers retired");
        }
    }
    private static byte[] join(byte[] a, byte[] b) { byte[] out = Arrays.copyOf(a, a.length + b.length); System.arraycopy(b, 0, out, a.length, b.length); return out; }
    private static byte[] name(String... labels) throws Exception {
        ByteArrayOutputStream out = new ByteArrayOutputStream(); for (String label : labels) {
            byte[] value = label.getBytes(StandardCharsets.UTF_8); if (value.length == 0 || value.length > 63) throw new IllegalArgumentException("owned DNS label");
            out.write(value.length); out.write(value);
        } out.write(0); return out.toByteArray();
    }
    private static void header(DataOutputStream out, int count) throws Exception { out.writeShort(0); out.writeShort(0x8400); out.writeShort(0); out.writeShort(count); out.writeShort(0); out.writeShort(0); }
    private static void rr(DataOutputStream out, byte[] name, int type, byte[] body, int ttl, int cls) throws Exception {
        out.write(name); out.writeShort(type); out.writeShort(cls); out.writeInt(ttl); out.writeShort(body.length); out.write(body);
    }
    private static boolean contains(byte[] bytes, int size, byte[] owned) {
        outer: for (int at = 12; at + owned.length <= size; at++) { for (int n = 0; n < owned.length; n++) if (bytes[at + n] != owned[n]) continue outer; return true; } return false;
    }
}
