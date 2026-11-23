/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package com.gesher.server;

import android.app.AppOpsManager;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.net.MacAddress;
import android.net.wifi.BlockingOption;
import android.net.wifi.ScanResult;
import android.net.wifi.WifiConfiguration;
import android.net.wifi.WifiManager;
import android.net.wifi.WifiNetworkSpecifier;
import android.net.wifi.WifiNetworkSuggestion;
import android.net.wifi.WifiSsid;
import android.os.Bundle;
import android.os.IBinder;
import android.os.PatternMatcher;
import android.os.RemoteException;
import com.gesher.net.IWindowsWifi;
import com.gesher.net.WlanProfiles;
import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;
import java.security.SecureRandom;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.LinkedHashMap;
import java.util.LinkedHashSet;
import java.util.List;
import java.util.Map;
import java.util.Set;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.TimeUnit;
import java.util.function.BooleanSupplier;

/**
 * Android 10+ Wi-Fi connection contracts over Windows WLAN profiles (docs/wifi.md):
 * {@link WifiNetworkSpecifier} requests, joined only after the user picks a matching
 * network in a host dialog, and app network suggestions, joined by Windows only after
 * the user allows the app. Windows networks are never changed without that approval,
 * and availability is reported only after Windows' own connection completion and an
 * IPv4 address on the interface.
 */
final class WifiConnections {
    /** User-facing approval surfaces; the host dialogs in production, scripted in tests. */
    interface Ui {
        boolean available();
        /** Index of the chosen label, or a negative value for cancel/unavailable. */
        int choose(String packageName, String title, String[] labels, BooleanSupplier active);
        /** PermissionRequests.ALLOWED, DENIED or DISMISSED. */
        int confirm(String packageName, String text);
    }

    static final Ui HOST_UI = new Ui() {
        @Override public boolean available() { return IntentChooser.HOST.available(); }
        @Override public int choose(String packageName, String title, String[] labels, BooleanSupplier active) {
            boolean[] enabled = new boolean[labels.length];
            java.util.Arrays.fill(enabled, true);
            return IntentChooser.HOST.choose(packageName, title, labels, enabled, active);
        }
        @Override public int confirm(String packageName, String text) {
            return PermissionRequests.confirmDecision(packageName, text);
        }
    };

    /** Without a host UI nothing can be approved, so nothing is joined. */
    static final Ui NO_UI = new Ui() {
        @Override public boolean available() { return false; }
        @Override public int choose(String packageName, String title, String[] labels, BooleanSupplier active) { return -1; }
        @Override public int confirm(String packageName, String text) { return PermissionRequests.DISMISSED; }
    };

    static final int SCAN_ROUNDS = 3; // USER_APPROVED_SCAN_RETRY_MAX
    static final long SCAN_WAIT_MS = 12_000;
    static volatile long scanIntervalMs = 10_000; // PERIODIC_SCAN_INTERVAL_MS; test boundary only
    static final int CONNECT_TIMEOUT_MS = 30_000;
    static final int CONNECT_ATTEMPTS = 3; // USER_SELECTED_NETWORK_CONNECT_RETRY_MAX
    static volatile long ipWaitMs = 20_000; // test boundary only
    static final long LINGER_MS = 30_000; // DEFAULT_LINGER_DELAY_MS
    static final int APPROVED_PER_APP = 50; // NUM_OF_ACCESS_POINT_LIMIT_PER_APP
    private static final String REQUEST_PREFIX = WlanProfiles.PREFIX + "r-";
    private static final String SUGGESTION_PREFIX = WlanProfiles.PREFIX + "s-";

    private final WifiService mService;
    private final WifiService.Backend mBackend;
    private final Ui mUi;
    private final ExecutorService mWorker = Executors.newCachedThreadPool(r -> {
        Thread thread = new Thread(r, "gesher-wifi-connect");
        thread.setDaemon(true);
        return thread;
    });
    // One specifier request is served at a time (WifiNetworkFactory): a replaced request's
    // worker finishes (it is inactive) before the next one touches Windows.
    private final ExecutorService mRequests = Executors.newSingleThreadExecutor(r -> {
        Thread thread = new Thread(r, "gesher-wifi-request");
        thread.setDaemon(true);
        return thread;
    });
    private final java.util.concurrent.ScheduledExecutorService mTimers =
            Executors.newSingleThreadScheduledExecutor(r -> {
                Thread thread = new Thread(r, "gesher-wifi-linger");
                thread.setDaemon(true);
                return thread;
            });
    private final Object mLock = new Object();
    private Request mActive; // guarded by mLock: Android serves one specifier request at a time
    private final Map<String, LinkedHashSet<String>> mApproved = new HashMap<>(); // package -> approved APs
    private final Map<String, Apps> mSuggestions = new LinkedHashMap<>(); // package -> suggestions
    private boolean mCleaned;
    // At most one queued pass each: a burst of WLAN events or adds does not pile up tasks.
    private final java.util.concurrent.atomic.AtomicBoolean mConnectionPassQueued =
            new java.util.concurrent.atomic.AtomicBoolean();
    private final java.util.concurrent.atomic.AtomicBoolean mInstallPassQueued =
            new java.util.concurrent.atomic.AtomicBoolean();

