/* Copyright (C) 2026 The Gesher Authors. SPDX-License-Identifier: Apache-2.0 */
package com.gesher.net.mdns;

import android.net.Network;
import android.net.nsd.NsdServiceInfo;
import android.util.Pair;
import com.android.net.module.util.DnsUtils;
import com.gesher.net.WindowsNsdResolver;
import java.io.IOException;
import java.net.Inet4Address;
import java.net.Inet6Address;
import java.net.InetAddress;
import java.nio.charset.StandardCharsets;
import java.time.Instant;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collections;
import java.util.HashMap;
import java.util.LinkedHashSet;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.Set;
import java.util.TreeMap;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

/** One named instance on one real interface lifetime, confined to its owning worker. */
public final class MdnsSubscriptionState {
    private static final String TYPE_LABEL = "_[a-zA-Z0-9-_]{1,61}[a-zA-Z0-9]";
    private static final String SUBTYPE_LABEL = "_[\\x20-\\x2b\\x2d\\x2f-\\x7e]{1,62}";
    private static final Pattern TYPE = Pattern.compile("^(?:(" + SUBTYPE_LABEL + ")\\.)?("
            + TYPE_LABEL + "\\._(?:tcp|udp))\\.?((?:," + SUBTYPE_LABEL + ")*)$");
    private static final MdnsFeatureFlags FLAGS = MdnsFeatureFlags.newBuilder().build();
    private static final int MAX_PACKET = 65535, MAX_PACKET_RECORDS = 8192;
    private static final int MAX_ADDRESSES = 64, MAX_POINTERS = 65;
    private final String type;
    private final String[] name, typeLabels;
    private WindowsNsdResolver.Interface scope;
    private MdnsResponse response;
    private boolean discovered;
    private long observationTime;
    private final MdnsResponseDecoder decoder;
    private final MdnsQueryScheduler scheduler = new MdnsQueryScheduler();
    private final MdnsSearchOptions options;
    private MdnsQueryScheduler.ScheduledQueryTaskArgs scheduled;
    private long lastSent;
    private final Map<Object, Long> lastQuestions = new HashMap<>();

    /** No subscription subtype filter is inferred from a service-type suffix. */
    public static String baseType(String value) {
        if (value == null) return null;
        Matcher parsed = TYPE.matcher(value);
        return parsed.matches() ? parsed.group(2) : null;
    }

    public static final class Event {
        public final boolean lost;
        public final NsdServiceInfo update;
        private Event(boolean lost, NsdServiceInfo update) {
            this.lost = lost;
            this.update = update;
        }
    }

    /** A query is charged to the schedule only after the transport confirms its send. */
    public static final class Query {
        public final byte[] bytes;
        private final List<MdnsRecord> questions;
        private final MdnsQueryScheduler.ScheduledQueryTaskArgs scheduled;
        private Query(byte[] bytes, List<MdnsRecord> questions,
                MdnsQueryScheduler.ScheduledQueryTaskArgs scheduled) {
            this.bytes = bytes;
            this.questions = questions;
            this.scheduled = scheduled;
        }
    }

    public MdnsSubscriptionState(String instance, String requestedType,
            WindowsNsdResolver.Interface scope, long now) throws IOException {
        type = baseType(requestedType);
        if (type == null) throw new IOException("Invalid DNS-SD subscription type");
        int dot = type.indexOf('.');
        typeLabels = new String[] {type.substring(0, dot), type.substring(dot + 1), "local"};
        name = new String[] {instance, typeLabels[0], typeLabels[1], typeLabels[2]};
        if (!encodable(name)) throw new IOException("Unencodable DNS-SD instance name");
        this.scope = scope;
        response = empty(now);
        options = MdnsSearchOptions.newBuilder().setResolveInstanceName(instance)
                .setNetwork(scope.netId == 0 ? null : new Network(scope.netId))
                .setInterfaceIndex(scope.netId == 0 ? scope.index : 0)
                .setQueryMode(MdnsSearchOptions.PASSIVE_QUERY_MODE)
                .setRemoveExpiredService(true).build();
        decoder = new MdnsResponseDecoder(new MdnsSubscriptionUtils.Clock() {
            @Override public long elapsedRealtime() { return observationTime; }
        }, typeLabels);
        scheduled = scheduler.scheduleFirstRun(new QueryTaskConfig(options.getQueryMode()),
                now, 0, 1);
    }

