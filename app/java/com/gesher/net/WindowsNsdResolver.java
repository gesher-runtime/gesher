/* Copyright (C) 2026 The Gesher Authors. SPDX-License-Identifier: Apache-2.0 */
package com.gesher.net;

import android.net.Network;
import android.net.nsd.NsdServiceInfo;
import android.os.SystemClock;

import com.android.net.module.util.DnsUtils;
import com.gesher.net.mdns.MdnsFeatureFlags;
import com.gesher.net.mdns.MdnsInetAddressRecord;
import com.gesher.net.mdns.MdnsPacket;
import com.gesher.net.mdns.MdnsPacketReader;
import com.gesher.net.mdns.MdnsPacketWriter;
import com.gesher.net.mdns.MdnsPointerRecord;
import com.gesher.net.mdns.MdnsRecord;
import com.gesher.net.mdns.MdnsServiceInfo.TextEntry;
import com.gesher.net.mdns.MdnsServiceRecord;
import com.gesher.net.mdns.MdnsTextRecord;

import java.io.IOException;
import java.net.Inet4Address;
import java.net.Inet6Address;
import java.net.InetAddress;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.HashMap;
import java.util.Iterator;
import java.util.LinkedHashMap;
import java.util.LinkedHashSet;
import java.util.List;
import java.util.Map;
import java.util.Objects;
import java.util.TreeMap;
import java.util.concurrent.Semaphore;
import java.util.concurrent.ThreadLocalRandom;
import java.util.function.LongSupplier;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

/** One-shot DNS-SD resolution over request-owned, interface-qualified raw Windows packets. */
public final class WindowsNsdResolver {
    private WindowsNsdResolver() {}

    public interface Session { void stop(); }
    public interface Callback {
        void resolved(NsdServiceInfo result);
        void stopped();
        void failed(int error);
        void stopFailed(int error);
    }
    public interface Factory { Session start(NsdServiceInfo request, Callback callback); }
    public interface DriverFactory { Driver open() throws IOException; }

    /** Numeric values originate in IP Helper and the existing WindowsNetworks mapping. */
    public static final class Interface {
        public final int index, ipv4Index, ipv6Index, families, netId;
        public final long luid, generation;
        public Interface(int index, int ipv4Index, int ipv6Index, long luid, long generation,
                int families, int netId) {
            this.index = index; this.ipv4Index = ipv4Index; this.ipv6Index = ipv6Index;
            this.luid = luid; this.generation = generation; this.families = families;
            this.netId = netId;
        }
        @Override public boolean equals(Object other) {
            if (!(other instanceof Interface)) return false;
            Interface value = (Interface) other;
            return index == value.index && ipv4Index == value.ipv4Index
                    && ipv6Index == value.ipv6Index && luid == value.luid
                    && generation == value.generation && families == value.families
                    && netId == value.netId;
        }
        @Override public int hashCode() {
            return Objects.hash(index, ipv4Index, ipv6Index, luid, generation, families, netId);
        }
    }

    /** An actual interface scope and its request-owned, observed continuity identifier. */
    public static final class SubscriptionScope {
        public final Interface scope;
        public final long lifetime;
        public SubscriptionScope(Interface scope, long lifetime) {
            this.scope = scope;
            this.lifetime = lifetime;
        }
    }

    /** An owned UDP payload, with receiving-interface metadata supplied by the transport. */
    public static final class Packet {
        public final long generation;
        public final int index, family, receivedIndex;
        public final byte[] bytes;
        public Packet(long generation, int index, int family, int receivedIndex, byte[] bytes) {
            this.generation = generation; this.index = index; this.family = family;
            this.receivedIndex = receivedIndex; this.bytes = bytes;
        }
    }

