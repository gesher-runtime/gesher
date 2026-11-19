/* Copyright (C) 2026 The Gesher Authors. SPDX-License-Identifier: Apache-2.0 */
package gesher.services;

import android.content.Context;
import android.net.Network;
import android.net.nsd.NsdManager;
import android.net.nsd.NsdServiceInfo;
import com.gesher.net.WindowsNsd;
import com.gesher.net.WindowsNsdResolver;
import com.gesher.net.WindowsNsdResolver.Interface;
import com.gesher.net.WindowsNsdResolver.Packet;
import com.gesher.net.WindowsNsdResolver.State;
import com.gesher.net.mdns.MdnsFeatureFlags;
import com.gesher.net.mdns.MdnsPacket;
import com.gesher.net.mdns.MdnsPacketReader;
import com.gesher.net.mdns.MdnsRecord;

import java.io.IOException;
import java.lang.reflect.Constructor;
import java.net.Inet6Address;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayDeque;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.util.Set;
import java.util.concurrent.Executor;
import java.util.concurrent.RejectedExecutionException;
import java.util.function.IntConsumer;

/** Valid independently encoded packets, production state/manager, explicit transport seams. */
public final class NsdResolveContractTest {
    private static final String NAME = "Owned.שלום\\😀", TYPE = "_grcontract._tcp";
    private static final Interface SCOPE = new Interface(7, 7, 17, 77, 3, 3, 107);
    private static final Map<String, byte[]> packets = new HashMap<>();
    private static int checks;
    private static void check(boolean value, String message) {
        if (!value) throw new AssertionError(message);
        checks++;
    }
    private static void expect(Class<? extends Throwable> type, Runnable action) {
        try { action.run(); }
        catch (Throwable error) { check(type.isInstance(error), "Wrong exception: " + error); return; }
        throw new AssertionError("Missing " + type.getSimpleName());
    }
    private static byte[] hex(String value) {
        byte[] out = new byte[value.length() / 2];
        for (int at = 0; at < out.length; at++) out[at] = (byte) Integer.parseInt(value.substring(at * 2, at * 2 + 2), 16);
        return out;
    }
    private static NsdServiceInfo request() {
        NsdServiceInfo out = new NsdServiceInfo(); out.setServiceName(NAME); out.setServiceType(TYPE); return out;
    }
    private static Packet packet(String key, Interface scope) {
        return new Packet(scope.generation, scope.index, 4, scope.ipv4Index, packets.get(key));
    }
    private static State state() throws Exception { return new State(NAME, TYPE, SCOPE); }
    private static void accept(State state, String key, long now) throws Exception {
        state.accept(packet(key, state.scope), now);
    }
    private static void binary(NsdServiceInfo info) {
        Map<String, byte[]> values = info.getAttributes();
        check(values.containsKey("flag") && values.get("flag") == null, "key-only TXT retains null");
        check(values.containsKey("empty") && values.get("empty").length == 0, "empty TXT remains a byte array");
        byte[] expected = new byte[205]; expected[0] = 0; expected[1] = -1; expected[2] = -2;
        expected[3] = '='; expected[4] = -128;
        for (int at = 0; at < 200; at++) expected[at + 5] = (byte) at;
        check(Arrays.equals(values.get("bin"), expected), "NUL, non-UTF8, equals and all fixture bytes survive");
        check(Arrays.equals(values.get("dup"), "first".getBytes(java.nio.charset.StandardCharsets.US_ASCII)), "case-insensitive duplicate retains first non-null");
        check(Arrays.equals(values.get("replace"), "later".getBytes(java.nio.charset.StandardCharsets.US_ASCII)), "upstream null duplicate replacement retains first spelling");
        check(values.size() == 9, "distinct TXT map entries");
        for (int at = 0; at < 4; at++) {
            byte[] value = values.get("p" + at);
            check(value.length == 230 && value[0] == 65 + at && value[229] == 65 + at, "large TXT field " + at);
        }
    }
    private static void stateContracts() throws Exception {
        check(packets.get("full4").length > 1200, "independent packet exceeds legacy 512-byte boundary");
        for (String wire : List.of("full4", "compressed")) {
            State state = state(); accept(state, wire, 100);
            long beforeResult = System.currentTimeMillis();
            NsdServiceInfo result = state.result(100);
            long afterResult = System.currentTimeMillis();
            check(result != null, wire + " complete"); binary(result);
            check(result.getServiceName().equals(NAME), "Unicode, dot and backslash form one unchanged instance");
            check(result.getServiceType().equals("." + TYPE), "leading-dot resolved type");
            check(result.getPort() == 43123 && result.getHostname().equals("fixture"), "actual SRV fields");
            check(result.getExpirationTime() != null
                    && result.getExpirationTime().toEpochMilli() >= (beforeResult / 1000) * 1000 + 120000
                    && result.getExpirationTime().toEpochMilli() <= (afterResult / 1000) * 1000 + 120000,
                    "observed SRV lifetime becomes expiration metadata");
            check(result.getNetwork().equals(new Network(107)) && result.getInterfaceIndex() == 0, "mapped Network metadata");
            check(result.getHostAddresses().get(0).getHostAddress().equals("192.0.2.10"), "observed address");
            result.getAttributes().get("bin")[0] = 42;
            check(state.result(100).getAttributes().get("bin")[0] == 0, "success values own their byte arrays");
        }
        State split = state(); accept(split, "srv", 100);
        check(split.result(100) == null, "SRV alone cannot resolve");
        accept(split, "txt", 200); check(split.result(200) == null, "SRV/TXT require an address");
        accept(split, "a", 300); check(split.result(300) != null, "complete split observations resolve without PTR");
        State earlyAddress = state(); accept(earlyAddress, "a", 10); accept(earlyAddress, "srv", 20); accept(earlyAddress, "txt", 20);
        check(earlyAddress.result(20) == null, "no unrelated address cache before observed SRV target");
        MdnsPacket queries = MdnsPacket.parse(new MdnsPacketReader(earlyAddress.query(20), earlyAddress.query(20).length,
                MdnsFeatureFlags.newBuilder().build()));
        check(queries.questions.size() == 2 && queries.questions.get(0).getType() == MdnsRecord.TYPE_A
                && queries.questions.get(1).getType() == MdnsRecord.TYPE_AAAA, "missing target addresses are queried");
        for (MdnsRecord question : queries.questions) check(!question.isUnicastReplyRequested(), "multicast replies coexist with other local clients");
        accept(earlyAddress, "a", 30); check(earlyAddress.result(30) != null, "fresh queried address completes");
        State empty = state(); accept(empty, "empty_txt", 100);
        check(empty.result(100) != null && empty.result(100).getAttributes().isEmpty(), "observed empty TXT is complete");
        State both = state(); accept(both, "srv", 100); accept(both, "addresses", 100);
        accept(both, "flush4", 200); accept(both, "txt", 200);
        NsdServiceInfo flushed = both.result(200);
        check(flushed.getHostAddresses().size() == 2, "same-type flush preserves other family");
        check(flushed.getHostAddresses().get(0).getHostAddress().equals("192.0.2.11"), "complete new IPv4 RRset replaces old");
        check(((Inet6Address) flushed.getHostAddresses().get(1)).getScopeId() == 17, "IPv6 scope uses actual IPv6 index");
        State scoped6 = state(); accept(scoped6, "ipv6_scopes", 100);
        List<java.net.InetAddress> scopedAddresses = scoped6.result(100).getHostAddresses();
        check(scopedAddresses.size() == 3, "all observed IPv6 scope classes retained");
        for (java.net.InetAddress address : scopedAddresses) {
            check(((Inet6Address) address).getScopeId() == (address.isLinkLocalAddress() ? 17 : 0),
                    "only link-local IPv6 receives the actual interface scope");
        }
        State noV6Index = new State(NAME, TYPE, new Interface(7, 7, 0, 77, 3, 1, 107));
        accept(noV6Index, "ipv6_scopes", 100);
        check(noV6Index.result(100).getHostAddresses().size() == 2,
                "unknown IPv6 scope omits only link-local addresses");
        State many = state(); accept(many, "srv", 100); accept(many, "addresses", 100); accept(many, "txt", 100);
        check(many.result(100).getHostAddresses().size() == 3, "cache flush retains all records in the incoming RRset");
        State changed = state(); accept(changed, "srv", 100); accept(changed, "a", 100);
        accept(changed, "srv_changed", 200); accept(changed, "txt", 200);
        check(changed.result(200) == null, "changed SRV target discards old addresses");
        accept(changed, "a", 300); check(changed.result(300) == null, "old target cannot satisfy new SRV");
        accept(changed, "a_changed", 400); check(changed.result(400).getHostname().equals("changed"), "new target uses new observations");
        for (String goodbye : List.of("goodbye_srv", "goodbye_txt", "goodbye_a", "goodbye_ptr")) {
            State gone = state(); accept(gone, "srv", 100); accept(gone, "a", 100);
            accept(gone, goodbye, 200); accept(gone, "txt", 200);
            check(gone.result(200) == null, goodbye + " retires the cached instance immediately in r4 profile");
            accept(gone, "srv", 300); check(gone.result(300) == null, goodbye + " cannot reuse retired addresses");
            accept(gone, "a", 300); check(gone.result(300) != null, goodbye + " fresh observations recover");
        }
        State expired = state(); accept(expired, "short_srv", 100); accept(expired, "txt", 100);
        accept(expired, "a", 1101); check(expired.result(1101) == null, "expired SRV cannot complete later");
        accept(expired, "srv", 1200); accept(expired, "a", 1200);
        check(expired.result(1200) == null, "SRV expiry retires prior TXT too");
        accept(expired, "txt", 1200); check(expired.result(1200) != null, "fresh full data after expiry");
        State refreshed = state(); accept(refreshed, "short_srv", 100); accept(refreshed, "short_srv", 900);
        accept(refreshed, "txt", 1200); accept(refreshed, "a", 1200);
        check(refreshed.result(1200) != null, "same-record refresh updates its lifetime");
        State subtypes = state(); accept(subtypes, "subtypes", 100);
        check(subtypes.result(100) == null, "optional subtype PTR does not establish resolution");
        accept(subtypes, "full4", 200);
        check(subtypes.result(200).getSubtypes().equals(Set.of("_fast", "_slow")),
                "only observed type-qualified PTRs targeting the selected instance are forwarded");
        check(subtypes.result(1200).getSubtypes().equals(Set.of("_fast", "_slow")),
                "r4 retains known subtype metadata without independent subtype TTL expiry");
        accept(subtypes, "subtype_case", 1300);
        check(subtypes.result(1300).getSubtypes().equals(Set.of("_FAST", "_slow")),
                "case-insensitive subtype refresh keeps the latest observed spelling");
        accept(subtypes, "subtype_goodbye", 1400);
        check(subtypes.result(1400).getSubtypes().equals(Set.of("_fast", "_slow")),
                "subtype-only zero TTL retains metadata and does not retire the service in r4");
        accept(subtypes, "goodbye_ptr", 1500); accept(subtypes, "full4", 1600);
        check(subtypes.result(1600).getSubtypes().isEmpty(), "whole-service goodbye clears subtype metadata");
        State subtypeExpiry = state(); accept(subtypeExpiry, "full_subtypes", 100);
        accept(subtypeExpiry, "short_srv", 200); accept(subtypeExpiry, "full4", 1201);
        check(subtypeExpiry.result(1201).getSubtypes().isEmpty(), "SRV expiry clears subtype metadata");
        State requestSubtype = new State(NAME, "_hint." + TYPE, SCOPE);
        accept(requestSubtype, "full4", 100);
        check(requestSubtype.result(100).getSubtypes().isEmpty(), "request syntax does not invent supported subtypes");
        State limitedSubtypes = state();
        try { accept(limitedSubtypes, "subtypes_budget", 100); throw new AssertionError("Missing subtype resource bound"); }
        catch (IOException expected) { check(expected.getMessage().contains("subtype metadata budget"), "subtype observations are bounded"); }
        for (String unrelated : List.of("other_instance", "other_host", "query", "status")) {
            State selected = state(); accept(selected, unrelated, 100);
            check(selected.result(100) == null, unrelated + " cannot establish a selected response");
        }
        for (Packet other : List.of(new Packet(2, 7, 4, 7, packets.get("full4")),
                new Packet(3, 11, 4, 11, packets.get("full4")),
                new Packet(3, 7, 4, 17, packets.get("full4")))) {
            State selected = state(); selected.accept(other, 100);
            check(selected.result(100) == null, "wrong generation/interface metadata ignored");
        }
        Interface local = new Interface(7, 7, 17, 77, 3, 3, 0);
        State unMapped = new State(NAME, TYPE, local); accept(unMapped, "full6", 100);
        check(unMapped.result(100).getNetwork() == null && unMapped.result(100).getInterfaceIndex() == 7,
                "unmapped local scope does not invent an Android Network");
    }