    WifiConnections(WifiService service, WifiService.Backend backend, Ui ui) {
        mService = service;
        mBackend = backend;
        mUi = ui;
        // An uninstalled app's approvals, suggestions and profiles go at once, as
        // Android's package-removal listeners drop them.
        try {
            service.pm().onPackageEvent(intent -> {
                if (!Intent.ACTION_PACKAGE_REMOVED.equals(intent.getAction())
                        || intent.getBooleanExtra(Intent.EXTRA_REPLACING, false) || intent.getData() == null) return;
                String name = intent.getData().getSchemeSpecificPart();
                if (name != null) mWorker.execute(() -> removeApp(name, true));
            });
        } catch (RuntimeException unavailable) {
            // No package events (a host fixture without them): pruning on use still applies.
        }
    }

    // ---- WifiNetworkSpecifier requests -----------------------------------------------

    final class Request implements IBinder.DeathRecipient {
        final WifiService.Owner owner;
        final WifiNetworkSpecifier specifier;
        final IBinder callback;
        volatile boolean released;
        int ifIndex;
        String profile, previous;
        boolean connected, connecting;
        volatile boolean finished;

        Request(WifiService.Owner owner, WifiNetworkSpecifier specifier, IBinder callback) {
            this.owner = owner; this.specifier = specifier; this.callback = callback;
        }
        // A finished (replaced, failed or released) request never acts on Windows again.
        boolean active() { return !released && !finished && mService.current(owner); }
        @Override public void binderDied() { release(this); }
    }

    void request(WifiService.Owner owner, WifiNetworkSpecifier specifier, IBinder callback) {
        int level = mService.foregroundLevel(owner);
        Request request = new Request(owner, specifier, callback);
        try {
            callback.linkToDeath(request, 0);
        } catch (RemoteException dead) {
            return;
        }
        // WifiNetworkFactory.acceptRequest: an invalid specifier, or a requester that is
        // neither a foreground app nor a foreground service, is unfulfillable and leaves
        // any current request alone.
        if (level == WifiService.NOT_FOREGROUND || !valid(specifier)) {
            finish(request, IWindowsWifi.NETWORK_UNAVAILABLE, false);
            return;
        }
        Request replaced = null;
        boolean accepted = false;
        for (int round = 0; round < 4 && !accepted; round++) {
            Request existing;
            synchronized (mLock) { existing = mActive; }
            // canNewRequestOverrideExistingRequest: a foreground app (or NETWORK_SETTINGS)
            // overrides anything; a foreground service only a request that is not from a
            // foreground app.
            if (existing != null && level == WifiService.FOREGROUND_SERVICE
                    && mService.foregroundLevel(existing.owner) >= WifiService.FOREGROUND_APP) {
                break;
            }
            synchronized (mLock) {
                if (mActive == existing) {
                    replaced = existing;
                    mActive = request;
                    accepted = true;
                }
            }
        }
        if (!accepted) {
            finish(request, IWindowsWifi.NETWORK_UNAVAILABLE, false);
            return;
        }
        // One active request: the earlier one is released as unfulfillable, or loses its network.
        if (replaced != null) finish(replaced, replaced.connected
                ? IWindowsWifi.NETWORK_LOST : IWindowsWifi.NETWORK_UNAVAILABLE, true);
        mRequests.execute(() -> run(request));
    }

    /** WifiConfigurationUtil.validateNetworkSpecifier, for the fields this service relies on. */
    static boolean valid(WifiNetworkSpecifier specifier) {
        if (specifier == null || specifier.wifiConfiguration == null || specifier.ssidPatternMatcher == null
                || specifier.bssidPatternMatcher == null || specifier.bssidPatternMatcher.first == null
                || specifier.bssidPatternMatcher.second == null) return false;
        String ssid = specifier.ssidPatternMatcher.getPath();
        if (ssid == null) return false;
        int bytes = ssid.getBytes(StandardCharsets.UTF_8).length;
        boolean literal = specifier.ssidPatternMatcher.getType() == PatternMatcher.PATTERN_LITERAL;
        if (bytes > 32 || literal && bytes < 1) return false;
        // A pattern that matches every network (empty prefix and any BSSID) does not
        // request a specific network.
        boolean anySsid = specifier.ssidPatternMatcher.getType() == PatternMatcher.PATTERN_PREFIX && ssid.isEmpty();
        boolean anyBssid = MacAddress.ALL_ZEROS_ADDRESS.equals(specifier.bssidPatternMatcher.second);
        return !(anySsid && anyBssid) && !MacAddress.BROADCAST_ADDRESS.equals(specifier.bssidPatternMatcher.first);
    }

    void release(WifiService.Owner owner, IBinder callback) {
        Request request;
        synchronized (mLock) {
            request = mActive != null && mActive.callback == callback
                    && mActive.owner.uid() == owner.uid() ? mActive : null;
        }
        if (request != null) release(request);
    }

    private void release(Request request) {
        request.released = true;
        finish(request, 0, true);
    }

    /** Ends a request once: tears down Gesher's connection and optionally notifies. */
    private void finish(Request request, int event, boolean teardown) {
        boolean wasConnected, notify, undo;
        synchronized (mLock) {
            if (request.finished) return;
            request.finished = true;
            if (mActive == request) mActive = null;
            wasConnected = request.connected;
            request.connected = false;
            notify = event != 0;
            // While its worker waits in a native connect, that worker undoes the attempt
            // once Windows returns, so a late completion cannot outlive the request.
            undo = teardown && !request.connecting;
        }
        try { request.callback.unlinkToDeath(request, 0); } catch (RuntimeException ignored) { }
        if (undo) teardown(request);
        if (notify && (event != IWindowsWifi.NETWORK_LOST || wasConnected)) {
            IWindowsWifi.Callback.send(request.callback, IWindowsWifi.CALLBACK_NETWORK_REQUEST, event, null);
        }
    }

