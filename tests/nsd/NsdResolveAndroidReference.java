/* Copyright (C) 2026 The Gesher Authors. SPDX-License-Identifier: Apache-2.0 */
package gesher.nsd;

import android.content.Context;
import android.net.ConnectivityManager;
import android.net.LinkProperties;
import android.net.Network;
import android.net.nsd.NsdManager;
import android.net.nsd.NsdServiceInfo;
import android.os.Build;
import android.os.Looper;

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
import java.net.SocketException;
import java.net.SocketTimeoutException;
import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collections;
import java.util.List;
import java.util.UUID;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicReference;

/** SDK36-only stock reference. Raw peer uses no NsdManager registration or hidden TXT setter. */
public final class NsdResolveAndroidReference {
    private static int checks;
    private static void check(boolean condition, String message) {
        if (!condition) throw new AssertionError(message);
        System.out.println("NSD_RESOLVE_REFERENCE_CHECK " + (++checks) + " " + message);
    }
    private static String hex(byte[] value) {
        if (value == null) return "null";
        StringBuilder out = new StringBuilder();
        for (byte one : value) out.append(String.format(java.util.Locale.ROOT, "%02x", one & 255));
        return out.toString();
    }
    private static byte[] binary() {
        byte[] value = new byte[205]; value[1] = -1; value[2] = -2; value[3] = '='; value[4] = -128;
        for (int at = 0; at < 200; at++) value[at + 5] = (byte) at;
        return value;
    }
    private static byte[] name(String... labels) throws Exception {
        ByteArrayOutputStream bytes = new ByteArrayOutputStream();
        for (String label : labels) {
            byte[] value = label.getBytes(StandardCharsets.UTF_8);
            if (value.length < 1 || value.length > 63) throw new IllegalArgumentException("owned fixture label");
            bytes.write(value.length); bytes.write(value);
        }
        bytes.write(0); return bytes.toByteArray();
    }
    private static byte[] concat(byte[] first, byte[] second) {
        byte[] out = Arrays.copyOf(first, first.length + second.length);
        System.arraycopy(second, 0, out, first.length, second.length); return out;
    }
    private static byte[] text() throws Exception {
        List<byte[]> entries = new ArrayList<>();
        for (String value : new String[] {"flag", "empty="}) entries.add(value.getBytes(StandardCharsets.US_ASCII));
        entries.add(concat("bin=".getBytes(StandardCharsets.US_ASCII), binary()));
        for (String value : new String[] {"dup=first", "DUP=second", "replace", "REPLACE=later"}) entries.add(value.getBytes(StandardCharsets.US_ASCII));
        for (int at = 0; at < 4; at++) {
            byte[] value = new byte[230]; Arrays.fill(value, (byte) (65 + at));
            entries.add(concat(("p" + at + "=").getBytes(StandardCharsets.US_ASCII), value));
        }
        ByteArrayOutputStream out = new ByteArrayOutputStream();
        for (byte[] entry : entries) { out.write(entry.length); out.write(entry); }
        return out.toByteArray();
    }
    private static void rr(DataOutputStream out, byte[] name, int type, byte[] data) throws Exception {
        out.write(name); out.writeShort(type); out.writeShort(0x8001); out.writeInt(120);
        out.writeShort(data.length); out.write(data);
    }
    private static boolean contains(byte[] bytes, int count, byte[] owned) {
        outer: for (int at = 12; at + owned.length <= count; at++) {
            for (int n = 0; n < owned.length; n++) if (bytes[at + n] != owned[n]) continue outer;
            return true;
        }
        return false;
    }