    /** Fixtures must pass a driver explicitly; the ordinary factory always uses native sockets. */
    public interface Driver {
        Interface[] interfaces() throws IOException;
        boolean configure(Interface[] interfaces) throws IOException;
        long generation() throws IOException;
        Packet poll(int timeoutMillis) throws IOException;
        boolean send(int index, byte[] query) throws IOException;
        /** True only after sockets and both change-notification registrations are released. */
        boolean close();
        /** Null means a concurrent snapshot must be retried, never an empty network set. */
        default SubscriptionScope[] subscriptionInterfaces() throws IOException {
            throw new IOException("DNS-SD subscription continuity is unavailable");
        }
        default boolean current(Interface scope) throws IOException {
            if (generation() != scope.generation) return false;
            for (Interface present : interfaces()) {
                if (present.equals(scope)) return generation() == scope.generation;
            }
            return false;
        }
    }

    private static final class NativeDriver implements Driver {
        private long handle;
        NativeDriver() throws IOException {
            System.loadLibrary("gesher_nsd_resolve");
            handle = nativeOpen();
        }
        public Interface[] interfaces() throws IOException {
            long[] rows = nativeInterfaces(handle);
            WindowsNetworks.Snapshot mapped = WindowsNetworks.parse(WindowsNetworks.snapshot());
            List<Interface> out = new ArrayList<>();
            for (int at = 0; at < rows.length; at += 6) {
                int index = (int) rows[at];
                // Link-only adapters are real mDNS scopes but are not necessarily
                // Android Networks in Gesher's existing connectivity snapshot.
                WindowsNetworks.Net network = mapped.byNetId(index + 100);
                out.add(new Interface(index, (int) rows[at + 1], (int) rows[at + 2],
                        rows[at + 3], rows[at + 4], (int) rows[at + 5],
                        network == null ? 0 : network.netId()));
            }
            return out.toArray(new Interface[0]);
        }
        public boolean configure(Interface[] scopes) throws IOException {
            long[] rows = new long[scopes.length * 6];
            for (int at = 0; at < scopes.length; at++) {
                Interface value = scopes[at]; int start = at * 6;
                rows[start] = value.index; rows[start + 1] = value.ipv4Index;
                rows[start + 2] = value.ipv6Index; rows[start + 3] = value.luid;
                rows[start + 4] = value.generation; rows[start + 5] = value.families;
            }
            return nativeConfigure(handle, rows);
        }
        public long generation() throws IOException { return nativeGeneration(handle); }
        public Packet poll(int timeout) throws IOException { return nativePoll(handle, timeout); }
        public boolean send(int index, byte[] query) throws IOException {
            return nativeSend(handle, index, query);
        }
        public boolean close() {
            if (handle == 0) return true;
            if (!nativeClose(handle)) return false;
            handle = 0;
            return true;
        }
    }

