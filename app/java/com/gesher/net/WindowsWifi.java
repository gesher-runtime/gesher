/* Copyright (C) 2026 The Gesher Authors. SPDX-License-Identifier: Apache-2.0 */
package com.gesher.net;

import android.net.DhcpInfo;
import android.net.wifi.ScanResult;
import android.net.wifi.SupplicantState;
import android.net.wifi.WifiConfiguration;
import android.net.wifi.WifiInfo;
import android.net.wifi.WifiManager;
import android.net.wifi.WifiSsid;
import android.os.SystemClock;
import java.net.Inet4Address;
import java.net.InetAddress;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.concurrent.Executors;
import java.util.concurrent.ScheduledExecutorService;
import java.util.concurrent.ScheduledFuture;
import java.util.concurrent.TimeUnit;
import java.util.function.Consumer;
import java.util.function.BooleanSupplier;

/**
 * Internal WLAN backend for the authenticated Wi-Fi service, not an Android application API.
 * Native calls run on one daemon; service reads never wait for RPC.
 */
public final class WindowsWifi {
    private static final long CACHE_NS = TimeUnit.SECONDS.toNanos(2);
    private static final long MAX_STATE_NS = TimeUnit.SECONDS.toNanos(10);
    private static final int MAX_CALLBACKS = 32;
    private static final Object LOCK = new Object();
    private static final ScheduledExecutorService DEADLINES = Executors.newSingleThreadScheduledExecutor(r -> {
        Thread thread = new Thread(r, "gesher-wifi-deadline");
        thread.setDaemon(true);
        return thread;
    });
    private static Data sData;
    private static long sReadNanos;
    private static long sGeneration;
    private static Operation sOperation;
    private static boolean sLoaded;
    private WindowsWifi() { }

    // The built-in library uses WlanQueryInterface/WlanGetNetworkBssList. A requested
    // scan uses WlanScan and waits for its completion notification, never netsh output.
    private static native String nativeQuery(boolean scan, BooleanSupplier eligible);

    private static final class Operation {
        final boolean scan;
        final long generation;
        final List<Consumer<Boolean>> callbacks = new ArrayList<>();
        final List<BooleanSupplier> eligible = new ArrayList<>();
        boolean expired;
        ScheduledFuture<?> deadline;
        Operation(boolean scan) { this.scan = scan; generation = sGeneration; }
    }

    static final class State {
        int index;
        int radio; // -1 unknown, 0 off, 1 on
        boolean connected;
        byte[] ssid;
        String bssid;
        int frequency;
        int rssi;
        int rxKbps;
        int txKbps;
        int phy;
        int auth;
    }

    static final class Data {
        final Map<Integer, State> states = new LinkedHashMap<>();
        final List<ScanResult> scans = new ArrayList<>();
        boolean scansValid;
        boolean scansDenied;
        boolean unmappedRadioEnabled;
        boolean unmappedRadioUnknown;
        WindowsNetworks.Snapshot networks;
    }

    /** Invalidates results and rejects publication by any earlier in-flight query. */
    public static void invalidateCache() {
        synchronized (LOCK) {
            sGeneration++;
            sData = null;
            sReadNanos = 0;
        }
    }

    public static int wifiState() {
        Data data = current();
        if (data == null) return WifiManager.WIFI_STATE_UNKNOWN;
        if (data.unmappedRadioEnabled) return WifiManager.WIFI_STATE_ENABLED;
        boolean unknown = data.unmappedRadioUnknown;
        for (State state : data.states.values()) {
            if (state.radio == 1) return WifiManager.WIFI_STATE_ENABLED;
            unknown |= state.radio < 0;
        }
        return unknown ? WifiManager.WIFI_STATE_UNKNOWN : WifiManager.WIFI_STATE_DISABLED;
    }

    public static boolean isWifiEnabled() { return wifiState() == WifiManager.WIFI_STATE_ENABLED; }