    private static final class Queue implements Executor {
        final ArrayDeque<Runnable> tasks = new ArrayDeque<>();
        boolean reject;
        public void execute(Runnable task) { if (reject) throw new RejectedExecutionException("fixture"); tasks.add(task); }
        void drain() { while (!tasks.isEmpty()) tasks.remove().run(); }
    }
    private static final class Operation implements WindowsNsdResolver.Session {
        final WindowsNsdResolver.Callback callback;
        boolean stopped;
        int stopCalls;
        Operation(WindowsNsdResolver.Callback callback) { this.callback = callback; }
        public void stop() { stopped = true; stopCalls++; }
    }
    private static final class Backend implements WindowsNsdResolver.Factory {
        final List<Operation> operations = new ArrayList<>();
        public WindowsNsdResolver.Session start(NsdServiceInfo request, WindowsNsdResolver.Callback callback) {
            Operation op = new Operation(callback); operations.add(op); return op;
        }
        Operation last() { return operations.get(operations.size() - 1); }
    }
    private static class Listener implements NsdManager.ResolveListener {
        final List<String> events = new ArrayList<>();
        NsdServiceInfo value;
        public void onServiceResolved(NsdServiceInfo info) { value = info; events.add("resolved"); }
        public void onResolveFailed(NsdServiceInfo info, int error) { value = info; events.add("failed " + error); }
        public void onResolutionStopped(NsdServiceInfo info) { value = info; events.add("stopped"); }
        public void onStopResolutionFailed(NsdServiceInfo info, int error) { value = info; events.add("stopfailed " + error); }
    }
    private static final class EqualListener extends Listener {
        @Override public boolean equals(Object other) { return other instanceof EqualListener; }
        @Override public int hashCode() { return 1; }
    }
    private static final class BothListener extends Listener implements NsdManager.DiscoveryListener {
        public void onStartDiscoveryFailed(String type, int error) {}
        public void onStopDiscoveryFailed(String type, int error) {}
        public void onDiscoveryStarted(String type) {}
        public void onDiscoveryStopped(String type) {}
        public void onServiceFound(NsdServiceInfo info) {}
        public void onServiceLost(NsdServiceInfo info) {}
    }
    private static void managerContracts() throws Exception {
        Queue service = new Queue(), app = new Queue(); Backend backend = new Backend();
        List<WindowsNsd.Callback> browse = new ArrayList<>();
        WindowsNsd.Factory browsers = new WindowsNsd.Factory() {
            public int[] interfaces() { return new int[] {7}; }
            public WindowsNsd.Session start(String type, String subtype, WindowsNsd.Scope scope, WindowsNsd.Callback callback) {
                browse.add(callback); return () -> {};
            }
        };
        Constructor<NsdManager> constructor = NsdManager.class.getDeclaredConstructor(Context.class,
                Executor.class, WindowsNsd.Factory.class, WindowsNsdResolver.Factory.class);
        constructor.setAccessible(true);
        NsdManager manager = constructor.newInstance(null, service, browsers, backend);
        Listener listener = new Listener(); NsdServiceInfo original = request();
        expect(NullPointerException.class, () -> manager.resolveService(null, app, listener));
        expect(IllegalArgumentException.class, () -> manager.resolveService(new NsdServiceInfo(), null, null));
        expect(NullPointerException.class, () -> manager.resolveService(original, app, null));
        expect(IllegalArgumentException.class, () -> manager.stopServiceResolution(listener));
        manager.resolveService(original, app, listener); Operation first = backend.last();
        expect(IllegalArgumentException.class, () -> manager.resolveService(original, app, listener));
        manager.stopServiceResolution(listener); check(first.stopped, "cancel reaches owned transport");
        check(listener.events.isEmpty(), "stop is not successful before backend retirement");
        manager.stopServiceResolution(listener);
        check(first.stopCalls == 1, "Pending repeated stop coalesces without restarting transport cleanup");
        first.callback.stopped(); check(listener.events.isEmpty(), "backend callback enters service dispatcher");
        manager.stopServiceResolution(listener);
        check(first.stopCalls == 1, "Stop coalesces while terminal dispatch is queued");
        service.drain(); check(listener.events.isEmpty(), "application executor owns terminal callback");
        manager.resolveService(original, app, listener); Operation second = backend.last();
        app.drain(); check(listener.value == original && listener.events.equals(List.of("stopped")), "original request and reuse before callback execution");
        first.callback.resolved(request()); service.drain(); app.drain();
        check(listener.events.size() == 1, "stale terminal cannot target reused listener");
        second.callback.failed(0); service.drain(); app.drain();
        check(listener.value == original && listener.events.get(1).equals("failed 0"), "failure returns original object");
        manager.resolveService(original, listener); backend.last().callback.failed(0);
        check(listener.events.size() == 2, "default callback never runs on submitting thread");
        service.drain(); check(listener.events.size() == 3, "default executor runs inside service dispatcher");
        Listener reentrant = new Listener() {
            @Override public void onServiceResolved(NsdServiceInfo info) {
                super.onServiceResolved(info); manager.resolveService(original, app, this);
            }
        };
        manager.resolveService(original, app, reentrant); Operation before = backend.last();
        before.callback.resolved(request()); service.drain(); app.drain();
        check(backend.last() != before, "reentrant callback can reuse listener");
        backend.last().callback.stopped(); service.drain(); app.drain();
        EqualListener a = new EqualListener(), b = new EqualListener();
        manager.resolveService(original, app, a); Operation one = backend.last();
        manager.resolveService(original, app, b); Operation two = backend.last();
        one.callback.stopped(); two.callback.stopped(); service.drain(); app.drain();
        check(a.events.size() == 1 && b.events.size() == 1, "listener keys use identity");
        BothListener both = new BothListener();
        manager.resolveService(original, app, both);
        expect(IllegalArgumentException.class, () -> manager.discoverServices(TYPE, 1, (Network) null, app, both));
        backend.last().callback.stopped(); service.drain(); app.drain();
        manager.discoverServices(TYPE, 1, (Network) null, app, both);
        expect(IllegalArgumentException.class, () -> manager.resolveService(original, app, both));
        browse.get(0).stopped(); service.drain();
        manager.resolveService(original, app, listener); app.reject = true; backend.last().callback.stopped();
        expect(RejectedExecutionException.class, service::drain); app.reject = false;
        expect(IllegalArgumentException.class, () -> manager.stopServiceResolution(listener));
        manager.resolveService(original, app, listener); backend.last().callback.stopFailed(0); service.drain(); app.drain();
        check(listener.value == original && listener.events.get(listener.events.size() - 1).equals("stopfailed 0"), "failed cleanup has one terminal stop-failure callback");
        for (int at = 0; at < 200; at++) manager.resolveService(original, app, new Listener());
        int admitted = backend.operations.size();
        manager.resolveService(original, app, new Listener());
        check(backend.operations.size() == admitted + 1, "Java resolves have no discovery quota admission check");
        int[] discoveryError = {-1};
        NsdManager.DiscoveryListener limitedBrowse = new NsdManager.DiscoveryListener() {
            public void onStartDiscoveryFailed(String type, int code) { discoveryError[0] = code; }
            public void onDiscoveryStarted(String type) { throw new AssertionError("Discovery exceeded combined quota"); }
            public void onStopDiscoveryFailed(String type, int code) {}
            public void onDiscoveryStopped(String type) {}
            public void onServiceFound(NsdServiceInfo info) {}
            public void onServiceLost(NsdServiceInfo info) {}
        };
        int browsesBefore = browse.size();
        manager.discoverServices(TYPE, 1, (Network) null, app, limitedBrowse); service.drain(); app.drain();
        check(discoveryError[0] == NsdManager.FAILURE_MAX_LIMIT && browse.size() == browsesBefore,
                "discovery quota counts active resolutions before starting transport");
        for (int at = backend.operations.size() - 201; at < backend.operations.size(); at++) backend.operations.get(at).callback.stopped();
        service.drain(); app.drain();
    }