    /** Kept separate so one-shot resolution never acquires subscription lifetime semantics. */
    private static final class NativeSubscriptionDriver implements Driver {
        private long handle;
        NativeSubscriptionDriver() throws IOException {
            System.loadLibrary("gesher_nsd_resolve");
            // A partially initialized native owner is retained until close succeeds.
            handle = nativeSubscriptionOpen();
        }
        @Override public SubscriptionScope[] subscriptionInterfaces() throws IOException {
            long generation = nativeSubscriptionGeneration(handle);
            long[] rows = nativeSubscriptionInterfaces(handle);
            if (rows == null) return null;
            if (rows.length % 7 != 0) throw new IOException("Invalid DNS-SD subscription snapshot");
            WindowsNetworks.Snapshot mapped = WindowsNetworks.parse(WindowsNetworks.snapshot());
            SubscriptionScope[] out = new SubscriptionScope[rows.length / 7];
            for (int at = 0; at < out.length; ++at) {
                int start = at * 7;
                if (rows[start + 4] != generation) return null;
                if (rows[start + 6] <= 0) {
                    throw new IOException("Unobserved DNS-SD interface continuity");
                }
                int index = (int) rows[start];
                WindowsNetworks.Net network = mapped.byNetId(index + 100);
                Interface scope = new Interface(index, (int) rows[start + 1],
                        (int) rows[start + 2], rows[start + 3], rows[start + 4],
                        (int) rows[start + 5], network == null ? 0 : network.netId());
                out[at] = new SubscriptionScope(scope, rows[start + 6]);
            }
            return nativeSubscriptionGeneration(handle) == generation ? out : null;
        }
        @Override public Interface[] interfaces() throws IOException {
            SubscriptionScope[] snapshot = subscriptionInterfaces();
            if (snapshot == null) throw new IOException("DNS-SD subscription snapshot changed");
            Interface[] out = new Interface[snapshot.length];
            for (int at = 0; at < out.length; ++at) out[at] = snapshot[at].scope;
            return out;
        }
        @Override public boolean configure(Interface[] scopes) throws IOException {
            long[] rows = new long[scopes.length * 6];
            for (int at = 0; at < scopes.length; ++at) {
                Interface value = scopes[at]; int start = at * 6;
                rows[start] = value.index; rows[start + 1] = value.ipv4Index;
                rows[start + 2] = value.ipv6Index; rows[start + 3] = value.luid;
                rows[start + 4] = value.generation; rows[start + 5] = value.families;
            }
            return nativeSubscriptionConfigure(handle, rows);
        }
        @Override public long generation() throws IOException {
            return nativeSubscriptionGeneration(handle);
        }
        @Override public Packet poll(int timeout) throws IOException {
            return nativeSubscriptionPoll(handle, timeout);
        }
        @Override public boolean send(int index, byte[] query) throws IOException {
            return nativeSubscriptionSend(handle, index, query);
        }
        @Override public boolean close() {
            if (handle == 0) return true;
            if (!nativeSubscriptionClose(handle)) return false;
            handle = 0;
            return true;
        }
    }

    static DriverFactory subscriptionDrivers() { return NativeSubscriptionDriver::new; }

    /** Requests, sockets, watches, in-flight callbacks, polled requests, actual sent datagrams. */
    static int[] subscriptionNativeState() {
        System.loadLibrary("gesher_nsd_resolve");
        return nativeSubscriptionActive();
    }

    public static Factory factory() {
        return (request, callback) -> {
            Resolver session = new Resolver(NativeDriver::new, request, callback,
                    SystemClock::elapsedRealtime,
                    () -> ThreadLocalRandom.current().nextLong(20, 121));
            session.start();
            return session;
        };
    }

    private static native long nativeOpen() throws IOException;
    private static native long[] nativeInterfaces(long id) throws IOException;
    private static native boolean nativeConfigure(long id, long[] scopes) throws IOException;
    private static native long nativeGeneration(long id) throws IOException;
    private static native Packet nativePoll(long id, int timeoutMillis) throws IOException;
    private static native boolean nativeSend(long id, int index, byte[] query) throws IOException;
    private static native boolean nativeClose(long id);
    private static native int[] nativeActive();

    private static native long nativeSubscriptionOpen() throws IOException;
    private static native long[] nativeSubscriptionInterfaces(long id) throws IOException;
    private static native boolean nativeSubscriptionConfigure(long id, long[] scopes) throws IOException;
    private static native long nativeSubscriptionGeneration(long id) throws IOException;
    private static native Packet nativeSubscriptionPoll(long id, int timeoutMillis) throws IOException;
    private static native boolean nativeSubscriptionSend(long id, int index, byte[] query) throws IOException;
    private static native boolean nativeSubscriptionClose(long id);
    private static native int[] nativeSubscriptionActive();