    /** True means an asynchronous scan request was accepted; its broadcast conveys the outcome. */
    public static boolean startScan(Consumer<Boolean> finished) {
        return startScan(() -> true, finished);
    }

    public static boolean startScan(BooleanSupplier eligible, Consumer<Boolean> finished) {
        if (eligible == null) throw new NullPointerException("eligible");
        if (!isWifiEnabled()) return false;
        synchronized (LOCK) {
            if (sOperation != null) {
                if (!sOperation.scan || sOperation.expired || sOperation.generation != sGeneration
                        || sOperation.eligible.size() >= MAX_CALLBACKS) return false;
                sOperation.eligible.add(eligible);
                if (finished != null) sOperation.callbacks.add(finished);
                return true;
            }
            Operation operation = new Operation(true);
            operation.eligible.add(eligible);
            if (finished != null) operation.callbacks.add(finished);
            launch(operation);
            return true;
        }
    }

    private static Data current() {
        synchronized (LOCK) {
            long age = System.nanoTime() - sReadNanos;
            if (sOperation == null && (sReadNanos == 0 || age >= CACHE_NS)) launch(new Operation(false));
            return sData != null && age < MAX_STATE_NS ? sData : null;
        }
    }

    // Called with LOCK. At most one native query exists, even if a driver/RPC hangs.
    private static void launch(Operation operation) {
        sOperation = operation;
        Thread worker = new Thread(() -> refresh(operation), "gesher-wifi");
        worker.setDaemon(true);
        try {
            if (operation.scan) operation.deadline = DEADLINES.schedule(() -> expire(operation), 15, TimeUnit.SECONDS);
            worker.start();
        } catch (RuntimeException | Error e) {
            sOperation = null;
            if (operation.deadline != null) operation.deadline.cancel(false);
            throw e;
        }
    }

    private static void expire(Operation operation) {
        List<Consumer<Boolean>> callbacks;
        synchronized (LOCK) {
            if (sOperation != operation || operation.expired) return;
            operation.expired = true;
            callbacks = drain(operation);
        }
        notifyFinished(callbacks, false);
    }

    private static void refresh(Operation operation) {
        Data result = null;
        try {
            if (!sLoaded) {
                System.loadLibrary("gesher_wifi");
                sLoaded = true;
            }
            String raw = nativeQuery(operation.scan, () -> eligible(operation));
            result = parse(raw, elapsedMicros());
            if (result != null) {
                try { result.networks = WindowsNetworks.parse(WindowsNetworks.snapshot()); }
                catch (RuntimeException | LinkageError ignored) { }
            }
        } catch (RuntimeException | LinkageError ignored) {
            // Unavailable WLAN or Windows location denial remains unavailable.
        } finally {
            List<Consumer<Boolean>> callbacks;
            boolean updated = false;
            synchronized (LOCK) {
                if (operation.deadline != null) operation.deadline.cancel(false);
                if (!operation.expired && operation.generation == sGeneration) {
                    if (result != null) {
                        if (!result.scansValid && !result.scansDenied && sData != null) result.scans.addAll(sData.scans);
                        sData = result;
                        sReadNanos = System.nanoTime();
                        updated = result.scansValid;
                    } else {
                        sData = null;
                        // A short retry interval also applies to failed queries.
                        sReadNanos = System.nanoTime();
                    }
                }
                callbacks = drain(operation);
                if (sOperation == operation) sOperation = null;
            }
            notifyFinished(callbacks, updated);
        }
    }

    private static boolean eligible(Operation operation) {
        List<BooleanSupplier> candidates;
        synchronized (LOCK) {
            if (operation.expired || operation.generation != sGeneration || sOperation != operation) return false;
            candidates = new ArrayList<>(operation.eligible);
        }
        for (BooleanSupplier candidate : candidates) {
            try { if (candidate.getAsBoolean()) return true; }
            catch (RuntimeException ignored) { }
        }
        return false;
    }

