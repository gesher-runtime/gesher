/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.net;

import android.net.DhcpInfo;
import android.net.wifi.ScanResult;
import android.net.wifi.SupplicantState;
import android.net.wifi.WifiConfiguration;
import android.net.wifi.WifiInfo;
import android.net.wifi.WifiManager;
import android.net.wifi.WifiSsid;
import android.os.SystemClock;

import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.net.Inet4Address;
import java.net.InetAddress;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.List;
import java.util.Locale;
import java.util.concurrent.ExecutionException;
import java.util.concurrent.FutureTask;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.TimeoutException;
import java.util.function.Consumer;

/**
 * Windows' Wi-Fi adapter and scan state exposed to Android's {@link WifiManager},
 * {@link WifiInfo}, {@link DhcpInfo}, and {@link ScanResult} using {@code netsh.exe wlan}
 * merged with IP configuration from {@link WindowsNetworks}.
 */
public final class WindowsWifi {
    private static final long INTERFACE_CACHE_NS = TimeUnit.MILLISECONDS.toNanos(2000L);
    private static final long SCAN_CACHE_NS = TimeUnit.MILLISECONDS.toNanos(3000L);

    private static final Object LOCK = new Object();
    private static InterfaceState sCachedState;
    private static long sCachedStateNanos;
    private static boolean sHasCachedState;
    private static List<ScanResult> sCachedScans;
    private static long sCachedScansNanos;
    private static boolean sHasCachedScans;
    private static boolean sScanRunning;
    private static Consumer<Boolean> sScanFinished;

    private WindowsWifi() {
    }

    /** Parsed state of Windows' active (or first) Wi-Fi interface and IPv4 settings. */
    public static final class InterfaceState {
        public boolean hasInterface;
        public boolean enabled;
        public boolean connected;
        public String ssid = "";
        public String bssid;
        public String macAddress;
        public String band = "";
        public int channel;
        public int frequencyMhz = -1;
        public int rssi = WifiInfo.INVALID_RSSI;
        public int rxMbps;
        public int txMbps;
        public int securityType = WifiInfo.SECURITY_TYPE_UNKNOWN;
        public int wifiStandard;
        public InetAddress inetAddress;
        public int ipAddress;
        public int gateway;
        public int netmask;
        public int dns1;
        public int dns2;
        public int serverAddress;
    }

    /** Removes cached interface and scan state so the next query refreshes from Windows. */
    public static void invalidateCache() {
        synchronized (LOCK) {
            sHasCachedState = false;
            sCachedState = null;
            sHasCachedScans = false;
            sCachedScans = null;
        }
    }

    /** Queues a refresh of Windows' Wi-Fi results without blocking the caller. */
    public static boolean startScan(Consumer<Boolean> finished) {
        if (!isWifiEnabled()) {
            return false;
        }
        synchronized (LOCK) {
            if (finished != null) sScanFinished = finished;
            if (!sScanRunning) {
                sScanRunning = true;
                Thread scan = new Thread(WindowsWifi::refreshScan, "gesher-wifi-scan");
                scan.setDaemon(true);
                scan.start();
            }
        }
        return true;
    }

    private static void refreshScan() {
        boolean updated = false;
        try {
            // Windows can spend several seconds filling its visible BSS list.
            String output = runNetsh(15, "wlan", "show", "networks", "mode=bssid");
            if (!output.isEmpty()) {
                List<ScanResult> results = parseNetworksOutput(output);
                synchronized (LOCK) {
                    sCachedScans = results;
                    sCachedScansNanos = System.nanoTime();
                    sHasCachedScans = true;
                }
                updated = true;
            }
        } finally {
            Consumer<Boolean> finished;
            synchronized (LOCK) {
                finished = sScanFinished;
                sScanFinished = null;
                sScanRunning = false;
            }
            if (finished != null) finished.accept(updated);
        }
    }

    public static boolean isWifiEnabled() {
        return currentState(null).enabled;
    }

    public static WifiInfo connectionInfo() {
        return toWifiInfo(currentState(null));
    }

    public static WifiInfo connectionInfo(WindowsNetworks.Snapshot snapshot) {
        return toWifiInfo(currentState(snapshot));
    }

    public static DhcpInfo dhcpInfo() {
        InterfaceState state = currentState(null);
        DhcpInfo info = new DhcpInfo();
        if (state.connected) {
            info.ipAddress = state.ipAddress;
            info.gateway = state.gateway;
            info.netmask = state.netmask;
            info.dns1 = state.dns1;
            info.dns2 = state.dns2;
            info.serverAddress = state.serverAddress;
            info.leaseDuration = state.ipAddress != 0 ? 3600 : 0;
        }
        return info;
    }