    private static final String LABEL = "_[a-zA-Z0-9_-]{1,61}[a-zA-Z0-9]";
    private static final String SUBTYPE = "_[\\x20-\\x2B\\x2D\\x2F-\\x7E]{1,62}";
    private static final Pattern TYPE = Pattern.compile(
            "^(?:(?:" + SUBTYPE + ")\\.)?(" + LABEL + "\\._(?:tcp|udp))\\.?((?:," + SUBTYPE + ")*)$");
    private static final int MAX_INTERFACES = 64;
    private static final int MAX_ADDRESSES = 64;
    private static final int MAX_SUBTYPES = 64;
    private static final int MAX_PACKET = 65535;
    private static final int MAX_RECORDS = 8192;
    private static final MdnsFeatureFlags FLAGS = MdnsFeatureFlags.newBuilder().build();
    private static final Semaphore WORKERS = new Semaphore(200);

    private static boolean same(String[] a, String[] b) {
        return DnsUtils.equalsDnsLabelIgnoreDnsCase(a, b);
    }
    private static boolean encodable(String[] labels) {
        int length = 1;
        for (String label : labels) {
            int bytes = label.getBytes(StandardCharsets.UTF_8).length;
            if (bytes == 0 || bytes > 63) return false;
            length += bytes + 1;
        }
        return length <= 256;
    }
    private static final class Observed<T extends MdnsRecord> {
        final T record;
        final long expires;
        Observed(T record, long now) {
            this.record = record;
            expires = now + record.getTtl();
        }
        boolean refresh(long now) { return expires - now <= record.getTtl() / 2; }
    }

    /** The selected instance's state on one actual adapter generation. No global DNS cache. */
    public static final class State {
        public final Interface scope;
        private final String[] name;
        private final String type;
        private Observed<MdnsServiceRecord> service;
        private Observed<MdnsTextRecord> text;
        private final Map<InetAddress, Observed<MdnsInetAddressRecord>> addresses = new LinkedHashMap<>();
        private final Map<Object, MdnsPointerRecord> subtypes = new LinkedHashMap<>();
        private final Map<Object, Long> lastAsked = new HashMap<>();
        private final Object serviceQuestionKey, textQuestionKey;
        private long revision;

        public State(String instance, String type, Interface scope) throws IOException {
            Matcher matcher = TYPE.matcher(type);
            if (!matcher.matches()) throw new IOException("Invalid DNS-SD service type");
            this.type = matcher.group(1);
            this.name = new String[] {instance, this.type.substring(0, this.type.indexOf('.')),
                    this.type.substring(this.type.indexOf('.') + 1), "local"};
            if (!encodable(name)) throw new IOException("Unencodable DNS-SD instance label");
            this.scope = scope;
            serviceQuestionKey = new MdnsServiceRecord(name, false).getKey();
            textQuestionKey = new MdnsTextRecord(name, false).getKey();
        }

        public void expire(long now) {
            if (service != null && service.expires <= now) clear();
            if (text != null && text.expires <= now) { text = null; revision++; }
            if (addresses.values().removeIf(value -> value.expires <= now)) revision++;
        }

        private void forgetTargetQuestions() {
            lastAsked.keySet().removeIf(key -> !key.equals(serviceQuestionKey) && !key.equals(textQuestionKey));
        }
        private void clear() {
            if (service != null || text != null || !addresses.isEmpty() || !subtypes.isEmpty()) revision++;
            service = null; text = null; addresses.clear(); subtypes.clear(); forgetTargetQuestions();
        }

