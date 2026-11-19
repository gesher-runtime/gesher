/* Copyright (C) 2026 The Gesher Authors. SPDX-License-Identifier: Apache-2.0 */
package com.gesher.net.mdns;

import android.net.nsd.NsdServiceInfo;
import android.os.SystemClock;
import com.gesher.net.WindowsNsdResolver;
import java.net.Inet6Address;
import java.net.InetAddress;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.Arrays;
import java.util.List;
import java.util.Set;

/** Genuine records/decoder/scheduler with explicit receipt time and valid wire. */
public final class NsdSubscriptionStateTest {
    static int checks;
    static final String[] NAME = {"Owned subscription", "_owned", "_tcp", "local"};
    static final String[] TYPE = {"_owned", "_tcp", "local"};
    static final String[] HOST = {"OwnedHost", "local"};
    static final WindowsNsdResolver.Interface SCOPE = new WindowsNsdResolver.Interface(7, 7, 42, 9007, 3, 3, 107);
    static final MdnsFeatureFlags FLAGS = MdnsFeatureFlags.newBuilder().build();
    static void check(boolean value, String reason) { if (!value) throw new AssertionError(reason); checks++; }
    static MdnsServiceRecord srv(long now, long ttl, int port, String[] host) {
        return new MdnsServiceRecord(NAME, now, true, ttl, 0, 0, port, host);
    }
    static MdnsTextRecord txt(long now, long ttl, byte value) {
        return new MdnsTextRecord(NAME, now, true, ttl, List.of(
                new MdnsServiceInfo.TextEntry("Binary", new byte[]{0, (byte)255, value}),
                new MdnsServiceInfo.TextEntry("binary", new byte[]{88}),
                new MdnsServiceInfo.TextEntry("none", (byte[])null),
                new MdnsServiceInfo.TextEntry("empty", new byte[0])));
    }
    static MdnsInetAddressRecord address(long now, long ttl, String text) throws Exception {
        return new MdnsInetAddressRecord(HOST, now, true, ttl, InetAddress.getByName(text));
    }
    static MdnsPacket packet(MdnsRecord... records) {
        return new MdnsPacket(0x8400, List.of(), Arrays.asList(records), List.of(), List.of());
    }
    static MdnsSubscriptionState state(long now) throws Exception {
        return new MdnsSubscriptionState(NAME[0], "_ignored._owned._tcp,_also", SCOPE, now);
    }
    static MdnsPacket complete(long now, long srvTtl) throws Exception {
        return packet(srv(now, srvTtl, 41415, HOST), txt(now, 1000, (byte)1),
                address(now, 1000, "192.0.2.7"), address(now, 1000, "fe80::7"),
                new MdnsPointerRecord(new String[]{"_Print", "_sub", "_owned", "_tcp", "local"},
                        now, false, 1000, NAME));
    }
    static NsdServiceInfo oneUpdate(List<MdnsSubscriptionState.Event> events, String reason) {
        check(events.size() == 1 && !events.get(0).lost, reason);
        return events.get(0).update;
    }
    static void records() throws Exception {
        long now = 100_000;
        MdnsSubscriptionState state = state(now);
        NsdServiceInfo first = oneUpdate(state.acceptParsed(complete(now, 5000), now), "complete named response needs no base PTR");
        check(first.getServiceName().equals(NAME[0]) && first.getServiceType().equals("_owned._tcp"), "bare base type and ignored selector subtypes");
        check(first.getPort() == 41415 && first.getHostname().equals("OwnedHost"), "full SRV result");
        check(first.getNetwork().netId == 107, "actual Network association");
        check(first.getHostAddresses().size() == 2 && ((Inet6Address)first.getHostAddresses().get(1)).getScopeId() == 42,
                "IPv4-first addresses and true distinct IPv6 scope index");
        check(Arrays.equals(first.getAttributes().get("Binary"), new byte[]{0, (byte)255, 1}), "binary TXT and first case-insensitive key");
        check(first.getAttributes().containsKey("none") && first.getAttributes().get("none") == null
                && first.getAttributes().get("empty").length == 0, "key-only and empty TXT remain distinct");
        check(first.getSubtypes().equals(Set.of("_Print")), "response subtype preserved");
        check(state.expire(now + 1500).isEmpty(), "TXT/address/subtype TTL alone does not remove service");
        check(state.acceptParsed(complete(now + 2000, 5000), now + 2000).isEmpty(), "receipt-only refresh remains silent");
        check(state.expire(now + 5001).isEmpty(), "receipt-only refresh extends SRV lifetime");
        NsdServiceInfo changed = oneUpdate(state.acceptParsed(packet(txt(now + 5002, 5000, (byte)2)), now + 5002), "TXT content change is an update");
        check(changed.getPort() == 41415 && changed.getAttributes().get("Binary")[2] == 2
                && changed.getHostAddresses().size() == 2, "update is a complete current value");
        NsdServiceInfo v4 = oneUpdate(state.acceptParsed(packet(address(now + 5003, 5000, "192.0.2.8")), now + 5003), "IPv4 cache flush updates");
        check(v4.getHostAddresses().size() == 2 && v4.getHostAddresses().get(0).getHostAddress().equals("192.0.2.8")
                && v4.getHostAddresses().get(1) instanceof Inet6Address, "IPv4 flush preserves IPv6 family");
        NsdServiceInfo noSubtype = oneUpdate(state.acceptParsed(packet(new MdnsPointerRecord(
                new String[]{"_Print", "_sub", "_owned", "_tcp", "local"}, now + 5004, false, 0, NAME)), now + 5004),
                "subtype goodbye is an update rather than service loss");
        check(noSubtype.getSubtypes().equals(Set.of("_Print")), "pinned profile retains subtype PTR including its zero TTL");
        List<MdnsSubscriptionState.Event> expired = state.expire(now + 7000);
        check(expired.size() == 1 && expired.get(0).lost && state.expire(now + 7001).isEmpty(), "SRV expiry delivers exactly one loss");
        oneUpdate(state.acceptParsed(complete(now + 8000, 5000), now + 8000), "continuing request reports reappearance");
        List<MdnsSubscriptionState.Event> goodbye = state.acceptParsed(packet(new MdnsPointerRecord(TYPE,
                now + 8001, false, 0, NAME)), now + 8001);
        check(goodbye.size() == 1 && goodbye.get(0).lost, "base PTR goodbye retires complete response");
        check(state.retire(now + 8002) == null, "retirement does not duplicate loss");
        MdnsSubscriptionState partial = state(now);
        oneUpdate(partial.acceptParsed(complete(now, 5000), now), "second complete state");
        check(partial.acceptParsed(packet(srv(now + 1, 3000, 42, new String[]{"NewTarget", "local"})), now + 1).isEmpty(), "incomplete target change stays silent");
        check(partial.expire(now + 3001).isEmpty(), "incomplete current response follows pinned removal rule");
        MdnsSubscriptionState unmatched = state(now);
        check(unmatched.acceptParsed(packet(address(now, 5000, "192.0.2.7")), now).isEmpty()
                && unmatched.retire(now + 1) == null, "unobserved scope never invents a loss");
    }
    static void wire(Path output) throws Exception {
        Files.createDirectories(output);
        long now = SystemClock.elapsedRealtime();
        byte[] bytes = MdnsAdvertisingPacket.createRawDnsPacket(new byte[1500], complete(now, 120000));
        Files.write(output.resolve("owned-complete-response.bin"), bytes);
        MdnsSubscriptionState state = state(now);
        check(state.accept(new WindowsNsdResolver.Packet(2, 7, 4, 7, bytes), now).isEmpty(), "stale transport generation discarded");
        check(state.accept(new WindowsNsdResolver.Packet(3, 7, 6, 7, bytes), now).isEmpty(), "different family receiving index discarded");
        NsdServiceInfo value = oneUpdate(state.accept(new WindowsNsdResolver.Packet(3, 7, 6, 42, bytes), now), "genuine valid wire decoded");
        check(value.getAttributes().get("Binary")[1] == (byte)255, "wire keeps opaque TXT");
        state.rebind(new WindowsNsdResolver.Interface(7, 7, 42, 9007, 4, 3, 107));
        check(state.accept(new WindowsNsdResolver.Packet(3, 7, 4, 7, bytes), now).isEmpty(), "rebind rejects former raw generation");
        check(state.accept(new WindowsNsdResolver.Packet(4, 7, 4, 7, bytes), now).isEmpty(), "same lifetime rebind retains silent value");
        WindowsNsdResolver.Interface link = new WindowsNsdResolver.Interface(7, 7, 42, 9007, 3, 3, 0);
        MdnsSubscriptionState linkState = new MdnsSubscriptionState(NAME[0], "_owned._tcp", link, now);
        value = oneUpdate(linkState.accept(new WindowsNsdResolver.Packet(3, 7, 4, 7, bytes), now), "link-only response");
        check(value.getNetwork() == null && value.getInterfaceIndex() == 7, "link-only scope is not a fabricated Network");
    }
    static void queries(Path output) throws Exception {
        long now = 200_000;
        MdnsSubscriptionState state = state(now);
        MdnsSubscriptionState.Query query = state.query(now, true);
        check(query != null, "initial passive query due");
        MdnsPacket decoded = MdnsPacket.parse(new MdnsPacketReader(query.bytes, query.bytes.length, FLAGS));
        check(decoded.questions.size() == 2 && decoded.questions.get(0) instanceof MdnsServiceRecord
                && decoded.questions.get(1) instanceof MdnsTextRecord, "initial query asks exact SRV/TXT instance");
        check(decoded.questions.stream().allMatch(record -> record.getRecordClass() == 1), "query requests multicast replies");
        check(state.query(now, true) != null, "unsent query does not charge schedule");
        state.sent(query, now);
        check(state.query(now + 999, true) == null, "successful send enforces one-second question spacing");
        check(state.query(now + 1000, true) != null, "passive schedule permits next initial question");
        state.acceptParsed(packet(srv(now + 5, 10000, 41415, HOST)), now + 5);
        MdnsSubscriptionState.Query target = state.query(now + 6, true);
        decoded = MdnsPacket.parse(new MdnsPacketReader(target.bytes, target.bytes.length, FLAGS));
        check(decoded.questions.size() == 2 && decoded.questions.stream().allMatch(record -> record instanceof MdnsInetAddressRecord),
                "new target asks both address families without repeating fresh question");
        Files.write(output.resolve("owned-target-query.bin"), target.bytes);
        check(MdnsSubscriptionState.baseType("_sub._owned._tcp,_more").equals("_owned._tcp")
                && MdnsSubscriptionState.baseType("invalid") == null, "pinned type normalization and invalid admission");
    }
    public static void main(String[] args) throws Exception {
        System.load(args[0]); records(); wire(Path.of(args[1])); queries(Path.of(args[1]));
        System.out.println("NSD_SUBSCRIPTION_STATE_OK checks=" + checks);
    }
}
