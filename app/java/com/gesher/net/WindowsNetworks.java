/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.net;

import java.net.InetAddress;
import java.util.ArrayList;
import java.util.Collections;
import java.util.List;

/**
 * Windows' networks, as Android's connectivity sees them (docs/network.md): the
 * adapters that are up and reach beyond their link, with their kind, addresses,
 * gateways, DNS servers, link speeds and what the Network List Manager says of them
 * (internet, metered, roaming). The natives are compat/net/windows_networks.cc
 * (the built-in library libgesher_net.so).
 *
 * <p>A snapshot is text, one fact per line, so it crosses binder as a String and
 * compares with equals: the connectivity service ({@code com.gesher.server.ConnectivityService})
 * takes one, and app processes parse it ({@link #parse}).
 */
public final class WindowsNetworks {
    private WindowsNetworks() {
    }

    private static volatile boolean sLoaded;

    private static void load() {
        if (!sLoaded) {
            synchronized (WindowsNetworks.class) {
                if (!sLoaded) {
                    System.loadLibrary("gesher_net");
                    sLoaded = true;
                }
            }
        }
    }

    private static native String nativeSnapshot();

    private static native boolean nativeWaitForChange(int timeoutMs);

    private static native void nativeSetInetAllowed(boolean allowed);

    private static native boolean nativeInetAllowed();

    /** Windows' networks now, as text ({@link #parse} reads it). */
    public static String snapshot() {
        load();
        return nativeSnapshot();
    }

    /**
     * Waits until Windows reports a change of an interface, an address or a route, or
     * {@code timeoutMs} passes; true for a change.
     */
    public static boolean waitForChange(int timeoutMs) {
        load();
        return nativeWaitForChange(timeoutMs);
    }

    /**
     * Android's INTERNET permission for this process: without it, AF_INET and AF_INET6
     * sockets and name lookups fail with EACCES, as for an app without the inet group
     * (gesher-base's {@code <gesher/inet.h>}).
     */
    public static void setInternetAllowed(boolean allowed) {
        load();
        nativeSetInetAllowed(allowed);
    }

    public static boolean internetAllowed() {
        load();
        return nativeInetAllowed();
    }

    /** One network of a snapshot. */
    public static final class Net {
        public int ifIndex;
        /** wifi, ethernet, cellular, vpn or other. */
        public String kind;
        public int mtu;
        public long receiveBps;
        public long transmitBps;
        public boolean metered;
        public boolean roaming;
        public boolean congested;
        /** The Network List Manager reaches the internet through it. */
        public boolean internet;
        /** Windows routes the internet through it. */
        public boolean isDefault;
        public String friendlyName = "";
        public String description = "";
        public final List<InetAddress> addresses = new ArrayList<>();
        public final List<Integer> prefixLengths = new ArrayList<>();
        public final List<InetAddress> gateways = new ArrayList<>();
        public final List<InetAddress> dnsServers = new ArrayList<>();
        public String dnsSuffix;

        /** Android's netId for it: 100 and up, as netd numbers networks. */
        public int netId() {
            return 100 + ifIndex;
        }
    }

    /** A parsed snapshot. */
    public static final class Snapshot {
        public final String text;
        public final List<Net> networks;
        /** The Network List Manager says this machine reaches the internet. */
        public final boolean internet;

        Snapshot(String text, List<Net> networks, boolean internet) {
            this.text = text;
            this.networks = Collections.unmodifiableList(networks);
            this.internet = internet;
        }

        /** The network Windows routes the internet through (a VPN's, when one is up). */
        public Net defaultNetwork() {
            Net best = null;
            for (Net n : networks) {
                if (!n.isDefault) continue;
                if (best == null || (n.kind.equals("vpn") && !best.kind.equals("vpn"))) best = n;
            }
            return best;
        }

        public Net byNetId(int netId) {
            for (Net n : networks) {
                if (n.netId() == netId) return n;
            }
            return null;
        }
    }

    /** Parses {@link #snapshot}'s text. */
    public static Snapshot parse(String text) {
        List<Net> nets = new ArrayList<>();
        boolean internet = false;
        for (String line : text.split("\n")) {
            String[] f = line.split("\t", -1);
            try {
                switch (f[0]) {
                    case "internet":
                        internet = f[1].equals("1");
                        break;
                    case "net": {
                        Net n = new Net();
                        n.ifIndex = Integer.parseInt(f[1]);
                        n.kind = f[2];
                        n.mtu = Integer.parseInt(f[3]);
                        n.receiveBps = Long.parseLong(f[4]);
                        n.transmitBps = Long.parseLong(f[5]);
                        n.metered = f[6].equals("1");
                        n.roaming = f[7].equals("1");
                        n.congested = f[8].equals("1");
                        n.internet = f[9].equals("1");
                        n.isDefault = f[10].equals("1");
                        n.friendlyName = f.length > 11 ? f[11] : "";
                        n.description = f.length > 12 ? f[12] : "";
                        nets.add(n);
                        break;
                    }
                    case "addr": {
                        Net n = find(nets, f[1]);
                        if (n != null) {
                            n.addresses.add(InetAddress.getByName(f[2]));
                            n.prefixLengths.add(Integer.parseInt(f[3]));
                        }
                        break;
                    }
                    case "gw": {
                        Net n = find(nets, f[1]);
                        if (n != null) n.gateways.add(InetAddress.getByName(f[2]));
                        break;
                    }
                    case "dns": {
                        Net n = find(nets, f[1]);
                        if (n != null) n.dnsServers.add(InetAddress.getByName(f[2]));
                        break;
                    }
                    case "suffix": {
                        Net n = find(nets, f[1]);
                        if (n != null) n.dnsSuffix = f[2];
                        break;
                    }
                    default:
                        break;
                }
            } catch (RuntimeException | java.net.UnknownHostException e) {
                // A line this version does not read.
            }
        }
        return new Snapshot(text, nets, internet);
    }

    private static Net find(List<Net> nets, String ifIndex) {
        int index = Integer.parseInt(ifIndex);
        for (Net n : nets) {
            if (n.ifIndex == index) return n;
        }
        return null;
    }
}
