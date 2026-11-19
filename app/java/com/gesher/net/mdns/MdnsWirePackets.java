/*
 * Copyright (C) 2021 The Android Open Source Project
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.net.mdns;

import com.android.net.module.util.DnsUtils;
import java.io.EOFException;
import java.io.IOException;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collections;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.Objects;

/**
 * Windows datagram layout around the pinned Android record/writer bodies.
 * Response bursts, complete-owner probes and conventional legacy replies have
 * separate paths. This class never clips a record or reuses compression bytes.
 */
public final class MdnsWirePackets {
    private MdnsWirePackets() {}
    public static final int MAX_BURST_BYTES = 8 * 1024 * 1024;
    public static final int MAX_BURST_PACKETS = 1024;
    private static final int RESPONSE = 0x8400;
    private static final int TC = 0x0200;

    private static final class Key {
        final List<String> name;
        final int type, cls;
        Key(MdnsRecord record, boolean ownerOnly) {
            name = Arrays.asList(DnsUtils.toDnsLabelsUpperCase(record.getName()));
            type = ownerOnly ? 0 : record.getType();
            cls = ownerOnly ? 0 : record.getRecordClass();
        }
        @Override public boolean equals(Object other) {
            if (!(other instanceof Key)) return false;
            Key value = (Key) other;
            return type == value.type && cls == value.cls && name.equals(value.name);
        }
        @Override public int hashCode() { return Objects.hash(name, type, cls); }
    }

    public static boolean sameSet(MdnsRecord left, MdnsRecord right) {
        return new Key(left, false).equals(new Key(right, false));
    }

    private static int ceiling(int family) throws IOException {
        if (family != 4 && family != 6) throw new IOException("Unknown IP family");
        return 9000 - (family == 4 ? 20 : 40) - 8;
    }

    private static int budget(int mtu, int family) throws IOException {
        int headers = family == 4 ? 28 : 48;
        int result = Math.min(mtu - headers, ceiling(family));
        if (result < 12) throw new IOException("Missing or unusable actual interface MTU");
        return result;
    }

    private static byte[] encode(MdnsPacket packet, int capacity) throws IOException {
        return MdnsAdvertisingPacket.createRawDnsPacket(new byte[capacity], packet);
    }

    private static byte[] fits(MdnsPacket packet, int capacity) throws IOException {
        try { return encode(packet, capacity); }
        catch (EOFException full) { return null; }
    }

    private static final class Burst {
        final ArrayList<byte[]> packets = new ArrayList<>();
        int bytes;
        void add(byte[] packet) throws IOException {
            if (packets.size() == MAX_BURST_PACKETS
                    || bytes > MAX_BURST_BYTES - packet.length) {
                throw new IOException("NSD logical burst capacity exceeded");
            }
            packets.add(packet);
            bytes += packet.length;
        }
        byte[][] finish() { return packets.toArray(new byte[0][]); }
    }

    private static final class Item {
        final MdnsRecord record;
        final int section;
        Item(MdnsRecord record, int section) { this.record = record; this.section = section; }
    }

    private static MdnsPacket response(List<Item> items) {
        List<MdnsRecord> answers = new ArrayList<>(), extra = new ArrayList<>();
        for (Item item : items) (item.section == 1 ? answers : extra).add(item.record);
        return new MdnsPacket(RESPONSE, Collections.emptyList(), answers,
                Collections.emptyList(), extra);
    }

    /** Complete immutable source response, including all supplied additional RRs. */
    public static byte[][] responses(MdnsPacket packet, int mtu, int family) throws IOException {
        if (packet.transactionId != 0 || packet.flags != RESPONSE || !packet.questions.isEmpty()
                || !packet.authorityRecords.isEmpty()) {
            throw new IOException("Non-mDNS-response passed to response layout");
        }
        int limit = budget(mtu, family);
        byte[] whole = fits(packet, limit);
        if (whole != null) return new byte[][]{whole};
        Map<Key, List<Item>> sets = new LinkedHashMap<>();
        for (MdnsRecord record : packet.answers) {
            sets.computeIfAbsent(new Key(record, false), key -> new ArrayList<>()).add(new Item(record, 1));
        }
        for (MdnsRecord record : packet.additionalRecords) {
            if (packet.answers.contains(record)) continue;
            sets.computeIfAbsent(new Key(record, false), key -> new ArrayList<>()).add(new Item(record, 3));
        }
        Burst out = new Burst();
        List<Item> current = new ArrayList<>();
        for (List<Item> set : sets.values()) {
            List<Item> trial = new ArrayList<>(current); trial.addAll(set);
            if (fits(response(trial), limit) != null) { current = trial; continue; }
            if (!current.isEmpty()) { out.add(encode(response(current), limit)); current.clear(); }
            if (fits(response(set), limit) != null) { current.addAll(set); continue; }
            for (Item item : set) {
                trial = new ArrayList<>(current); trial.add(item);
                if (fits(response(trial), limit) != null) { current = trial; continue; }
                if (!current.isEmpty()) { out.add(encode(response(current), limit)); current.clear(); }
                byte[] single = fits(response(List.of(item)), limit);
                if (single == null) {
                    // RFC6762 17: one oversized RR alone, including when its
                    // source section was Additional. No other resource records.
                    out.add(encode(response(List.of(item)), ceiling(family)));
                } else current.add(item);
            }
        }
        if (!current.isEmpty()) out.add(encode(response(current), limit));
        return out.finish();
    }

