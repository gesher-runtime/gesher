/*
 * Copyright (C) 2022 The Android Open Source Project
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.net;

import android.net.Network;
import android.net.nsd.NsdManager;
import android.net.nsd.NsdServiceInfo;
import android.os.SystemClock;
import com.android.net.module.util.DnsUtils;
import com.gesher.net.mdns.*;
import java.io.IOException;
import java.net.Inet6Address;
import java.net.InetAddress;
import java.net.InetSocketAddress;
import java.security.SecureRandom;
import java.util.ArrayDeque;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collections;
import java.util.HashSet;
import java.util.Iterator;
import java.util.LinkedHashMap;
import java.util.LinkedHashSet;
import java.util.List;
import java.util.Map;
import java.util.Objects;
import java.util.Set;
import java.util.UUID;
import java.util.concurrent.RejectedExecutionException;
import java.util.function.IntSupplier;
import java.util.function.LongSupplier;

/** System-owned scheduler around the genuine Android16r4 DNS-SD record core. */
public final class WindowsNsdAdvertiser {
    private WindowsNsdAdvertiser() {}
    public interface Backend {
        void register(int id, NsdServiceInfo info, MdnsAdvertisingOptions options, int uid, Callback callback);
        void update(int id, NsdServiceInfo info, MdnsAdvertisingOptions options);
        void unregister(int id, Runnable logicalDone);
        /** Retire ownership without retaining a client completion callback. */
        default void withdraw(int id) { unregister(id, () -> {}); }
        /** DeviceWork deadline; retirement is bounded work in flight, not client process liveness. */
        default long nextDueElapsed() { return Long.MAX_VALUE; }
        /** Actual owning-process counters; no substitute values on unavailable providers. */
        default int[] nativeState() { throw new UnsupportedOperationException("Native counters unavailable"); }
    }
    public interface Callback {
        void registered(NsdServiceInfo info);
        void failed(int error, boolean retained);
    }
    public interface DriverFactory { Driver open() throws IOException; }
    public interface Driver {
        Scope[] interfaces() throws IOException;
        boolean configure(Scope[] scopes) throws IOException;
        long generation() throws IOException;
        Packet poll(int timeoutMillis) throws IOException;
        /** High32: actual full datagrams sent; low32: OS error, zero for a complete burst. */
        long send(Scope scope, int family, InetSocketAddress destination, byte[][] datagrams) throws IOException;
        boolean close();
    }

    /** Raw Windows metadata. Random suffix/cluster transient flags are not Linux privacy flags. */
    public static final class Address {
        public final long generation;
        public final int index, family, prefixLength, windowsFlags, prefixOrigin, suffixOrigin, dadState, skipAsSource;
        public final byte[] bytes;
        public Address(long generation, int index, int family, byte[] bytes, int prefixLength,
                int windowsFlags, int prefixOrigin, int suffixOrigin, int dadState, int skipAsSource) {
            this.generation = generation; this.index = index; this.family = family;
            this.bytes = bytes.clone(); this.prefixLength = prefixLength; this.windowsFlags = windowsFlags;
            this.prefixOrigin = prefixOrigin; this.suffixOrigin = suffixOrigin;
            this.dadState = dadState; this.skipAsSource = skipAsSource;
        }
        InetAddress inet(int ipv6Index) throws IOException {
            if (family != 4 && family != 6 || bytes.length != (family == 4 ? 4 : 16)) {
                throw new IOException("Unusable actual unicast address");
            }
            return family == 6 ? Inet6Address.getByAddress(null, bytes,
                    InetAddress.getByAddress(bytes).isLinkLocalAddress() ? ipv6Index : 0)
                    : InetAddress.getByAddress(bytes);
        }
        int addressFlags() {
            // Windows IpDadStateDeprecated is 3; other selected addresses are
            // Preferred (4). No unsupported Linux TEMPORARY assertion is made.
            return dadState == 3 ? 0x20 : 0;
        }
        @Override public boolean equals(Object other) {
            if (!(other instanceof Address)) return false;
            Address a = (Address) other;
            return generation == a.generation && index == a.index && family == a.family
                    && prefixLength == a.prefixLength && windowsFlags == a.windowsFlags
                    && prefixOrigin == a.prefixOrigin && suffixOrigin == a.suffixOrigin
                    && dadState == a.dadState && skipAsSource == a.skipAsSource && Arrays.equals(bytes, a.bytes);
        }
        @Override public int hashCode() { return Objects.hash(index, family, Arrays.hashCode(bytes), prefixLength); }
    }