    private static final class Peer implements AutoCloseable {
        final MulticastSocket udp;
        final ServerSocket tcp;
        final InetAddress address, group;
        final NetworkInterface iface;
        final byte[] instanceName, hostName, wire;
        final String hostname;
        final AtomicReference<Throwable> error = new AtomicReference<>();
        final Thread receiver, endpoint;
        volatile boolean running = true;
        volatile int queries, replies, connections;
        Peer(Network network, NetworkInterface iface, InetAddress address, String type, String instance, String hostname) throws Exception {
            this.iface = iface; this.address = address; this.hostname = hostname;
            group = InetAddress.getByName("224.0.0.251");
            instanceName = name(instance, type.substring(0, type.indexOf('.')), "_tcp", "local");
            hostName = name(hostname, "local");
            tcp = new ServerSocket(); tcp.bind(new InetSocketAddress(address, 0)); tcp.setSoTimeout(250);
            MulticastSocket socket = null;
            try {
                socket = new MulticastSocket(null); socket.setReuseAddress(true);
                socket.bind(new InetSocketAddress(5353));
                network.bindSocket(socket); socket.setNetworkInterface(iface);
                socket.setTimeToLive(255); socket.setLoopbackMode(false); socket.setSoTimeout(250);
                socket.joinGroup(new InetSocketAddress(group, 5353), iface);
            } catch (Throwable failure) { if (socket != null) socket.close(); tcp.close(); throw failure; }
            udp = socket;
            ByteArrayOutputStream srvBytes = new ByteArrayOutputStream(); DataOutputStream srv = new DataOutputStream(srvBytes);
            srv.writeShort(0); srv.writeShort(0); srv.writeShort(tcp.getLocalPort()); srv.write(hostName);
            ByteArrayOutputStream packetBytes = new ByteArrayOutputStream(); DataOutputStream packet = new DataOutputStream(packetBytes);
            packet.writeShort(0); packet.writeShort(0x8400); packet.writeShort(0); packet.writeShort(3); packet.writeShort(0); packet.writeShort(0);
            rr(packet, instanceName, 33, srvBytes.toByteArray()); rr(packet, instanceName, 16, text()); rr(packet, hostName, 1, address.getAddress());
            wire = packetBytes.toByteArray();
            receiver = new Thread(() -> {
                try {
                    while (running) {
                        DatagramPacket incoming = new DatagramPacket(new byte[65535], 65535);
                        try { udp.receive(incoming); }
                        catch (SocketTimeoutException pending) { continue; }
                        byte[] data = incoming.getData(); int length = incoming.getLength();
                        if (length < 12 || (data[2] & 0xf8) != 0 || (data[3] & 15) != 0) continue;
                        if (!contains(data, length, instanceName) && !contains(data, length, hostName)) continue;
                        queries++;
                        udp.send(new DatagramPacket(wire, wire.length, group, 5353)); replies++;
                    }
                } catch (Throwable failure) { if (running) error.set(failure); }
            }, "nsd-reference-independent-wire-peer");
            endpoint = new Thread(() -> {
                try {
                    while (running) {
                        Socket accepted;
                        try { accepted = tcp.accept(); }
                        catch (SocketTimeoutException pending) { continue; }
                        try (Socket client = accepted) {
                            client.getOutputStream().write("gesher-owned-nsd-peer\n".getBytes(StandardCharsets.US_ASCII)); connections++;
                        }
                    }
                } catch (Throwable failure) { if (running) error.set(failure); }
            }, "nsd-reference-independent-tcp-peer");
            receiver.start(); endpoint.start();
        }
        @Override public void close() throws Exception {
            running = false; udp.close(); tcp.close(); receiver.join(3000); endpoint.join(3000);
            check(!receiver.isAlive() && !endpoint.isAlive(), "owned peer sockets/workers retired");
        }
    }