    private static List<Consumer<Boolean>> drain(Operation operation) {
        List<Consumer<Boolean>> result = new ArrayList<>(operation.callbacks);
        operation.callbacks.clear();
        operation.eligible.clear();
        return result;
    }

    private static void notifyFinished(List<Consumer<Boolean>> callbacks, boolean updated) {
        for (Consumer<Boolean> callback : callbacks) {
            try { callback.accept(updated); }
            catch (RuntimeException ignored) { }
        }
    }

    public static WifiInfo connectionInfo() {
        Data data = current();
        State state = choose(data);
        return info(state, state == null ? null : network(data, state.index));
    }

    /** Service-side exact interface lookup; missing interfaces never use a default. */
    public static WifiInfo connectionInfo(int interfaceIndex) {
        Data data = current();
        State state = data == null ? null : data.states.get(interfaceIndex);
        return info(state, network(data, interfaceIndex));
    }

    /** Never substitutes a different adapter for the Network the app asked about. */
    public static WifiInfo connectionInfo(WindowsNetworks.Net network) {
        Data data = current();
        State state = data == null || network == null || !"wifi".equals(network.kind)
                ? null : data.states.get(network.ifIndex);
        return info(state, network);
    }

    public static WifiInfo connectionInfo(WindowsNetworks.Snapshot snapshot) {
        if (snapshot == null) return connectionInfo();
        WindowsNetworks.Net selected = snapshot.defaultNetwork();
        if (selected != null && "wifi".equals(selected.kind)) return connectionInfo(selected);
        for (WindowsNetworks.Net network : snapshot.networks) {
            if ("wifi".equals(network.kind)) return connectionInfo(network);
        }
        return new WifiInfo();
    }

    private static State choose(Data data) {
        if (data == null) return null;
        WindowsNetworks.Net preferred = data.networks == null ? null : data.networks.defaultNetwork();
        if (preferred != null) {
            State state = data.states.get(preferred.ifIndex);
            if (state != null && state.connected) return state;
        }
        for (State state : data.states.values()) if (state.connected) return state;
        for (State state : data.states.values()) if (state.radio == 1) return state;
        return data.states.isEmpty() ? null : data.states.values().iterator().next();
    }

    private static WindowsNetworks.Net network(Data data, int index) {
        if (data == null || data.networks == null) return null;
        WindowsNetworks.Net network = data.networks.byNetId(100 + index);
        return network != null && "wifi".equals(network.kind) ? network : null;
    }

    private static WifiInfo info(State state, WindowsNetworks.Net network) {
        WifiInfo info = new WifiInfo();
        if (state == null || !state.connected) {
            info.setSupplicantState(state != null && state.radio == 1
                    ? SupplicantState.DISCONNECTED : SupplicantState.UNINITIALIZED);
            return info;
        }
        info.setSupplicantState(SupplicantState.COMPLETED);
        info.setSSID(WifiSsid.fromBytes(state.ssid));
        info.setBSSID(state.bssid);
        info.setMacAddress(WifiInfo.DEFAULT_MAC_ADDRESS);
        info.setRssi(state.rssi);
        info.setFrequency(state.frequency);
        info.setLinkSpeed(state.txKbps > 0 ? state.txKbps / 1000 : WifiInfo.LINK_SPEED_UNKNOWN);
        info.setTxLinkSpeedMbps(state.txKbps > 0 ? state.txKbps / 1000 : WifiInfo.LINK_SPEED_UNKNOWN);
        info.setRxLinkSpeedMbps(state.rxKbps > 0 ? state.rxKbps / 1000 : WifiInfo.LINK_SPEED_UNKNOWN);
        info.setCurrentSecurityType(securityType(state.auth));
        info.setWifiStandard(standard(state.phy));
        // No Android-owned saved profile exists: keep networkId == -1.
        if (network != null && network.ifIndex == state.index) {
            for (InetAddress address : network.addresses) {
                if (address instanceof Inet4Address) { info.setInetAddress(address); break; }
            }
        }
        return info;
    }