    /**
     * Removes Gesher's temporary profile. Only while the interface is still on that profile
     * (or on none) does it disconnect and return to the previous network, as Android
     * re-enables auto-join after a single-STA specifier connection; a network the user or
     * Windows has meanwhile chosen is never left for the old one.
     */
    private void teardown(Request request) {
        String profile, previous;
        int ifIndex;
        synchronized (mLock) { profile = request.profile; previous = request.previous; ifIndex = request.ifIndex; }
        if (profile == null) return;
        String current = mBackend.currentProfile(ifIndex);
        if (current == null || current.equals(profile)) mBackend.disconnect(ifIndex, profile, previous);
        else mBackend.storeProfile(ifIndex, null, profile);
    }

    private void unavailable(Request request, int failure, boolean report) {
        if (report) {
            Bundle extras = new Bundle();
            extras.putParcelable(IWindowsWifi.EXTRA_SPECIFIER, request.specifier);
            String name = request.owner.name();
            mService.dispatch(IWindowsWifi.CALLBACK_LOCAL_ONLY_FAILURE, failure, extras,
                    candidate -> candidate.name().equals(name));
        }
        finish(request, IWindowsWifi.NETWORK_UNAVAILABLE, true);
    }

    private void run(Request request) {
        try {
            runChecked(request);
        } catch (RuntimeException failed) {
            SystemServer.log("Wi-Fi network request failed: " + failed);
            unavailable(request, WifiManager.STATUS_LOCAL_ONLY_CONNECTION_FAILURE_UNKNOWN, true);
        }
    }

    private void runChecked(Request request) {
        WifiNetworkSpecifier specifier = request.specifier;
        int security = WlanProfiles.securityType(specifier.wifiConfiguration);
        // Unfulfillable requests: bands (not yet supported by Android either), a non
        // foreground requester, Wi-Fi off, or security Windows profiles cannot express.
        if (specifier.getBand() != ScanResult.UNSPECIFIED
                || mBackend.wifiState() != WifiManager.WIFI_STATE_ENABLED || security < 0) {
            unavailable(request, 0, false);
            return;
        }
        int ifIndex = mBackend.primaryInterface();
        if (ifIndex <= 0) { unavailable(request, 0, false); return; }
        request.ifIndex = ifIndex;
        cleanup(ifIndex);
        boolean hidden = specifier.wifiConfiguration.hiddenSSID;
        // WifiNetworkFactory keeps scanning periodically until a matching network appears
        // or the request ends (released, timed out, its app gone, or replaced). Only a
        // request whose network the user approved before, which would skip the dialog,
        // gives up after USER_APPROVED_SCAN_RETRY_MAX scans, as unfulfillable.
        boolean skipDialog = approvedRequest(request, security);
        List<ScanResult> matches = matches(specifier, security, mBackend.scanResults());
        for (int scans = 0; matches.isEmpty() && !hidden && request.active(); scans++) {
            if (skipDialog && scans >= SCAN_ROUNDS) {
                unavailable(request, 0, false);
                return;
            }
            if (scans > 0 && !pause(request, scanIntervalMs)) break;
            CountDownLatch done = new CountDownLatch(1);
            if (mBackend.startScan(request::active, updated -> done.countDown())) {
                long deadline = System.nanoTime() + TimeUnit.MILLISECONDS.toNanos(SCAN_WAIT_MS);
                try {
                    while (request.active() && System.nanoTime() < deadline
                            && !done.await(250, TimeUnit.MILLISECONDS)) { }
                } catch (InterruptedException e) {
                    Thread.currentThread().interrupt();
                    return;
                }
            }
            matches = matches(specifier, security, mBackend.scanResults());
        }
        if (!request.active()) return;
        byte[] ssid;
        String bssid = null;
        if (!matches.isEmpty()) {
            List<ScanResult> networks = networks(matches);
            ScanResult chosen = approved(request, matches, security);
            if (chosen == null) {
                if (!mUi.available()) {
                    unavailable(request, WifiManager.STATUS_LOCAL_ONLY_CONNECTION_FAILURE_UNKNOWN, true);
                    return;
                }
                String[] labels = new String[networks.size()];
                for (int i = 0; i < labels.length; i++) labels[i] = label(networks.get(i));
                int index = mUi.choose(request.owner.name(), "Connect to a Wi-Fi device for this app", labels,
                        request::active);
                if (index < 0 || index >= labels.length || !request.active()) {
                    unavailable(request, WifiManager.STATUS_LOCAL_ONLY_CONNECTION_FAILURE_USER_REJECT, request.active());
                    return;
                }
                chosen = networks.get(index);
            }
            ssid = chosen.getWifiSsid() == null ? null : chosen.getWifiSsid().getBytes();
            bssid = chosen.BSSID;
            if (ssid == null || ssid.length == 0) {
                unavailable(request, WifiManager.STATUS_LOCAL_ONLY_CONNECTION_FAILURE_UNKNOWN, true);
                return;
            }
        } else if (hidden && specifier.ssidPatternMatcher.getType() == PatternMatcher.PATTERN_LITERAL) {
            // A hidden network is not in an undirected scan: the user approves the literal
            // SSID the app named, and Windows probes for it directly.
            ssid = specifier.ssidPatternMatcher.getPath().getBytes(StandardCharsets.UTF_8);
            if (!mUi.available()) {
                unavailable(request, WifiManager.STATUS_LOCAL_ONLY_CONNECTION_FAILURE_UNKNOWN, true);
                return;
            }
            int index = mUi.choose(request.owner.name(), "Connect to a Wi-Fi device for this app",
                    new String[] {specifier.ssidPatternMatcher.getPath() + " (hidden network)"}, request::active);
            if (index != 0 || !request.active()) {
                unavailable(request, WifiManager.STATUS_LOCAL_ONLY_CONNECTION_FAILURE_USER_REJECT, request.active());
                return;
            }
        } else {
            return; // the request ended while searching
        }
        connect(request, ssid, bssid, security, hidden, matches);
    }