    public static final class Scope {
        public final int index, ipv4Index, ipv6Index, families, netId, mtu4, mtu6;
        public final long luid, generation;
        public final List<Address> addresses;
        public Scope(int index, int ipv4Index, int ipv6Index, long luid, long generation,
                int families, int netId, int mtu4, int mtu6, List<Address> addresses) {
            this.index = index; this.ipv4Index = ipv4Index; this.ipv6Index = ipv6Index;
            this.luid = luid; this.generation = generation; this.families = families;
            this.netId = netId; this.mtu4 = mtu4; this.mtu6 = mtu6;
            this.addresses = List.copyOf(addresses);
        }
        boolean selects(Network network) { return network == null || netId != 0 && network.getNetId() == netId; }
        boolean onLink(InetAddress target) {
            byte[] destination = target.getAddress();
            for (Address address : addresses) {
                if (address.bytes.length != destination.length || address.prefixLength < 0
                        || address.prefixLength > destination.length * 8) continue;
                boolean match = true;
                for (int bit = 0; bit < address.prefixLength; bit++) {
                    int mask = 1 << (7 - bit % 8);
                    if ((address.bytes[bit / 8] & mask) != (destination[bit / 8] & mask)) {
                        match = false; break;
                    }
                }
                if (match) return true;
            }
            return false;
        }
        @Override public boolean equals(Object other) {
            if (!(other instanceof Scope)) return false;
            Scope s = (Scope) other;
            return index == s.index && ipv4Index == s.ipv4Index && ipv6Index == s.ipv6Index
                    && luid == s.luid && generation == s.generation && families == s.families
                    && netId == s.netId && mtu4 == s.mtu4 && mtu6 == s.mtu6 && addresses.equals(s.addresses);
        }
        @Override public int hashCode() { return Objects.hash(index, luid, generation, families, netId); }
    }

    public static final class Packet {
        public final long generation;
        public final int index, family, receivedIndex, sourcePort, hopLimit;
        public final byte[] sourceAddress, bytes;
        public final boolean multicast;
        public Packet(long generation, int index, int family, int receivedIndex, byte[] sourceAddress,
                int sourcePort, boolean multicast, int hopLimit, byte[] bytes) {
            this.generation = generation; this.index = index; this.family = family;
            this.receivedIndex = receivedIndex; this.sourceAddress = sourceAddress;
            this.sourcePort = sourcePort; this.multicast = multicast; this.hopLimit = hopLimit; this.bytes = bytes;
        }
    }

    private static final class NativeDriver implements Driver {
        long handle;
        NativeDriver() throws IOException {
            System.loadLibrary("gesher_nsd_resolve"); handle = nativeOpen();
        }
        public Scope[] interfaces() throws IOException {
            long[] rows = nativeInterfaces(handle);
            Address[] addresses = nativeAddresses(handle);
            WindowsNetworks.Snapshot mapped = WindowsNetworks.parse(WindowsNetworks.snapshot());
            List<Scope> result = new ArrayList<>();
            for (int at = 0; at < rows.length; at += 8) {
                int index = (int) rows[at]; long generation = rows[at + 4];
                List<Address> selected = new ArrayList<>();
                for (Address address : addresses) {
                    if (address.generation != generation) return new Scope[0];
                    if (address.index == index) selected.add(address);
                }
                WindowsNetworks.Net net = mapped.byNetId(index + 100);
                result.add(new Scope(index, (int) rows[at + 1], (int) rows[at + 2], rows[at + 3],
                        generation, (int) rows[at + 5], net == null ? 0 : net.netId(),
                        (int) rows[at + 6], (int) rows[at + 7], selected));
            }
            return result.toArray(new Scope[0]);
        }
        public boolean configure(Scope[] scopes) throws IOException {
            long[] rows = new long[scopes.length * 8];
            for (int n = 0; n < scopes.length; n++) {
                Scope s = scopes[n]; int at = n * 8;
                rows[at] = s.index; rows[at + 1] = s.ipv4Index; rows[at + 2] = s.ipv6Index;
                rows[at + 3] = s.luid; rows[at + 4] = s.generation; rows[at + 5] = s.families;
                rows[at + 6] = s.mtu4; rows[at + 7] = s.mtu6;
            }
            return nativeConfigure(handle, rows);
        }
        public long generation() throws IOException { return nativeGeneration(handle); }
        public Packet poll(int timeout) throws IOException { return nativePoll(handle, timeout); }
        public long send(Scope scope, int family, InetSocketAddress destination, byte[][] packets) throws IOException {
            return nativeSend(handle, scope.index, scope.generation, family,
                    destination.getAddress().getAddress(), destination.getPort(), packets);
        }
        public boolean close() {
            if (handle == 0) return true;
            if (!nativeClose(handle)) return false;
            handle = 0; return true;
        }
    }

    private static native long nativeOpen() throws IOException;
    private static native long[] nativeInterfaces(long id) throws IOException;
    private static native Address[] nativeAddresses(long id) throws IOException;
    private static native boolean nativeConfigure(long id, long[] scopes) throws IOException;
    private static native long nativeGeneration(long id) throws IOException;
    private static native Packet nativePoll(long id, int timeoutMillis) throws IOException;
    private static native long nativeSend(long id, int index, long generation, int family,
            byte[] destination, int port, byte[][] datagrams) throws IOException;
    private static native boolean nativeClose(long id);
    private static native int[] nativeActive();

    private static final MdnsFeatureFlags FLAGS = MdnsFeatureFlags.newBuilder().build();
    private static final int MAX_REGISTRATIONS = 1024, MAX_COMMANDS = 4096;
    private static final int MAX_GROUPS = 256, MAX_PENDING_BYTES = 8 * 1024 * 1024;
    private static final int MAX_PENDING_RECORDS = 16384;
    private static final SecureRandom RANDOM = new SecureRandom();

    public static Backend create() {
        return new Owner(NativeDriver::new, SystemClock::elapsedRealtime, () -> RANDOM.nextInt(250));
    }