    private static final class Driver implements WindowsNsdResolver.Driver {
        Interface[] scopes = {SCOPE}; long now = 0, generation = 3;
        final ArrayDeque<Packet> incoming = new ArrayDeque<>();
        final List<Integer> sent = new ArrayList<>();
        final List<Long> sentAt = new ArrayList<>();
        final List<byte[]> queries = new ArrayList<>();
        IntConsumer tick = ignored -> {};
        java.util.function.IntPredicate sendResult = ignored -> true;
        int polls, configurations, failCloses;
        volatile int closes;
        public Interface[] interfaces() { return scopes; }
        public boolean configure(Interface[] selected) { configurations++; return true; }
        public long generation() { return generation; }
        public Packet poll(int timeout) {
            now += Math.max(1, timeout); tick.accept(++polls);
            if (polls > 200) throw new AssertionError("Fixture failed to stop its own resolver");
            return incoming.poll();
        }
        public boolean send(int index, byte[] query) {
            sent.add(index); sentAt.add(now); queries.add(query); return sendResult.test(sent.size());
        }
        public synchronized boolean close() { return ++closes > failCloses; }
    }
    private static final class Callback implements WindowsNsdResolver.Callback {
        final Driver driver;
        String terminal; NsdServiceInfo result; int count;
        Callback(Driver driver) { this.driver = driver; }
        private void done(String kind) { check(driver.closes > 0, "cleanup precedes " + kind); terminal = kind; count++; }
        public void resolved(NsdServiceInfo value) { result = value; done("resolved"); }
        public void stopped() { done("stopped"); }
        public void failed(int error) { check(error == 0, "backend error code"); done("failed"); }
        public void stopFailed(int error) { check(error == 0, "cleanup error code"); done("stopfailed"); }
    }
    private static WindowsNsdResolver.Resolver resolver(Driver driver, NsdServiceInfo info, Callback callback) {
        return new WindowsNsdResolver.Resolver(() -> driver, info, callback, () -> driver.now, () -> 0);
    }
    private static void lifecycleContracts() throws Exception {
        Driver quiet = new Driver(); Callback quietCallback = new Callback(quiet);
        WindowsNsdResolver.Resolver neverTimesOut = resolver(quiet, request(), quietCallback);
        quiet.tick = step -> { quiet.now += 60 * 60 * 1000L; if (step == 20) neverTimesOut.stop(); };
        neverTimesOut.run();
        check(quietCallback.terminal.equals("stopped") && quietCallback.count == 1, "twenty fixture hours without a response remain cancelable, not timed out");
        check(quiet.sent.size() > 3 && quiet.closes == 1, "continuous questions and confirmed close");
        Driver late = new Driver(); Callback lateCallback = new Callback(late);
        WindowsNsdResolver.Resolver lateResolver = resolver(late, request(), lateCallback);
        late.tick = step -> {
            if (step <= 12) late.now += 60 * 60 * 1000L;
            if (step == 13) late.incoming.add(packet("short_srv", SCOPE));
            if (step == 14) {
                try {
                    byte[] query = late.queries.get(late.queries.size() - 1);
                    MdnsPacket decoded = MdnsPacket.parse(new MdnsPacketReader(query, query.length, MdnsFeatureFlags.newBuilder().build()));
                    if (decoded.questions.stream().anyMatch(value -> value.getType() == MdnsRecord.TYPE_A)) {
                        check(decoded.questions.size() == 2, "fresh target questions do not repeat rate-limited SRV/TXT questions");
                        late.incoming.add(packet("txt_a", SCOPE));
                    }
                } catch (IOException error) { throw new AssertionError(error); }
            }
            if (step == 40) lateResolver.stop();
        };
        lateResolver.run();
        check(lateCallback.terminal.equals("resolved") && late.polls == 14,
                "late short-lived SRV triggers prompt address questions despite prior hour backoff");
        Driver retry = new Driver(); Callback retryCallback = new Callback(retry);
        WindowsNsdResolver.Resolver retryResolver = resolver(retry, request(), retryCallback);
        retry.sendResult = number -> number != 13;
        retry.tick = step -> {
            if (step <= 12) retry.now += 60 * 60 * 1000L;
            if (retry.sent.size() >= 14) retry.incoming.add(packet("full4", SCOPE));
            if (step == 40) retryResolver.stop();
        };
        retryResolver.run();
        check(retryCallback.terminal.equals("resolved"), "temporary unsent query retries despite prior hour backoff");
        check(retry.sentAt.get(13) - retry.sentAt.get(12) == 1000,
                "nonblocking send retry preserves the one-second minimum question interval");
        Driver live = new Driver(); Callback liveCallback = new Callback(live);
        live.incoming.add(packet("full4", SCOPE)); resolver(live, request(), liveCallback).run();
        check(liveCallback.terminal.equals("resolved") && liveCallback.count == 1, "full observations yield one terminal result"); binary(liveCallback.result);
        Driver changed = new Driver(); Callback changedCallback = new Callback(changed);
        Interface returned = new Interface(7, 7, 17, 88, 4, 3, 107);
        changed.tick = step -> {
            if (step == 1) changed.incoming.add(packet("srv", SCOPE));
            if (step == 2) { changed.generation = 4; changed.scopes = new Interface[] {returned}; changed.incoming.add(packet("full4", SCOPE)); }
            if (step == 3) changed.incoming.add(packet("a", returned));
            if (step == 4) changed.incoming.add(packet("srv", returned));
            if (step == 5) changed.incoming.add(packet("txt", returned));
            if (step == 6) changed.incoming.add(packet("a", returned));
        };
        resolver(changed, request(), changedCallback).run();
        check(changed.polls == 6 && changed.configurations == 2 && changedCallback.terminal.equals("resolved"),
                "same numeric index with new LUID/generation never reuses old records");
        Driver restricted = new Driver(); Callback restrictedCallback = new Callback(restricted);
        restricted.scopes = new Interface[] {SCOPE, new Interface(11, 11, 21, 111, 3, 3, 111)};
        NsdServiceInfo scoped = request(); scoped.setNetwork(new Network(107));
        WindowsNsdResolver.Resolver scopedResolver = resolver(restricted, scoped, restrictedCallback);
        restricted.tick = step -> { if (step == 2) scopedResolver.stop(); };
        scopedResolver.run(); check(restricted.sent.equals(List.of(7)), "explicit Network sends only on its current mapping");
        Driver absent = new Driver(); Callback absentCallback = new Callback(absent);
        NsdServiceInfo unknown = request(); unknown.setNetwork(new Network(999));
        WindowsNsdResolver.Resolver absentResolver = resolver(absent, unknown, absentCallback);
        absent.tick = step -> { if (step == 2) absentResolver.stop(); };
        absentResolver.run(); check(absent.sent.isEmpty() && absentCallback.terminal.equals("stopped"), "absent Network never falls back");
        Driver failedClose = new Driver(); failedClose.failCloses = 2;
        Callback failedCloseCallback = new Callback(failedClose);
        WindowsNsdResolver.Resolver cancel = resolver(failedClose, request(), failedCloseCallback);
        failedClose.tick = step -> cancel.stop(); cancel.run();
        check(failedCloseCallback.terminal.equals("stopfailed") && failedCloseCallback.count == 1, "unconfirmed cleanup cannot report successful stop");
        long deadline = System.nanoTime() + 3_000_000_000L;
        while (failedClose.closes < 3 && System.nanoTime() < deadline) Thread.sleep(10);
        check(failedClose.closes >= 3 && failedCloseCallback.count == 1, "app-free retirement retry cannot emit a second callback");
        Driver tooMany = new Driver(); tooMany.scopes = new Interface[65];
        for (int at = 0; at < 65; at++) tooMany.scopes[at] = new Interface(at + 1, at + 1, 0, at + 1, 3, 1, 0);
        Callback limited = new Callback(tooMany); resolver(tooMany, request(), limited).run();
        check(limited.terminal.equals("failed") && tooMany.configurations == 0, "interface resource budget fails before opening transport scopes");
    }

    public static void main(String[] args) throws Exception {
        if (args.length != 2) throw new IllegalArgumentException("native initialization library or --windows, packet fixture path");
        if (args[0].equals("--windows")) System.loadLibrary("android_runtime"); else System.load(args[0]);
        for (String line : Files.readAllLines(Path.of(args[1]))) {
            String[] fields = line.split("\t", 2); packets.put(fields[0], hex(fields[1]));
        }
        stateContracts(); managerContracts(); lifecycleContracts();
        System.out.println("NsdResolveContractTest all passed: " + checks + "; transport=explicit-fixtures; windowsExecution=false");
    }
}