    /** Waits while the request stays active; false once it ended. */
    private static boolean pause(Request request, long millis) {
        long deadline = System.nanoTime() + TimeUnit.MILLISECONDS.toNanos(millis);
        while (request.active()) {
            long left = deadline - System.nanoTime();
            if (left <= 0) return true;
            try { Thread.sleep(Math.min(250, Math.max(1, TimeUnit.NANOSECONDS.toMillis(left)))); }
            catch (InterruptedException e) { Thread.currentThread().interrupt(); return false; }
        }
        return false;
    }

    private void connect(Request request, byte[] ssid, String bssid, int security, boolean hidden,
            List<ScanResult> matches) {
        String profile = REQUEST_PREFIX + random();
        String xml = WlanProfiles.xml(profile, ssid, hidden, security,
                request.specifier.wifiConfiguration.preSharedKey, false);
        if (xml == null) { unavailable(request, WifiManager.STATUS_LOCAL_ONLY_CONNECTION_FAILURE_UNKNOWN, true); return; }
        String previous = mBackend.currentProfile(request.ifIndex);
        synchronized (mLock) {
            if (request.finished) return;
            request.previous = previous != null && !previous.startsWith(WlanProfiles.PREFIX) ? previous : null;
            request.profile = profile;
        }
        byte[] pinned = bssid == null ? null : MacAddress.fromString(bssid).toByteArray();
        int outcome = WifiManager.STATUS_LOCAL_ONLY_CONNECTION_FAILURE_UNKNOWN;
        synchronized (mLock) {
            if (request.finished) return;
            request.connecting = true;
        }
        try {
            for (int attempt = 0; attempt < CONNECT_ATTEMPTS && request.active(); attempt++) {
                outcome = mBackend.connect(request.ifIndex, xml, profile, pinned, request::active, CONNECT_TIMEOUT_MS);
                if (outcome == com.gesher.net.WindowsWifi.CONNECTED || outcome == com.gesher.net.WindowsWifi.CANCELLED
                        || outcome == WifiManager.STATUS_LOCAL_ONLY_CONNECTION_FAILURE_AUTHENTICATION) break;
            }
        } finally {
            boolean ended;
            synchronized (mLock) {
                request.connecting = false;
                ended = request.finished;
            }
            // Finished while Windows was connecting: undo what that attempt left behind.
            if (ended) teardown(request);
        }
        if (request.finished) return;
        if (!request.active()) { finish(request, 0, true); return; }
        if (outcome != com.gesher.net.WindowsWifi.CONNECTED) { unavailable(request, outcome, true); return; }
        long deadline = System.nanoTime() + TimeUnit.MILLISECONDS.toNanos(ipWaitMs);
        while (!mBackend.hasIpv4(request.ifIndex)) {
            if (!request.active()) { finish(request, 0, true); return; }
            if (System.nanoTime() > deadline) {
                unavailable(request, WifiManager.STATUS_LOCAL_ONLY_CONNECTION_FAILURE_IP_PROVISIONING, true);
                return;
            }
            try { Thread.sleep(250); } catch (InterruptedException e) { Thread.currentThread().interrupt(); return; }
        }
        if (!profile.equals(mBackend.currentProfile(request.ifIndex))) {
            unavailable(request, WifiManager.STATUS_LOCAL_ONLY_CONNECTION_FAILURE_UNKNOWN, true);
            return;
        }
        if (bssid == null) {
            // A hidden network was joined by SSID: report the access point Windows chose.
            android.net.wifi.WifiInfo info = mBackend.connectionInfo(request.ifIndex);
            String joined = info == null ? null : info.getBSSID();
            if (joined != null && !WifiManager.UNKNOWN_SSID.equals(joined)) bssid = joined;
        }
        remember(request.owner.name(), ssid, matches, bssid, security);
        Bundle extras = new Bundle();
        extras.putInt(IWindowsWifi.EXTRA_INTERFACE, request.ifIndex);
        extras.putByteArray(IWindowsWifi.EXTRA_SSID, ssid);
        extras.putString(IWindowsWifi.EXTRA_BSSID, bssid);
        extras.putInt(IWindowsWifi.EXTRA_SECURITY, security);
        int frequency = 0;
        for (ScanResult match : matches) if (match.BSSID != null && match.BSSID.equals(bssid)) frequency = match.frequency;
        extras.putInt(IWindowsWifi.EXTRA_FREQUENCY, frequency);
        synchronized (mLock) {
            if (request.finished || !request.active()) {
                // Released while connecting: undo outside the lock.
            } else {
                request.connected = true;
                IWindowsWifi.Callback.send(request.callback, IWindowsWifi.CALLBACK_NETWORK_REQUEST,
                        IWindowsWifi.NETWORK_AVAILABLE, extras);
                return;
            }
        }
        finish(request, 0, true);
    }