    private static String[] hostname() {
        if (!FLAGS.isShortHostnamesEnabled()) return new String[]{"Android_" + UUID.randomUUID().toString().replace("-", ""), "local"};
        String chars = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ";
        StringBuilder label = new StringBuilder("Android_");
        for (int i = 0; i < 8; i++) label.append(chars.charAt(RANDOM.nextInt(chars.length())));
        return new String[]{label.toString(), "local"};
    }

    private static final class Registration {
        final int id, uid;
        final String originalName, originalHost;
        final MdnsAdvertisingOptions options;
        NsdServiceInfo info;
        Callback callback;
        int nameConflicts, hostConflicts;
        boolean notified;
        Registration(int id, int uid, NsdServiceInfo info, MdnsAdvertisingOptions options, Callback callback) {
            this.id = id; this.uid = uid; this.info = info; this.options = options; this.callback = callback;
            originalName = info.getServiceName(); originalHost = info.getHostname();
        }
        boolean subtypeOnly(NsdServiceInfo next) {
            return Objects.equals(originalName, next.getServiceName())
                    && Objects.equals(info.getServiceType(), next.getServiceType()) && info.getPort() == next.getPort()
                    && Objects.equals(originalHost, next.getHostname())
                    && Objects.equals(info.getHostAddresses(), next.getHostAddresses())
                    && Objects.equals(info.getNetwork(), next.getNetwork());
        }
        void rename(boolean service) {
            String suffix = service ? " (" + (++nameConflicts + 1) + ")" : "-" + (++hostConflicts + 1);
            NsdServiceInfo next = new NsdServiceInfo(info); // pinned key omission, not a transport copy
            String changed = NsdRegistrationContract.truncateServiceName(
                    service ? originalName : originalHost, 63 - suffix.length()) + suffix;
            if (service) next.setServiceName(changed); else next.setHostname(changed);
            info = next;
        }
    }

    private static final class Repeat {
        final int id;
        final MdnsAdvertisingPacket.Request request;
        final boolean probe, exit;
        int index;
        long due;
        Repeat(int id, MdnsAdvertisingPacket.Request request, long due) {
            this.id = id; this.request = request; this.due = due;
            probe = request instanceof MdnsAdvertisingPacket.ProbingInfo;
            exit = request instanceof MdnsAdvertisingPacket.ExitAnnouncementInfo;
        }
    }

    private static final class ReplyKey {
        final Scope scope;
        final int family, legacyId;
        final InetSocketAddress source;
        ReplyKey(Scope scope, int family, InetSocketAddress source, int legacyId) {
            this.scope = scope; this.family = family; this.source = source; this.legacyId = legacyId;
        }
        @Override public boolean equals(Object other) {
            if (!(other instanceof ReplyKey)) return false;
            ReplyKey k = (ReplyKey) other;
            return scope.equals(k.scope) && family == k.family && legacyId == k.legacyId && source.equals(k.source);
        }
        @Override public int hashCode() { return Objects.hash(scope, family, source, legacyId); }
    }
    private static final class Reply {
        final ReplyKey key;
        final List<MdnsRecord> answers = new ArrayList<>(), additional = new ArrayList<>(), known = new ArrayList<>();
        MdnsPacket legacyQuery;
        InetSocketAddress destination;
        long due;
        int bytes, records;
        Reply(ReplyKey key) { this.key = key; }
    }

    private static final class State {
        final Scope scope;
        final MdnsRecordRepository repository;
        final Map<Integer, Repeat> repeats = new LinkedHashMap<>();
        final Map<ReplyKey, Reply> replies = new LinkedHashMap<>();
        State(Scope scope, String[] hostname, LongSupplier clock) throws IOException {
            this.scope = scope;
            repository = new MdnsRecordRepository(new MdnsRecordRepository.Dependencies() {
                @Override public long elapsedRealTime() { return clock.getAsLong(); }
            }, hostname, FLAGS);
            List<InetAddress> addresses = new ArrayList<>();
            int[] flags = new int[scope.addresses.size()];
            for (Address address : scope.addresses) {
                flags[addresses.size()] = address.addressFlags();
                addresses.add(address.inet(scope.ipv6Index));
            }
            repository.updateWindowsAddresses(addresses, flags);
        }
    }

    /** One lazy system-service worker; fixtures supply every boundary explicitly. */
    public static final class Owner implements Backend, Runnable {
        private static final class Admission {
            final int id;
            boolean withdrawing;
            Runnable logicalDone;
            Admission(int id) { this.id = id; }
        }
        private static final class Command {
            final Runnable action;
            final boolean ordinary;
            Command(Runnable action, boolean ordinary) {
                this.action = action; this.ordinary = ordinary;
            }
        }
        private final DriverFactory factory;
        private final LongSupplier clock;
        private final IntSupplier firstProbeDelay;
        private final Object queueLock = new Object();
        private final ArrayDeque<Command> commands = new ArrayDeque<>();
        // Each accepted ID reserves a withdrawal entry independently of the
        // ordinary queue budget. Both kinds share FIFO submission ordering.
        private final Map<Integer, Admission> admissions = new LinkedHashMap<>();
        private int ordinaryCommands;
        private final Map<Integer, Registration> registrations = new LinkedHashMap<>();
        private final Map<Integer, State> states = new LinkedHashMap<>();
        private final Set<Integer> completedProbes = new LinkedHashSet<>();
        private Thread thread;
        private Driver driver;
        private boolean closing;
        private String[] deviceHostname = hostname();
        private List<Scope> configured = Collections.emptyList();
        private long configuredGeneration = -1;
        private boolean dirty;
        private int pendingGroups, pendingBytes, pendingRecords;
        private volatile long nextDue = Long.MAX_VALUE;
        private volatile long actualSent, failedBursts;