        public void accept(Packet received, long now) throws IOException {
            if (received.generation != scope.generation || received.index != scope.index) return;
            if (received.family == 4) {
                if ((scope.families & 1) == 0 || received.receivedIndex != scope.ipv4Index) return;
            } else if (received.family == 6) {
                if ((scope.families & 2) == 0 || received.receivedIndex != scope.ipv6Index) return;
            } else return;
            byte[] bytes = received.bytes;
            if (bytes == null || bytes.length < 12 || bytes.length > MAX_PACKET) return;
            int flags = (bytes[2] & 255) << 8 | bytes[3] & 255;
            // Responses to ordinary questions only; queries/probes/other opcodes
            // cannot establish a result. Multicast transaction IDs are ignored.
            if ((flags & 0x8000) == 0 || (flags & 0x780f) != 0) return;
            int count = 0;
            for (int at = 4; at < 12; at += 2) count += (bytes[at] & 255) << 8 | bytes[at + 1] & 255;
            if (count > MAX_RECORDS) return;
            MdnsPacket packet;
            try { packet = MdnsPacket.parse(new MdnsPacketReader(bytes, bytes.length, FLAGS)); }
            catch (MdnsPacket.ParseException invalid) { return; }
            List<MdnsRecord> records = new ArrayList<>();
            records.addAll(packet.answers); records.addAll(packet.authorityRecords);
            records.addAll(packet.additionalRecords);
            expire(now);
            // The pinned Java profile treats a zero-TTL selected service record
            // as a goodbye for the whole cached instance. It does not wait for
            // the RFC's one-second grace period before notifying removal.
            for (MdnsRecord record : records) {
                if (record.getRecordClass() != 1 || record.getTtl() != 0) continue;
                if ((same(name, record.getName())
                        && (record instanceof MdnsServiceRecord || record instanceof MdnsTextRecord))
                        || (service != null && record instanceof MdnsInetAddressRecord
                            && same(service.record.getServiceHost(), record.getName()))
                        || (record instanceof MdnsPointerRecord
                            && same(Arrays.copyOfRange(name, 1, name.length), record.getName())
                            && same(name, ((MdnsPointerRecord) record).getPointer()))) {
                    clear(); return;
                }
            }
            // A qualified observed subtype PTR is optional result metadata. The
            // r4 cache retains this metadata, including a subtype-only zero TTL,
            // until the whole service is retired; it has no subtype expiry path.
            // Never infer supported subtypes from the request's syntax.
            String[] serviceType = Arrays.copyOfRange(name, 1, name.length);
            for (MdnsRecord record : records) {
                if (!(record instanceof MdnsPointerRecord) || record.getRecordClass() != 1) continue;
                MdnsPointerRecord pointer = (MdnsPointerRecord) record;
                String[] owner = pointer.getName();
                if (!pointer.hasSubtype() || owner.length != serviceType.length + 2
                        || !same(serviceType, Arrays.copyOfRange(owner, 2, owner.length))
                        || !same(name, pointer.getPointer())) continue;
                Object key = pointer.getKey();
                if (!subtypes.containsKey(key) && subtypes.size() >= MAX_SUBTYPES) {
                    throw new IOException("DNS-SD subtype metadata budget exceeded");
                }
                subtypes.remove(key);
                subtypes.put(key, pointer);
            }
            // r4 decoder order: optional PTR metadata, selected SRV/TXT, then addresses.
            for (MdnsRecord record : records) {
                if (record.getRecordClass() != 1 || !same(name, record.getName())) continue;
                if (record instanceof MdnsServiceRecord) {
                    MdnsServiceRecord next = (MdnsServiceRecord) record;
                    if (next.getTtl() == 0) { service = null; addresses.clear(); continue; }
                    if (!encodable(next.getServiceHost())) continue;
                    if (service == null || !same(service.record.getServiceHost(), next.getServiceHost())) {
                        addresses.clear(); forgetTargetQuestions(); revision++;
                    }
                    service = new Observed<>(next, now);
                } else if (record instanceof MdnsTextRecord) {
                    if (text == null) revision++;
                    text = record.getTtl() == 0 ? null : new Observed<>((MdnsTextRecord) record, now);
                }
            }
            if (service == null) return;
            List<MdnsInetAddressRecord> incoming = new ArrayList<>();
            boolean flush4 = false, flush6 = false;
            for (MdnsRecord record : records) {
                if (!(record instanceof MdnsInetAddressRecord) || record.getRecordClass() != 1
                        || !same(service.record.getServiceHost(), record.getName())) continue;
                MdnsInetAddressRecord address = (MdnsInetAddressRecord) record;
                incoming.add(address);
                if (address.getCacheFlush()) {
                    // Pinned r4 clears the same-type old RRset before adding the
                    // whole packet's new RRset. Its deferred one-second flush
                    // semantics remain a TODO upstream; do not mix profiles.
                    flush4 |= address.getType() == MdnsRecord.TYPE_A;
                    flush6 |= address.getType() == MdnsRecord.TYPE_AAAA;
                }
            }
            if (flush4 || flush6) {
                Iterator<InetAddress> it = addresses.keySet().iterator();
                while (it.hasNext()) {
                    InetAddress address = it.next();
                    if ((flush4 && address instanceof Inet4Address)
                            || (flush6 && address instanceof Inet6Address)) it.remove();
                }
            }
            boolean hadAddress = !addresses.isEmpty();
            for (MdnsInetAddressRecord record : incoming) {
                InetAddress address = record.getInet4Address();
                if (address == null) address = record.getInet6Address();
                if (address == null) continue;
                if (record.getTtl() == 0) { addresses.remove(address); continue; }
                if (!addresses.containsKey(address) && addresses.size() >= MAX_ADDRESSES) {
                    throw new IOException("DNS-SD address budget exceeded");
                }
                addresses.put(address, new Observed<>(record, now));
            }
            if (hadAddress != !addresses.isEmpty()) revision++;
        }