    /** WLAN association changed: a request whose profile is gone has lost its network. */
    void connectionChanged() {
        if (!mConnectionPassQueued.compareAndSet(false, true)) return;
        mWorker.execute(() -> {
            // Cleared first: an event during this pass queues exactly one more.
            mConnectionPassQueued.set(false);
            cleanup(mBackend.primaryInterface());
            Request request;
            synchronized (mLock) { request = mActive; }
            if (request != null) {
                boolean connected;
                synchronized (mLock) { connected = request.connected; }
                if (connected && !request.profile.equals(mBackend.currentProfile(request.ifIndex))) {
                    finish(request, IWindowsWifi.NETWORK_LOST, true);
                }
            }
            suggestionConnectionChanged();
        });
    }

    static List<ScanResult> matches(WifiNetworkSpecifier specifier, int security, List<ScanResult> scans) {
        List<ScanResult> result = new ArrayList<>();
        for (ScanResult scan : scans) {
            WifiSsid wifiSsid = scan.getWifiSsid();
            if (wifiSsid == null || scan.BSSID == null) continue;
            byte[] bytes = wifiSsid.getBytes();
            if (bytes.length == 0) continue;
            String text = new String(bytes, StandardCharsets.UTF_8);
            if (!specifier.ssidPatternMatcher.match(text)) continue;
            MacAddress address;
            try { address = MacAddress.fromString(scan.BSSID); }
            catch (IllegalArgumentException malformed) { continue; }
            if (!address.matches(specifier.bssidPatternMatcher.first, specifier.bssidPatternMatcher.second)) continue;
            boolean secure = false;
            for (int type : scan.getSecurityTypes()) secure |= type == security;
            if (secure) result.add(scan);
        }
        return result;
    }

    /** One entry per network (SSID), represented by its strongest access point. */
    static List<ScanResult> networks(List<ScanResult> matches) {
        Map<String, ScanResult> best = new LinkedHashMap<>();
        for (ScanResult scan : matches) {
            String key = java.util.HexFormat.of().formatHex(scan.getWifiSsid().getBytes());
            ScanResult current = best.get(key);
            if (current == null || scan.level > current.level) best.put(key, scan);
        }
        return new ArrayList<>(best.values());
    }

    private static String label(ScanResult scan) {
        String ssid = new String(scan.getWifiSsid().getBytes(), StandardCharsets.UTF_8).replace('\n', ' ').replace('\0', ' ');
        String band = scan.frequency >= 5925 ? "6 GHz" : scan.frequency >= 4900 ? "5 GHz" : "2.4 GHz";
        return ssid + "  (" + band + ", " + scan.level + " dBm)";
    }

    private static String key(byte[] ssid, String bssid, int security) {
        return java.util.HexFormat.of().formatHex(ssid) + "|" + String.valueOf(bssid).toLowerCase(java.util.Locale.ROOT)
                + "|" + security;
    }

    /**
     * WifiNetworkFactory.triggerConnectIfUserApprovedMatchFound: the approval list is looked
     * up with the request's own SSID (pattern path) and BSSID, and only for a single-network
     * request. Open and OWE networks need the request itself to name the approved BSSID;
     * other security types match on SSID and type. The strongest matching access point of
     * the approved network is joined.
     */
    private boolean approvedRequest(Request request, int security) {
        WifiNetworkSpecifier specifier = request.specifier;
        boolean literal = specifier.ssidPatternMatcher.getType() == PatternMatcher.PATTERN_LITERAL;
        boolean exactBssid = MacAddress.BROADCAST_ADDRESS.equals(specifier.bssidPatternMatcher.second);
        if (!literal && !exactBssid) return false;
        byte[] ssid = specifier.ssidPatternMatcher.getPath().getBytes(StandardCharsets.UTF_8);
        String bssid = specifier.bssidPatternMatcher.first.toString();
        synchronized (mLock) {
            Set<String> approved = mApproved.get(request.owner.name());
            if (approved == null) return false;
            boolean open = security == WifiConfiguration.SECURITY_TYPE_OPEN
                    || security == WifiConfiguration.SECURITY_TYPE_OWE;
            return approved.contains(key(ssid, bssid, security)) || !open && approved.contains(key(ssid, "*", security));
        }
    }

    private ScanResult approved(Request request, List<ScanResult> matches, int security) {
        if (!approvedRequest(request, security)) return null;
        WifiNetworkSpecifier specifier = request.specifier;
        boolean exactBssid = MacAddress.BROADCAST_ADDRESS.equals(specifier.bssidPatternMatcher.second);
        byte[] ssid = specifier.ssidPatternMatcher.getPath().getBytes(StandardCharsets.UTF_8);
        String bssid = specifier.bssidPatternMatcher.first.toString();
        ScanResult best = null;
        for (ScanResult match : matches) {
            if (!java.util.Arrays.equals(match.getWifiSsid().getBytes(), ssid)) continue;
            if (exactBssid && !bssid.equalsIgnoreCase(match.BSSID)) continue;
            if (best == null || match.level > best.level) best = match;
        }
        return best;
    }

    private void remember(String packageName, byte[] ssid, List<ScanResult> matches, String bssid, int security) {
        synchronized (mLock) {
            LinkedHashSet<String> approved = mApproved.computeIfAbsent(packageName, name -> new LinkedHashSet<>());
            List<String> keys = new ArrayList<>();
            keys.add(key(ssid, "*", security));
            if (bssid != null) keys.add(key(ssid, bssid, security));
            for (ScanResult match : matches) {
                if (java.util.Arrays.equals(match.getWifiSsid().getBytes(), ssid)) keys.add(key(ssid, match.BSSID, security));
            }
            for (String entry : keys) { approved.remove(entry); approved.add(entry); }
            while (approved.size() > APPROVED_PER_APP) approved.remove(approved.iterator().next());
        }
    }