        public Owner(DriverFactory factory, LongSupplier clock, IntSupplier firstProbeDelay) {
            this.factory = Objects.requireNonNull(factory); this.clock = Objects.requireNonNull(clock);
            this.firstProbeDelay = Objects.requireNonNull(firstProbeDelay);
        }
        @Override public long nextDueElapsed() { return nextDue; }
        @Override public int[] nativeState() {
            System.loadLibrary("gesher_nsd_resolve");
            return nativeActive();
        }
        public long actualSentDatagrams() { return actualSent; }
        public long failedBursts() { return failedBursts; }

        /** queueLock is held; only ordinary work consumes the data-queue budget. */
        private void enqueue(Runnable action, boolean ordinary) {
            if (ordinary && ordinaryCommands >= MAX_COMMANDS) {
                throw new RejectedExecutionException("NSD owner queue full");
            }
            Command command = new Command(action, ordinary);
            commands.add(command);
            if (ordinary) ordinaryCommands++;
            nextDue = 0;
            if (thread == null) {
                thread = new Thread(this, "Gesher-NsdAdvertiser"); thread.setDaemon(true);
                try { thread.start(); }
                catch (RuntimeException | Error failure) {
                    thread = null; commands.remove(command);
                    if (ordinary) ordinaryCommands--;
                    throw failure;
                }
            }
        }
        @Override public void register(int id, NsdServiceInfo info, MdnsAdvertisingOptions options,
                int uid, Callback callback) {
            NsdServiceInfo copy = NsdRegistrationContract.copyForTransport(info);
            Objects.requireNonNull(options); Objects.requireNonNull(callback);
            synchronized (queueLock) {
                if (admissions.containsKey(id)) throw new IllegalArgumentException("NSD ID already owned");
                if (admissions.size() >= MAX_COMMANDS + MAX_REGISTRATIONS) {
                    throw new RejectedExecutionException("NSD ownership capacity exceeded");
                }
                Admission admitted = new Admission(id);
                admissions.put(id, admitted);
                try {
                    enqueue(() -> {
                        try { add(new Registration(id, uid, copy, options, callback)); }
                        finally {
                            if (!registrations.containsKey(id)) releaseAdmission(id);
                        }
                    }, true);
                } catch (RuntimeException | Error failure) {
                    admissions.remove(id, admitted);
                    throw failure;
                }
            }
        }
        @Override public void update(int id, NsdServiceInfo info, MdnsAdvertisingOptions options) {
            NsdServiceInfo copy = NsdRegistrationContract.copyForTransport(info);
            Objects.requireNonNull(options);
            synchronized (queueLock) {
                Admission admitted = admissions.get(id);
                if (admitted == null || admitted.withdrawing) return;
                enqueue(() -> updateNow(id, copy), true);
            }
        }
        @Override public void unregister(int id, Runnable logicalDone) {
            requestWithdrawal(id, Objects.requireNonNull(logicalDone));
        }
        @Override public void withdraw(int id) { requestWithdrawal(id, null); }

        private void releaseAdmission(int id) {
            synchronized (queueLock) { admissions.remove(id); }
        }
        private void requestWithdrawal(int id, Runnable logicalDone) {
            synchronized (queueLock) {
                Admission admitted = admissions.get(id);
                if (admitted != null) {
                    if (logicalDone != null) {
                        if (admitted.logicalDone != null && admitted.logicalDone != logicalDone) {
                            throw new IllegalStateException("NSD unregister already pending");
                        }
                        admitted.logicalDone = logicalDone;
                    }
                    if (admitted.withdrawing) return;
                    admitted.withdrawing = true;
                    try {
                        enqueue(() -> {
                            boolean owns;
                            synchronized (queueLock) { owns = admissions.get(id) == admitted; }
                            if (owns) remove(id);
                            Runnable done;
                            synchronized (queueLock) {
                                admissions.remove(id, admitted);
                                done = admitted.logicalDone;
                                admitted.logicalDone = null;
                            }
                            if (done != null) done.run();
                        }, false);
                    } catch (RuntimeException | Error failure) {
                        admitted.withdrawing = false;
                        admitted.logicalDone = null;
                        throw failure;
                    }
                    return;
                }
            }
            // A reservation survives a queued add. Its absence therefore means
            // that this ID has already failed or completed logical withdrawal.
            if (logicalDone != null) logicalDone.run();
        }