    public static List<WifiConfiguration> configuredNetworks() {
        InterfaceState state = currentState(null);
        List<WifiConfiguration> list = new ArrayList<>();
        if (state.connected && state.ssid != null && !state.ssid.isEmpty()) {
            WifiConfiguration cfg = new WifiConfiguration();
            cfg.networkId = 1;
            cfg.SSID = "\"" + state.ssid + "\"";
            cfg.BSSID = state.bssid;
            cfg.status = WifiConfiguration.Status.CURRENT;
            list.add(cfg);
        }
        return list;
    }

    public static List<ScanResult> scanResults() {
        synchronized (LOCK) {
            if (!sHasCachedScans || (System.nanoTime() - sCachedScansNanos) >= SCAN_CACHE_NS) {
                startScan(null);
            }
            return sHasCachedScans && sCachedScans != null
                    ? new ArrayList<>(sCachedScans)
                    : new ArrayList<>();
        }
    }

    private static WifiInfo toWifiInfo(InterfaceState state) {
        WifiInfo info = new WifiInfo();
        if (state.connected && state.ssid != null && !state.ssid.isEmpty()) {
            info.setSSID(WifiSsid.fromBytes(state.ssid.getBytes(StandardCharsets.UTF_8)));
            info.setBSSID(state.bssid);
            info.setMacAddress(WifiInfo.DEFAULT_MAC_ADDRESS);
            info.setRssi(state.rssi);
            int linkSpeed = state.txMbps > 0 ? state.txMbps
                    : (state.rxMbps > 0 ? state.rxMbps : WifiInfo.LINK_SPEED_UNKNOWN);
            info.setLinkSpeed(linkSpeed);
            info.setTxLinkSpeedMbps(state.txMbps > 0 ? state.txMbps : WifiInfo.LINK_SPEED_UNKNOWN);
            info.setRxLinkSpeedMbps(state.rxMbps > 0 ? state.rxMbps : WifiInfo.LINK_SPEED_UNKNOWN);
            info.setFrequency(state.frequencyMhz);
            if (state.inetAddress != null) {
                info.setInetAddress(state.inetAddress);
            }
            info.setNetworkId(1);
            info.setSupplicantState(SupplicantState.COMPLETED);
            info.setCurrentSecurityType(state.securityType);
            info.setWifiStandard(state.wifiStandard);
            return info;
        }
        info.setSupplicantState(state.enabled
                ? SupplicantState.DISCONNECTED
                : SupplicantState.UNINITIALIZED);
        return info;
    }

    private static InterfaceState currentState(WindowsNetworks.Snapshot providedSnapshot) {
        if (providedSnapshot != null) {
            InterfaceState cached = getCachedState();
            if (cached != null) {
                return applyNetworkOverlay(copyInterfaceState(cached), findWifiNet(providedSnapshot));
            }
        } else {
            InterfaceState cached = getCachedState();
            if (cached != null) {
                return cached;
            }
        }
        WindowsNetworks.Net wifiNet = findWifiNet(providedSnapshot != null ? providedSnapshot : safeSnapshot());
        String output = runNetsh(3, "wlan", "show", "interfaces");
        InterfaceState state = parseInterfacesOutput(output, wifiNet);
        long now = System.nanoTime();
        synchronized (LOCK) {
            if (providedSnapshot == null) {
                sCachedState = state;
                sCachedStateNanos = now;
                sHasCachedState = true;
            }
        }
        return state;
    }

    private static InterfaceState getCachedState() {
        long now = System.nanoTime();
        synchronized (LOCK) {
            if (sHasCachedState && (now - sCachedStateNanos) < INTERFACE_CACHE_NS && sCachedState != null) {
                return sCachedState;
            }
            return null;
        }
    }

    private static InterfaceState copyInterfaceState(InterfaceState src) {
        InterfaceState copy = new InterfaceState();
        copy.hasInterface = src.hasInterface;
        copy.enabled = src.enabled;
        copy.connected = src.connected;
        copy.ssid = src.ssid;
        copy.bssid = src.bssid;
        copy.macAddress = src.macAddress;
        copy.band = src.band;
        copy.channel = src.channel;
        copy.frequencyMhz = src.frequencyMhz;
        copy.rssi = src.rssi;
        copy.rxMbps = src.rxMbps;
        copy.txMbps = src.txMbps;
        copy.securityType = src.securityType;
        copy.wifiStandard = src.wifiStandard;
        copy.inetAddress = src.inetAddress;
        copy.ipAddress = src.ipAddress;
        copy.gateway = src.gateway;
        copy.netmask = src.netmask;
        copy.dns1 = src.dns1;
        copy.dns2 = src.dns2;
        copy.serverAddress = src.serverAddress;
        return copy;
    }