    public static DhcpInfo dhcpInfo() {
        Data data = current();
        State state = choose(data);
        WindowsNetworks.Net network = state == null || !state.connected ? null : network(data, state.index);
        DhcpInfo info = new DhcpInfo();
        if (network == null) return info;
        for (int i = 0; i < network.addresses.size(); i++) {
            InetAddress address = network.addresses.get(i);
            if (!(address instanceof Inet4Address)) continue;
            info.ipAddress = ipv4(address);
            int prefix = i < network.prefixLengths.size() ? network.prefixLengths.get(i) : 0;
            if (prefix >= 0 && prefix <= 32)
                info.netmask = Integer.reverseBytes(prefix == 0 ? 0 : -1 << (32 - prefix));
            break;
        }
        for (InetAddress gateway : network.gateways) {
            if (gateway instanceof Inet4Address) { info.gateway = ipv4(gateway); break; }
        }
        int count = 0;
        for (InetAddress dns : network.dnsServers) {
            if (!(dns instanceof Inet4Address)) continue;
            if (count++ == 0) info.dns1 = ipv4(dns);
            else { info.dns2 = ipv4(dns); break; }
        }
        // The route is not evidence of the DHCP server or lease duration.
        return info;
    }

    public static List<WifiConfiguration> configuredNetworks() { return new ArrayList<>(); }

    public static List<ScanResult> scanResults() {
        Data data = current();
        List<ScanResult> result = new ArrayList<>();
        if (data != null) for (ScanResult scan : data.scans) result.add(new ScanResult(scan));
        return result;
    }

    static Data parse(String raw, long nowUs) {
        if (raw == null || raw.length() > 4 * 1024 * 1024) return null;
        String[] lines = raw.split("\n");
        if (lines.length == 0 || !(lines[0].equals("ok\t0") || lines[0].equals("ok\t1") || lines[0].equals("ok\t2"))) return null;
        Data data = new Data();
        data.scansValid = lines[0].equals("ok\t1");
        data.scansDenied = lines[0].equals("ok\t2");
        try {
            for (int i = 1; i < lines.length; i++) {
                String[] values = lines[i].split("\t", -1);
                if (values[0].equals("radio") && values.length == 2) {
                    int radio = integer(values[1], -1, 1);
                    data.unmappedRadioEnabled |= radio == 1;
                    data.unmappedRadioUnknown |= radio == -1;
                } else if (values[0].equals("if") && values.length == 13) {
                    State state = new State();
                    state.index = integer(values[1], 1, Integer.MAX_VALUE - 100);
                    state.radio = integer(values[2], -1, 1);
                    state.connected = integer(values[3], 0, 1) == 1;
                    state.ssid = hex(values[4], 32);
                    state.bssid = mac(values[5], !state.connected);
                    state.frequency = integer(values[6], -1, 100000);
                    state.rssi = integer(values[7], -127, 0);
                    state.rxKbps = integer(values[8], 0, Integer.MAX_VALUE);
                    state.txKbps = integer(values[9], 0, Integer.MAX_VALUE);
                    state.phy = integer(values[10], 0, Integer.MAX_VALUE);
                    state.auth = integer(values[11], 0, Integer.MAX_VALUE);
                    // Reserved wire version is explicit; unknown records must not become valid state.
                    if (!values[12].equals("1") || data.states.put(state.index, state) != null) return null;
                } else if (values[0].equals("bss") && values.length == 9 && data.scansValid) {
                    ScanResult scan = new ScanResult();
                    scan.setWifiSsid(WifiSsid.fromBytes(hex(values[1], 32)));
                    scan.BSSID = mac(values[2], false);
                    scan.frequency = integer(values[3], 1, 100000);
                    scan.level = integer(values[4], -127, 0);
                    long observedUs = Long.parseLong(values[5]);
                    if (observedUs < 0 || observedUs > nowUs) continue;
                    // Native converts FILETIME using the same elapsed clock. A
                    // delayed later RPC must never make this observation newer.
                    scan.timestamp = observedUs;
                    scan.setWifiStandard(standard(integer(values[6], 0, Integer.MAX_VALUE)));
                    int flags = integer(values[7], 0, 65535);
                    byte[] ies = hex(values[8], 65535);
                    scan.capabilities = capabilities(ies, flags);
                    data.scans.add(scan);
                } else if (!lines[i].isEmpty()) return null;
            }
        } catch (IllegalArgumentException e) { return null; }
        return data;
    }