        private void add(Registration value) {
            if (registrations.containsKey(value.id)) { value.callback.failed(0, true); return; }
            if (registrations.size() >= MAX_REGISTRATIONS) { value.callback.failed(4, false); return; }
            try {
                if (driver == null) driver = factory.open();
                chooseLocalNames(value);
                registrations.put(value.id, value);
                for (State state : states.values()) {
                    if (state.scope.selects(value.info.getNetwork())) addToState(state, value);
                }
                dirty = true;
            } catch (IOException | LinkageError | RuntimeException failure) {
                Callback callback = value.callback;
                remove(value.id);
                if (callback != null) callback.failed(0, false);
            }
        }
        private static boolean overlap(Registration a, Registration b) {
            return a.info.getNetwork() == null || b.info.getNetwork() == null
                    || a.info.getNetwork().equals(b.info.getNetwork());
        }
        private boolean serviceConflict(Registration value) {
            if (value.info.getServiceName() == null || value.info.getServiceName().isEmpty()) return false;
            for (Registration other : registrations.values()) {
                if (other == value || !overlap(value, other)) continue;
                if (DnsUtils.equalsIgnoreDnsCase(value.info.getServiceName(), other.info.getServiceName())
                        && DnsUtils.equalsIgnoreDnsCase(value.info.getServiceType(), other.info.getServiceType())) return true;
            }
            return false;
        }
        private boolean hostConflict(Registration value) {
            String hostname = value.info.getHostname();
            if (hostname == null || hostname.isEmpty()) return false;
            if (DnsUtils.equalsIgnoreDnsCase(hostname, deviceHostname[0])) return true;
            for (Registration other : registrations.values()) {
                if (other == value || !overlap(value, other)) continue;
                if (DnsUtils.equalsIgnoreDnsCase(hostname, other.info.getHostname())
                        && (value.uid != other.uid || !value.info.getHostAddresses().isEmpty()
                                && !other.info.getHostAddresses().isEmpty())) return true;
            }
            return false;
        }
        private void chooseLocalNames(Registration value) {
            while (serviceConflict(value)) value.rename(true);
            while (hostConflict(value)) value.rename(false);
        }
        private void updateNow(int id, NsdServiceInfo next) {
            Registration value = registrations.get(id);
            if (value == null) return;
            // Pinned host-only withSubtypes has no SRV. Keep this additional
            // hidden profile explicitly unavailable and retain its cleanup owner.
            if (!value.subtypeOnly(next) || value.info.getServiceName() == null
                    || value.info.getServiceName().isEmpty()) {
                Callback callback = value.callback; value.callback = null;
                if (callback != null) callback.failed(0, true);
                return;
            }
            value.info = new NsdServiceInfo(value.info); // genuine later copy drops KEY
            value.info.setSubtypes(next.getSubtypes());
            for (State state : states.values()) if (state.repository.hasActiveService(id)) {
                clearReplies(state); state.repository.updateService(id, next.getSubtypes());
            }
            // Pinned queued announcement snapshots/options remain unchanged.
        }
        private void addToState(State state, Registration value) throws IOException {
            int scoped = 0;
            for (State s : states.values()) scoped += s.repository.getServicesCount();
            if (scoped >= 4096) throw new IOException("NSD scoped registration capacity exceeded");
            try {
                int replaced = state.repository.addService(value.id, value.info, value.options);
                clearReplies(state);
                if (replaced >= 0) state.repeats.remove(replaced);
                MdnsAdvertisingPacket.ProbingInfo probe = state.repository.setServiceProbing(value.id);
                if (value.options.skipProbing()) finishProbe(state, probe);
                else {
                    int delay = firstProbeDelay.getAsInt();
                    if (delay < 0 || delay >= 250) throw new IllegalStateException("Invalid probe delay source");
                    state.repeats.put(value.id, new Repeat(value.id, probe, clock.getAsLong() + delay));
                }
            } catch (NameConflictException impossibleAfterLocalCheck) {
                throw new IOException("NSD local ownership changed during admission", impossibleAfterLocalCheck);
            }
        }
        private void remove(int id) {
            Registration value = registrations.remove(id);
            completedProbes.remove(id);
            releaseAdmission(id);
            if (value == null) return;
            value.callback = null; // exit values retain no application/service client
            for (State state : states.values()) {
                if (!state.repository.hasActiveService(id)) continue;
                state.repeats.remove(id); clearReplies(state);
                String host = state.repository.getHostnameForServiceId(id);
                MdnsAdvertisingPacket.ExitAnnouncementInfo exit = state.repository.exitService(id);
                if (exit != null) state.repeats.put(id, new Repeat(id, exit, clock.getAsLong() + 100));
                else state.repository.removeService(id);
                if (host != null) {
                    for (MdnsAdvertisingPacket.ProbingInfo p : state.repository.restartProbingForHostname(host)) {
                        state.repeats.put(p.getServiceId(), new Repeat(p.getServiceId(), p,
                                clock.getAsLong() + firstProbeDelay.getAsInt()));
                    }
                    for (MdnsAdvertisingPacket.AnnouncementInfo a : state.repository.restartAnnouncingForHostname(host)) {
                        state.repeats.put(a.getServiceId(), new Repeat(a.getServiceId(), a, clock.getAsLong()));
                    }
                }
            }
            if (registrations.isEmpty()) deviceHostname = hostname();
            dirty = true;
        }
        private void finishProbe(State state, MdnsAdvertisingPacket.ProbingInfo probe) throws IOException {
            // Logical callback is dispatched before first announcement transmission.
            MdnsAdvertisingPacket.AnnouncementInfo announcement = state.repository.onProbingSucceeded(probe);
            completedProbes.add(probe.getServiceId());
            state.repeats.put(probe.getServiceId(), new Repeat(probe.getServiceId(), announcement, clock.getAsLong()));
            String host = state.repository.getHostnameForServiceId(probe.getServiceId());
            if (host != null) {
                for (MdnsAdvertisingPacket.AnnouncementInfo shared : state.repository.restartAnnouncingForHostname(host)) {
                    if (shared.getServiceId() != probe.getServiceId()) {
                        state.repeats.put(shared.getServiceId(), new Repeat(shared.getServiceId(), shared, clock.getAsLong()));
                    }
                }
            }
        }
        private void notifyCompleted() {
            for (int id : new ArrayList<>(completedProbes)) {
                Registration value = registrations.get(id);
                if (value == null || value.notified) { completedProbes.remove(id); continue; }
                boolean present = false, probing = false;
                for (State state : states.values()) {
                    present |= state.repository.hasActiveService(id);
                    probing |= state.repository.isProbing(id);
                }
                if (!present || probing) continue;
                completedProbes.remove(id); value.notified = true;
                if (value.callback != null) value.callback.registered(value.info);
            }
        }
        private void conflict(int id, int type) {
            Registration value = registrations.get(id);
            if (value == null) return;
            boolean rename = !value.notified;
            value.notified = false; completedProbes.remove(id);
            if (rename) {
                if ((type & MdnsAdvertisingPacket.CONFLICT_SERVICE) != 0) value.rename(true);
                if ((type & MdnsAdvertisingPacket.CONFLICT_HOST) != 0) value.rename(false);
                chooseLocalNames(value);
            }
            for (State state : states.values()) {
                if (!state.repository.hasActiveService(id)) continue;
                clearReplies(state);
                MdnsAdvertisingPacket.ProbingInfo probe = rename
                        ? state.repository.renameServiceForConflict(id, value.info)
                        : state.repository.setServiceProbing(id);
                state.repeats.put(id, new Repeat(id, probe, clock.getAsLong() + 5000));
            }
        }
        private void clearReplies(State state) {
            for (Reply reply : state.replies.values()) release(reply);
            state.replies.clear();
        }
        private void release(Reply reply) {
            pendingGroups--; pendingBytes -= reply.bytes; pendingRecords -= reply.records;
        }
        private void discard(State state) {
            clearReplies(state); state.repeats.clear(); state.repository.clearServices();
        }
        private void reconcile() throws IOException {
            long generation = driver.generation();
            Scope[] available = driver.interfaces();
            if (generation != driver.generation()) return;
            List<Scope> selected = new ArrayList<>();
            for (Scope scope : available) {
                if (scope.generation != generation) return;
                boolean needed = false;
                for (Registration value : registrations.values()) needed |= scope.selects(value.info.getNetwork());
                State old = states.get(scope.index);
                if (old != null && old.scope.equals(scope) && old.repository.getServicesCount() != 0) needed = true;
                if (needed) selected.add(scope);
            }
            if (selected.size() > 64) throw new IOException("NSD selected interface capacity exceeded");
            if ((configuredGeneration < 0 || !selected.equals(configured))
                    && !driver.configure(selected.toArray(new Scope[0]))) return;
            if (generation != driver.generation()) return;
            configured = selected; configuredGeneration = generation;
            for (Iterator<State> it = states.values().iterator(); it.hasNext();) {
                State old = it.next();
                if (!selected.contains(old.scope)) { discard(old); it.remove(); }
            }
            List<State> added = new ArrayList<>();
            for (Scope scope : selected) if (!states.containsKey(scope.index)) {
                State state = new State(scope, deviceHostname, clock); states.put(scope.index, state); added.add(state);
            }
            for (State state : added) for (Registration value : registrations.values()) {
                if (state.scope.selects(value.info.getNetwork())) addToState(state, value);
            }
            dirty = false;
        }
        private static int u16(byte[] bytes, int at) { return (bytes[at] & 255) << 8 | bytes[at + 1] & 255; }
        private static <T> void union(List<T> destination, List<T> source) {
            for (T value : source) if (!destination.contains(value)) destination.add(value);
        }
        private void receive(Packet packet) throws IOException {
            State state = states.get(packet.index);
            if (state == null || packet.generation != state.scope.generation
                    || driver.generation() != packet.generation
                    || packet.family != 4 && packet.family != 6
                    || packet.receivedIndex != (packet.family == 4 ? state.scope.ipv4Index : state.scope.ipv6Index)
                    || packet.multicast && packet.hopLimit != 255) return;
            byte[] bytes = packet.bytes;
            if (bytes.length < 12 || bytes.length > 9000 - (packet.family == 4 ? 28 : 48)) return;
            int flags = u16(bytes, 2);
            if ((flags & 0x780f) != 0 || (flags & 0x8000) != 0 && packet.sourcePort != 5353) return;
            int records = u16(bytes, 4) + u16(bytes, 6) + u16(bytes, 8) + u16(bytes, 10);
            if (records > 4096) return;
            MdnsPacket incoming;
            try { incoming = MdnsPacket.parse(new MdnsPacketReader(bytes, bytes.length, FLAGS)); }
            catch (MdnsPacket.ParseException invalid) { return; }
            InetAddress address = packet.family == 6
                    ? Inet6Address.getByAddress(null, packet.sourceAddress, state.scope.ipv6Index)
                    : InetAddress.getByAddress(packet.sourceAddress);
            InetSocketAddress source = new InetSocketAddress(address, packet.sourcePort);
            Map<Integer, Integer> conflicts = state.repository.getConflictingServices(incoming);
            if ((flags & 0x8000) == 0) {
                boolean legacy = packet.sourcePort != 5353;
                MdnsReplyInfo answer = state.repository.getReply(
                        legacy ? MdnsWirePackets.legacyLookup(incoming) : incoming, source);
                if (answer != null) queueReply(state, packet.family, source, incoming, answer,
                        legacy, bytes.length, records);
            }
            for (Map.Entry<Integer, Integer> conflict : conflicts.entrySet()) conflict(conflict.getKey(), conflict.getValue());
        }