    public WindowsNsdResolver.Interface scope() { return scope; }

    /** The transport owner must independently prove lifetime continuity before calling this. */
    public void rebind(WindowsNsdResolver.Interface current) {
        if (scope.luid != current.luid || scope.index != current.index
                || scope.netId != current.netId) {
            throw new IllegalArgumentException("Cannot rebind another DNS-SD scope");
        }
        scope = current;
    }

    private MdnsResponse empty(long now) {
        return new MdnsResponse(now, name, scope.index,
                scope.netId == 0 ? null : new Network(scope.netId));
    }

    private static boolean same(String[] a, String[] b) {
        return DnsUtils.equalsDnsLabelIgnoreDnsCase(a, b);
    }

    private static boolean encodable(String[] labels) {
        int bytes = 1;
        for (String label : labels) {
            if (label == null) return false;
            int length = label.getBytes(StandardCharsets.UTF_8).length;
            if (length == 0 || length > 63) return false;
            bytes += length + 1;
        }
        return bytes <= 256;
    }

    private boolean accepts(WindowsNsdResolver.Packet packet) {
        if (packet.generation != scope.generation || packet.index != scope.index) return false;
        if (packet.family == 4) {
            return (scope.families & 1) != 0 && packet.receivedIndex == scope.ipv4Index;
        }
        return packet.family == 6 && (scope.families & 2) != 0
                && packet.receivedIndex == scope.ipv6Index;
    }

    /** Ordinary production entry: genuine transport attribution and the pinned packet decoder. */
    public List<Event> accept(WindowsNsdResolver.Packet received, long now) throws IOException {
        if (!accepts(received)) return Collections.emptyList();
        byte[] bytes = received.bytes;
        if (bytes == null || bytes.length < 12 || bytes.length > MAX_PACKET) {
            return Collections.emptyList();
        }
        int flags = (bytes[2] & 255) << 8 | bytes[3] & 255;
        if ((flags & 0x8000) == 0 || (flags & 0x780f) != 0) return Collections.emptyList();
        int count = 0;
        for (int at = 4; at < 12; at += 2) count += (bytes[at] & 255) << 8 | bytes[at + 1] & 255;
        if (count > MAX_PACKET_RECORDS) return Collections.emptyList();
        final MdnsPacket packet;
        try { packet = MdnsResponseDecoder.parseResponse(bytes, bytes.length, FLAGS); }
        catch (MdnsPacket.ParseException ignored) { return Collections.emptyList(); }
        return acceptParsed(packet, now);
    }

