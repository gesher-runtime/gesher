#!/usr/bin/env python3
# Copyright (C) 2026 The Gesher Authors. SPDX-License-Identifier: Apache-2.0
"""Reproduce the coherent Android16r4 named-response/query closure.

All six complete source files retain their original bodies. The separate utility
adapter contains complete upstream declarations needed by that closure; it does
not replace an Android runtime provider. Supplied source files are never fetched
by this transform.
"""
from pathlib import Path

from nsd_registration_imports import block, relocate

IMPORTED = (
    'MdnsResponse.java', 'MdnsResponseDecoder.java', 'MdnsQueryScheduler.java',
    'QueryTaskConfig.java', 'MdnsConfigs.java', 'MdnsSearchOptions.java',
)


def sources(upstream: Path):
    result = {}
    for name in IMPORTED:
        text = relocate((upstream / name).read_text())
        text = text.replace('import com.gesher.net.mdns.util.MdnsUtils;\n', '')
        result[name] = text.replace('MdnsUtils.', 'MdnsSubscriptionUtils.')
    utility = (upstream / 'MdnsUtils.java').read_text()
    declarations = [block(utility, marker) for marker in (
        '    public static boolean typeEqualsOrIsSubtype(',
        '    public static <Type> Set<Type> newSet()',
        '    public static class Clock {',
    )]
    header = utility[:utility.index('package ')]
    result['MdnsSubscriptionUtils.java'] = header + '''package com.gesher.net.mdns;

import static com.android.net.module.util.DnsUtils.equalsDnsLabelIgnoreDnsCase;
import static com.android.net.module.util.DnsUtils.equalsIgnoreDnsCase;

import android.annotation.NonNull;
import android.os.Build;
import android.os.SystemClock;
import android.util.ArraySet;
import java.util.HashSet;
import java.util.Set;

/** Complete selected upstream utility declarations for the response/query core. */
public final class MdnsSubscriptionUtils {
    private MdnsSubscriptionUtils() {}

''' + '\n\n'.join(declarations) + '\n}\n'
    return result