        private void queueReply(State state, int family, InetSocketAddress source, MdnsPacket incoming,
                MdnsReplyInfo answer, boolean legacy, int bytes, int records) throws IOException {
            ReplyKey key = new ReplyKey(state.scope, family, source, legacy ? incoming.transactionId : -1);
            Reply reply = state.replies.get(key);
            if (legacy && reply != null) { state.replies.remove(key); release(reply); reply = null; }
            if (reply == null && answer.answers.isEmpty()) return;
            if (reply == null && pendingGroups >= MAX_GROUPS || pendingBytes > MAX_PENDING_BYTES - bytes
                    || pendingRecords > MAX_PENDING_RECORDS - records) return;
            if (reply == null) {
                reply = new Reply(key); state.replies.put(key, reply); pendingGroups++;
            }
            pendingBytes += bytes; pendingRecords += records; reply.bytes += bytes; reply.records += records;
            union(reply.answers, answer.answers); union(reply.additional, answer.additionalAnswers);
            if (!legacy) union(reply.known, answer.knownAnswers);
            // Preserve source-keyed deferral. A different legitimate querier,
            // family, interface generation, or legacy ID has a distinct group.
            reply.due = clock.getAsLong() + answer.sendDelayMs;
            if (legacy) {
                reply.destination = source; reply.legacyQuery = incoming;
            } else if (reply.destination == null || answer.destination.getAddress().isMulticastAddress()) {
                // RFC6762 11: QU to an overlaid off-prefix querier must use
                // multicast; a native-observed endpoint alone is not proof that
                // a conventional unicast response has an on-link destination.
                reply.destination = answer.destination.getAddress().isMulticastAddress()
                        || state.scope.onLink(answer.destination.getAddress())
                        ? answer.destination : multicast(family);
            }
            final Reply selected = reply;
            selected.answers.removeIf(record -> !record.getCacheFlush() && MdnsWirePackets.known(record, selected.known));
            if (selected.answers.isEmpty()) { state.replies.remove(key); release(selected); }
        }