    // Package-local deterministic fixture seam uses the actual record objects and
    // their explicit receipt times. Ordinary network intake always parses above.
    List<Event> acceptParsed(MdnsPacket packet, long now) throws IOException {
        List<Event> events = expire(now);
        observationTime = now;
        List<MdnsRecord> all = new ArrayList<>(packet.answers);
        all.addAll(packet.authorityRecords);
        all.addAll(packet.additionalRecords);
        List<String[]> targets = new ArrayList<>();
        if (response.hasServiceRecord()) targets.add(response.getServiceRecord().getServiceHost());
        for (MdnsRecord record : all) {
            if (record.getRecordClass() == 1 && record instanceof MdnsServiceRecord
                    && same(record.getName(), name)) {
                String[] target = ((MdnsServiceRecord) record).getServiceHost();
                if (encodable(target)) targets.add(target);
            }
        }
        List<MdnsRecord> selected = new ArrayList<>();
        for (MdnsRecord record : all) {
            if (record.getRecordClass() != 1) continue;
            if ((record instanceof MdnsServiceRecord || record instanceof MdnsTextRecord)
                    && same(record.getName(), name)) {
                if (!(record instanceof MdnsServiceRecord)
                        || encodable(((MdnsServiceRecord) record).getServiceHost())) {
                    selected.add(record);
                }
            } else if (record instanceof MdnsPointerRecord) {
                MdnsPointerRecord pointer = (MdnsPointerRecord) record;
                if (same(pointer.getPointer(), name)
                        && MdnsSubscriptionUtils.typeEqualsOrIsSubtype(typeLabels, pointer.getName())) {
                    selected.add(record);
                }
            } else if (record instanceof MdnsInetAddressRecord) {
                for (String[] target : targets) if (same(record.getName(), target)) {
                    selected.add(record);
                    break;
                }
            }
        }
        if (selected.isEmpty()) return events;
        MdnsPacket filtered = new MdnsPacket(packet.flags, Collections.emptyList(),
                selected, Collections.emptyList(), Collections.emptyList());
        Pair<Set<MdnsResponse>, ArrayList<MdnsResponse>> augmented = decoder.augmentResponses(
                filtered, Collections.singletonList(response), scope.index,
                scope.netId == 0 ? null : new Network(scope.netId), FLAGS);
        MdnsResponse next = augmented.second.get(0);
        if (next.getInet4AddressRecords().size() + next.getInet6AddressRecords().size()
                > MAX_ADDRESSES || next.getPointerRecords().size() > MAX_POINTERS) {
            throw new IOException("DNS-SD subscription record budget exceeded");
        }
        if (next.isGoodbye()) {
            Event lost = retire(now);
            if (lost != null) events.add(lost);
            return events;
        }
        boolean changed = augmented.first.contains(next);
        String[] oldHost = response.hasServiceRecord()
                ? response.getServiceRecord().getServiceHost() : null;
        response = next;
        if (response.hasServiceRecord() && (oldHost == null
                || !same(oldHost, response.getServiceRecord().getServiceHost()))) {
            lastQuestions.keySet().removeIf(key -> !key.equals(new MdnsServiceRecord(name, false).getKey())
                    && !key.equals(new MdnsTextRecord(name, false).getKey()));
        }
        if (changed) {
            discovered = true;
            if (response.isComplete()) events.add(new Event(false, result(now)));
        }
        MdnsQueryScheduler.ScheduledQueryTaskArgs rescheduled = scheduler.maybeRescheduleCurrentRun(
                now, remaining(now), lastSent, 1, options.numOfQueriesBeforeBackoff());
        if (rescheduled != null) scheduled = rescheduled;
        return events;
    }

    public List<Event> expire(long now) {
        List<Event> events = new ArrayList<>();
        // In this pinned profile only SRV expires the service. TXT, address and
        // subtype records are retained until service retirement or replacement.
        if (response.hasServiceRecord() && response.getMinRemainingTtl(now) <= 0) {
            Event event = retire(now);
            if (event != null) events.add(event);
        }
        return events;
    }

    /** A real socket/scope retirement follows the pinned current-response completeness rule. */
    public Event retire(long now) {
        boolean lost = discovered && response.isComplete();
        response = empty(now);
        discovered = false;
        lastQuestions.clear();
        scheduler.cancelScheduledRun();
        scheduled = scheduler.scheduleFirstRun(new QueryTaskConfig(options.getQueryMode()), now, 0, 1);
        return lost ? new Event(true, null) : null;
    }

    private long remaining(long now) {
        return response.hasServiceRecord() ? response.getMinRemainingTtl(now) : 0;
    }

    public long nextDue(long now) {
        long ttl = remaining(now);
        return response.hasServiceRecord() ? Math.min(scheduled.timeToRun, now + ttl)
                : scheduled.timeToRun;
    }