        public byte[] query(long now) throws IOException {
            return query(now, false);
        }

        private byte[] query(long now, boolean limitRate) throws IOException {
            expire(now);
            List<MdnsRecord> questions = new ArrayList<>();
            if (service == null || service.refresh(now)) questions.add(new MdnsServiceRecord(name, false));
            if (text == null || text.refresh(now)) questions.add(new MdnsTextRecord(name, false));
            if (service != null) {
                boolean v4 = false, v6 = false;
                for (Map.Entry<InetAddress, Observed<MdnsInetAddressRecord>> entry : addresses.entrySet()) {
                    if (entry.getValue().refresh(now)) continue;
                    v4 |= entry.getKey() instanceof Inet4Address;
                    v6 |= entry.getKey() instanceof Inet6Address;
                }
                if (!v4) questions.add(new MdnsInetAddressRecord(service.record.getServiceHost(), MdnsRecord.TYPE_A, false));
                if (!v6) questions.add(new MdnsInetAddressRecord(service.record.getServiceHost(), MdnsRecord.TYPE_AAAA, false));
            }
            if (limitRate) {
                questions.removeIf(question -> {
                    Long previous = lastAsked.get(question.getKey());
                    return previous != null && now - previous < 1000;
                });
                if (questions.isEmpty()) return null;
                // At most SRV/TXT plus A/AAAA for the current target. New target
                // questions can be sent immediately, without repeating the old
                // questions faster than RFC6762's one-second minimum interval.
                for (MdnsRecord question : questions) lastAsked.put(question.getKey(), now);
            }
            byte[] bytes = new byte[1500];
            MdnsPacketWriter writer = new MdnsPacketWriter(bytes);
            writer.writeUInt16(0); writer.writeUInt16(0);
            writer.writeUInt16(questions.size()); writer.writeUInt16(0);
            writer.writeUInt16(0); writer.writeUInt16(0);
            for (MdnsRecord question : questions) question.writeHeaderFields(writer);
            return Arrays.copyOf(bytes, writer.getWritePosition());
        }