    private static int integer(String value, int low, int high) {
        int parsed = Integer.parseInt(value);
        if (parsed < low || parsed > high) throw new IllegalArgumentException("Invalid WLAN value");
        return parsed;
    }
    private static byte[] hex(String value, int maxBytes) {
        if (value.length() % 2 != 0 || value.length() / 2 > maxBytes) throw new IllegalArgumentException("WLAN bytes");
        byte[] result = new byte[value.length() / 2];
        for (int i = 0; i < result.length; i++) {
            int high = Character.digit(value.charAt(2 * i), 16), low = Character.digit(value.charAt(2 * i + 1), 16);
            if (high < 0 || low < 0) throw new IllegalArgumentException("WLAN hex");
            result[i] = (byte) (16 * high + low);
        }
        return result;
    }
    private static String mac(String value, boolean emptyAllowed) {
        if (emptyAllowed && value.isEmpty()) return null;
        if (!value.matches("[0-9a-f]{2}(:[0-9a-f]{2}){5}")) throw new IllegalArgumentException("WLAN BSSID");
        return value;
    }
    private static int ipv4(InetAddress address) {
        byte[] b = address.getAddress();
        return (b[0] & 255) | ((b[1] & 255) << 8) | ((b[2] & 255) << 16) | ((b[3] & 255) << 24);
    }
    private static long elapsedMicros() {
        return SystemClock.elapsedRealtimeNanos() / 1000;
    }
    static int standard(int phy) {
        switch (phy) {
            case 1: case 2: case 4: case 5: case 6: return ScanResult.WIFI_STANDARD_LEGACY;
            case 7: return ScanResult.WIFI_STANDARD_11N;
            case 8: return ScanResult.WIFI_STANDARD_11AC;
            case 10: return ScanResult.WIFI_STANDARD_11AX;
            case 11: return ScanResult.WIFI_STANDARD_11BE;
            default: return ScanResult.WIFI_STANDARD_UNKNOWN;
        }
    }
    static int securityType(int auth) {
        switch (auth) {
            case 1: return WifiInfo.SECURITY_TYPE_OPEN;
            case 2: return WifiInfo.SECURITY_TYPE_WEP;
            case 3: case 6: return WifiInfo.SECURITY_TYPE_EAP;
            case 4: case 7: return WifiInfo.SECURITY_TYPE_PSK;
            case 8: return WifiInfo.SECURITY_TYPE_EAP_WPA3_ENTERPRISE_192_BIT;
            case 9: return WifiInfo.SECURITY_TYPE_SAE;
            case 10: return WifiInfo.SECURITY_TYPE_OWE;
            case 11: return WifiInfo.SECURITY_TYPE_EAP_WPA3_ENTERPRISE;
            default: return WifiInfo.SECURITY_TYPE_UNKNOWN;
        }
    }

