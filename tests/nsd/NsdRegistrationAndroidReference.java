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
import android.os.SystemClock;
import org.json.JSONArray;
import org.json.JSONObject;

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
import java.security.MessageDigest;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Base64;
import java.util.Collections;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.TreeMap;
import java.util.UUID;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicReference;

/** Normal owned-service SDK36 reference. Defines no platform/provider classes. */
public final class NsdRegistrationAndroidReference {
    private static int checks;
    private static final long START = SystemClock.elapsedRealtime();
    private static void check(boolean value, String label) {
        if (!value) throw new AssertionError(label);
        System.out.println("NSD_REGISTRATION_REFERENCE_CHECK " + (++checks) + " " + label);
    }
    private static String hex(byte[] value) {
        if (value == null) return null;
        StringBuilder text = new StringBuilder();
        for (byte one : value) text.append(String.format(java.util.Locale.ROOT, "%02x", one & 255));
        return text.toString();
    }
    private static Object nullable(Object value) { return value == null ? JSONObject.NULL : value; }
    private static void emit(String kind, JSONObject data) {
        try { data.put("elapsedMs", SystemClock.elapsedRealtime() - START); }
        catch (Exception impossible) { throw new AssertionError(impossible); }
        System.out.println("NSD_REGISTRATION_REFERENCE_" + kind + " " + data);
    }
    private static JSONObject info(NsdServiceInfo value) throws Exception {
        JSONObject out = new JSONObject();
        out.put("name", nullable(value.getServiceName()));
        out.put("type", nullable(value.getServiceType()));
        out.put("port", value.getPort());
        out.put("hostname", nullable(value.getHostname()));
        out.put("network", value.getNetwork() == null ? JSONObject.NULL : value.getNetwork().toString());
        JSONObject attributes = new JSONObject();
        for (Map.Entry<String, byte[]> entry : new TreeMap<>(value.getAttributes()).entrySet()) {
            attributes.put(entry.getKey(), nullable(hex(entry.getValue())));
        }
        out.put("attributeHex", attributes);
        JSONArray addresses = new JSONArray();
        for (InetAddress address : value.getHostAddresses()) addresses.put(address.getHostAddress());
        out.put("addresses", addresses);
        return out;
    }
    private static String asciiLower(String value) {
        StringBuilder out = new StringBuilder(value.length());
        for (int i = 0; i < value.length(); i++) {
            char c = value.charAt(i); out.append(c >= 'A' && c <= 'Z' ? (char)(c + 32) : c);
        }
        return out.toString();
    }
    private static List<String> canonical(List<String> value) {
        List<String> out = new ArrayList<>(); for (String part : value) out.add(asciiLower(part)); return out;
    }
    private static int u16(byte[] data, int at) {
        if (at < 0 || at + 2 > data.length) throw new IllegalArgumentException("DNS extent");
        return ((data[at] & 255) << 8) | (data[at + 1] & 255);
    }
    private static long u32(byte[] data, int at) {
        return ((long)u16(data, at) << 16) | u16(data, at + 2);
    }
    private static List<String> readName(byte[] data, int[] position) {
        List<String> labels = new ArrayList<>();
        int at = position[0]; boolean jumped = false; boolean[] visited = new boolean[data.length];
        for (int count = 0; count < 256; count++) {
            if (at < 0 || at >= data.length || visited[at]) throw new IllegalArgumentException("DNS name extent");
            visited[at] = true; int size = data[at++] & 255;
            if ((size & 0xc0) == 0xc0) {
                if (at >= data.length) throw new IllegalArgumentException("DNS pointer extent");
                int target = ((size & 0x3f) << 8) | (data[at++] & 255);
                if (!jumped) position[0] = at;
                jumped = true; at = target; continue;
            }
            if (size > 63 || at + size > data.length) throw new IllegalArgumentException("DNS label extent");
            if (size == 0) { if (!jumped) position[0] = at; return labels; }
            labels.add(new String(data, at, size, StandardCharsets.UTF_8)); at += size;
        }
        throw new IllegalArgumentException("DNS label count");
    }
    private static final class Record {
        List<String> owner, target;
        int type, clazz, port, section;
        long ttl;
        byte[] rdata;
        Map<String, byte[]> text;
    }
    private static final class Packet {
        final byte[] bytes;
        final int id, flags, questions;
        final long receivedElapsed = SystemClock.elapsedRealtime();
        int sequence;
        final List<Record> records = new ArrayList<>();
        Packet(byte[] input) {
            bytes = input;
            if (input.length < 12) throw new IllegalArgumentException("DNS header extent");
            id = u16(input, 0); flags = u16(input, 2); questions = u16(input, 4);
            int answers = u16(input, 6), authorities = u16(input, 8), additional = u16(input, 10);
            if (questions + answers + authorities + additional > 4096) throw new IllegalArgumentException("DNS count");
            int[] at = {12};
            for (int q = 0; q < questions; q++) { readName(input, at); u16(input, at[0]); u16(input, at[0] + 2); at[0] += 4; }
            int[] counts = {answers, authorities, additional};
            for (int section = 0; section < counts.length; section++) {
                for (int i = 0; i < counts[section]; i++) {
                    Record r = new Record(); r.owner = readName(input, at); r.section = section;
                    r.type = u16(input, at[0]); r.clazz = u16(input, at[0] + 2); r.ttl = u32(input, at[0] + 4);
                    int size = u16(input, at[0] + 8); at[0] += 10; int begin = at[0], end = begin + size;
                    if (end < begin || end > input.length) throw new IllegalArgumentException("DNS record extent");
                    r.rdata = Arrays.copyOfRange(input, begin, end);
                    if (r.type == 33 && size >= 7) {
                        r.port = u16(input, begin + 4); int[] target = {begin + 6};
                        r.target = readName(input, target);
                        if (target[0] > end) throw new IllegalArgumentException("SRV name extent");
                    } else if (r.type == 12) {
                        int[] target = {begin}; r.target = readName(input, target);
                        if (target[0] > end) throw new IllegalArgumentException("PTR name extent");
                    } else if (r.type == 16) {
                        r.text = new LinkedHashMap<>(); int cursor = begin;
                        while (cursor < end) {
                            int length = input[cursor++] & 255, finish = cursor + length;
                            if (finish > end) throw new IllegalArgumentException("TXT extent");
                            int split = cursor; while (split < finish && input[split] != '=') split++;
                            String key = new String(input, cursor, split - cursor, StandardCharsets.US_ASCII);
                            if (!r.text.containsKey(key)) r.text.put(key, split == finish ? null : Arrays.copyOfRange(input, split + 1, finish));
                            cursor = finish;
                        }
                    }
                    records.add(r); at[0] = end;
                }
            }
        }
        boolean owned(String nonce) {
            for (Record r : records) {
                for (String label : r.owner) if (label.contains(nonce)) return true;
                if (r.target != null) for (String label : r.target) if (label.contains(nonce)) return true;
            }
            return false;
        }
    }
    private static void writeName(DataOutputStream output, List<String> labels) throws Exception {
        for (String label : labels) {
            byte[] raw = label.getBytes(StandardCharsets.UTF_8);
            if (raw.length < 1 || raw.length > 63) throw new IllegalArgumentException("Owned DNS label");
            output.writeByte(raw.length); output.write(raw);
        }
        output.writeByte(0);
    }
    private static byte[] query(List<String> owner, int id, int[] types) throws Exception {
        ByteArrayOutputStream bytes = new ByteArrayOutputStream(); DataOutputStream output = new DataOutputStream(bytes);
        output.writeShort(id); output.writeShort(0); output.writeShort(types.length);
        output.writeShort(0); output.writeShort(0); output.writeShort(0);
        for (int type : types) { writeName(output, owner); output.writeShort(type); output.writeShort(1); }
        return bytes.toByteArray();
    }
    private static final class Endpoint implements AutoCloseable {
        final ServerSocket socket;
        final byte[] greeting;
        final Thread worker;
        final AtomicReference<Throwable> error = new AtomicReference<>();
        final AtomicInteger connections = new AtomicInteger();
        volatile boolean running = true;
        Endpoint(InetAddress address, String label) throws Exception {
            greeting = ("gesher-owned-registration:" + label + "\n").getBytes(StandardCharsets.UTF_8);
            socket = new ServerSocket(); socket.bind(new InetSocketAddress(address, 0)); socket.setSoTimeout(250);
            worker = new Thread(() -> {
                try {
                    while (running) {
                        Socket accepted;
                        try { accepted = socket.accept(); } catch (SocketTimeoutException waiting) { continue; }
                        try (Socket client = accepted) { client.getOutputStream().write(greeting); connections.incrementAndGet(); }
                    }
                } catch (Throwable failure) { if (running) error.compareAndSet(null, failure); }
            }, "nsd-reference-owned-tcp-" + label);
            worker.start();
        }
        @Override public void close() throws Exception {
            running = false; socket.close(); worker.join(3000); check(!worker.isAlive(), "owned TCP worker retired");
        }
    }
    private static final class Peer implements AutoCloseable {
        final Network network;
        final NetworkInterface iface;
        final InetAddress address, group;
        final String nonce;
        final MulticastSocket observer;
        final Thread worker;
        final List<Packet> multicast = Collections.synchronizedList(new ArrayList<>());
        final List<List<String>> requestedHosts = Collections.synchronizedList(new ArrayList<>());
        final AtomicReference<Throwable> error = new AtomicReference<>();
        volatile boolean running = true;
        int wireSequence;
        Peer(Network network, NetworkInterface iface, InetAddress address, String nonce) throws Exception {
            this.network = network; this.iface = iface; this.address = address; this.nonce = nonce;
            group = InetAddress.getByName("224.0.0.251");
            observer = new MulticastSocket(null);
            try {
                observer.setReuseAddress(true); observer.bind(new InetSocketAddress(5353));
                network.bindSocket(observer); observer.setNetworkInterface(iface);
                observer.setTimeToLive(255); observer.setLoopbackMode(false); observer.setSoTimeout(250);
                observer.joinGroup(new InetSocketAddress(group, 5353), iface);
            } catch (Throwable failure) { observer.close(); throw failure; }
            worker = new Thread(() -> {
                try {
                    while (running) {
                        DatagramPacket incoming = new DatagramPacket(new byte[9000], 9000);
                        try { observer.receive(incoming); } catch (SocketTimeoutException waiting) { continue; }
                        if (incoming.getPort() != 5353 || !incoming.getAddress().equals(address)) continue;
                        Packet packet;
                        try { packet = new Packet(Arrays.copyOf(incoming.getData(), incoming.getLength())); }
                        catch (IllegalArgumentException unrelated) { continue; }
                        boolean owned = packet.owned(nonce);
                        if (!owned) synchronized (requestedHosts) {
                            for (Record record : packet.records) if (record.type == 1
                                    && requestedHosts.contains(canonical(record.owner))) owned = true;
                        }
                        if (!owned) continue;
                        synchronized (multicast) {
                            if (multicast.size() >= 128) throw new IllegalStateException("Owned observation bound");
                            wire("multicast-observer", packet, incoming.getAddress(), incoming.getPort());
                            multicast.add(packet);
                            multicast.notifyAll();
                        }
                    }
                } catch (Throwable failure) { if (running) error.compareAndSet(null, failure); }
            }, "nsd-reference-owned-wire-observer");
            worker.start();
        }
        synchronized void wire(String channel, Packet packet, InetAddress source, int port) throws Exception {
            packet.sequence = ++wireSequence;
            JSONObject out = new JSONObject(); out.put("sequence", packet.sequence); out.put("channel", channel);
            out.put("receivedElapsedMs", packet.receivedElapsed - START);
            out.put("source", source.getHostAddress()); out.put("sourcePort", port);
            out.put("id", packet.id); out.put("flags", packet.flags); out.put("questions", packet.questions);
            out.put("bytes", packet.bytes.length); out.put("sha256", hex(MessageDigest.getInstance("SHA-256").digest(packet.bytes)));
            out.put("base64", Base64.getEncoder().encodeToString(packet.bytes)); emit("WIRE", out);
        }
        List<Packet> ask(String name, String type, int id) throws Exception {
            List<String> owner = Arrays.asList(name, type.substring(0, type.indexOf('.')), "_tcp", "local");
            return ask(owner, id, new int[]{33, 16});
        }
        List<Packet> ask(List<String> owner, int scenario, int[] types) throws Exception {
            // Standard QM multicast. This measures fresh wire availability during
            // a query window; a periodic announcement can supply the records.
            // It does not establish causal query response or legacy-unicast support.
            byte[] question = query(owner, 0, types); List<Packet> seen = new ArrayList<>();
            if (types.length == 1 && types[0] == 1) requestedHosts.add(canonical(owner));
            int examined, afterSequence;
            synchronized (multicast) { examined = multicast.size(); afterSequence = wireSequence; }
            long boundary = SystemClock.elapsedRealtime();
            JSONObject sent = new JSONObject(); sent.put("ownerLabels", new JSONArray(owner)); sent.put("id", 0);
            sent.put("scenario", scenario); sent.put("afterSequence", afterSequence); sent.put("freshnessBoundaryMs", boundary - START);
            sent.put("responseCausalityEstablished", false);
            JSONArray questionTypes = new JSONArray(); for (int one : types) questionTypes.put(one); sent.put("types", questionTypes);
            sent.put("sourcePort", observer.getLocalPort()); sent.put("base64", Base64.getEncoder().encodeToString(question)); emit("QUERY", sent);
            for (int attempt = 0; attempt < 3; attempt++) {
                observer.send(new DatagramPacket(question, question.length, group, 5353));
                long end = SystemClock.elapsedRealtime() + 1200;
                while (SystemClock.elapsedRealtime() < end) {
                    synchronized (multicast) {
                        while (examined < multicast.size()) {
                            Packet packet = multicast.get(examined++);
                            if (packet.receivedElapsed < boundary || (packet.flags & 0x8000) == 0) continue;
                            boolean matches = false;
                            for (Record record : packet.records) if (canonical(record.owner).equals(canonical(owner))) matches = true;
                            if (matches) seen.add(packet);
                        }
                        boolean complete = true; for (int one : types) complete &= has(seen, owner, one);
                        if (complete) return seen;
                        multicast.wait(100);
                    }
                }
            }
            return seen;
        }
        @Override public void close() throws Exception {
            running = false; observer.close(); worker.join(3000); check(!worker.isAlive(), "owned UDP observer retired");
        }
    }
    private static boolean has(List<Packet> packets, List<String> owner, int type) {
        for (Packet packet : packets) for (Record r : packet.records) {
            if (r.type == type && r.ttl > 0 && canonical(r.owner).equals(canonical(owner))) return true;
        }
        return false;
    }
    private static final class Registration implements NsdManager.RegistrationListener {
        final String label;
        final NsdServiceInfo original;
        final CountDownLatch registered = new CountDownLatch(1), unregistered = new CountDownLatch(1);
        final AtomicReference<Throwable> error = new AtomicReference<>();
        final AtomicInteger registerEvents = new AtomicInteger(), unregisterEvents = new AtomicInteger();
        volatile NsdServiceInfo result;
        volatile String registerThread;
        volatile boolean active;
        volatile boolean submitted;
        Registration(String label, NsdServiceInfo original) { this.label = label; this.original = original; }
        void event(String name, NsdServiceInfo service, Integer failure) {
            try {
                JSONObject out = info(service); out.put("registration", label); out.put("event", name);
                out.put("thread", Thread.currentThread().getName()); out.put("sameAsOriginal", service == original);
                out.put("error", nullable(failure)); emit("CALLBACK", out);
            } catch (Throwable problem) { error.compareAndSet(null, problem); }
        }
        public void onServiceRegistered(NsdServiceInfo service) {
            active = true; result = service; registerThread = Thread.currentThread().getName(); registerEvents.incrementAndGet();
            event("registered", service, null); registered.countDown();
        }
        public void onRegistrationFailed(NsdServiceInfo service, int code) {
            error.compareAndSet(null, new IllegalStateException("Registration failed " + code));
            event("registrationFailed", service, code); registered.countDown();
        }
        public void onServiceUnregistered(NsdServiceInfo service) {
            active = false; unregisterEvents.incrementAndGet(); event("unregistered", service, null); unregistered.countDown();
        }
        public void onUnregistrationFailed(NsdServiceInfo service, int code) {
            error.compareAndSet(null, new IllegalStateException("Unregistration failed " + code));
            event("unregistrationFailed", service, code); unregistered.countDown();
        }
    }
    private static NsdServiceInfo request(String name, String type, Network network, int port, String nonce) {
        NsdServiceInfo value = new NsdServiceInfo(); value.setServiceName(name); value.setServiceType(type);
        value.setNetwork(network); value.setPort(port);
        // The SDK36 public String overload is the only TXT setter used. No hidden raw-byte access.
        value.setAttribute("flag", (String)null); value.setAttribute("empty", "");
        value.setAttribute("binary", "nul\u0000equals=\u05e9\u05dc\u05d5\u05dd\ud83d\ude00"); value.setAttribute("owned", nonce);
        return value;
    }
    private static void registered(Registration registration) throws Exception {
        check(registration.registered.await(25, TimeUnit.SECONDS), registration.label + " callback delivered");
        check(registration.error.get() == null && registration.result != null, registration.label + " registration succeeded");
        check(registration.result.getServiceName() != null && !registration.result.getServiceName().isEmpty(), registration.label + " actual assigned name");
    }
    private static void stop(NsdManager manager, Registration registration) throws Exception {
        manager.unregisterService(registration);
        check(registration.unregistered.await(15, TimeUnit.SECONDS), registration.label + " unregister callback delivered");
        check(registration.error.get() == null && !registration.active, registration.label + " logically unregistered");
    }
    private static void verifyPeer(Peer peer, Registration registration, Endpoint endpoint, String type, int id) throws Exception {
        String name = registration.result.getServiceName(); List<String> owner = Arrays.asList(name, type.substring(0, type.indexOf('.')), "_tcp", "local");
        List<Packet> packets = peer.ask(name, type, id); Record srv = null, txt = null;
        for (Packet packet : packets) for (Record record : packet.records) {
            if (record.ttl <= 0 || !canonical(record.owner).equals(canonical(owner))) continue;
            if (record.type == 33) srv = record;
            if (record.type == 16) txt = record;
        }
        check(srv != null && txt != null, registration.label + " independent wire SRV and TXT observed");
        check(srv.port == endpoint.socket.getLocalPort(), registration.label + " advertised actual listening port");
        for (Map.Entry<String, byte[]> expected : registration.original.getAttributes().entrySet()) {
            check(txt.text.containsKey(expected.getKey()) && Arrays.equals(txt.text.get(expected.getKey()), expected.getValue()), registration.label + " TXT bytes preserved " + expected.getKey());
        }
        check(txt.text.size() == registration.original.getAttributes().size(), registration.label + " complete TXT map");
        InetAddress observedAddress = null;
        for (Packet packet : packets) for (Record record : packet.records) {
            if (record.type == 1 && record.ttl > 0 && canonical(record.owner).equals(canonical(srv.target)) && Arrays.equals(record.rdata, peer.address.getAddress())) observedAddress = InetAddress.getByAddress(record.rdata);
        }
        boolean addressIncludedWithServiceRecords = observedAddress != null;
        if (observedAddress == null) {
            List<Packet> addressPackets = peer.ask(srv.target, id + 0x100, new int[]{1});
            packets.addAll(addressPackets);
            for (Packet packet : addressPackets) for (Record record : packet.records) {
                if (record.type == 1 && record.ttl > 0 && canonical(record.owner).equals(canonical(srv.target)) && Arrays.equals(record.rdata, peer.address.getAddress())) observedAddress = InetAddress.getByAddress(record.rdata);
            }
        }
        check(observedAddress != null, registration.label + " actual owned address obtained from DNS wire");
        JSONObject observation = new JSONObject(); observation.put("registration", registration.label);
        observation.put("observedHostLabels", new JSONArray(srv.target)); observation.put("ownedAddressIncludedWithServiceRecords", addressIncludedWithServiceRecords);
        observation.put("observedAddress", observedAddress.getHostAddress());
        observation.put("scenario", id);
        JSONArray packetSequences = new JSONArray(); for (Packet packet : packets) packetSequences.put(packet.sequence);
        observation.put("packetSequences", packetSequences); observation.put("responseCausalityEstablished", false);
        observation.put("endpointAddressSource", "observed-owned-service-DNS-A-record"); emit("ENDPOINT", observation);
        try (Socket client = new Socket()) {
            client.connect(new InetSocketAddress(observedAddress, srv.port), 3000); client.setSoTimeout(3000);
            byte[] got = new byte[endpoint.greeting.length]; int used = 0;
            while (used < got.length) { int count = client.getInputStream().read(got, used, got.length - used); if (count < 0) break; used += count; }
            check(used == got.length && Arrays.equals(got, endpoint.greeting), registration.label + " wire port reaches real owned endpoint");
        }
    }
    public static void main(String[] args) {
        try {
            Looper.prepareMainLooper(); Class<?> activity = Class.forName("android.app.ActivityThread");
            Object thread = activity.getMethod("systemMain").invoke(null);
            Context context = (Context)activity.getMethod("getSystemContext").invoke(thread);
            new Thread(() -> {
                try { run(context); System.out.println("NsdRegistrationAndroidReference all passed: " + checks + "; stockReference=true; windowsExecution=false; ordinaryApkPermissions=false"); System.exit(0); }
                catch (Throwable failure) { failure.printStackTrace(System.err); System.exit(1); }
            }, "nsd-registration-reference-worker").start();
            Looper.loop();
        } catch (Throwable failure) { failure.printStackTrace(System.err); System.exit(1); }
    }
    private static void run(Context context) throws Exception {
        JSONObject profile = new JSONObject(); profile.put("sdk", Build.VERSION.SDK_INT); profile.put("fingerprint", Build.FINGERPRINT);
        profile.put("uid", android.os.Process.myUid()); profile.put("pid", android.os.Process.myPid()); profile.put("package", context.getPackageName());
        profile.put("targetSdk", context.getApplicationInfo().targetSdkVersion); profile.put("context", "app_process-system-context");
        profile.put("ordinaryApkPermissions", false); profile.put("txtSetter", "public SDK36 String overload only");
        profile.put("peerMode", "multicast-5353-fresh-owned-wire");
        profile.put("periodicAnnouncementsCanSupplyRecords", true); profile.put("legacyUnicastQualified", false);
        profile.put("nsdLoader", String.valueOf(NsdManager.class.getClassLoader())); profile.put("referenceLoader", String.valueOf(NsdRegistrationAndroidReference.class.getClassLoader())); emit("PROFILE", profile);
        check(Build.VERSION.SDK_INT == 36, "stock API36 profile");
        check(NsdManager.class.getClassLoader() == NsdServiceInfo.class.getClassLoader() && NsdManager.class.getClassLoader() != NsdRegistrationAndroidReference.class.getClassLoader(), "genuine platform NSD provider graph");
        NsdManager manager = context.getSystemService(NsdManager.class); ConnectivityManager connectivity = context.getSystemService(ConnectivityManager.class);
        Network network = connectivity.getActiveNetwork(); LinkProperties links = connectivity.getLinkProperties(network);
        check(manager != null && network != null && links != null, "actual NSD service and active Network");
        NetworkInterface iface = NetworkInterface.getByName(links.getInterfaceName()); InetAddress address = null;
        for (InetAddress candidate : Collections.list(iface.getInetAddresses())) if (candidate instanceof Inet4Address && !candidate.isLoopbackAddress()) { address = candidate; break; }
        check(address != null && iface.isUp() && iface.supportsMulticast(), "actual multicast-capable IPv4 scope");
        String nonce = UUID.randomUUID().toString().replace("-", "").substring(0, 10), type = "_gr" + nonce + "._tcp";
        String offered = "Owned.\u05e9\u05dc\u05d5\u05dd\\\ud83d\ude00-" + nonce;
        JSONObject scope = new JSONObject(); scope.put("network", network.toString()); scope.put("interface", iface.getName()); scope.put("index", iface.getIndex());
        scope.put("address", address.getHostAddress()); scope.put("type", type); scope.put("offeredName", offered); emit("SCOPE", scope);
        ExecutorService executor = Executors.newSingleThreadExecutor(task -> new Thread(task, "nsd-reference-registration-executor"));
        List<Registration> registrations = new ArrayList<>();
        try (Endpoint firstEndpoint = new Endpoint(address, nonce + "-a"); Endpoint secondEndpoint = new Endpoint(address, nonce + "-b"); Peer peer = new Peer(network, iface, address, nonce)) {
            try {
                Registration first = new Registration("first-default-executor", request(offered, type, network, firstEndpoint.socket.getLocalPort(), nonce)); registrations.add(first);
                emit("REQUEST", info(first.original).put("registration", first.label)); manager.registerService(first.original, NsdManager.PROTOCOL_DNS_SD, first); first.submitted = true; registered(first);
                verifyPeer(peer, first, firstEndpoint, type, 0x5a01);
                Registration second = new Registration("second-explicit-executor", request(offered, type, network, secondEndpoint.socket.getLocalPort(), nonce)); registrations.add(second);
                emit("REQUEST", info(second.original).put("registration", second.label)); manager.registerService(second.original, NsdManager.PROTOCOL_DNS_SD, executor, second); second.submitted = true; registered(second);
                check("nsd-reference-registration-executor".equals(second.registerThread), "explicit application executor selected callback thread");
                check(!first.result.getServiceName().equals(second.result.getServiceName()), "two owned registrations receive distinct names after conflict");
                verifyPeer(peer, second, secondEndpoint, type, 0x5a02);
                stop(manager, first); verifyPeer(peer, second, secondEndpoint, type, 0x5a03);
                Registration replacement = new Registration("replacement", request(offered, type, network, firstEndpoint.socket.getLocalPort(), nonce)); registrations.add(replacement);
                emit("REQUEST", info(replacement.original).put("registration", replacement.label)); manager.registerService(replacement.original, NsdManager.PROTOCOL_DNS_SD, executor, replacement); replacement.submitted = true; registered(replacement);
                verifyPeer(peer, replacement, firstEndpoint, type, 0x5a04);
                stop(manager, replacement); stop(manager, second);
                Thread.sleep(4500); // Bounded observation of normal goodbye retirement, separate from logical callbacks.
                int goodbyePtrs = 0, uniqueGoodbyes = 0, multicastResponses = 0;
                synchronized (peer.multicast) {
                    for (Packet packet : peer.multicast) {
                        if ((packet.flags & 0x8000) == 0) continue; multicastResponses++;
                        for (Record record : packet.records) if (record.ttl == 0) { if (record.type == 12) goodbyePtrs++; else uniqueGoodbyes++; }
                    }
                }
                JSONObject withdrawal = new JSONObject(); withdrawal.put("observedMulticastResponses", multicastResponses);
                withdrawal.put("goodbyePtrRecords", goodbyePtrs); withdrawal.put("otherZeroTtlRecords", uniqueGoodbyes);
                withdrawal.put("observationWindowMs", 4500); withdrawal.put("multicastObservationRequiredForCallbackPass", false); emit("WITHDRAWAL", withdrawal);
                for (Registration registration : registrations) check(registration.registerEvents.get() == 1 && registration.unregisterEvents.get() == 1 && registration.error.get() == null, registration.label + " exactly one register and unregister callback");
                check(peer.error.get() == null && firstEndpoint.error.get() == null && secondEndpoint.error.get() == null, "owned peers completed without worker errors");
            } finally {
                for (Registration registration : registrations) if (registration.submitted && registration.unregisterEvents.get() == 0) {
                    try { manager.unregisterService(registration); registration.unregistered.await(5, TimeUnit.SECONDS); }
                    catch (Throwable cleanup) { cleanup.printStackTrace(System.err); }
                }
            }
        } finally {
            executor.shutdown(); check(executor.awaitTermination(5, TimeUnit.SECONDS), "owned callback executor retired");
        }
    }
}