    private static String random() {
        byte[] bytes = new byte[8];
        new SecureRandom().nextBytes(bytes);
        return java.util.HexFormat.of().formatHex(bytes);
    }

    // ---- Network suggestions -----------------------------------------------------

    private static final class Apps {
        final List<WifiNetworkSuggestion> suggestions = new ArrayList<>();
        final Map<WifiNetworkSuggestion, String> profiles = new HashMap<>();
        boolean approved, asking;
        String lastConnected;
    }

    int approvalStatus(WifiService.Owner owner) {
        if (!owner.system() && mService.pm().appOps().checkOperationRaw(AppOpsManager.OP_CHANGE_WIFI_STATE,
                owner.uid(), owner.name(), null) == AppOpsManager.MODE_IGNORED) {
            return WifiManager.STATUS_SUGGESTION_APPROVAL_REJECTED_BY_USER;
        }
        synchronized (mLock) {
            Apps app = mSuggestions.get(owner.name());
            if (app == null) return WifiManager.STATUS_SUGGESTION_APPROVAL_UNKNOWN;
            return app.approved ? WifiManager.STATUS_SUGGESTION_APPROVAL_APPROVED_BY_USER
                    : WifiManager.STATUS_SUGGESTION_APPROVAL_PENDING;
        }
    }

    private void approvalChanged(String packageName) {
        // Each listener's status is per caller: send it individually.
        mService.dispatchEach(IWindowsWifi.CALLBACK_SUGGESTION_APPROVAL,
                candidate -> candidate.name().equals(packageName), this::approvalStatus);
    }

    private int maxPerApp() {
        android.app.ActivityManager am = null;
        try { am = android.app.ActivityThread.currentApplication() == null ? null
                : android.app.ActivityThread.currentApplication().getSystemService(android.app.ActivityManager.class); }
        catch (RuntimeException | LinkageError ignored) { }
        return am != null && am.isLowRamDevice() ? 256 : 1024;
    }

    /** Windows can express this suggestion faithfully (see WlanProfiles). */
    private static boolean expressible(WifiNetworkSuggestion suggestion) {
        WifiConfiguration config = suggestion.wifiConfiguration;
        if (config == null || suggestion.passpointConfiguration != null) return false;
        int security = WlanProfiles.securityType(config);
        byte[] ssid = WlanProfiles.ssidBytes(config.SSID);
        return security >= 0 && ssid != null && WlanProfiles.xml(SUGGESTION_PREFIX + "check", ssid, config.hiddenSSID,
                security, config.preSharedKey, true) != null;
    }

    int addSuggestions(WifiService.Owner owner, List<WifiNetworkSuggestion> list) {
        if (list == null || list.isEmpty()) return WifiManager.STATUS_NETWORK_SUGGESTIONS_SUCCESS;
        for (WifiNetworkSuggestion suggestion : list) {
            if (suggestion == null || suggestion.wifiConfiguration == null) {
                return WifiManager.STATUS_NETWORK_SUGGESTIONS_ERROR_ADD_INVALID;
            }
            WifiConfiguration config = suggestion.wifiConfiguration;
            // Carrier and subscription suggestions need carrier privileges.
            if (config.carrierId != android.telephony.TelephonyManager.UNKNOWN_CARRIER_ID
                    || config.subscriptionId != android.telephony.SubscriptionManager.INVALID_SUBSCRIPTION_ID) {
                return WifiManager.STATUS_NETWORK_SUGGESTIONS_ERROR_ADD_NOT_ALLOWED;
            }
            byte[] ssid = WlanProfiles.ssidBytes(config.SSID);
            if (ssid == null && suggestion.passpointConfiguration == null) {
                return WifiManager.STATUS_NETWORK_SUGGESTIONS_ERROR_ADD_INVALID;
            }
            // Enterprise, WEP, WAPI and Passpoint cannot become a faithful Windows profile.
            if (!expressible(suggestion)) return WifiManager.STATUS_NETWORK_SUGGESTIONS_ERROR_ADD_NOT_ALLOWED;
        }
        boolean ask;
        synchronized (mLock) {
            pruneRemovedPackagesLocked();
            Apps app = mSuggestions.get(owner.name());
            boolean created = app == null;
            if (created) app = new Apps();
            List<WifiNetworkSuggestion> merged = new ArrayList<>(app.suggestions);
            for (WifiNetworkSuggestion suggestion : list) {
                int existing = merged.indexOf(suggestion);
                if (existing >= 0) merged.set(existing, suggestion); else merged.add(suggestion);
            }
            if (merged.size() > maxPerApp()) return WifiManager.STATUS_NETWORK_SUGGESTIONS_ERROR_ADD_EXCEEDS_MAX_PER_APP;
            if (created) mSuggestions.put(owner.name(), app);
            app.suggestions.clear();
            app.suggestions.addAll(merged);
            ask = !app.approved && !app.asking;
            if (ask) app.asking = true;
        }
        if (ask) {
            approvalChanged(owner.name());
            String name = owner.name();
            mWorker.execute(() -> askApproval(name));
        } else {
            queueInstall();
        }
        return WifiManager.STATUS_NETWORK_SUGGESTIONS_SUCCESS;
    }