    /** Never divides the proposed authority set for a single owner name. */
    public static byte[][] probes(MdnsPacket packet, int mtu, int family) throws IOException {
        if (packet.transactionId != 0 || packet.flags != 0 || !packet.answers.isEmpty()
                || !packet.additionalRecords.isEmpty()) {
            throw new IOException("Non-probe passed to probe layout");
        }
        int limit = budget(mtu, family);
        byte[] whole = fits(packet, limit);
        if (whole != null) return new byte[][]{whole};
        Map<Key, List<MdnsRecord>> names = new LinkedHashMap<>();
        for (MdnsRecord record : packet.authorityRecords) {
            names.computeIfAbsent(new Key(record, true), key -> new ArrayList<>()).add(record);
        }
        Burst out = new Burst();
        for (Map.Entry<Key, List<MdnsRecord>> entry : names.entrySet()) {
            List<MdnsRecord> questions = new ArrayList<>();
            for (MdnsRecord question : packet.questions) {
                if (entry.getKey().equals(new Key(question, true))) questions.add(question);
            }
            MdnsPacket group = new MdnsPacket(0, questions, Collections.emptyList(),
                    entry.getValue(), Collections.emptyList());
            byte[] encoded = fits(group, limit);
            if (encoded == null) {
                if (entry.getValue().size() != 1) {
                    throw new EOFException("Complete owner probe exceeds actual MTU");
                }
                encoded = encode(group, ceiling(family));
            }
            out.add(encoded);
        }
        return out.finish();
    }

    /** Strict greater-than-half is the pinned r4 boundary, including exact-half replies. */
    public static boolean known(MdnsRecord record, List<MdnsRecord> known) {
        for (MdnsRecord value : known) {
            if (record.equals(value) && value.getTtl() > record.getTtl() / 2) return true;
        }
        return false;
    }

    /**
     * Fill a selected unique set from current authoritative repository values.
     * Query-supplied RDATA never becomes authoritative. Entire-known sets can be
     * suppressed; a needed partial unique set is emitted together under RFC10.2.
     */
    public static MdnsPacket completeReply(List<MdnsRecord> answers, List<MdnsRecord> additional,
            List<MdnsRecord> known, List<MdnsRecord> authoritative) {
        ArrayList<MdnsRecord> completedAnswers = new ArrayList<>();
        ArrayList<MdnsRecord> completedAdditional = new ArrayList<>();
        complete(answers, completedAnswers, known, authoritative);
        complete(additional, completedAdditional, Collections.emptyList(), authoritative);
        completedAdditional.removeAll(completedAnswers);
        return new MdnsPacket(RESPONSE, Collections.emptyList(), completedAnswers,
                Collections.emptyList(), completedAdditional);
    }

    private static void complete(List<MdnsRecord> source, List<MdnsRecord> out,
            List<MdnsRecord> known, List<MdnsRecord> authoritative) {
        for (MdnsRecord selected : source) {
            if (!selected.getCacheFlush()) {
                if (!known(selected, known) && !out.contains(selected)) out.add(selected);
                continue;
            }
            List<MdnsRecord> set = new ArrayList<>();
            for (MdnsRecord record : authoritative) {
                if (record.getCacheFlush() && sameSet(record, selected) && !set.contains(record)) set.add(record);
            }
            // A generated negative NSEC can be absent from an announcement
            // catalog; it remains a genuine repository-owned singleton.
            if (set.isEmpty() && selected instanceof MdnsNsecRecord) set.add(selected);
            boolean entireKnown = !set.isEmpty();
            for (MdnsRecord record : set) entireKnown &= known(record, known);
            if (entireKnown) continue;
            for (MdnsRecord record : set) if (!out.contains(record)) out.add(record);
        }
    }

