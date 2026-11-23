/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.net;

import java.net.InetAddress;
import java.net.UnknownHostException;
import java.util.concurrent.atomic.AtomicInteger;

/** Explicit host boundary for the synchronous Windows resolver, never shipped in the runtime. */
public final class WindowsNetworks {
    public interface Backend {
        InetAddress[] resolve(int network, String domain) throws UnknownHostException;
    }
    public static volatile Backend backend;
    public static final AtomicInteger calls = new AtomicInteger();
    public static final AtomicInteger active = new AtomicInteger();
    public static final AtomicInteger peak = new AtomicInteger();
    public static final AtomicInteger lastNetwork = new AtomicInteger();
    public static volatile String lastDomain;

    public static int processNetwork() { return 17; }

    public static InetAddress[] resolve(int network, String domain) throws UnknownHostException {
        calls.incrementAndGet();
        lastNetwork.set(network);
        lastDomain = domain;
        int count = active.incrementAndGet();
        peak.accumulateAndGet(count, Math::max);
        try {
            return backend.resolve(network, domain);
        } finally {
            active.decrementAndGet();
        }
    }
}