    private void askApproval(String packageName) {
        int decision = mUi.confirm(packageName, "suggest Wi-Fi networks that Windows may connect to automatically");
        WifiService.Owner owner = null;
        synchronized (mLock) {
            Apps app = mSuggestions.get(packageName);
            if (app == null) return;
            app.asking = false;
            if (decision == PermissionRequests.ALLOWED) app.approved = true;
        }
        if (decision == PermissionRequests.DENIED) {
            // "Don't allow": Android ignores the app's CHANGE_WIFI_STATE operation and
            // forgets its suggestions.
            ApkPackage code = mService.pm().get(packageName);
            if (code != null) {
                mService.pm().appOps().setMode(AppOpsManager.OP_CHANGE_WIFI_STATE, code.uid(), packageName,
                        AppOpsManager.MODE_IGNORED);
            }
            removeApp(packageName, true);
        }
        if (decision != PermissionRequests.DISMISSED) approvalChanged(packageName);
        installApproved();
    }

    private void queueInstall() {
        if (!mInstallPassQueued.compareAndSet(false, true)) return;
        mWorker.execute(() -> {
            mInstallPassQueued.set(false);
            installApproved();
        });
    }

    /** Stores Windows auto-connect profiles for approved apps' suggestions. */
    private void installApproved() {
        int ifIndex = mBackend.primaryInterface();
        cleanup(ifIndex);
        List<String[]> store = new ArrayList<>();
        synchronized (mLock) {
            for (Map.Entry<String, Apps> entry : mSuggestions.entrySet()) {
                Apps app = entry.getValue();
                if (!app.approved) continue;
                for (WifiNetworkSuggestion suggestion : app.suggestions) {
                    if (app.profiles.containsKey(suggestion)) continue;
                    String profile = SUGGESTION_PREFIX + digest(entry.getKey(), suggestion);
                    WifiConfiguration config = suggestion.wifiConfiguration;
                    String xml = WlanProfiles.xml(profile, WlanProfiles.ssidBytes(config.SSID), config.hiddenSSID,
                            WlanProfiles.securityType(config), config.preSharedKey, suggestion.isInitialAutoJoinEnabled);
                    if (xml == null || ifIndex <= 0) continue;
                    app.profiles.put(suggestion, profile);
                    store.add(new String[] {xml, profile});
                }
            }
        }
        for (String[] item : store) {
            if (!mBackend.storeProfile(ifIndex, item[0], item[1])) {
                SystemServer.log("Wi-Fi suggestion profile could not be stored");
            }
        }
    }

    /** Names Gesher generates: random request profiles and suggestion digests. */
    private static final java.util.regex.Pattern OWN_PROFILE =
            java.util.regex.Pattern.compile("Gesher-(r-[0-9a-f]{16}|s-[0-9a-f]{24})");

    /**
     * Profiles from an earlier run (including one a crashed system server left connected)
     * have no Android owner any more: delete them once, as soon as the WLAN interface is
     * known. Only per-user profiles with exactly the names Gesher generates are touched,
     * never a profile of this run.
     */
    void cleanup(int ifIndex) {
        if (ifIndex <= 0) return;
        synchronized (mLock) {
            if (mCleaned) return;
            mCleaned = true;
        }
        for (String stale : mBackend.profiles(ifIndex, WlanProfiles.PREFIX)) {
            if (!OWN_PROFILE.matcher(stale).matches()) continue;
            synchronized (mLock) {
                if (mActive != null && stale.equals(mActive.profile)) continue;
                boolean current = false;
                for (Apps app : mSuggestions.values()) current |= app.profiles.containsValue(stale);
                if (current) continue;
            }
            mBackend.storeProfile(ifIndex, null, stale);
        }
    }

    private static String digest(String packageName, WifiNetworkSuggestion suggestion) {
        try {
            MessageDigest sha = MessageDigest.getInstance("SHA-256");
            sha.update(packageName.getBytes(StandardCharsets.UTF_8));
            sha.update((byte) 0);
            sha.update(String.valueOf(suggestion.wifiConfiguration.SSID).getBytes(StandardCharsets.UTF_8));
            sha.update((byte) 0);
            sha.update(String.valueOf(suggestion.wifiConfiguration.BSSID).getBytes(StandardCharsets.UTF_8));
            sha.update((byte) WlanProfiles.securityType(suggestion.wifiConfiguration));
            return java.util.HexFormat.of().formatHex(sha.digest(), 0, 12);
        } catch (java.security.NoSuchAlgorithmException impossible) {
            throw new IllegalStateException(impossible);
        }
    }

    int removeSuggestions(WifiService.Owner owner, List<WifiNetworkSuggestion> list, int action) {
        if (list == null) return WifiManager.STATUS_NETWORK_SUGGESTIONS_SUCCESS;
        List<String> removed = new ArrayList<>();
        synchronized (mLock) {
            Apps app = mSuggestions.get(owner.name());
            if (list.isEmpty()) {
                if (app == null) return WifiManager.STATUS_NETWORK_SUGGESTIONS_SUCCESS;
                removed.addAll(app.profiles.values());
                app.suggestions.clear();
                app.profiles.clear();
            } else {
                if (app == null) return WifiManager.STATUS_NETWORK_SUGGESTIONS_ERROR_REMOVE_INVALID;
                for (WifiNetworkSuggestion suggestion : list) {
                    if (suggestion == null || !app.suggestions.contains(suggestion)) {
                        return WifiManager.STATUS_NETWORK_SUGGESTIONS_ERROR_REMOVE_INVALID;
                    }
                }
                for (WifiNetworkSuggestion suggestion : list) {
                    app.suggestions.remove(suggestion);
                    String profile = app.profiles.remove(suggestion);
                    if (profile != null) removed.add(profile);
                }
            }
        }
        deleteProfiles(removed, action == WifiManager.ACTION_REMOVE_SUGGESTION_LINGER);
        return WifiManager.STATUS_NETWORK_SUGGESTIONS_SUCCESS;
    }