    // Decode only capabilities actually present in the AP's RSN/WPA information elements.
    // Unknown/malformed protected suites keep '?' so ScanResult will not classify them as open.
    static String capabilities(byte[] ies, int flags) {
        StringBuilder out = new StringBuilder();
        boolean security = false;
        int offset = 0;
        for (; offset + 2 <= ies.length;) {
            int id = ies[offset++] & 255, count = ies[offset++] & 255;
            if (count > ies.length - offset) { out.append("[?]"); security = true; break; }
            boolean wpa = id == 221 && count >= 4 && ies[offset] == 0 && ies[offset + 1] == 0x50
                    && (ies[offset + 2] & 255) == 0xf2 && ies[offset + 3] == 1;
            if (id == 68) { out.append("[?]"); security = true; } // WAPI is not WEP.
            if (id == 48 || wpa) {
                security = true;
                try { out.append(securityElement(Arrays.copyOfRange(ies, offset + (wpa ? 4 : 0), offset + count), wpa)); }
                catch (IllegalArgumentException e) { out.append("[?]"); }
            }
            offset += count;
        }
        if (offset != ies.length) { out.append("[?]"); security = true; }
        if (!security && (flags & 16) != 0) out.append("[WEP]");
        if ((flags & 1) != 0) out.append("[ESS]");
        if ((flags & 2) != 0) out.append("[IBSS]");
        return out.toString();
    }

    private static final class Reader {
        final byte[] bytes;
        int offset;
        Reader(byte[] bytes) { this.bytes = bytes; }
        int word() {
            if (offset + 2 > bytes.length) throw new IllegalArgumentException("Truncated RSN");
            return (bytes[offset++] & 255) | ((bytes[offset++] & 255) << 8);
        }
        int suite(boolean wpa) {
            if (offset + 4 > bytes.length) throw new IllegalArgumentException("Truncated suite");
            int a = bytes[offset++] & 255, b = bytes[offset++] & 255, c = bytes[offset++] & 255;
            int type = bytes[offset++] & 255;
            return a == 0 && b == (wpa ? 0x50 : 0x0f) && c == (wpa ? 0xf2 : 0xac) ? type : -1;
        }
    }
    private static String securityElement(byte[] bytes, boolean wpa) {
        Reader reader = new Reader(bytes);
        if (reader.word() != 1) throw new IllegalArgumentException("Unknown RSN version");
        reader.suite(wpa); // Group cipher; Android's capabilities describe pairwise ciphers.
        int pairwise = reader.word();
        if (pairwise < 1 || pairwise > 64) throw new IllegalArgumentException("Cipher count");
        List<String> ciphers = new ArrayList<>();
        for (int i = 0; i < pairwise; i++) {
            int suite = reader.suite(wpa);
            String cipher = suite == 2 ? "TKIP" : suite == 4 ? "CCMP" : suite == 8 ? "GCMP-128"
                    : suite == 9 ? "GCMP-256" : suite == 10 ? "CCMP-256" : "?";
            if (!ciphers.contains(cipher)) ciphers.add(cipher);
        }
        int akms = reader.word();
        if (akms < 1 || akms > 64) throw new IllegalArgumentException("AKM count");
        List<String> auth = new ArrayList<>();
        for (int i = 0; i < akms; i++) {
            int suite = reader.suite(wpa);
            if (wpa && suite != 1 && suite != 2) suite = -1;
            String type;
            switch (suite) {
                case 1: type = wpa ? "EAP" : "EAP/SHA1"; break;
                case 2: type = "PSK"; break;
                case 3: type = "FT/EAP"; break;
                case 4: type = "FT/PSK"; break;
                case 5: type = "EAP/SHA256"; break;
                case 6: type = "PSK/SHA256"; break;
                case 8: case 9: type = "SAE"; break;
                case 12: type = "EAP_SUITE_B_192"; break;
                case 14: type = "FILS-SHA256"; break;
                case 15: type = "FILS-SHA384"; break;
                case 18: type = "OWE"; break;
                default: type = "?"; break;
            }
            if (!auth.contains(type)) auth.add(type);
        }
        String result = "[" + (wpa ? "WPA" : "RSN") + "-" + String.join("+", auth)
                + "-" + String.join("+", ciphers) + "]";
        if (!wpa && reader.offset + 2 <= bytes.length) {
            int capabilities = reader.word();
            if ((capabilities & 64) != 0) result += "[MFPR]";
            if ((capabilities & 128) != 0) result += "[MFPC]";
        }
        return result;
    }
}