        public NsdServiceInfo result(long now) throws IOException {
            expire(now);
            if (service == null || text == null || addresses.isEmpty()) return null;
            NsdServiceInfo out = new NsdServiceInfo();
            // Named resolution is seeded with the requested instance/type in r4.
            out.setServiceName(name[0]); out.setServiceType("." + type);
            out.setPort(service.record.getServicePort());
            LinkedHashSet<String> knownSubtypes = new LinkedHashSet<>();
            for (MdnsPointerRecord pointer : subtypes.values()) knownSubtypes.add(pointer.getSubtype());
            out.setSubtypes(knownSubtypes);
            out.setExpirationTime(java.time.Instant.ofEpochMilli(
                    System.currentTimeMillis() + service.expires - now));
            String[] host = service.record.getServiceHost();
            if (host.length >= 2 && host[host.length - 1].equals("local")) host = Arrays.copyOf(host, host.length - 1);
            out.setHostname(String.join(".", host));
            Map<String, byte[]> values = new TreeMap<>(String.CASE_INSENSITIVE_ORDER);
            for (TextEntry entry : text.record.getEntries()) values.putIfAbsent(entry.getKey(), entry.getValue());
            for (Map.Entry<String, byte[]> entry : values.entrySet()) {
                try {
                    byte[] value = entry.getValue();
                    out.setAttribute(entry.getKey(), value == null ? null : value.clone());
                } catch (IllegalArgumentException invalid) {
                    // NsdService skips invalid/oversize entries but retains the
                    // existence of the observed TXT record, including empty TXT.
                }
            }
            List<InetAddress> ordered = new ArrayList<>();
            for (InetAddress address : addresses.keySet()) if (address instanceof Inet4Address) ordered.add(address);
            for (InetAddress address : addresses.keySet()) if (address instanceof Inet6Address) {
                // A record received over IPv4 can contain a scoped IPv6 address.
                // Use this adapter's IPv6 index, never its distinct IPv4 index.
                if (address.isLinkLocalAddress()) {
                    if (scope.ipv6Index == 0) continue;
                    address = Inet6Address.getByAddress(null, address.getAddress(), scope.ipv6Index);
                }
                ordered.add(address);
            }
            if (ordered.isEmpty()) return null;
            out.setHostAddresses(ordered);
            if (scope.netId != 0) out.setNetwork(new Network(scope.netId));
            else out.setInterfaceIndex(scope.index);
            return out;
        }
    }

    /** One worker owns every driver call. Stop is observed within the bounded poll. */
    public static final class Resolver implements Session, Runnable {
        private final DriverFactory factory;
        private final String name, type;
        private final int requestedNetId, requestedIndex;
        private final LongSupplier clock, initialDelay;
        private Callback callback;
        private volatile boolean stop;
        private boolean workerPermit;

        public Resolver(DriverFactory factory, NsdServiceInfo request, Callback callback,
                LongSupplier clock, LongSupplier initialDelay) {
            this.factory = Objects.requireNonNull(factory);
            this.name = request.getServiceName(); this.type = request.getServiceType();
            Network network = request.getNetwork();
            requestedNetId = network == null ? 0 : network.netId;
            requestedIndex = request.getInterfaceIndex();
            this.callback = Objects.requireNonNull(callback);
            this.clock = Objects.requireNonNull(clock); this.initialDelay = Objects.requireNonNull(initialDelay);
        }
        public void start() {
            if (!WORKERS.tryAcquire()) {
                Callback failed = callback; callback = null;
                failed.failed(0); return;
            }
            workerPermit = true;
            Thread worker = new Thread(this, "Gesher-NsdResolve");
            worker.setDaemon(true);
            try { worker.start(); }
            catch (RuntimeException | Error error) {
                workerPermit = false; WORKERS.release(); throw error;
            }
        }
        @Override public void stop() { stop = true; }