    private void deleteProfiles(List<String> profiles, boolean linger) {
        int ifIndex = mBackend.primaryInterface();
        if (ifIndex <= 0) return;
        String current = mBackend.currentProfile(ifIndex);
        for (String profile : profiles) {
            if (linger && profile.equals(current)) {
                // ACTION_REMOVE_SUGGESTION_LINGER: keep the current connection for the
                // linger period, then remove it.
                mTimers.schedule(() -> mBackend.storeProfile(ifIndex, null, profile), LINGER_MS, TimeUnit.MILLISECONDS);
            } else {
                mBackend.storeProfile(ifIndex, null, profile);
            }
        }
    }

    List<WifiNetworkSuggestion> suggestions(WifiService.Owner owner) {
        synchronized (mLock) {
            Apps app = mSuggestions.get(owner.name());
            return app == null ? new ArrayList<>() : new ArrayList<>(app.suggestions);
        }
    }

    private void removeApp(String packageName, boolean deleteProfiles) {
        List<String> profiles = new ArrayList<>();
        synchronized (mLock) {
            Apps app = mSuggestions.remove(packageName);
            if (app != null) profiles.addAll(app.profiles.values());
            mApproved.remove(packageName);
        }
        if (deleteProfiles) deleteProfiles(profiles, false);
    }

    /** An uninstalled package's suggestions and approvals do not outlive it. */
    private void pruneRemovedPackagesLocked() {
        List<String> gone = new ArrayList<>();
        for (String name : mSuggestions.keySet()) if (mService.pm().get(name) == null) gone.add(name);
        for (String name : mApproved.keySet()) if (mService.pm().get(name) == null && !gone.contains(name)) gone.add(name);
        for (String name : gone) mWorker.execute(() -> removeApp(name, true));
    }

    void disallowCurrent(WifiService.Owner owner, BlockingOption option) {
        if (mBackend.wifiState() != WifiManager.WIFI_STATE_ENABLED) return;
        int ifIndex = mBackend.primaryInterface();
        String current = ifIndex <= 0 ? null : mBackend.currentProfile(ifIndex);
        WifiNetworkSuggestion suggestion = null;
        synchronized (mLock) {
            Apps app = mSuggestions.get(owner.name());
            if (app == null || current == null) return;
            for (Map.Entry<WifiNetworkSuggestion, String> entry : app.profiles.entrySet()) {
                if (entry.getValue().equals(current)) suggestion = entry.getKey();
            }
            if (suggestion == null) return;
            app.profiles.remove(suggestion);
        }
        // Windows has no per-BSSID block list: removing the profile for the blocking
        // period disconnects and prevents auto-join for the whole network, then it returns.
        mBackend.disconnect(ifIndex, current, null);
        mTimers.schedule(this::installApproved, Math.max(1, option.getBlockingTimeSeconds()), TimeUnit.SECONDS);
    }

    /** Post-connection broadcast for an app-interaction suggestion Windows joined. */
    private void suggestionConnectionChanged() {
        int ifIndex = mBackend.primaryInterface();
        if (ifIndex <= 0) return;
        String current = mBackend.currentProfile(ifIndex);
        String owner = null;
        WifiNetworkSuggestion joined = null;
        synchronized (mLock) {
            pruneRemovedPackagesLocked();
            for (Map.Entry<String, Apps> entry : mSuggestions.entrySet()) {
                Apps app = entry.getValue();
                String previous = app.lastConnected;
                app.lastConnected = null;
                for (Map.Entry<WifiNetworkSuggestion, String> profile : app.profiles.entrySet()) {
                    if (profile.getValue().equals(current)) {
                        app.lastConnected = current;
                        if (!current.equals(previous) && profile.getKey().isAppInteractionRequired) {
                            owner = entry.getKey();
                            joined = profile.getKey();
                        }
                    }
                }
            }
        }
        if (owner == null || !mService.locationEnabled()) return;
        // enforceCanAccessScanResults for the suggesting package at delivery time.
        PackageManagerService pm = mService.pm();
        if (pm.permissions().checkPermission(owner, "android.permission.ACCESS_FINE_LOCATION")
                != PackageManager.PERMISSION_GRANTED) return;
        // enforceCanAccessScanResults also notes the fine-location app op for the package.
        ApkPackage code = pm.get(owner);
        if (code == null) return;
        long identity = android.os.Binder.clearCallingIdentity();
        try {
            if (pm.appOps().noteOperation(AppOpsManager.OP_FINE_LOCATION, code.uid(), owner, null, false, null, false)
                    .getOpMode() != AppOpsManager.MODE_ALLOWED) return;
        } finally {
            android.os.Binder.restoreCallingIdentity(identity);
        }
        Intent intent = new Intent(WifiManager.ACTION_WIFI_NETWORK_SUGGESTION_POST_CONNECTION)
                .setPackage(owner)
                .putExtra(WifiManager.EXTRA_NETWORK_SUGGESTION, joined);
        try { mService.am().sendBroadcastFor("android", intent, null); }
        catch (RuntimeException e) { SystemServer.log("Wi-Fi post-connection broadcast not sent: " + e); }
    }
}