        private List<MdnsRecord> authoritative(State state) {
            Set<String> selectors = new HashSet<>();
            for (Registration value : registrations.values()) {
                if (state.repository.hasActiveService(value.id)) selectors.add(value.info.getHostname());
            }
            List<MdnsRecord> result = new ArrayList<>();
            // The selected genuine DnsUtils provider distinguishes null from
            // empty; both default selectors are retained and independently tested.
            for (String selector : selectors) {
                for (MdnsAdvertisingPacket.AnnouncementInfo info : state.repository.restartAnnouncingForHostname(selector)) {
                    MdnsPacket value = info.getPacket(0);
                    union(result, value.answers); union(result, value.additionalRecords);
                }
            }
            return result;
        }

        private int send(State state, int family, InetSocketAddress destination, MdnsPacket packet,
                boolean probe, MdnsPacket legacy) {
            if ((state.scope.families & (family == 4 ? 1 : 2)) == 0) return 0;
            try {
                if (driver.generation() != state.scope.generation) return 0;
                // Exactly one burst is prepared at a time by this owner. Pending
                // groups are globally bounded at8MiB and preparation at8MiB,
                // including old delayed trains until their budget is released.
                int mtu = family == 4 ? state.scope.mtu4 : state.scope.mtu6;
                byte[][] packets = legacy != null ? MdnsWirePackets.legacy(legacy, packet)
                        : probe ? MdnsWirePackets.probes(packet, mtu, family)
                                : MdnsWirePackets.responses(packet, mtu, family);
                long result = driver.send(state.scope, family, destination, packets);
                int sent = (int) (result >>> 32);
                if (sent < 0 || sent > packets.length) throw new IOException("Invalid native send count");
                actualSent += sent;
                if ((int) result != 0 || sent != packets.length) failedBursts++;
                return sent;
            } catch (IOException sendFailure) { failedBursts++; return 0; }
        }