    private static WindowsNetworks.Snapshot safeSnapshot() {
        try {
            return WindowsNetworks.parse(WindowsNetworks.snapshot());
        } catch (Throwable ignored) {
            return null;
        }
    }

    private static WindowsNetworks.Net findWifiNet(WindowsNetworks.Snapshot snapshot) {
        if (snapshot == null) return null;
        WindowsNetworks.Net def = snapshot.defaultNetwork();
        if (def != null && "wifi".equals(def.kind)) return def;
        for (WindowsNetworks.Net n : snapshot.networks) {
            if ("wifi".equals(n.kind)) return n;
        }
        return null;
    }

    /** Parses {@code netsh wlan show interfaces} output and merges {@code wifiNet} IP settings. */
    public static InterfaceState parseInterfacesOutput(String output, WindowsNetworks.Net wifiNet) {
        List<InterfaceBlock> blocks = new ArrayList<>();
        InterfaceBlock current = null;
        if (output != null && !output.isEmpty()) {
            String[] lines = output.split("\\r?\\n");
            for (String rawLine : lines) {
                int colon = rawLine.indexOf(':');
                if (colon < 0) continue;
                String key = stripBidi(rawLine.substring(0, colon)).trim();
                String val = stripBidi(rawLine.substring(colon + 1)).trim();
                String keyLower = key.toLowerCase(Locale.ROOT);
                if ("name".equals(keyLower)) {
                    current = new InterfaceBlock();
                    blocks.add(current);
                    continue;
                }
                if (current == null) continue;
                switch (keyLower) {
                    case "state":
                        current.connected = "connected".equalsIgnoreCase(val);
                        break;
                    case "software":
                    case "hardware":
                    case "radio status":
                        if (val.toLowerCase(Locale.ROOT).contains("off")) {
                            current.radioOff = true;
                        }
                        break;
                    case "ssid":
                        current.ssid = val;
                        break;
                    case "ap bssid":
                    case "bssid":
                        if (!val.isEmpty()) current.bssid = val.toLowerCase(Locale.ROOT);
                        break;
                    case "physical address":
                        if (!val.isEmpty()) current.macAddress = val.toLowerCase(Locale.ROOT);
                        break;
                    case "band":
                        current.band = val;
                        break;
                    case "channel":
                        current.channel = parseLeadingInt(val, current.channel);
                        break;
                    case "rssi":
                        current.rssi = parseLeadingInt(val, WifiInfo.INVALID_RSSI);
                        current.hasRssi = current.rssi != WifiInfo.INVALID_RSSI;
                        break;
                    case "signal":
                        if (!current.hasRssi) {
                            int pct = parseLeadingInt(val, -1);
                            if (pct >= 0) {
                                current.rssi = -100 + (Math.min(100, pct) / 2);
                            }
                        }
                        break;
                    case "receive rate (mbps)":
                        current.rxMbps = parseLeadingInt(val, current.rxMbps);
                        break;
                    case "transmit rate (mbps)":
                        current.txMbps = parseLeadingInt(val, current.txMbps);
                        break;
                    case "radio type":
                        current.wifiStandard = parseWifiStandard(val);
                        break;
                    case "authentication":
                        current.securityType = parseSecurityType(val);
                        break;
                    default:
                        break;
                }
            }
        }

        InterfaceBlock chosen = null;
        for (InterfaceBlock b : blocks) {
            if (b.connected) {
                chosen = b;
                break;
            }
        }
        if (chosen == null && !blocks.isEmpty()) {
            chosen = blocks.get(0);
        }

        InterfaceState state = new InterfaceState();
        if (chosen != null) {
            state.hasInterface = true;
            state.connected = chosen.connected;
            state.ssid = chosen.ssid != null ? chosen.ssid : "";
            state.bssid = chosen.bssid;
            state.macAddress = chosen.macAddress;
            state.band = chosen.band != null ? chosen.band : "";
            state.channel = chosen.channel;
            state.frequencyMhz = channelToFrequencyMhz(chosen.channel, state.band);
            state.rssi = chosen.rssi;
            state.rxMbps = chosen.rxMbps;
            state.txMbps = chosen.txMbps;
            state.securityType = chosen.securityType;
            state.wifiStandard = chosen.wifiStandard;
            state.enabled = !chosen.radioOff || chosen.connected || wifiNet != null;
        } else {
            state.enabled = wifiNet != null;
        }
        return applyNetworkOverlay(state, wifiNet);
    }