    public static void main(String[] args) {
        try { run(); System.out.println("NsdResolveAndroidReference all passed: " + checks + "; family=4; stockReference=true; windowsExecution=false"); System.exit(0); }
        catch (Throwable error) { error.printStackTrace(System.err); System.exit(1); }
    }
    private static NsdServiceInfo request(String name, String type, Network network) {
        NsdServiceInfo info = new NsdServiceInfo(); info.setServiceName(name); info.setServiceType(type); info.setNetwork(network); return info;
    }
    private static void run() throws Exception {
        System.out.println("NSD_RESOLVE_REFERENCE_PROFILE sdk=" + Build.VERSION.SDK_INT + " build=" + Build.FINGERPRINT
                + " uid=" + android.os.Process.myUid() + " identity=app_process-system-context; ordinaryApkPermissions=false");
        check(Build.VERSION.SDK_INT == 36, "API36 stock profile");
        check(NsdManager.class.getClassLoader() != NsdResolveAndroidReference.class.getClassLoader()
                && NsdManager.class.getClassLoader() == NsdServiceInfo.class.getClassLoader(), "platform NSD provider ownership");
        Looper.prepareMainLooper(); Class<?> activity = Class.forName("android.app.ActivityThread");
        Object thread = activity.getMethod("systemMain").invoke(null);
        Context context = (Context) activity.getMethod("getSystemContext").invoke(thread);
        NsdManager manager = context.getSystemService(NsdManager.class);
        ConnectivityManager connectivity = context.getSystemService(ConnectivityManager.class);
        Network network = connectivity.getActiveNetwork();
        LinkProperties links = connectivity.getLinkProperties(network);
        check(manager != null && network != null && links != null, "actual NSD and active Network");
        NetworkInterface iface = NetworkInterface.getByName(links.getInterfaceName());
        InetAddress address = null;
        for (InetAddress candidate : Collections.list(iface.getInetAddresses())) {
            if (candidate instanceof Inet4Address && !candidate.isLoopbackAddress()) { address = candidate; break; }
        }
        check(address != null && iface.isUp() && iface.supportsMulticast(), "actual usable IPv4 interface");
        String nonce = UUID.randomUUID().toString().replace("-", "").substring(0, 12);
        String type = "_gr" + nonce + "._tcp", instance = "Owned.שלום\\😀" + nonce, hostname = "hgr" + nonce;
        System.out.println("NSD_RESOLVE_REFERENCE_SCOPE network=" + network + " interface=" + iface.getName()
                + " index=" + iface.getIndex() + " ownedAddress=" + address.getHostAddress() + " ownedType=" + type);
        ExecutorService executor = Executors.newSingleThreadExecutor(task -> new Thread(task, "nsd-reference-callback"));
        AtomicReference<NsdServiceInfo> resolved = new AtomicReference<>(), terminalOriginal = new AtomicReference<>();
        AtomicReference<String> failure = new AtomicReference<>(), callbackThread = new AtomicReference<>();
        CountDownLatch complete = new CountDownLatch(1), stopped = new CountDownLatch(1);
        NsdManager.ResolveListener listener = new NsdManager.ResolveListener() {
            public void onServiceResolved(NsdServiceInfo info) { callbackThread.set(Thread.currentThread().getName()); resolved.set(info); complete.countDown(); }
            public void onResolveFailed(NsdServiceInfo info, int error) { failure.set("resolve " + error); complete.countDown(); stopped.countDown(); }
            public void onResolutionStopped(NsdServiceInfo info) { terminalOriginal.set(info); stopped.countDown(); }
            public void onStopResolutionFailed(NsdServiceInfo info, int error) { failure.set("stop " + error); stopped.countDown(); }
        };
        try (Peer peer = new Peer(network, iface, address, type, instance, hostname)) {
            System.out.println("NSD_RESOLVE_REFERENCE_WIRE bytes=" + peer.wire.length + " sha256="
                    + hex(MessageDigest.getInstance("SHA-256").digest(peer.wire)) + " rawTxtBytes=" + text().length
                    + " port=" + peer.tcp.getLocalPort());
            check(peer.wire.length > 1200, "independent raw TXT response exceeds 512 bytes");
            NsdServiceInfo original = request(instance, type, network);
            manager.resolveService(original, executor, listener);
            check(complete.await(30, TimeUnit.SECONDS), "actual resolve callback");
            check(peer.error.get() == null && failure.get() == null, "peer and system service completed without error");
            check(peer.queries > 0 && peer.replies > 0, "real multicast query and response exchange");
            NsdServiceInfo result = resolved.get();
            check(result != null && result != original, "separate result object");
            check("nsd-reference-callback".equals(callbackThread.get()), "application executor selected callback thread");
            check(result.getServiceName().equals(instance) && result.getServiceType().equals("." + type), "Unicode instance and leading-dot resolved type");
            check(result.getPort() == peer.tcp.getLocalPort() && result.getHostname().equals(hostname), "observed SRV port and hostname");
            check(result.getNetwork().equals(network), "actual requested Network preserved");
            check(result.getHostAddresses().stream().anyMatch(value -> Arrays.equals(value.getAddress(), peer.address.getAddress())), "observed peer address");
            for (String key : result.getAttributes().keySet()) System.out.println("NSD_RESOLVE_REFERENCE_TXT " + key + "=" + hex(result.getAttributes().get(key)));
            check(result.getAttributes().containsKey("flag") && result.getAttributes().get("flag") == null, "wire key-only remains null");
            check(result.getAttributes().containsKey("empty") && result.getAttributes().get("empty").length == 0, "wire empty remains empty byte array");
            check(Arrays.equals(result.getAttributes().get("bin"), binary()), "wire NUL/non-UTF8/equals bytes preserved");
            check(Arrays.equals(result.getAttributes().get("dup"), "first".getBytes(StandardCharsets.US_ASCII)), "duplicate first non-null value");
            check(Arrays.equals(result.getAttributes().get("replace"), "later".getBytes(StandardCharsets.US_ASCII)), "null duplicate replacement retains original key spelling");
            check(result.getAttributes().size() == 9, "complete large TXT map");
            int policyBytes = 0;
            for (java.util.Map.Entry<String, byte[]> entry : result.getAttributes().entrySet()) {
                policyBytes += entry.getKey().getBytes(StandardCharsets.US_ASCII).length
                        + (entry.getValue() == null ? 0 : entry.getValue().length) + 2;
            }
            System.out.println("NSD_RESOLVE_REFERENCE_ATTRIBUTE_POLICY bytes=" + policyBytes + " limit=1300");
            check(policyBytes <= 1300 && policyBytes > 512, "successful attributes fit Android 1300-byte policy");
            try (Socket client = new Socket()) {
                client.connect(new InetSocketAddress(result.getHostAddresses().get(0), result.getPort()), 3000);
                client.setSoTimeout(3000); byte[] greeting = new byte[22]; int used = 0;
                while (used < greeting.length) { int amount = client.getInputStream().read(greeting, used, greeting.length - used); if (amount < 0) break; used += amount; }
                check(new String(greeting, 0, used, StandardCharsets.US_ASCII).equals("gesher-owned-nsd-peer\n"), "resolved endpoint reaches the real peer");
            }
            try { manager.stopServiceResolution(listener); throw new AssertionError("Completed listener retained"); }
            catch (IllegalArgumentException expected) { check(true, "completed listener removed before reuse"); }
            NsdServiceInfo pending = request(instance + "-cancel", type, network);
            manager.resolveService(pending, executor, listener);
            try { manager.resolveService(pending, executor, listener); throw new AssertionError("Duplicate active listener accepted"); }
            catch (IllegalArgumentException expected) { check(true, "active duplicate listener rejected"); }
            manager.stopServiceResolution(listener);
            check(stopped.await(15, TimeUnit.SECONDS) && failure.get() == null, "pending request canceled");
            check(terminalOriginal.get() == pending, "stop callback retains original request object");
        } finally { executor.shutdownNow(); }
    }
}
