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
import java.util.function.IntPredicate;

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
    private static volatile boolean sLoaded;
    private WindowsWifi() { }

    private static void load() {
        if (sLoaded) return;
        synchronized (WindowsWifi.class) {
            if (!sLoaded) {
                System.loadLibrary("gesher_wifi");
                sLoaded = true;
            }
        }
    }

    // The built-in library uses WlanQueryInterface/WlanGetNetworkBssList. A requested
    // scan uses WlanScan and waits for its completion notification, never netsh output.
    private static native String nativeQuery(boolean scan, BooleanSupplier eligible);
    // Synchronous WlanEnumInterfaces census with each adapter's PHY and auth/cipher support.
    private static native String nativeHardware();
    // Blocks while WLAN notifications are watched; sink receives event-class bits.
    private static native int nativeWatch(IntPredicate sink, int pollMs);
    // User-approved connections through per-user Windows profiles (WlanSetProfile/WlanConnect).
    private static native int nativeConnect(int ifIndex, String xml, String profile, byte[] bssid,
            BooleanSupplier active, int timeoutMs);
    private static native boolean nativeDisconnect(int ifIndex, String removeProfile, String restoreProfile);
    private static native boolean nativeStoreProfile(int ifIndex, String xml, String profile);
    private static native String nativeCurrentProfile(int ifIndex);
    private static native String nativeProfiles(int ifIndex, String prefix);

    private static final class Operation {
        final boolean scan;
        final long generation;
        final List<Consumer<Boolean>> callbacks = new ArrayList<>();
        final List<BooleanSupplier> eligible = new ArrayList<>();
        boolean expired;
        // This read follows a Windows-initiated scan completion (set by launch).
        boolean externalScan;
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
        operation.externalScan = sExternalScanPending && !operation.scan;
        if (operation.externalScan) sExternalScanPending = false;
        sEventPending = false;
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
            load();
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
            boolean updated = false, externalScan = false;
            int previous = WifiManager.WIFI_STATE_UNKNOWN, state = WifiManager.WIFI_STATE_UNKNOWN;
            synchronized (LOCK) {
                if (operation.deadline != null) operation.deadline.cancel(false);
                if (!operation.expired && operation.generation == sGeneration) {
                    if (result != null) {
                        if (!result.scansValid && !result.scansDenied && sData != null) result.scans.addAll(sData.scans);
                        sData = result;
                        sReadNanos = System.nanoTime();
                        updated = result.scansValid;
                        // Only a successful read with a known state is a transition source;
                        // a failed RPC is not evidence that the radio changed.
                        int observed = state(result);
                        if (observed != WifiManager.WIFI_STATE_UNKNOWN && observed != sPublishedState) {
                            previous = sPublishedState;
                            sPublishedState = observed;
                            state = observed;
                        }
                        if (operation.scan) sOwnScanNanos = System.nanoTime();
                        // Windows completed a scan this backend did not request.
                        externalScan = operation.externalScan && result.scansValid && !result.scansDenied;
                    } else {
                        sData = null;
                        // A short retry interval also applies to failed queries.
                        sReadNanos = System.nanoTime();
                    }
                }
                callbacks = drain(operation);
                if (sOperation == operation) sOperation = null;
                // A WLAN event that arrived during this read asks for a fresh one.
                if (sOperation == null && sEventPending) {
                    try { launch(new Operation(false)); }
                    catch (RuntimeException | Error ignored) { /* The next read retries. */ }
                }
            }
            notifyFinished(callbacks, updated);
            if (state != WifiManager.WIFI_STATE_UNKNOWN) {
                for (Events listener : EVENTS) {
                    try { listener.onWifiStateChanged(previous, state); }
                    catch (RuntimeException ignored) { }
                }
            }
            if (externalScan) {
                for (Events listener : EVENTS) {
                    try { listener.onScanResultsAvailable(); }
                    catch (RuntimeException ignored) { }
                }
            }
        }
    }

    private static int state(Data data) {
        if (data.unmappedRadioEnabled) return WifiManager.WIFI_STATE_ENABLED;
        boolean unknown = data.unmappedRadioUnknown;
        for (State state : data.states.values()) {
            if (state.radio == 1) return WifiManager.WIFI_STATE_ENABLED;
            unknown |= state.radio < 0;
        }
        return unknown ? WifiManager.WIFI_STATE_UNKNOWN : WifiManager.WIFI_STATE_DISABLED;
    }

    // ---- WLAN notifications ------------------------------------------------------

    /** Service-side observers of real WLAN events. Called on Wi-Fi backend threads. */
    public interface Events {
        /** A successful read observed a known state different from the last one. */
        void onWifiStateChanged(int previous, int current);
        /** Windows completed a scan that this backend did not initiate. */
        void onScanResultsAvailable();
        /** Association, disconnection, radio or adapter presence changed. */
        default void onConnectionChanged() { }
    }

    static final int EVENT_STATE = 1, EVENT_SCAN = 2, EVENT_CONNECTION = 4, EVENT_INTERFACES = 8;
    private static final long OWN_SCAN_NS = TimeUnit.SECONDS.toNanos(2);
    private static final List<Events> EVENTS = new java.util.concurrent.CopyOnWriteArrayList<>();
    private static int sPublishedState = WifiManager.WIFI_STATE_UNKNOWN; // guarded by LOCK
    private static long sOwnScanNanos; // guarded by LOCK
    private static boolean sEventPending, sExternalScanPending; // guarded by LOCK
    private static Thread sWatcher; // guarded by LOCK
    private static volatile boolean sWatching;

    /** Registers an observer and starts the single notification watcher. */
    public static void addListener(Events listener) {
        EVENTS.add(java.util.Objects.requireNonNull(listener));
        synchronized (LOCK) {
            if (sWatcher != null) return;
            sWatching = true;
            sWatcher = new Thread(WindowsWifi::watch, "gesher-wifi-watch");
            sWatcher.setDaemon(true);
            sWatcher.start();
        }
    }

    public static void removeListener(Events listener) { EVENTS.remove(listener); }

    private static void watch() {
        long backoff = TimeUnit.SECONDS.toMillis(30);
        while (sWatching) {
            int result;
            try {
                load();
                result = nativeWatch(WindowsWifi::onEvents, 30_000);
            } catch (RuntimeException | LinkageError unavailable) {
                result = -1;
            }
            if (!sWatching) return;
            if (result == 0) { backoff = TimeUnit.SECONDS.toMillis(30); continue; }
            // No WLAN service or library: look again later, since wlansvc or a USB
            // adapter can start after boot. The census stays "absent" until then.
            try { Thread.sleep(backoff); } catch (InterruptedException e) { return; }
            backoff = Math.min(backoff * 2, TimeUnit.MINUTES.toMillis(5));
            refreshHardware();
        }
    }

    /** Native watcher delivery; false stops watching. Runs on the watcher thread. */
    static boolean onEvents(int bits) {
        if (!sWatching) return false;
        if (bits == 0) return true;
        if ((bits & EVENT_INTERFACES) != 0) refreshHardware();
        synchronized (LOCK) {
            if ((bits & EVENT_SCAN) != 0) {
                boolean own = sOperation != null && sOperation.scan
                        || sOwnScanNanos != 0 && System.nanoTime() - sOwnScanNanos < OWN_SCAN_NS;
                // A requested scan's completion is published by its own requester path.
                if (!own) sExternalScanPending = true;
            }
            sEventPending = true;
            sReadNanos = 0;
            if (sOperation == null) {
                try { launch(new Operation(false)); }
                catch (RuntimeException | Error ignored) { }
            }
        }
        if ((bits & (EVENT_CONNECTION | EVENT_STATE)) != 0) {
            for (Events listener : EVENTS) {
                try { listener.onConnectionChanged(); }
                catch (RuntimeException ignored) { }
            }
        }
        return true;
    }

    /** Test boundary: the watcher loop stops at its next delivery. */
    static void stopWatching() {
        synchronized (LOCK) { sWatching = false; sWatcher = null; }
    }

    // ---- Connections ---------------------------------------------------------------

    /** {@link #connect} outcome for a completed association; failures use Android's codes. */
    public static final int CONNECTED = -1;
    /** {@link #connect} outcome when the requester went away before Windows completed. */
    public static final int CANCELLED = 100;

    /**
     * Stores a per-user, manual-mode profile and connects the interface with it, waiting
     * (bounded) for Windows' completion notification of this attempt. Returns
     * {@link #CONNECTED}, {@link #CANCELLED} or a STATUS_LOCAL_ONLY_CONNECTION_FAILURE_* code.
     */
    public static int connect(int ifIndex, String xml, String profile, byte[] bssid,
            BooleanSupplier active, int timeoutMs) {
        try {
            load();
            return nativeConnect(ifIndex, xml, profile, bssid, active, timeoutMs);
        } catch (RuntimeException | LinkageError unavailable) {
            return WifiManager.STATUS_LOCAL_ONLY_CONNECTION_FAILURE_UNKNOWN;
        } finally {
            synchronized (LOCK) { sReadNanos = 0; }
        }
    }

    /** Disconnects, deletes {@code removeProfile} and reconnects {@code restoreProfile}. */
    public static boolean disconnect(int ifIndex, String removeProfile, String restoreProfile) {
        try {
            load();
            return nativeDisconnect(ifIndex, removeProfile, restoreProfile);
        } catch (RuntimeException | LinkageError unavailable) {
            return false;
        } finally {
            synchronized (LOCK) { sReadNanos = 0; }
        }
    }

    /** Stores ({@code xml} non-null) or deletes a per-user profile without connecting. */
    public static boolean storeProfile(int ifIndex, String xml, String profile) {
        try {
            load();
            return nativeStoreProfile(ifIndex, xml, profile);
        } catch (RuntimeException | LinkageError unavailable) {
            return false;
        }
    }

    /** The Windows profile name of the interface's current connection, or null. */
    public static String currentProfile(int ifIndex) {
        try {
            load();
            return nativeCurrentProfile(ifIndex);
        } catch (RuntimeException | LinkageError unavailable) {
            return null;
        }
    }

    /**
     * This user's per-user profiles on the interface whose names start with prefix, or
     * null when the interface's profile list could not be read (no WLAN service, an
     * absent interface, a failed list): that is not evidence that a profile is absent.
     */
    public static List<String> profiles(int ifIndex, String prefix) {
        String raw;
        try {
            load();
            raw = nativeProfiles(ifIndex, prefix);
        } catch (RuntimeException | LinkageError unavailable) {
            return null;
        }
        if (raw == null) return null;
        List<String> names = new ArrayList<>();
        for (String name : raw.split("\n")) if (!name.isEmpty() && name.startsWith(prefix)) names.add(name);
        return names;
    }

    /** The IP interface index of an adapter with an enabled radio, or 0. */
    public static int primaryInterface() {
        Data data = current();
        if (data == null) return 0;
        for (State state : data.states.values()) if (state.radio == 1) return state.index;
        return 0;
    }

    /**
     * Whether Windows has a provisioned IPv4 address on this interface. A 169.254/16
     * automatic (APIPA) address is not provisioning: Android's IpClient acquires IPv4 by
     * DHCP and never counts a link-local address.
     */
    public static boolean hasIpv4(int ifIndex) {
        try {
            WindowsNetworks.Net network = WindowsNetworks.parse(WindowsNetworks.snapshot()).byNetId(100 + ifIndex);
            if (network == null) return false;
            for (InetAddress address : network.addresses) if (provisioned(address)) return true;
        } catch (RuntimeException | LinkageError unavailable) {
            // Unknown addressing is not provisioning.
        }
        return false;
    }

    static boolean provisioned(InetAddress address) {
        return address instanceof Inet4Address && !address.isLinkLocalAddress() && !address.isAnyLocalAddress();
    }

    // ---- Adapter census --------------------------------------------------------

    /** The union of all present WLAN adapters' reported capabilities. */
    public static final class Hardware {
        public final int interfaces;
        /** Bit n: DOT11_PHY_TYPE n reported by WlanGetInterfaceCapability. */
        public final int phyTypes;
        /** Bit n: DOT11_AUTH_ALGORITHM n in the infrastructure auth/cipher pairs. */
        public final int authAlgorithms;
        /** Bit n: DOT11_CIPHER_ALGORITHM n; bit 31: DOT11_CIPHER_ALGO_WEP. */
        public final int ciphers;
        public Hardware(int interfaces, int phyTypes, int authAlgorithms, int ciphers) {
            this.interfaces = interfaces; this.phyTypes = phyTypes;
            this.authAlgorithms = authAlgorithms; this.ciphers = ciphers;
        }
        public boolean present() { return interfaces > 0; }
        public boolean phy(int type) { return type > 0 && type < 31 && (phyTypes & (1 << type)) != 0; }
        public boolean auth(int algorithm) {
            return algorithm > 0 && algorithm < 31 && (authAlgorithms & (1 << algorithm)) != 0;
        }
        public boolean cipher(int bit) { return bit >= 0 && bit < 32 && (ciphers & (1 << bit)) != 0; }
    }

    public static final Hardware NO_HARDWARE = new Hardware(0, 0, 0, 0);
    /** How long a caller waits for the first census (the package manager's boot features). */
    static final long CENSUS_TIMEOUT_MS = 5_000;

    /**
     * The cached adapter census. The first read runs on its own thread and its caller
     * waits at most a bound: a WLAN service that hangs (or is slow to start) in
     * WlanOpenHandle/WlanEnumInterfaces must not stall system-server boot. A census not
     * finished in time reports no hardware to that caller. A later caller arriving while
     * it runs waits only for the rest of that first bound, never a new one, so a healthy
     * census is not misreported and a hung one does not hold every caller again. The
     * late result is cached for later callers, and WLAN interface arrival/removal
     * refreshes it.
     */
    static final class Census {
        private final java.util.function.Supplier<Hardware> mSource;
        private Hardware mValue; // guarded by this
        private Thread mProbe; // guarded by this: the first census, while it runs
        private long mDeadline; // guarded by this: nanoTime the first caller's wait ends

        Census(java.util.function.Supplier<Hardware> source) { mSource = source; }

        Hardware get(long timeoutMs) {
            Thread probe;
            long waitNanos;
            boolean first;
            synchronized (this) {
                if (mValue != null) return mValue;
                long now = System.nanoTime();
                first = mProbe == null;
                if (first) {
                    mDeadline = now + TimeUnit.MILLISECONDS.toNanos(Math.max(1, timeoutMs));
                    mProbe = new Thread(() -> {
                        Hardware fresh = null;
                        try {
                            fresh = mSource.get();
                        } finally {
                            synchronized (this) {
                                // A refresh that finished meanwhile is newer than this read.
                                if (mValue == null) mValue = fresh != null ? fresh : NO_HARDWARE;
                                mProbe = null;
                            }
                        }
                    }, "gesher-wlan-census");
                    mProbe.setDaemon(true);
                    mProbe.start();
                }
                probe = mProbe;
                // A later caller waits at most what is left of the first caller's bound.
                waitNanos = Math.min(mDeadline - now, TimeUnit.MILLISECONDS.toNanos(Math.max(0, timeoutMs)));
            }
            if (waitNanos > 0) {
                try {
                    TimeUnit.NANOSECONDS.timedJoin(probe, waitNanos);
                } catch (InterruptedException e) {
                    Thread.currentThread().interrupt();
                }
            }
            synchronized (this) {
                if (mValue != null) return mValue;
            }
            if (first) {
                System.out.println("WindowsWifi: WLAN census did not finish within " + timeoutMs
                        + " ms; reporting no Wi-Fi hardware");
            }
            return NO_HARDWARE;
        }

        void refresh() {
            Hardware fresh = mSource.get();
            synchronized (this) { mValue = fresh != null ? fresh : NO_HARDWARE; }
        }
    }

    private static final Census CENSUS = new Census(WindowsWifi::census);

    /**
     * The adapter census (system features are fixed when the package manager starts),
     * refreshed on WLAN interface arrival/removal. No WLAN library, a stopped WLAN
     * service, no adapter, or a census that does not finish in CENSUS_TIMEOUT_MS all
     * mean no hardware.
     */
    public static Hardware hardware() {
        return CENSUS.get(CENSUS_TIMEOUT_MS);
    }

    private static void refreshHardware() {
        CENSUS.refresh();
    }

    private static Hardware census() {
        try {
            load();
            Hardware parsed = parseHardware(nativeHardware());
            return parsed != null ? parsed : NO_HARDWARE;
        } catch (RuntimeException | LinkageError unavailable) {
            return NO_HARDWARE;
        }
    }

    static Hardware parseHardware(String raw) {
        if (raw == null || raw.length() > 64 * 1024) return null;
        String[] lines = raw.split("\n");
        try {
            String[] head = lines[0].split("\t", -1);
            if (head.length != 2 || !head[0].equals("hw")) return null;
            int count = integer(head[1], 0, 64);
            if (lines.length != count + 1) return null;
            int phy = 0, auth = 0, cipher = 0;
            for (int i = 1; i < lines.length; i++) {
                String[] values = lines[i].split("\t", -1);
                if (values.length != 4 || !values[0].equals("cap")) return null;
                phy |= unsigned(values[1]);
                auth |= unsigned(values[2]);
                cipher |= unsigned(values[3]);
            }
            return new Hardware(count, phy, auth, cipher);
        } catch (RuntimeException malformed) {
            return null;
        }
    }

    private static int unsigned(String value) {
        long parsed = Long.parseLong(value);
        if (parsed < 0 || parsed > 0xffffffffL) throw new IllegalArgumentException("WLAN mask");
        return (int) parsed;
    }

    /** True only with real evidence: an observed BSS or a connection in the band. */
    public static boolean bandObserved(int lowMhz, int highMhz) {
        Data data;
        synchronized (LOCK) { data = sData; }
        if (data == null) return false;
        for (ScanResult scan : data.scans) {
            if (scan.frequency >= lowMhz && scan.frequency <= highMhz) return true;
        }
        for (State state : data.states.values()) {
            if (state.connected && state.frequency >= lowMhz && state.frequency <= highMhz) return true;
        }
        return false;
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