    private static InterfaceState applyNetworkOverlay(InterfaceState state, WindowsNetworks.Net wifiNet) {
        if (wifiNet == null) return state;
        for (int i = 0; i < wifiNet.addresses.size(); i++) {
            InetAddress addr = wifiNet.addresses.get(i);
            if (addr instanceof Inet4Address) {
                state.inetAddress = addr;
                state.ipAddress = toLittleEndianIpv4(addr.getAddress());
                int prefix = i < wifiNet.prefixLengths.size() ? wifiNet.prefixLengths.get(i) : 0;
                int hostOrderMask = prefix <= 0 ? 0 : (prefix >= 32 ? 0xffffffff : (0xffffffff << (32 - prefix)));
                state.netmask = Integer.reverseBytes(hostOrderMask);
                break;
            }
        }
        for (InetAddress gw : wifiNet.gateways) {
            if (gw instanceof Inet4Address) {
                state.gateway = toLittleEndianIpv4(gw.getAddress());
                state.serverAddress = state.gateway;
                break;
            }
        }
        int dnsCount = 0;
        for (InetAddress dns : wifiNet.dnsServers) {
            if (dns instanceof Inet4Address) {
                int v = toLittleEndianIpv4(dns.getAddress());
                if (dnsCount == 0) {
                    state.dns1 = v;
                    dnsCount++;
                } else {
                    state.dns2 = v;
                    break;
                }
            }
        }
        if (state.rxMbps <= 0 && wifiNet.receiveBps > 0) {
            state.rxMbps = (int) Math.min(Integer.MAX_VALUE, wifiNet.receiveBps / 1_000_000L);
        }
        if (state.txMbps <= 0 && wifiNet.transmitBps > 0) {
            state.txMbps = (int) Math.min(Integer.MAX_VALUE, wifiNet.transmitBps / 1_000_000L);
        }
        return state;
    }

    /** Parses {@code netsh wlan show networks mode=bssid} output into Android {@link ScanResult}s. */
    public static List<ScanResult> parseNetworksOutput(String output) {
        List<ScanResult> out = new ArrayList<>();
        if (output == null || output.isEmpty()) return out;

        long nowUs = currentTimestampMicros();
        String currentSsid = null;
        String currentAuth = "";
        String currentEnc = "";
        String currentBssid = null;
        int currentSignalPct = -1;
        String currentBand = "";
        int currentChannel = 0;

        String[] lines = output.split("\\r?\\n");
        for (String rawLine : lines) {
            int colon = rawLine.indexOf(':');
            if (colon < 0) continue;
            String key = stripBidi(rawLine.substring(0, colon)).trim();
            String val = stripBidi(rawLine.substring(colon + 1)).trim();
            String keyLower = key.toLowerCase(Locale.ROOT);

            if (isIndexedKey(keyLower, "ssid")) {
                flushScanResult(out, currentSsid, currentBssid, currentAuth, currentEnc,
                        currentBand, currentChannel, currentSignalPct, nowUs);
                currentSsid = val;
                currentAuth = "";
                currentEnc = "";
                currentBssid = null;
                currentSignalPct = -1;
                currentBand = "";
                currentChannel = 0;
            } else if (isIndexedKey(keyLower, "bssid")) {
                flushScanResult(out, currentSsid, currentBssid, currentAuth, currentEnc,
                        currentBand, currentChannel, currentSignalPct, nowUs);
                currentBssid = val.isEmpty() ? null : val.toLowerCase(Locale.ROOT);
                currentSignalPct = -1;
                currentBand = "";
                currentChannel = 0;
            } else if ("authentication".equals(keyLower)) {
                currentAuth = val;
            } else if ("encryption".equals(keyLower)) {
                currentEnc = val;
            } else if ("signal".equals(keyLower)) {
                currentSignalPct = parseLeadingInt(val, -1);
            } else if ("band".equals(keyLower)) {
                currentBand = val;
            } else if ("channel".equals(keyLower)) {
                currentChannel = parseLeadingInt(val, 0);
            }
        }
        flushScanResult(out, currentSsid, currentBssid, currentAuth, currentEnc,
                currentBand, currentChannel, currentSignalPct, nowUs);
        return out;
    }

