/*
 * Copyright (C) 2022 The Android Open Source Project
 * Copyright (C) 2023 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

package com.gesher.net.mdns;

import android.annotation.NonNull;
import com.android.net.module.util.CollectionUtils;
import com.android.net.module.util.DnsUtils;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collections;
import java.util.List;
import java.io.IOException;

/** Pinned Android16r4 packet requests, scheduled by the Windows service owner. */
public final class MdnsAdvertisingPacket {
    private MdnsAdvertisingPacket() {}
    // Pinned interface conflict bitmap and announcer schedule.
    public static final int CONFLICT_SERVICE = 1;
    public static final int CONFLICT_HOST = 2;
    private static final long ANNOUNCEMENT_INITIAL_DELAY_MS = 1000L;
    private static final int ANNOUNCEMENT_COUNT = 8;
    private static final long EXIT_DELAY_MS = 2000L;
    private static final int EXIT_COUNT = 3;

    public interface Request {
        /**
         * Get a packet to send for one iteration.
         */
        @NonNull
        MdnsPacket getPacket(int index);

        /**
         * Get the delay in milliseconds until the next packet transmission.
         */
        long getDelayMs(int nextIndex);

        /**
         * Get the number of packets that should be sent.
         */
        int getNumSends();
    }

    public static class ProbingInfo implements Request {

        private final int mServiceId;
        @NonNull
        private final MdnsPacket mPacket;

        /**
         * Create a new ProbingInfo
         * @param serviceId Service to probe for.
         * @param probeRecords Records to be probed for uniqueness.
         */
        ProbingInfo(int serviceId, @NonNull List<MdnsRecord> probeRecords) {
            mServiceId = serviceId;
            mPacket = makePacket(probeRecords);
        }

        public int getServiceId() {
            return mServiceId;
        }

        @NonNull
        @Override
        public MdnsPacket getPacket(int index) {
            return mPacket;
        }

        @Override
        public long getDelayMs(int nextIndex) {
            // As per https://datatracker.ietf.org/doc/html/rfc6762#section-8.1
            return 250L;
        }

        @Override
        public int getNumSends() {
            // 3 packets as per https://datatracker.ietf.org/doc/html/rfc6762#section-8.1
            return 3;
        }

        private static MdnsPacket makePacket(@NonNull List<MdnsRecord> records) {
            final ArrayList<MdnsRecord> questions = new ArrayList<>(records.size());
            for (final MdnsRecord record : records) {
                if (containsName(questions, record.getName())) {
                    // Already added this name
                    continue;
                }

                // TODO: legacy Android mDNS used to send the first probe (only) as unicast, even
                //  though https://datatracker.ietf.org/doc/html/rfc6762#section-8.1 says they
                // SHOULD all be. rfc6762 15.1 says that if the port is shared with another
                // responder unicast questions should not be used, and the legacy mdnsresponder may
                // be running, so not using unicast at all may be better. Consider using legacy
                // behavior if this causes problems.
                questions.add(new MdnsAnyRecord(record.getName(), false /* unicast */));
            }

            return new MdnsPacket(
                    MdnsConstants.FLAGS_QUERY,
                    questions,
                    Collections.emptyList() /* answers */,
                    records /* authorityRecords */,
                    Collections.emptyList() /* additionalRecords */);
        }

        /**
         * Return whether the specified name is present in the list of records.
         */
        private static boolean containsName(@NonNull List<MdnsRecord> records,
                @NonNull String[] name) {
            return CollectionUtils.any(records,
                    r -> DnsUtils.equalsDnsLabelIgnoreDnsCase(name, r.getName()));
        }
    }

    public abstract static class BaseAnnouncementInfo implements Request {
        private final int mServiceId;
        @NonNull
        private final MdnsPacket mPacket;

        protected BaseAnnouncementInfo(int serviceId, @NonNull List<MdnsRecord> announcedRecords,
                @NonNull List<MdnsRecord> additionalRecords) {
            mServiceId = serviceId;
            final int flags = 0x8400; // Response, authoritative (rfc6762 18.4)
            mPacket = new MdnsPacket(flags,
                    Collections.emptyList() /* questions */,
                    announcedRecords,
                    Collections.emptyList() /* authorityRecords */,
                    additionalRecords);
        }

        public int getServiceId() {
            return mServiceId;
        }

        @Override
        public MdnsPacket getPacket(int index) {
            return mPacket;
        }
    }

    public static class AnnouncementInfo extends BaseAnnouncementInfo {
        AnnouncementInfo(int serviceId, List<MdnsRecord> announcedRecords,
                List<MdnsRecord> additionalRecords) {
            super(serviceId, announcedRecords, additionalRecords);
        }

        @Override
        public long getDelayMs(int nextIndex) {
            // Delay is doubled for each announcement
            return ANNOUNCEMENT_INITIAL_DELAY_MS << (nextIndex - 1);
        }

        @Override
        public int getNumSends() {
            return ANNOUNCEMENT_COUNT;
        }
    }

    public static class ExitAnnouncementInfo extends BaseAnnouncementInfo {
        ExitAnnouncementInfo(int serviceId, List<MdnsRecord> announcedRecords) {
            super(serviceId, announcedRecords, Collections.emptyList() /* additionalRecords */);
        }

        @Override
        public long getDelayMs(int nextIndex) {
            return EXIT_DELAY_MS;
        }

        @Override
        public int getNumSends() {
            return EXIT_COUNT;
        }
    }

    public static void writeMdnsPacket(@NonNull MdnsPacketWriter writer, @NonNull MdnsPacket packet)
            throws IOException {
        writer.writeUInt16(packet.transactionId); // Transaction ID (advertisement: 0)
        writer.writeUInt16(packet.flags); // Response, authoritative (rfc6762 18.4)
        writer.writeUInt16(packet.questions.size()); // questions count
        writer.writeUInt16(packet.answers.size()); // answers count
        writer.writeUInt16(packet.authorityRecords.size()); // authority entries count
        writer.writeUInt16(packet.additionalRecords.size()); // additional records count

        for (MdnsRecord record : packet.questions) {
            // Questions do not have TTL or data
            record.writeHeaderFields(writer);
        }
        for (MdnsRecord record : packet.answers) {
            record.write(writer, 0L);
        }
        for (MdnsRecord record : packet.authorityRecords) {
            record.write(writer, 0L);
        }
        for (MdnsRecord record : packet.additionalRecords) {
            record.write(writer, 0L);
        }
    }

    public static byte[] createRawDnsPacket(@NonNull byte[] packetCreationBuffer,
            @NonNull MdnsPacket packet) throws IOException {
        // TODO: support packets over size (send in multiple packets with TC bit set)
        final MdnsPacketWriter writer = new MdnsPacketWriter(packetCreationBuffer);
        writeMdnsPacket(writer, packet);

        final int len = writer.getWritePosition();
        return Arrays.copyOfRange(packetCreationBuffer, 0, len);
    }
}