        private static InetSocketAddress multicast(int family) throws IOException {
            byte[] address = family == 4 ? new byte[]{(byte)224,0,0,(byte)251}
                    : new byte[]{(byte)255,2,0,0,0,0,0,0,0,0,0,0,0,0,0,(byte)251};
            return new InetSocketAddress(InetAddress.getByAddress(address), 5353);
        }
        private void dueWork() throws IOException {
            long now = clock.getAsLong();
            for (State state : new ArrayList<>(states.values())) {
                if (driver.generation() != state.scope.generation) continue;
                for (Repeat repeat : new ArrayList<>(state.repeats.values())) {
                    if (repeat.due > now || state.repeats.get(repeat.id) != repeat) continue;
                    if (repeat.index >= repeat.request.getNumSends()) {
                        state.repeats.remove(repeat.id);
                        if (repeat.probe) {
                            finishProbe(state, (MdnsAdvertisingPacket.ProbingInfo) repeat.request);
                            notifyCompleted();
                        } else if (repeat.exit) {
                            state.repository.removeService(repeat.id); dirty = true;
                        }
                        continue;
                    }
                    MdnsPacket packet = repeat.request.getPacket(repeat.index);
                    int sent = 0;
                    for (int family : new int[]{4, 6}) sent += send(state, family, multicast(family), packet, repeat.probe, null);
                    repeat.index++;
                    repeat.due = clock.getAsLong() + repeat.request.getDelayMs(repeat.index);
                    if (repeat.probe) state.repository.onProbingSent(repeat.id, sent);
                    else state.repository.onAdvertisementSent(repeat.id, sent);
                    // Source timestamps include attempted sends; actualSent is
                    // independent and never inferred from those timestamps.
                }
                for (Iterator<Reply> iterator = state.replies.values().iterator(); iterator.hasNext();) {
                    Reply reply = iterator.next();
                    if (reply.due > now) continue;
                    iterator.remove();
                    try {
                        MdnsPacket packet = MdnsWirePackets.completeReply(reply.answers, reply.additional,
                                reply.known, authoritative(state));
                        if (!packet.answers.isEmpty()) send(state, reply.key.family,
                                reply.destination, packet, false, reply.legacyQuery);
                    } finally { release(reply); }
                }
            }
        }

        private void updateDeadline() {
            long due = Long.MAX_VALUE;
            for (State state : states.values()) {
                for (Repeat repeat : state.repeats.values()) {
                    if (repeat.exit) { due = 0; break; } // bounded retirement sequence in flight
                    due = Math.min(due, repeat.due);
                }
                for (Reply reply : state.replies.values()) due = Math.min(due, reply.due);
            }
            synchronized (queueLock) { nextDue = commands.isEmpty() ? due : 0; }
        }

        private void failOwned() {
            List<Callback> callbacks = new ArrayList<>();
            for (Registration value : registrations.values()) {
                releaseAdmission(value.id);
                if (value.callback != null) {
                    callbacks.add(value.callback); value.callback = null;
                }
            }
            registrations.clear(); completedProbes.clear();
            for (State state : states.values()) discard(state);
            states.clear(); configured = Collections.emptyList(); configuredGeneration = -1;
            for (Callback callback : callbacks) callback.failed(0, false);
        }
        @Override public void run() {
            for (;;) {
                try {
                    if (closing) {
                        boolean closed;
                        try { closed = driver.close(); }
                        catch (RuntimeException | LinkageError failure) { closed = false; }
                        if (!closed) {
                            nextDue = clock.getAsLong() + 1000;
                            Thread.sleep(1000);
                            continue;
                        }
                        driver = null; closing = false;
                        configured = Collections.emptyList(); configuredGeneration = -1;
                    }
                    for (int count = 0; count < MAX_COMMANDS; count++) {
                        Command command;
                        synchronized (queueLock) {
                            command = commands.pollFirst();
                            if (command != null && command.ordinary) ordinaryCommands--;
                        }
                        if (command == null) break;
                        command.action.run();
                    }
                    if (driver != null) {
                        if (dirty || driver.generation() != configuredGeneration) reconcile();
                        notifyCompleted();
                        dueWork();
                        // Empty repositories no longer need their sockets. This
                        // happens after complete exit sequence or actual scope loss.
                        for (Iterator<State> iterator = states.values().iterator(); iterator.hasNext();) {
                            State state = iterator.next();
                            if (state.repository.getServicesCount() == 0) { discard(state); iterator.remove(); dirty = true; }
                        }
                        if (registrations.isEmpty() && states.isEmpty()) {
                            closing = true;
                            continue;
                        }
                    }
                    updateDeadline();
                    synchronized (queueLock) {
                        if (driver == null && commands.isEmpty()) { thread = null; nextDue = Long.MAX_VALUE; return; }
                    }
                    if (driver != null) {
                        long delta = nextDue == Long.MAX_VALUE || nextDue == 0 ? 50 : Math.max(0, nextDue - clock.getAsLong());
                        Packet packet = driver.poll((int) Math.min(50, delta));
                        if (packet != null) receive(packet);
                    }
                } catch (InterruptedException ignored) {
                    // Interruption cannot abandon system-owned advertisements.
                } catch (IOException | LinkageError | RuntimeException failure) {
                    failOwned();
                    // A retiring native handle cannot be reconciled again.
                    // Both error and ordinary retirement use the bounded close
                    // retry above, retaining the actual driver until release.
                    closing = driver != null;
                }
            }
        }
    }
}