    private static void flushScanResult(List<ScanResult> out, String ssid, String bssid,
            String auth, String enc, String band, int channel, int signalPct, long timestampUs) {
        if (bssid == null || bssid.isEmpty()) return;
        ScanResult sr = new ScanResult();
        String rawSsid = ssid != null ? ssid : "";
        sr.setWifiSsid(WifiSsid.fromBytes(rawSsid.getBytes(StandardCharsets.UTF_8)));
        sr.BSSID = bssid;
        sr.capabilities = buildCapabilities(auth, enc);
        sr.frequency = channelToFrequencyMhz(channel, band);
        sr.level = signalPct >= 0 ? (-100 + (Math.min(100, signalPct) / 2)) : WifiInfo.INVALID_RSSI;
        sr.timestamp = timestampUs;
        out.add(sr);
    }

    private static String buildCapabilities(String auth, String enc) {
        String authUpper = auth == null ? "" : auth.toUpperCase(Locale.ROOT);
        String encUpper = enc == null ? "" : enc.toUpperCase(Locale.ROOT);
        StringBuilder sb = new StringBuilder();
        String cipher = encUpper.contains("CCMP") ? "-CCMP"
                : (encUpper.contains("TKIP") ? "-TKIP" : "");
        if (authUpper.contains("WPA3") || authUpper.contains("SAE")) {
            sb.append("[WPA3-SAE").append(cipher).append(']');
        } else if (authUpper.contains("WPA2")) {
            sb.append("[WPA2-").append(authUpper.contains("ENTERPRISE") ? "EAP" : "PSK")
                    .append(cipher).append(']');
        } else if (authUpper.contains("WPA")) {
            sb.append("[WPA-").append(authUpper.contains("ENTERPRISE") ? "EAP" : "PSK")
                    .append(cipher).append(']');
        } else if (authUpper.contains("WEP") || encUpper.contains("WEP")) {
            sb.append("[WEP]");
        }
        sb.append("[ESS]");
        return sb.toString();
    }

    private static boolean isIndexedKey(String keyLower, String prefix) {
        if (!keyLower.startsWith(prefix + " ")) return false;
        String rest = keyLower.substring(prefix.length() + 1).trim();
        if (rest.isEmpty()) return false;
        for (int i = 0; i < rest.length(); i++) {
            if (!Character.isDigit(rest.charAt(i))) return false;
        }
        return true;
    }

    private static int channelToFrequencyMhz(int channel, String band) {
        if (channel <= 0) return -1;
        String bandLower = band == null ? "" : band.toLowerCase(Locale.ROOT);
        if (bandLower.contains("6")) return 5950 + channel * 5;
        if (bandLower.contains("5") || (bandLower.isEmpty() && channel >= 32)) {
            return 5000 + channel * 5;
        }
        if (channel == 14) return 2484;
        if (channel <= 13) return 2407 + channel * 5;
        return -1;
    }

    private static int parseWifiStandard(String radioType) {
        if (radioType == null) return ScanResult.WIFI_STANDARD_UNKNOWN;
        String lower = radioType.toLowerCase(Locale.ROOT);
        if (lower.contains("be")) return ScanResult.WIFI_STANDARD_11BE;
        if (lower.contains("ax")) return ScanResult.WIFI_STANDARD_11AX;
        if (lower.contains("ac")) return ScanResult.WIFI_STANDARD_11AC;
        if (lower.contains("n")) return ScanResult.WIFI_STANDARD_11N;
        if (lower.contains("802.11a") || lower.contains("802.11g") || lower.contains("802.11b")) {
            return ScanResult.WIFI_STANDARD_LEGACY;
        }
        return ScanResult.WIFI_STANDARD_UNKNOWN;
    }

    private static int parseSecurityType(String auth) {
        if (auth == null) return WifiInfo.SECURITY_TYPE_UNKNOWN;
        String upper = auth.toUpperCase(Locale.ROOT);
        if (upper.contains("WPA3") || upper.contains("SAE")) return WifiInfo.SECURITY_TYPE_SAE;
        if (upper.contains("ENTERPRISE")) return WifiInfo.SECURITY_TYPE_EAP;
        if (upper.contains("WPA2") || upper.contains("WPA")) return WifiInfo.SECURITY_TYPE_PSK;
        if (upper.contains("WEP")) return WifiInfo.SECURITY_TYPE_WEP;
        if (upper.contains("OPEN")) return WifiInfo.SECURITY_TYPE_OPEN;
        return WifiInfo.SECURITY_TYPE_UNKNOWN;
    }