    private static final class Question extends MdnsRecord {
        Question(MdnsRecord source) {
            super(source.getName(), source.getType(), source.getRecordClass(), 0L, true, 0L);
        }
        @Override protected void readData(MdnsPacketReader reader) throws IOException {
            throw new IOException("Question has no RDATA");
        }
        @Override protected void writeData(MdnsPacketWriter writer) throws IOException {
            throw new IOException("Question has no RDATA");
        }
    }

    /** Conventional legacy lookup bypasses mDNS known-answer/rate-limit semantics. */
    public static MdnsPacket legacyLookup(MdnsPacket query) {
        List<MdnsRecord> questions = new ArrayList<>();
        for (MdnsRecord question : query.questions) questions.add(new Question(question));
        return new MdnsPacket(0, questions, Collections.emptyList(),
                Collections.emptyList(), Collections.emptyList());
    }

    private static List<List<MdnsRecord>> groups(List<MdnsRecord> records) {
        Map<Key, List<MdnsRecord>> out = new LinkedHashMap<>();
        for (MdnsRecord record : records) {
            List<MdnsRecord> group = out.computeIfAbsent(new Key(record, false), key -> new ArrayList<>());
            if (!group.contains(record)) group.add(record);
        }
        return new ArrayList<>(out.values());
    }

    /** RFC6762 6.7 / conventional 512-byte UDP profile; no EDNS/TCP claim. */
    public static byte[][] legacy(MdnsPacket query, MdnsPacket reply) throws IOException {
        List<MdnsRecord> answers = new ArrayList<>(), additional = new ArrayList<>();
        // A question section that itself cannot fit is an explicit capacity error.
        legacyEncode(query, answers, additional, false);
        boolean truncated = false;
        for (List<MdnsRecord> set : groups(reply.answers)) {
            List<MdnsRecord> trial = new ArrayList<>(answers); trial.addAll(set);
            try { legacyEncode(query, trial, additional, truncated); answers = trial; }
            catch (EOFException full) { truncated = true; }
        }
        for (List<MdnsRecord> set : groups(reply.additionalRecords)) {
            List<MdnsRecord> trial = new ArrayList<>(additional); trial.addAll(set);
            try { legacyEncode(query, answers, trial, truncated); additional = trial; }
            catch (EOFException optional) { /* whole optional RRSet omitted */ }
        }
        return new byte[][]{legacyEncode(query, answers, additional, truncated)};
    }

    private static byte[] legacyEncode(MdnsPacket query, List<MdnsRecord> answers,
            List<MdnsRecord> additional, boolean truncated) throws IOException {
        byte[] buffer = new byte[512];
        MdnsPacketWriter writer = new MdnsPacketWriter(buffer);
        writer.writeUInt16(query.transactionId);
        writer.writeUInt16(RESPONSE | (query.flags & 0x0100) | (truncated ? TC : 0));
        writer.writeUInt16(query.questions.size()); writer.writeUInt16(answers.size());
        writer.writeUInt16(0); writer.writeUInt16(additional.size());
        for (MdnsRecord question : query.questions) {
            writer.writeLabels(question.getName()); writer.writeUInt16(question.getType());
            writer.writeUInt16(question.getRecordClass());
        }
        for (MdnsRecord record : answers) legacyRecord(writer, record);
        for (MdnsRecord record : additional) legacyRecord(writer, record);
        return Arrays.copyOf(buffer, writer.getWritePosition());
    }

    private static void legacyRecord(MdnsPacketWriter writer, MdnsRecord record) throws IOException {
        writer.writeLabels(record.getName()); writer.writeUInt16(record.getType());
        writer.writeUInt16(record.getRecordClass()); // no cache-flush bit
        writer.writeUInt32(Math.min(10L, record.getTtl() / 1000L));
        int lengthAt = writer.getWritePosition(); writer.writeUInt16(0);
        int dataAt = writer.getWritePosition();
        record.writeData(writer); // genuine source RDATA body and fresh compression dictionary
        int end = writer.getWritePosition();
        writer.rewind(lengthAt); writer.writeUInt16(end - dataAt); writer.unrewind();
    }
}