        @Override public void run() {
            Driver driver = null;
            NsdServiceInfo resolved = null;
            boolean failed = false, closeFailed = false;
            Map<Integer, State> states = new LinkedHashMap<>();
            try {
                // Validate even when no eligible interface is currently present.
                new State(name, type, new Interface(1, 1, 1, 1, 1, 3, 0));
                if (requestedNetId != 0 && requestedNetId <= 100) throw new IOException("Invalid network");
                driver = factory.open();
                long nextScan = 0, nextQuery = clock.getAsLong() + initialDelay.getAsLong();
                long interval = 1000, lastGeneration = -1;
                while (!stop) {
                    long now = clock.getAsLong();
                    long generation = driver.generation();
                    if (now >= nextScan || generation != lastGeneration) {
                        Interface[] all = driver.interfaces();
                        List<Interface> selected = new ArrayList<>();
                        for (Interface scope : all) {
                            if (requestedNetId != 0 && scope.netId != requestedNetId) continue;
                            if (requestedNetId == 0 && requestedIndex != 0
                                    && scope.index != requestedIndex && scope.ipv4Index != requestedIndex
                                    && scope.ipv6Index != requestedIndex) continue;
                            selected.add(scope);
                        }
                        if (selected.size() > MAX_INTERFACES) throw new IOException("DNS-SD interface budget exceeded");
                        Map<Integer, State> current = new LinkedHashMap<>();
                        boolean changed = selected.size() != states.size();
                        for (Interface scope : selected) {
                            State prior = states.get(scope.index);
                            if (prior == null || !prior.scope.equals(scope)) {
                                prior = new State(name, type, scope); changed = true;
                            }
                            current.put(scope.index, prior);
                        }
                        if (changed && !driver.configure(selected.toArray(new Interface[0]))) {
                            states.clear(); nextScan = now + 100; lastGeneration = -1;
                            driver.poll(50); continue;
                        }
                        states = current; lastGeneration = generation; nextScan = now + 1000;
                        if (changed) { nextQuery = now + initialDelay.getAsLong(); interval = 1000; }
                    }
                    if (now >= nextQuery) {
                        boolean asked = false, retry = false;
                        for (State state : states.values()) {
                            byte[] query = state.query(now, true);
                            if (query != null) {
                                retry |= !driver.send(state.scope.index, query);
                                asked = true;
                            }
                        }
                        if (retry) {
                            // A nonblocking socket may temporarily reject a send.
                            // Keep the one-second question limit, but do not turn
                            // a query that was not sent into an hour-long backoff.
                            nextQuery = now + 1000; interval = 1000;
                        } else if (asked) {
                            nextQuery = now + interval;
                            // Continuous questions without a terminal no-response timeout.
                            interval = Math.min(interval * 2, 60 * 60 * 1000);
                        } else nextQuery = now + 100;
                    }
                    Packet packet = driver.poll(50);
                    now = clock.getAsLong();
                    if (packet != null) {
                        State state = states.get(packet.index);
                        if (state != null) {
                            long before = state.revision;
                            state.accept(packet, now);
                            if (state.revision != before) { nextQuery = now; interval = 1000; }
                            NsdServiceInfo result = state.result(now);
                            if (result != null && !stop && driver.current(state.scope)) {
                                resolved = result; break;
                            }
                        }
                    }
                    for (State state : states.values()) {
                        long before = state.revision;
                        state.expire(now);
                        if (state.revision != before) { nextQuery = now; interval = 1000; }
                    }
                }
            } catch (IOException | LinkageError | RuntimeException error) {
                failed = true;
            } finally {
                states.clear();
                if (driver != null && !driver.close()) {
                    closeFailed = true;
                    retire(driver);
                }
                if (workerPermit) { workerPermit = false; WORKERS.release(); }
            }
            Callback terminal = callback; callback = null;
            if (closeFailed) {
                if (stop) terminal.stopFailed(0); else terminal.failed(0);
            } else if (failed) terminal.failed(0);
            else if (resolved != null) terminal.resolved(resolved);
            else terminal.stopped();
        }
    }

    private static void retire(Driver driver) {
        // This lambda captures only the driver/native numeric handle. App values,
        // scopes, callbacks and the Resolver are not retained by retries.
        Thread worker = new Thread(() -> {
            while (!driver.close()) {
                try { Thread.sleep(1000); }
                catch (InterruptedException ignored) { /* Release still needs confirmation. */ }
            }
        }, "Gesher-NsdResolveRetire");
        worker.setDaemon(true); worker.start();
    }
}