    private static int parseLeadingInt(String val, int fallback) {
        if (val == null) return fallback;
        String s = stripBidi(val).trim();
        if (s.isEmpty()) return fallback;
        int i = 0;
        if (s.charAt(0) == '-' || s.charAt(0) == '+') i++;
        int startDigits = i;
        while (i < s.length() && Character.isDigit(s.charAt(i))) i++;
        if (i == startDigits) return fallback;
        try {
            return Integer.parseInt(s.substring(0, i));
        } catch (NumberFormatException e) {
            return fallback;
        }
    }

    private static String stripBidi(String s) {
        if (s == null || s.isEmpty()) return "";
        StringBuilder sb = null;
        for (int i = 0; i < s.length(); i++) {
            char c = s.charAt(i);
            if (c == '\u200E' || c == '\u200F' || c == '\uFEFF') {
                if (sb == null) {
                    sb = new StringBuilder(s.length());
                    sb.append(s, 0, i);
                }
            } else if (sb != null) {
                sb.append(c);
            }
        }
        return sb == null ? s : sb.toString();
    }

    private static int toLittleEndianIpv4(byte[] b) {
        if (b == null || b.length != 4) return 0;
        return (b[0] & 0xff)
                | ((b[1] & 0xff) << 8)
                | ((b[2] & 0xff) << 16)
                | ((b[3] & 0xff) << 24);
    }

    private static long currentTimestampMicros() {
        try {
            return SystemClock.elapsedRealtime() * 1000L;
        } catch (Throwable ignored) {
            return System.nanoTime() / 1000L;
        }
    }

    private static String runNetsh(int timeoutSeconds, String... args) {
        String[] primary = new String[args.length + 1];
        primary[0] = "netsh.exe";
        System.arraycopy(args, 0, primary, 1, args.length);
        try {
            return execCommand(primary, timeoutSeconds);
        } catch (IOException firstError) {
            String[] fallback = new String[args.length + 1];
            fallback[0] = "/Windows/System32/netsh.exe";
            System.arraycopy(args, 0, fallback, 1, args.length);
            try {
                return execCommand(fallback, timeoutSeconds);
            } catch (IOException secondError) {
                return "";
            }
        }
    }

    private static String execCommand(String[] cmd, int timeoutSeconds) throws IOException {
        ProcessBuilder pb = new ProcessBuilder(cmd);
        pb.redirectErrorStream(true);
        Process p = pb.start();
        try {
            p.getOutputStream().close();
        } catch (IOException ignored) {
        }
        FutureTask<String> output = new FutureTask<>(() -> {
            ByteArrayOutputStream bytes = new ByteArrayOutputStream();
            try (InputStream input = p.getInputStream()) {
                byte[] buffer = new byte[4096];
                int count;
                while ((count = input.read(buffer)) != -1) {
                    bytes.write(buffer, 0, count);
                }
            }
            return p.waitFor() == 0 ? bytes.toString(StandardCharsets.UTF_8) : "";
        });
        Thread reader = new Thread(output, "gesher-wifi-output");
        reader.setDaemon(true);
        reader.start();
        boolean completed = false;
        try {
            String text = output.get(timeoutSeconds, TimeUnit.SECONDS);
            completed = true;
            return text;
        } catch (TimeoutException e) {
            return "";
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
            return "";
        } catch (ExecutionException e) {
            throw new IOException("Failed to read netsh output", e.getCause());
        } finally {
            if (!completed) {
                output.cancel(true);
                // Process.destroyForcibly can wait for its stream close. Keep it
                // off the caller so a blocked pipe cannot extend the deadline.
                Thread cleanup = new Thread(() -> p.destroyForcibly(), "gesher-wifi-cleanup");
                cleanup.setDaemon(true);
                cleanup.start();
            }
        }
    }

    private static final class InterfaceBlock {
        boolean connected;
        boolean radioOff;
        String ssid;
        String bssid;
        String macAddress;
        String band;
        int channel;
        int rssi = WifiInfo.INVALID_RSSI;
        boolean hasRssi;
        int rxMbps;
        int txMbps;
        int securityType = WifiInfo.SECURITY_TYPE_UNKNOWN;
        int wifiStandard;
    }
}