    /** Progress may ask for a newly learned target, but never repeat a question within one second. */
    public Query query(long now, boolean progress) throws IOException {
        boolean due = now >= scheduled.timeToRun;
        if (!due && !progress) return null;
        List<MdnsRecord> questions = new ArrayList<>();
        if (!response.hasServiceRecord() || response.getServiceRecord().getStatus(now) != MdnsRecord.STATUS_OK) {
            questions.add(new MdnsServiceRecord(name, false));
        }
        if (!response.hasTextRecord() || response.getTextRecord().getStatus(now) != MdnsRecord.STATUS_OK) {
            questions.add(new MdnsTextRecord(name, false));
        }
        if (response.hasServiceRecord()) {
            boolean fresh4 = false, fresh6 = false;
            for (MdnsInetAddressRecord record : response.getInet4AddressRecords()) {
                fresh4 |= record.getStatus(now) == MdnsRecord.STATUS_OK;
            }
            for (MdnsInetAddressRecord record : response.getInet6AddressRecords()) {
                fresh6 |= record.getStatus(now) == MdnsRecord.STATUS_OK;
            }
            String[] host = response.getServiceRecord().getServiceHost();
            if (!fresh4) questions.add(new MdnsInetAddressRecord(host, MdnsRecord.TYPE_A, false));
            if (!fresh6) questions.add(new MdnsInetAddressRecord(host, MdnsRecord.TYPE_AAAA, false));
        }
        questions.removeIf(question -> {
            Long previous = lastQuestions.get(question.getKey());
            return previous != null && now - previous < 1000;
        });
        if (questions.isEmpty()) {
            if (due) advanceSchedule(now);
            return null;
        }
        byte[] buffer = new byte[1500];
        MdnsPacketWriter writer = new MdnsPacketWriter(buffer);
        // This transport requests multicast replies. It does not advertise a
        // unicast receive path that its native packet qualification excludes.
        writer.writeUInt16(scheduled.config.getTransactionId());
        writer.writeUInt16(0);
        writer.writeUInt16(questions.size());
        writer.writeUInt16(0); writer.writeUInt16(0); writer.writeUInt16(0);
        for (MdnsRecord question : questions) question.writeHeaderFields(writer);
        return new Query(Arrays.copyOf(buffer, writer.getWritePosition()), questions,
                due ? scheduled : null);
    }

    public void sent(Query query, long now) {
        for (MdnsRecord question : query.questions) lastQuestions.put(question.getKey(), now);
        lastSent = now;
        if (query.scheduled == scheduled) advanceSchedule(now);
    }

    private void advanceSchedule(long now) {
        scheduled = scheduler.scheduleNextRun(scheduled.config, remaining(now), now,
                now, 1, options.getQueryMode(), options.numOfQueriesBeforeBackoff(), false);
    }

    private NsdServiceInfo result(long now) throws IOException {
        NsdServiceInfo value = new NsdServiceInfo(name[0], type);
        MdnsServiceRecord service = response.getServiceRecord();
        value.setPort(service.getServicePort());
        List<String> subtypes = response.getSubtypes();
        Map<String, String> subtypeLabels = new LinkedHashMap<>();
        if (subtypes != null) for (String subtype : subtypes) {
            subtypeLabels.put(DnsUtils.toDnsUpperCase(subtype), subtype);
        }
        value.setSubtypes(new LinkedHashSet<>(subtypeLabels.values()));
        value.setExpirationTime(Instant.now().plusMillis(response.getMinRemainingTtl(now)));
        String[] host = service.getServiceHost();
        if (host.length >= 2 && host[host.length - 1].equals("local")) {
            host = Arrays.copyOf(host, host.length - 1);
        }
        value.setHostname(String.join(".", host));
        Map<String, byte[]> attributes = new TreeMap<>(String.CASE_INSENSITIVE_ORDER);
        for (MdnsServiceInfo.TextEntry entry : response.getTextRecord().getEntries()) {
            attributes.putIfAbsent(entry.getKey(), entry.getValue());
        }
        for (Map.Entry<String, byte[]> entry : attributes.entrySet()) {
            try {
                value.setAttribute(entry.getKey(), entry.getValue() == null
                        ? (byte[]) null : entry.getValue().clone());
            } catch (IllegalArgumentException invalid) {
                // The real Android service skips an invalid individual TXT attribute.
            }
        }
        List<InetAddress> addresses = new ArrayList<>();
        for (MdnsInetAddressRecord record : response.getInet4AddressRecords()) {
            Inet4Address address = record.getInet4Address();
            if (address != null) addresses.add(address);
        }
        for (MdnsInetAddressRecord record : response.getInet6AddressRecords()) {
            Inet6Address address = record.getInet6Address();
            if (address == null) continue;
            if (address.isLinkLocalAddress()) {
                if (scope.ipv6Index == 0) continue;
                address = Inet6Address.getByAddress(null, address.getAddress(), scope.ipv6Index);
            }
            addresses.add(address);
        }
        value.setHostAddresses(addresses);
        if (scope.netId == 0) value.setInterfaceIndex(scope.index);
        else value.setNetwork(new Network(scope.netId));
        return value;
    }
}
