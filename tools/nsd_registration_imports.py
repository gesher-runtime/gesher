#!/usr/bin/env python3
# Copyright (C) 2026 The Gesher Authors. SPDX-License-Identifier: Apache-2.0
"""Exact documented adaptations of the pinned Android16r4 advertiser record core.

Transforms supplied genuine source bytes; no downloads or runtime substitutes.
"""
from pathlib import Path


def block(text, marker):
    """Extract one complete source declaration, retaining its method bodies."""
    start = text.index(marker)
    at = text.index('{', start) + 1
    depth = 1
    while depth:
        if text[at] == '{':
            depth += 1
        elif text[at] == '}':
            depth -= 1
        at += 1
    return text[start:at]


def relocate(text):
    return text.replace('com.android.server.connectivity.mdns', 'com.gesher.net.mdns')


def sources(upstream: Path):
    raw = {name: (upstream / name).read_text() for name in (
        'MdnsRecordRepository.java', 'MdnsAdvertisingOptions.java', 'MdnsReplyInfo.java',
        'NameConflictException.java', 'MdnsProber.java', 'MdnsAnnouncer.java',
        'MdnsPacketRepeater.java', 'MdnsUtils.java')}
    out = {name: relocate(raw[name]) for name in (
        'MdnsAdvertisingOptions.java', 'MdnsReplyInfo.java', 'NameConflictException.java')}
    repository = relocate(raw['MdnsRecordRepository.java'])
    repository = repository.replace('import android.os.Looper;\n', '')
    repository = repository.replace('import com.gesher.net.mdns.util.MdnsUtils;\n', '')
    repository = repository.replace('    @NonNull\n    private final Looper mLooper;\n', '')
    repository = repository.replace('@NonNull Looper looper, ', '')
    repository = repository.replace('this(looper, new Dependencies(),', 'this(new Dependencies(),')
    repository = repository.replace('        mLooper = looper;\n', '')
    repository = repository.replace('Must be used on a consistent looper thread.',
                                    'Must be used on the advertiser owner thread.')
    repository = repository.replace('MdnsUtils.constructFullSubtype(serviceType, subtype)',
        'CollectionUtils.prependArray(String.class, serviceType, subtype,\n'
        '                                            MdnsConstants.SUBTYPE_LABEL)')
    for original in ('MdnsProber.', 'MdnsAnnouncer.'):
        repository = repository.replace(original, 'MdnsAdvertisingPacket.')
    repository = repository.replace('com.gesher.net.mdns.MdnsInterfaceAdvertiser.CONFLICT_',
                                    'com.gesher.net.mdns.MdnsAdvertisingPacket.CONFLICT_')
    repository = repository.replace('{@link MdnsInterfaceAdvertiser',
                                    '{@link com.gesher.net.WindowsNsdAdvertiser')
    repository = repository.replace('{@link MdnsAdvertiser}',
                                    '{@link com.gesher.net.WindowsNsdAdvertiser}')
    # Preserve the complete genuine LinkAddress method. Its record construction
    # uses address bytes/family and IFA flags, never a network prefix. Windows
    # can observe a real address with unknown OnLinkPrefixLength; requiring a
    # synthetic LinkAddress prefix would lose otherwise usable multicast data.
    original = block(repository, '    public void updateAddresses(')
    windows = original.replace(
        'updateAddresses(@NonNull List<LinkAddress> newAddresses)',
        'updateWindowsAddresses(@NonNull List<InetAddress> newAddresses,\n'
        '            @NonNull int[] addressFlags)')
    windows = windows.replace('        mGeneralRecords.clear();',
        '        if (newAddresses.size() != addressFlags.length) {\n'
        '            throw new IllegalArgumentException("Address flag count mismatch");\n'
        '        }\n        mGeneralRecords.clear();')
    windows = windows.replace('for (LinkAddress addr : newAddresses) {',
        'for (int index = 0; index < newAddresses.size(); index++) {\n'
        '            final InetAddress addr = newAddresses.get(index);')
    windows = windows.replace('addr.isIpv6()', '(addr instanceof java.net.Inet6Address)')
    windows = windows.replace('addr.getFlags()', 'addressFlags[index]')
    windows = windows.replace('addr.getAddress()', 'addr')
    comment = '''    /**
     * Windows address adapter: actual address values and supported IFA flags,
     * without requiring or synthesizing a prefix for multicast record creation.
     * The original LinkAddress method remains unchanged. Prefix-based routing
     * eligibility stays with the Windows scope/transport; suffix origins do not
     * become assertions of Android temporary-address flags.
     */
'''
    repository = repository.replace(original, original + '\n\n' + comment + windows)
    out['MdnsRecordRepository.java'] = repository

    # Complete pure request-value declarations; the Windows owner is the scheduler.
    declarations = [
        block(raw['MdnsPacketRepeater.java'], '    public interface Request {'),
        block(raw['MdnsProber.java'], '    public static class ProbingInfo implements Request {'),
        block(raw['MdnsAnnouncer.java'], '    public abstract static class BaseAnnouncementInfo'),
        block(raw['MdnsAnnouncer.java'], '    public static class AnnouncementInfo'),
        block(raw['MdnsAnnouncer.java'], '    public static class ExitAnnouncementInfo'),
        block(raw['MdnsUtils.java'], '    public static void writeMdnsPacket('),
        block(raw['MdnsUtils.java'], '    public static byte[] createRawDnsPacket('),
    ]
    header = raw['MdnsProber.java'][:raw['MdnsProber.java'].index('package ')]
    header = header.replace(' * Copyright (C) 2022 The Android Open Source Project',
        ' * Copyright (C) 2022 The Android Open Source Project\n * Copyright (C) 2023 The Android Open Source Project')
    packet = header + '''package com.gesher.net.mdns;

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

'''
    packet += '\n\n'.join(declarations).replace('MdnsPacketRepeater.Request', 'Request')
    out['MdnsAdvertisingPacket.java'] = packet + '\n}\n'
    return out
