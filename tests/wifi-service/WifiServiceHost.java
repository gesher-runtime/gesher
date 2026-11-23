/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package com.gesher.server;

import android.app.AppOpsManager;
import android.app.IApplicationThread;
import android.content.IIntentReceiver;
import android.content.Intent;
import android.content.IntentFilter;
import android.content.pm.PackageManager;
import android.content.pm.PermissionInfo;
import android.content.pm.Signature;
import android.net.DhcpInfo;
import android.net.wifi.ScanResult;
import android.net.wifi.WifiInfo;
import android.net.wifi.WifiManager;
import android.net.wifi.WifiSsid;
import android.os.Binder;
import android.os.Bundle;
import android.os.IBinder;
import android.os.Process;
import com.android.internal.pm.parsing.pkg.PackageImpl;
import com.android.internal.pm.pkg.component.ParsedAttributionImpl;
import com.android.internal.pm.pkg.component.ParsedUsesPermissionImpl;
import java.lang.reflect.Field;
import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.Set;
import java.util.concurrent.ScheduledThreadPoolExecutor;
import java.util.function.BooleanSupplier;
import java.util.function.Consumer;
import sun.misc.Unsafe;

/** Real server policy and broadcast routing, with explicitly synthetic radio/APKs. */
public final class WifiServiceHost {
    private static final String ACCESS = "android.permission.ACCESS_WIFI_STATE";
    private static final String NETWORK = "android.permission.ACCESS_NETWORK_STATE";
    private static final String CHANGE = "android.permission.CHANGE_WIFI_STATE";
    private static final String FINE = "android.permission.ACCESS_FINE_LOCATION";
    private static final String COARSE = "android.permission.ACCESS_COARSE_LOCATION";
    private static final String BACKGROUND = "android.permission.ACCESS_BACKGROUND_LOCATION";
    private static final String NEARBY = "android.permission.NEARBY_WIFI_DEVICES";
    private static final String CHANGE_NETWORK = "android.permission.CHANGE_NETWORK_STATE";
    private static final String[] EXEMPTIONS = {
        "android.permission.NETWORK_SETTINGS", "android.permission.NETWORK_SETUP_WIZARD",
        "android.permission.NETWORK_MANAGED_PROVISIONING", "android.permission.NETWORK_STACK",
        "android.permission.MAINLINE_NETWORK_STACK", "android.permission.RADIO_SCAN_WITHOUT_LOCATION"
    };
    private static final List<String> ORDINARY = List.of(ACCESS, CHANGE, NETWORK, FINE, COARSE, BACKGROUND, NEARBY,
            CHANGE_NETWORK);
    private static final Unsafe UNSAFE;
    private static int checks, nextUid = 14100;
    private static native void setOwnUid(int uid);
    static {
        try { Field f = Unsafe.class.getDeclaredField("theUnsafe"); f.setAccessible(true); UNSAFE = (Unsafe)f.get(null); }
        catch (Exception e) { throw new ExceptionInInitializerError(e); }
    }
    private interface Checked { void run() throws Exception; }
    private static void check(boolean condition, String message) {
        if (!condition) throw new AssertionError(message); checks++;
    }
    private static Field field(Class<?> type, String name) throws Exception {
        Field f = type.getDeclaredField(name); f.setAccessible(true); return f;
    }
    private static Object get(Object object, String name) throws Exception {
        return field(object.getClass(), name).get(object);
    }
    private static void set(Object object, String name, Object value) throws Exception {
        field(object.getClass(), name).set(object, value);
    }
    private static void rejects(Checked action, String why) throws Exception {
        try { action.run(); }
        catch (SecurityException expected) { checks++; return; }
        throw new AssertionError("Expected SecurityException: " + why);
    }
    private static void as(int uid, int pid, Checked action) throws Exception {
        long previous = Binder.clearCallingIdentity();
        try { Binder.restoreCallingIdentity(((long)uid << 32) | (pid & 0xffffffffL)); action.run(); }
        finally { Binder.restoreCallingIdentity(previous); }
    }
    private static ApkPackage pkg(String name, int uid, int target, byte signature,
            List<String> permissions) throws Exception {
        PackageImpl parsed = (PackageImpl)PackageImpl.forTesting(name, "/fixture/" + name + "/base.apk");
        parsed.setEnabled(true); parsed.setTargetSdkVersion(target);
        for (String permission : permissions) {
            parsed.addUsesPermission(new ParsedUsesPermissionImpl(permission, 0, Set.of()));
        }
        parsed.addAttribution(new ParsedAttributionImpl("wifi", 1, List.of()));
        parsed.hideAsFinal();
        ApkPackage result = (ApkPackage)UNSAFE.allocateInstance(ApkPackage.class);
        set(result, "packageName", name); set(result, "parsed", parsed); set(result, "mUid", uid);
        set(result, "apkPath", "/fixture/" + name + "/base.apk");
        set(result, "mDataDir", "/fixture/" + name + "/data");
        set(result, "sharedLibraries", SharedLibraryGraph.EMPTY);
        set(result, "mSigningIdentityChecked", true);
        set(result, "mVerifiedSignatures", new Signature[]{new Signature(new byte[]{signature})});
        return result;
    }
    private static final class AppThread extends IApplicationThread.Default {
        final Binder token = new Binder();
        int broadcasts, senderUid;
        String senderPackage;
        Intent latest;
        @Override public IBinder asBinder() { return token; }
        @Override public void scheduleRegisteredReceiver(IIntentReceiver receiver, Intent intent,
                int resultCode, String data, Bundle extras, boolean ordered, boolean sticky,
                boolean assumed, int userId, int processState, int sendingUid, String sendingPackage) {
            broadcasts++; senderUid = sendingUid; senderPackage = sendingPackage; latest = intent;
        }
    }
    private static final class App {
        final ApkPackage code;
        final int pid;
        final AppThread thread = new AppThread();
        ActivityTaskManagerService.ProcessRecord process;
        App(String name, int target, int pid, List<String> permissions) throws Exception {
            this.code = pkg(name, nextUid++, target, (byte)8, permissions); this.pid = pid;
        }
        String name() { return code.packageName; }
        void call(Checked action) throws Exception { as(code.uid(), pid, action); }
    }
    private static final class Radio implements WifiService.Backend {
        int starts, reads, physicalScans;
        int requestedIndex;
        boolean accepted = true;
        BooleanSupplier guard;
        Consumer<Boolean> completion;
        Runnable duringRead;
        final WifiInfo info = new WifiInfo();
        final DhcpInfo dhcp = new DhcpInfo();
        final ScanResult ap = new ScanResult();
        Radio() {
            info.setSSID(WifiSsid.fromBytes("Fixture AP".getBytes(java.nio.charset.StandardCharsets.UTF_8)));
            info.setBSSID("02:10:20:30:40:50"); info.setMacAddress("02:21:31:41:51:61");
            info.setRssi(-49); info.setFrequency(5180);
            dhcp.ipAddress = 0x0101a8c0; dhcp.gateway = 0x0100a8c0; dhcp.netmask = 0x00ffffff;
            ap.SSID = "Fixture AP"; ap.BSSID = "02:10:20:30:40:50";
            ap.capabilities = "[ESS]"; ap.frequency = 5180; ap.level = -49; ap.timestamp = 123456;
            ap.setWifiSsid(WifiSsid.fromBytes("Fixture AP".getBytes(java.nio.charset.StandardCharsets.UTF_8)));
        }
        private void read() { reads++; if (duringRead != null) duringRead.run(); }
        @Override public int wifiState() { read(); return state; }
        @Override public WifiInfo connectionInfo() { read(); return info; }
        @Override public WifiInfo connectionInfo(int index) {
            read(); requestedIndex = index;
            if (index != 12 && index != 13) return new WifiInfo();
            WifiInfo result = new WifiInfo(info);
            if (index == 13) result.setSSID(WifiSsid.fromBytes("Second AP".getBytes(java.nio.charset.StandardCharsets.UTF_8)));
            return result;
        }
        @Override public DhcpInfo dhcpInfo() { read(); return dhcp; }
        @Override public List<ScanResult> scanResults() { read(); return List.of(new ScanResult(ap)); }
        @Override public boolean startScan(BooleanSupplier eligible, Consumer<Boolean> completed) {
            starts++; guard = eligible; completion = completed;
            if (accepted && autoComplete) completed.accept(true);
            return accepted;
        }
        // Synthetic adapter census and observed bands; production policy maps them.
        com.gesher.net.WindowsWifi.Hardware hardware = new com.gesher.net.WindowsWifi.Hardware(0, 0, 0, 0);
        int observedLow = -1, observedHigh = -1;
        com.gesher.net.WindowsWifi.Events events;
        int state = WifiManager.WIFI_STATE_ENABLED;
        @Override public com.gesher.net.WindowsWifi.Hardware hardware() { return hardware; }
        @Override public boolean bandObserved(int low, int high) {
            return observedLow >= 0 && observedLow <= high && observedHigh >= low;
        }
        @Override public void listen(com.gesher.net.WindowsWifi.Events listener) { events = listener; }
        // Synthetic WLAN connection boundary: records what the production policy asked
        // Windows to do; the outcomes are scripted per test.
        int iface = 12, outcome = com.gesher.net.WindowsWifi.CONNECTED;
        boolean ipv4 = true;
        String current = "Home";
        final List<String> calls = new java.util.concurrent.CopyOnWriteArrayList<>();
        final Set<String> stored = java.util.concurrent.ConcurrentHashMap.newKeySet();
        String lastXml;
        @Override public int primaryInterface() { return iface; }
        @Override public int connect(int ifIndex, String xml, String profile, byte[] bssid, BooleanSupplier active, int timeoutMs) {
            calls.add("connect " + ifIndex + " " + (bssid != null)); lastXml = xml; stored.add(profile);
            if (!active.getAsBoolean()) return com.gesher.net.WindowsWifi.CANCELLED;
            if (outcome == com.gesher.net.WindowsWifi.CONNECTED) current = profile;
            return outcome;
        }
        @Override public boolean disconnect(int ifIndex, String remove, String restore) {
            calls.add("disconnect " + (remove != null) + " " + restore);
            if (remove != null) stored.remove(remove);
            current = restore; return true;
        }
        @Override public boolean storeProfile(int ifIndex, String xml, String profile) {
            calls.add((xml != null ? "store " : "delete ") + profile.substring(0, 9));
            if (xml != null) { stored.add(profile); lastXml = xml; } else stored.remove(profile);
            return true;
        }
        final java.util.concurrent.atomic.AtomicInteger profileReads = new java.util.concurrent.atomic.AtomicInteger();
        @Override public String currentProfile(int ifIndex) { profileReads.incrementAndGet(); return current; }
        @Override public List<String> profiles(int ifIndex, String prefix) {
            List<String> result = new ArrayList<>();
            for (String name : stored) if (name.startsWith(prefix)) result.add(name);
            return result;
        }
        @Override public boolean hasIpv4(int ifIndex) { return ipv4; }
        boolean autoComplete;
        boolean begin() {
            boolean eligible = guard.getAsBoolean(); if (eligible) physicalScans++; return eligible;
        }
        void finish(boolean updated) { completion.accept(updated); }
    }
    private static final class Harness implements AutoCloseable {
        final App owner, peer, noAccess, noChange;
        final PackageManagerService pm;
        final ActivityTaskManagerService atm;
        final ActivityManagerService am;
        final Radio radio = new Radio();
        final WifiService wifi;
        final Binder restriction = new Binder();
        final ScriptedUi ui = new ScriptedUi();
        boolean location = true;
        @SuppressWarnings("unchecked") Harness(int target, boolean standalone) throws Exception {
            owner = new App("fixture.wifi.owner" + nextUid, target,
                    standalone ? Process.myPid() : 981001, ORDINARY);
            peer = new App("fixture.wifi.peer" + nextUid, 36, 981002, ORDINARY);
            noAccess = new App("fixture.wifi.noaccess" + nextUid, 36, 981003, List.of(CHANGE, FINE, COARSE, BACKGROUND));
            noChange = new App("fixture.wifi.nochange" + nextUid, 36, 981004, List.of(ACCESS, FINE, COARSE, BACKGROUND));
            pm = new PackageManagerService(List.of(owner.code, peer.code, noAccess.code, noChange.code), false);
            set(pm, "mPackageEventSender", (Consumer<Intent>)event -> { });
            atm = new ActivityTaskManagerService(pm); am = new ActivityManagerService(atm, pm, null);
            // Host has no framework-res native reader. These are the actual Android
            // definitions; real grant/AppOps policy remains unchanged.
            Object catalog = get(pm.permissions(), "mCatalog");
            Map<String, PermissionInfo> definitions = (Map<String, PermissionInfo>)get(catalog, "mFramework");
            for (String name : ORDINARY) {
                PermissionInfo permission = new PermissionInfo(
                        name.equals(FINE) || name.equals(COARSE) ? BACKGROUND : null);
                permission.name = name; permission.packageName = "android";
                permission.protectionLevel = name.equals(ACCESS) || name.equals(CHANGE) || name.equals(NETWORK)
                        || name.equals(CHANGE_NETWORK)
                        ? PermissionInfo.PROTECTION_NORMAL : PermissionInfo.PROTECTION_DANGEROUS;
                if (name.equals(BACKGROUND)) permission.flags = PermissionInfo.FLAG_HARD_RESTRICTED;
                definitions.put(name, permission);
            }
            for (String name : List.of("android.permission.LOCAL_MAC_ADDRESS", "android.permission.PEERS_MAC_ADDRESS")) {
                PermissionInfo permission = new PermissionInfo(); permission.name = name;
                permission.packageName = "android"; permission.protectionLevel = PermissionInfo.PROTECTION_SIGNATURE;
                definitions.put(name, permission);
            }
            for (String name : EXEMPTIONS) {
                PermissionInfo permission = new PermissionInfo(); permission.name = name;
                permission.packageName = "android"; permission.protectionLevel = PermissionInfo.PROTECTION_SIGNATURE;
                definitions.put(name, permission);
            }
            ((Set<String>)get(catalog, "mProtectedBroadcasts")).add(WifiManager.SCAN_RESULTS_AVAILABLE_ACTION);
            ((Set<String>)get(catalog, "mProtectedBroadcasts")).add(WifiManager.WIFI_STATE_CHANGED_ACTION);
            ((Set<String>)get(catalog, "mProtectedBroadcasts")).add(WifiManager.ACTION_WIFI_NETWORK_SUGGESTION_POST_CONNECTION);
            PermissionInfo nearby = new PermissionInfo(); nearby.name = NEARBY; nearby.packageName = "android";
            nearby.protectionLevel = PermissionInfo.PROTECTION_DANGEROUS; definitions.put(NEARBY, nearby);
            set(catalog, "mLoaded", true); set(catalog, "mFrameworkRead", true);
            pm.permissions().initializePackages();
            for (App app : List.of(owner, peer, noAccess, noChange)) {
                attach(app, standalone && app == owner);
                grant(app, FINE); grant(app, COARSE);
                pm.permissions().updatePermissionFlags(app.name(), BACKGROUND,
                        PackageManager.FLAG_PERMISSION_APPLY_RESTRICTION | PackageManager.FLAG_PERMISSION_RESTRICTION_SYSTEM_EXEMPT,
                        PackageManager.FLAG_PERMISSION_RESTRICTION_SYSTEM_EXEMPT, true, "default:0", 0);
                grant(app, BACKGROUND);
            }
            wifi = new WifiService(am, atm, pm, () -> location, radio, ui);
        }
        @SuppressWarnings("unchecked") void attach(App app, boolean standalone) throws Exception {
            app.call(() -> {
                app.process = atm.new ProcessRecord(app.pid, app.thread);
                app.process.packageName = app.name(); app.process.processName = app.name();
                app.process.packageCode = app.code; app.process.ready = true;
                if (standalone) app.process.inProcessActivityState = ActivityTaskManagerService.State.RESUMED;
                ((List<ActivityTaskManagerService.ProcessRecord>)get(atm, "mProcesses")).add(app.process);
            });
        }
        void grant(App app, String name) { pm.permissions().grantRuntimePermission(app.name(), name, "default:0", 0); }
        void revoke(App app, String name) { pm.permissions().revokeRuntimePermission(app.name(), name, "default:0", 0, "Wi-Fi contract"); }
        void ignored(App app, int operation, boolean ignored) {
            // WIFI_SCAN switches to the location permission op. Its real independent
            // denial is a user restriction, not an ignored package runtime mode.
            if (operation == AppOpsManager.OP_WIFI_SCAN) {
                pm.appOps().setUserRestriction(operation, ignored, restriction, 0, null);
            } else {
                pm.appOps().setMode(operation, app.code.uid(), app.name(), ignored
                        ? AppOpsManager.MODE_IGNORED : AppOpsManager.MODE_ALLOWED);
            }
        }
        void listen(App app) throws Exception { listen(app, WifiManager.SCAN_RESULTS_AVAILABLE_ACTION); }
        void listen(App app, String action) throws Exception {
            IIntentReceiver receiver = new IIntentReceiver.Default() {
                final Binder token = new Binder(); @Override public IBinder asBinder() { return token; }
            };
            app.call(() -> am.registerReceiverWithFeature(app.thread, app.name(), null, null, receiver,
                    new IntentFilter(action), null, 0, 0));
        }
        @SuppressWarnings("unchecked") void replace(ApkPackage old, ApkPackage replacement) throws Exception {
            Map<String, ApkPackage> packages = new LinkedHashMap<>((Map<String, ApkPackage>)get(pm, "mPackages"));
            packages.put(old.packageName, replacement); set(pm, "mPackages", packages);
        }
        @Override public void close() throws Exception {
            ((ScheduledThreadPoolExecutor)get(get(am, "mBroadcasts"), "mTimeouts")).shutdownNow();
            ((ScheduledThreadPoolExecutor)get(pm.appOps(), "mEvents")).shutdownNow();
            setOwnUid(Process.SYSTEM_UID);
        }
    }
    private static void identityAndData() throws Exception {
        try (Harness h = new Harness(36, false)) {
            h.owner.call(() -> {
                check(h.wifi.getWifiState(h.owner.name()) == WifiManager.WIFI_STATE_ENABLED, "Real radio state");
                WifiInfo connection = h.wifi.getConnectionInfo(h.owner.name(), "wifi");
                check("\"Fixture AP\"".equals(connection.getSSID()), "Granted location discloses observed SSID");
                check("02:00:00:00:00:00".equals(connection.getMacAddress()), "Local MAC needs its separate privileged grant");
                connection.setBSSID("changed");
                check("02:10:20:30:40:50".equals(h.wifi.getConnectionInfo(h.owner.name(), "wifi").getBSSID()), "WifiInfo is detached");
                DhcpInfo dhcp = h.wifi.getDhcpInfo(h.owner.name()); dhcp.ipAddress = 0;
                check(h.wifi.getDhcpInfo(h.owner.name()).ipAddress == 0x0101a8c0, "DHCP is detached");
                check(h.wifi.getDhcpInfo(h.owner.name()).netmask == 0, "DHCP netmask stays 0 as WifiServiceImpl leaves it");
                List<ScanResult> scans = h.wifi.getScanResults(h.owner.name(), "wifi");
                check(scans.size() == 1 && scans.get(0).timestamp == 123456, "Cached observations retain real timestamps");
                scans.get(0).SSID = "changed"; scans.clear();
                check("Fixture AP".equals(h.wifi.getScanResults(h.owner.name(), "wifi").get(0).SSID), "Scan snapshots are detached");
                check(h.wifi.getConfiguredNetworks(h.owner.name(), "wifi").isEmpty(), "Windows observations are not saved Android configurations");
                rejects(() -> h.wifi.getWifiState("android"), "Package spoof");
                rejects(() -> h.wifi.startScan(h.peer.name(), "wifi"), "Other package's scan claim");
            });
            int read = h.radio.reads;
            h.noAccess.call(() -> {
                rejects(() -> h.wifi.getWifiState(h.noAccess.name()), "ACCESS_WIFI_STATE");
                rejects(() -> h.wifi.getConnectionInfo(h.noAccess.name(), null), "Connection permission");
                rejects(() -> h.wifi.getDhcpInfo(h.noAccess.name()), "DHCP permission");
                rejects(() -> h.wifi.getConfiguredNetworks(h.noAccess.name(), null), "Configured permission");
                rejects(() -> h.wifi.getScanResults(h.noAccess.name(), null), "Results permission");
            });
            check(h.radio.reads == read, "Missing permission never reads native data");
            h.noChange.call(() -> rejects(() -> h.wifi.startScan(h.noChange.name(), null), "CHANGE_WIFI_STATE"));
            as(h.owner.code.uid(), 991919, () -> rejects(() -> h.wifi.getWifiState(h.owner.name()), "Unknown app PID"));
            as(h.peer.code.uid(), h.owner.pid, () -> rejects(() -> h.wifi.getWifiState(h.owner.name()), "UID/PID mismatch"));
            h.owner.process.ready = false;
            h.owner.call(() -> rejects(() -> h.wifi.getWifiState(h.owner.name()), "Unready admission"));
            h.owner.process.ready = true;
            h.owner.process.enabledProcessLifetime = () -> false;
            h.owner.call(() -> rejects(() -> h.wifi.getWifiState(h.owner.name()), "Disabled process generation"));
            h.owner.process.enabledProcessLifetime = null;
            h.radio.duringRead = () -> h.owner.process.dead = true;
            h.owner.call(() -> rejects(() -> h.wifi.getWifiState(h.owner.name()), "Retired during native read"));
            h.radio.duringRead = null;
        }
    }
    private static void scanPolicy() throws Exception {
        for (int target : new int[]{28, 29, 36}) try (Harness h = new Harness(target, false)) {
            h.revoke(h.owner, FINE);
            h.owner.call(() -> {
                check(h.wifi.getScanResults(h.owner.name(), "wifi").size() == (target < 29 ? 1 : 0), "Target SDK selects fine/coarse contract");
                check(h.wifi.startScan(h.owner.name(), "wifi") == (target < 29), "Fine-denied/coarse-granted legacy fallback");
                if (target >= 29) check(WifiManager.UNKNOWN_SSID.equals(h.wifi.getConnectionInfo(h.owner.name(), "wifi").getSSID()), "Connection identifiers redact without location grant");
            });
            if (target < 29) h.radio.finish(true);
            h.grant(h.owner, FINE);
            h.pm.appOps().setUserRestriction(AppOpsManager.OP_FINE_LOCATION, true, h.restriction, 0, null);
            h.owner.call(() -> check(h.wifi.getScanResults(h.owner.name(), "wifi").size() == (target < 29 ? 1 : 0),
                    "Denied fine AppOp falls back to coarse only for legacy targets"));
            h.owner.call(() -> check(WifiManager.UNKNOWN_SSID.equals(
                    h.wifi.getConnectionInfoForNetwork(h.owner.name(), "wifi", 12, true).getSSID()) == (target >= 29),
                    "Connectivity has the same target-aware location fallback"));
            h.pm.appOps().setUserRestriction(AppOpsManager.OP_FINE_LOCATION, false, h.restriction, 0, null);
            h.location = false;
            h.owner.call(() -> {
                check(!h.wifi.startScan(h.owner.name(), null), "Location toggle blocks scans");
                check(h.wifi.getScanResults(h.owner.name(), null).isEmpty(), "Location toggle blocks cached identifiers");
            });
            h.location = true;
            for (int op : new int[]{AppOpsManager.OP_WIFI_SCAN, AppOpsManager.OP_CHANGE_WIFI_STATE}) {
                h.ignored(h.owner, op, true);
                h.owner.call(() -> check(!h.wifi.startScan(h.owner.name(), "wifi"), "Ignored operation blocks physical scan: " + op));
                if (op == AppOpsManager.OP_WIFI_SCAN) h.owner.call(() -> check(h.wifi.getScanResults(h.owner.name(), "wifi").isEmpty(), "Ignored WIFI_SCAN protects cached identifiers"));
                h.ignored(h.owner, op, false);
            }
            h.noAccess.call(() -> check(h.wifi.startScan(h.noAccess.name(), "wifi"), "Scan admission does not require ACCESS_WIFI_STATE or ACCESS_NETWORK_STATE"));
            check(h.radio.begin(), "Granted queued scan stays eligible");
            h.radio.finish(true);
        }
    }
    private static void rejectedAdmission() throws Exception {
        try (Harness h = new Harness(36, false)) {
            h.listen(h.owner); h.listen(h.peer); h.radio.accepted = false;
            h.owner.call(() -> check(!h.wifi.startScan(h.owner.name(), null), "Backend refusal remains a failed start"));
            check(h.owner.thread.broadcasts == 1 && h.peer.thread.broadcasts == 0,
                    "Refused admission reports failure only to its requester");
            check(h.owner.name().equals(h.owner.thread.latest.getPackage())
                    && !h.owner.thread.latest.getBooleanExtra(WifiManager.EXTRA_RESULTS_UPDATED, true),
                    "Refusal broadcast is targeted and does not claim updated observations");
            h.radio.finish(true);
            check(h.owner.thread.broadcasts == 1 && h.peer.thread.broadcasts == 0,
                    "Late callback after refusal cannot publish success");
            h.radio.accepted = true;
            h.owner.call(() -> check(h.wifi.startScan(h.owner.name(), null), "Refusal releases request capacity"));
            h.radio.finish(false);
            check(h.owner.thread.broadcasts == 2 && h.peer.thread.broadcasts == 1,
                    "Failure of an accepted device scan is global");
        }
    }
    private static void queuedAuthority() throws Exception {
        for (String change : List.of("revoke", "dead", "exit", "pid", "disabled", "replace", "remove", "appop", "location")) {
            try (Harness h = new Harness(36, false)) {
                h.owner.call(() -> check(h.wifi.startScan(h.owner.name(), "wifi"), "Queue accepts authorized request"));
                switch (change) {
                    case "revoke" -> h.revoke(h.owner, FINE);
                    case "dead" -> h.owner.process.dead = true;
                    case "exit" -> h.owner.process.exiting = true;
                    case "pid" -> h.owner.process.pidSuperseded = true;
                    case "disabled" -> h.owner.process.enabledProcessLifetime = () -> false;
                    case "replace" -> h.replace(h.owner.code, pkg(h.owner.name(), h.owner.code.uid(), 36, (byte)8, ORDINARY));
                    case "remove" -> h.pm.beginPackageRemoval(h.owner.code);
                    case "appop" -> h.ignored(h.owner, AppOpsManager.OP_WIFI_SCAN, true);
                    case "location" -> h.location = false;
                    default -> throw new AssertionError(change);
                }
                check(!h.radio.begin() && h.radio.physicalScans == 0, "Queued authority rechecked immediately before radio: " + change);
                h.radio.finish(false);
            }
        }
        try (Harness h = new Harness(36, false)) {
            h.listen(h.peer);
            h.owner.call(() -> check(h.wifi.startScan(h.owner.name(), null), "First request accepted"));
            h.peer.call(() -> check(h.wifi.startScan(h.peer.name(), null), "Second authorized request coalesces"));
            h.owner.process.dead = true;
            check(h.radio.starts == 1 && h.radio.begin(), "Another live owner authorizes the shared scan");
            h.radio.finish(true); h.radio.finish(true);
            check(h.peer.thread.broadcasts == 1, "One global result per physical scan, duplicate completions ignored");
            check(h.peer.thread.senderUid == Process.SYSTEM_UID && "android".equals(h.peer.thread.senderPackage), "Protected result has system origin");
            check(h.peer.thread.latest.getBooleanExtra(WifiManager.EXTRA_RESULTS_UPDATED, false), "Successful scan marks updated cache");
            check(h.peer.thread.latest.getExtras().size() == 1 && h.peer.thread.latest.getPackage() == null, "Global completion carries status only");
            h.peer.call(() -> rejects(() -> h.am.broadcastIntentWithFeature(h.peer.thread, null,
                    new Intent(WifiManager.SCAN_RESULTS_AVAILABLE_ACTION), null, null, 0, null, null,
                    null, null, null, AppOpsManager.OP_NONE, null, false, false, 0), "App cannot forge protected completion"));
            h.peer.call(() -> check(h.wifi.startScan(h.peer.name(), null), "Completion releases scan group"));
            check(h.radio.begin(), "Second physical scan begins");
            h.revoke(h.peer, FINE);
            h.radio.finish(true);
            check(h.peer.thread.broadcasts == 2, "A global status event survives initiating grant revocation");
            h.peer.call(() -> check(h.wifi.getScanResults(h.peer.name(), null).isEmpty(), "Revoked app cannot read the newly updated cache"));
        }
    }
    private static void standalone() throws Exception {
        try (Harness h = new Harness(36, true)) {
            setOwnUid(h.owner.code.uid());
            check(Process.myUid() == h.owner.code.uid() && Binder.getCallingUid() == h.owner.code.uid(), "Standalone fixture uses actual app process UID");
            h.listen(h.peer);
            h.owner.call(() -> {
                rejects(() -> h.wifi.getWifiState("android"), "Shared PID does not let APK claim system identity");
                check("\"Fixture AP\"".equals(h.wifi.getConnectionInfoForNetwork(h.owner.name(), null, 12, true).getSSID()),
                        "Standalone network data uses its authenticated application grant");
                check(h.wifi.startScan(h.owner.name(), "wifi"), "Admitted standalone APK can scan");
            });
            check(h.radio.begin(), "Standalone request is revalidated on worker");
            h.owner.call(() -> h.radio.finish(true));
            check(h.peer.thread.broadcasts == 1 && h.peer.thread.senderUid == Process.SYSTEM_UID
                    && "android".equals(h.peer.thread.senderPackage), "Server completion is system-origin even when local process UID is app UID");
            h.revoke(h.owner, FINE);
            h.owner.call(() -> {
                check(!h.wifi.startScan(h.owner.name(), "wifi"), "Standalone does not bypass revoked permission");
                check(WifiManager.UNKNOWN_SSID.equals(h.wifi.getConnectionInfoForNetwork(h.owner.name(), null, 12, true).getSSID()),
                        "Standalone network callback data is redacted after revocation");
            });
            h.owner.process.dead = true;
            rejects(() -> h.wifi.getWifiState("android"), "Retired standalone cannot become local system caller");
        }
    }
    @SuppressWarnings("unchecked") private static void privilegedPolicies() throws Exception {
        for (String exemption : EXEMPTIONS) try (Harness h = new Harness(36, false)) {
            // Installed synthetic platform identity exercises real signature policy,
            // rather than redefining a privileged permission as a normal grant.
            App signed = new App("fixture.wifi.platform" + nextUid, 36, 981005,
                    List.of(ACCESS, CHANGE, NETWORK, exemption));
            ApkPackage platform = pkg("android", Process.SYSTEM_UID, 36, (byte)8, List.of());
            Map<String, ApkPackage> packages = new LinkedHashMap<>((Map<String, ApkPackage>)get(h.pm, "mPackages"));
            packages.put(signed.name(), signed.code); packages.put("android", platform);
            set(h.pm, "mPackages", packages); h.attach(signed, false);
            check(h.pm.permissions().checkPermission(signed.name(), exemption) == PackageManager.PERMISSION_GRANTED, "Actual signature grant: " + exemption);
            h.location = false;
            h.ignored(signed, AppOpsManager.OP_WIFI_SCAN, true);
            signed.call(() -> check(h.wifi.getScanResults(signed.name(), null).size() == 1, "Privileged scan exemption without location or WIFI_SCAN op: " + exemption));
            boolean networkExemption = !exemption.equals("android.permission.NETWORK_MANAGED_PROVISIONING")
                    && !exemption.equals("android.permission.RADIO_SCAN_WITHOUT_LOCATION");
            signed.call(() -> check(WifiManager.UNKNOWN_SSID.equals(
                    h.wifi.getConnectionInfoForNetwork(signed.name(), null, 12, true).getSSID()) != networkExemption,
                    "Connectivity permits only its four location exemptions: " + exemption));
            signed.call(() -> check(WifiManager.UNKNOWN_SSID.equals(
                    h.wifi.getConnectionInfoForNetwork(signed.name(), null, 12, false).getSSID()),
                    "Callback include-location opt-out also applies to privileged callers"));
            signed.call(() -> check(h.wifi.startScan(signed.name(), null), "Privileged scan admission: " + exemption));
            check(h.radio.begin(), "Queued signature grant remains authoritative");
            h.pm.appOps().setMode(AppOpsManager.OP_CHANGE_WIFI_STATE, signed.code.uid(), signed.name(), AppOpsManager.MODE_IGNORED);
            check(h.radio.guard.getAsBoolean() == exemption.equals("android.permission.NETWORK_SETTINGS"), "Only NETWORK_SETTINGS bypasses change operation policy");
        }
        try (Harness h = new Harness(36, false)) {
            String peers = "android.permission.PEERS_MAC_ADDRESS";
            App signed = new App("fixture.wifi.peermac" + nextUid, 36, 981005,
                    List.of(ACCESS, CHANGE, COARSE, BACKGROUND, peers));
            ApkPackage platform = pkg("android", Process.SYSTEM_UID, 36, (byte)8, List.of());
            Map<String, ApkPackage> packages = new LinkedHashMap<>((Map<String, ApkPackage>)get(h.pm, "mPackages"));
            packages.put(signed.name(), signed.code); packages.put("android", platform);
            set(h.pm, "mPackages", packages); h.attach(signed, false);
            h.grant(signed, COARSE);
            h.pm.permissions().updatePermissionFlags(signed.name(), BACKGROUND,
                    PackageManager.FLAG_PERMISSION_APPLY_RESTRICTION | PackageManager.FLAG_PERMISSION_RESTRICTION_SYSTEM_EXEMPT,
                    PackageManager.FLAG_PERMISSION_RESTRICTION_SYSTEM_EXEMPT, true, "default:0", 0);
            h.grant(signed, BACKGROUND);
            check(h.pm.permissions().checkPermission(signed.name(), FINE) == PackageManager.PERMISSION_DENIED,
                    "PEERS_MAC fixture has no fine-location grant");
            signed.call(() -> check(h.wifi.getScanResults(signed.name(), null).size() == 1,
                    "PEERS_MAC bypasses the fine-location grant while WIFI_SCAN is allowed"));
            h.location = false;
            signed.call(() -> check(h.wifi.getScanResults(signed.name(), null).isEmpty(),
                    "PEERS_MAC does not bypass location enabled state"));
            h.location = true; h.ignored(signed, AppOpsManager.OP_WIFI_SCAN, true);
            signed.call(() -> check(h.wifi.getScanResults(signed.name(), null).isEmpty(),
                    "PEERS_MAC does not bypass WIFI_SCAN AppOps"));
        }
    }
    @SuppressWarnings("unchecked") private static void networkObservations() throws Exception {
        try (Harness h = new Harness(36, false)) {
            h.owner.call(() -> {
                WifiInfo first = h.wifi.getConnectionInfoForNetwork(h.owner.name(), "wifi", 12, true);
                check(h.radio.requestedIndex == 12 && "\"Fixture AP\"".equals(first.getSSID()), "Exact network index reaches backend");
                check("\"Second AP\"".equals(h.wifi.getConnectionInfoForNetwork(h.owner.name(), "wifi", 13, true).getSSID()),
                        "A different Network gets its own observation");
                check(WifiManager.UNKNOWN_SSID.equals(h.wifi.getConnectionInfoForNetwork(h.owner.name(), null, 99, true).getSSID()),
                        "Unknown Network does not fall back to the default adapter");
                check(WifiManager.UNKNOWN_SSID.equals(h.wifi.getConnectionInfoForNetwork(h.owner.name(), null, 12, false).getSSID()),
                        "No callback location opt-in means redacted identifiers");
                check("02:00:00:00:00:00".equals(first.getMacAddress()), "Connectivity also respects LOCAL_MAC_ADDRESS");
                rejects(() -> h.wifi.getConnectionInfoForNetwork("android", null, 12, true), "Network package spoof");
            });
            int reads = h.radio.reads;
            h.noAccess.call(() -> rejects(() -> h.wifi.getConnectionInfoForNetwork(h.noAccess.name(), null, 12, true),
                    "Network data requires ACCESS_NETWORK_STATE"));
            check(h.radio.reads == reads, "Missing network permission stops before backend access");
            h.ignored(h.owner, AppOpsManager.OP_WIFI_SCAN, true);
            h.ignored(h.owner, AppOpsManager.OP_CHANGE_WIFI_STATE, true);
            h.owner.call(() -> check("\"Fixture AP\"".equals(h.wifi.getConnectionInfoForNetwork(h.owner.name(), null, 12, true).getSSID()),
                    "Connectivity is independent of scan and change AppOps"));
            h.location = false;
            h.owner.call(() -> check(WifiManager.UNKNOWN_SSID.equals(h.wifi.getConnectionInfoForNetwork(h.owner.name(), null, 12, true).getSSID()),
                    "Location-disabled network data is redacted"));
            h.location = true;
            App networkOnly = new App("fixture.wifi.networkonly" + nextUid, 36, 981006,
                    List.of(NETWORK, FINE, COARSE, BACKGROUND));
            Map<String, ApkPackage> packages = new LinkedHashMap<>((Map<String, ApkPackage>)get(h.pm, "mPackages"));
            packages.put(networkOnly.name(), networkOnly.code); set(h.pm, "mPackages", packages);
            h.attach(networkOnly, false); h.grant(networkOnly, FINE);
            h.pm.permissions().updatePermissionFlags(networkOnly.name(), BACKGROUND,
                    PackageManager.FLAG_PERMISSION_APPLY_RESTRICTION | PackageManager.FLAG_PERMISSION_RESTRICTION_SYSTEM_EXEMPT,
                    PackageManager.FLAG_PERMISSION_RESTRICTION_SYSTEM_EXEMPT, true, "default:0", 0);
            h.grant(networkOnly, BACKGROUND);
            networkOnly.call(() -> check("\"Fixture AP\"".equals(h.wifi.getConnectionInfoForNetwork(networkOnly.name(), null, 12, true).getSSID()),
                    "Network query does not require either Wi-Fi manager permission"));
            h.radio.duringRead = () -> networkOnly.process.dead = true;
            networkOnly.call(() -> rejects(() -> h.wifi.getConnectionInfoForNetwork(networkOnly.name(), null, 12, true),
                    "Network caller retired during backend read"));
            h.radio.duringRead = null;
            h.replace(h.owner.code, pkg(h.owner.name(), h.owner.code.uid(), 36, (byte)8, ORDINARY));
            h.owner.call(() -> rejects(() -> h.wifi.getConnectionInfoForNetwork(h.owner.name(), null, 12, true),
                    "Reinstalled package cannot inherit old network disclosure authority"));
        }
    }
    /** Scripted user: the fixture decides what the approval dialogs answer. */
    static final class ScriptedUi implements WifiConnections.Ui {
        volatile boolean available = true;
        volatile int choice = 0, decision = PermissionRequests.ALLOWED;
        final List<String> shown = new java.util.concurrent.CopyOnWriteArrayList<>();
        volatile String[] labels;
        @Override public boolean available() { return available; }
        @Override public int choose(String packageName, String title, String[] labels, BooleanSupplier active) {
            shown.add("choose " + packageName); this.labels = labels; return choice;
        }
        @Override public int confirm(String packageName, String text) { shown.add("confirm " + packageName); return decision; }
    }
    /** Client end recording service events (delivered through the local-Binder path). */
    private static final class Events extends com.gesher.net.IWindowsWifi.Callback {
        final List<Integer> values = new ArrayList<>();
        @Override protected void onEvent(int kind, int value, Bundle extras) { values.add(kind * 100 + value); }
    }
    private static void capabilitiesAndEvents() throws Exception {
        try (Harness h = new Harness(36, false)) {
            int erp = 1 << 6, ofdm = 1 << 4, ht = 1 << 7, vht = 1 << 8, he = 1 << 10;
            h.owner.call(() -> check(h.wifi.getCapabilities(h.owner.name(), 0) == 0
                    && h.wifi.getCapabilities(h.owner.name(), 1) == 0, "No adapter: no feature, band or standard"));
            h.radio.hardware = new com.gesher.net.WindowsWifi.Hardware(1, erp | ofdm | ht | vht | he,
                    (1 << 7) | (1 << 9) | (1 << 10), (1 << 4) | (1 << 31));
            h.owner.call(() -> {
                long features = h.wifi.getCapabilities(h.owner.name(), com.gesher.net.IWindowsWifi.CAPABILITIES_FEATURES);
                check(features == (com.gesher.net.IWindowsWifi.FEATURE_WPA3_SAE | com.gesher.net.IWindowsWifi.FEATURE_OWE
                        | com.gesher.net.IWindowsWifi.FEATURE_WPA_PERSONAL | com.gesher.net.IWindowsWifi.FEATURE_WEP),
                        "Auth/cipher pairs map to SAE, OWE, WPA personal and WEP only");
                long bands = h.wifi.getCapabilities(h.owner.name(), com.gesher.net.IWindowsWifi.CAPABILITIES_BANDS);
                int shift = com.gesher.net.IWindowsWifi.STANDARD_SHIFT;
                check((bands & 0xff) == (com.gesher.net.IWindowsWifi.BAND_24_GHZ | com.gesher.net.IWindowsWifi.BAND_5_GHZ),
                        "ERP and OFDM/VHT establish 2.4 and 5 GHz; no 6 GHz without evidence");
                check(bands >>> shift == ((1L << ScanResult.WIFI_STANDARD_LEGACY) | (1L << ScanResult.WIFI_STANDARD_11N)
                        | (1L << ScanResult.WIFI_STANDARD_11AC) | (1L << ScanResult.WIFI_STANDARD_11AX)),
                        "PHY types map to exactly the supported standards");
            });
            h.radio.observedLow = 5955; h.radio.observedHigh = 5955;
            h.owner.call(() -> check((h.wifi.getCapabilities(h.owner.name(), 1) & com.gesher.net.IWindowsWifi.BAND_6_GHZ) != 0,
                    "An observed 6 GHz BSS establishes 6 GHz support"));
            h.radio.observedLow = -1; h.radio.observedHigh = -1;
            h.owner.call(() -> check((h.wifi.getCapabilities(h.owner.name(), 1) & com.gesher.net.IWindowsWifi.BAND_6_GHZ) != 0,
                    "Observed 6 GHz reception stays a hardware capability when that BSS leaves"));
            com.gesher.net.WindowsWifi.Hardware census = h.radio.hardware;
            h.radio.hardware = new com.gesher.net.WindowsWifi.Hardware(census.interfaces, census.phyTypes,
                    census.authAlgorithms, census.ciphers);
            h.owner.call(() -> check((h.wifi.getCapabilities(h.owner.name(), 1) & com.gesher.net.IWindowsWifi.BAND_6_GHZ) == 0,
                    "A new adapter census needs its own 6 GHz evidence"));
            h.radio.observedLow = 5955; h.radio.observedHigh = 5955;
            h.noAccess.call(() -> {
                rejects(() -> h.wifi.getCapabilities(h.noAccess.name(), 0), "Feature group requires ACCESS_WIFI_STATE");
                check(h.wifi.getCapabilities(h.noAccess.name(), 1) != 0, "Band group carries no permission");
                rejects(() -> h.wifi.getChannelData(h.noAccess.name(), null), "Channel data requires NEARBY_WIFI_DEVICES");
            });
            h.grant(h.owner, NEARBY);
            h.owner.call(() -> {
                List<Bundle> channels = h.wifi.getChannelData(h.owner.name(), "wifi");
                check(channels.size() == 1 && channels.get(0).getInt(WifiManager.CHANNEL_DATA_KEY_FREQUENCY_MHZ) == 5180
                        && channels.get(0).getInt(WifiManager.CHANNEL_DATA_KEY_NUM_AP) == 1, "Channel data counts real cached APs");
            });
            h.radio.ap.level = -85;
            h.owner.call(() -> check(h.wifi.getChannelData(h.owner.name(), "wifi").isEmpty(),
                    "APs weaker than -80 dBm are not counted"));

            Events ownerScan = new Events(), ownerState = new Events(), peerScan = new Events();
            h.owner.call(() -> {
                h.wifi.registerCallback(h.owner.name(), "wifi", com.gesher.net.IWindowsWifi.CALLBACK_SCAN_RESULTS, ownerScan);
                h.wifi.registerCallback(h.owner.name(), "wifi", com.gesher.net.IWindowsWifi.CALLBACK_WIFI_STATE, ownerState);
                try { h.wifi.registerCallback(h.owner.name(), null, 42, new Events()); throw new AssertionError("Unknown callback kind"); }
                catch (IllegalArgumentException expected) { checks++; }
            });
            check(ownerState.values.equals(List.of(200 + WifiManager.WIFI_STATE_ENABLED)),
                    "State listener reports the current state once on registration");
            check(ownerScan.values.isEmpty(), "Scan callback is not invoked on registration");
            h.peer.call(() -> h.wifi.registerCallback(h.peer.name(), null, com.gesher.net.IWindowsWifi.CALLBACK_SCAN_RESULTS, peerScan));
            h.noAccess.call(() -> rejects(() -> h.wifi.registerCallback(h.noAccess.name(), null,
                    com.gesher.net.IWindowsWifi.CALLBACK_SCAN_RESULTS, new Events()), "Callbacks require ACCESS_WIFI_STATE"));
            h.listen(h.peer);
            h.listen(h.peer, WifiManager.WIFI_STATE_CHANGED_ACTION);
            // A Windows-initiated scan completion: global broadcast and every callback.
            h.radio.events.onScanResultsAvailable();
            check(ownerScan.values.size() == 1 && peerScan.values.size() == 1, "External scan reaches each scan callback");
            check(h.peer.thread.broadcasts == 1 && h.peer.thread.latest.getBooleanExtra(WifiManager.EXTRA_RESULTS_UPDATED, false)
                    && h.peer.thread.senderUid == Process.SYSTEM_UID, "External scan is a system-origin updated result");
            // A requested scan's completion also notifies callbacks once.
            h.owner.call(() -> check(h.wifi.startScan(h.owner.name(), "wifi"), "Requested scan admitted"));
            check(h.radio.begin(), "Requested scan begins"); h.radio.finish(true);
            check(ownerScan.values.size() == 2 && peerScan.values.size() == 2, "Requested scan completion reaches callbacks");
            h.radio.finish(false);
            check(ownerScan.values.size() == 2, "A failed scan is not results-available");
            h.peer.process.exiting = true;
            h.radio.events.onScanResultsAvailable();
            check(peerScan.values.size() == 2 && ownerScan.values.size() == 3, "An exiting app's callback is not delivered");
            h.peer.process.exiting = false;
            h.radio.events.onScanResultsAvailable();
            check(peerScan.values.size() == 2 && ownerScan.values.size() == 4, "A dropped registration does not return");
            h.owner.call(() -> h.wifi.unregisterCallback(h.owner.name(), com.gesher.net.IWindowsWifi.CALLBACK_SCAN_RESULTS, ownerScan));
            h.radio.events.onScanResultsAvailable();
            check(ownerScan.values.size() == 4, "Unregistered callback receives nothing");
            int before = h.peer.thread.broadcasts;
            h.radio.events.onWifiStateChanged(WifiManager.WIFI_STATE_ENABLED, WifiManager.WIFI_STATE_DISABLED);
            check(ownerState.values.get(ownerState.values.size() - 1) == 200 + WifiManager.WIFI_STATE_DISABLED,
                    "State listener follows the real radio change");
            check(h.peer.thread.broadcasts == before + 1
                    && WifiManager.WIFI_STATE_CHANGED_ACTION.equals(h.peer.thread.latest.getAction())
                    && h.peer.thread.latest.getIntExtra(WifiManager.EXTRA_WIFI_STATE, -1) == WifiManager.WIFI_STATE_DISABLED
                    && h.peer.thread.latest.getIntExtra(WifiManager.EXTRA_PREVIOUS_WIFI_STATE, -1) == WifiManager.WIFI_STATE_ENABLED,
                    "WIFI_STATE_CHANGED carries the new and previous states");
            Intent sticky = StickyBroadcasts.match(new IntentFilter(WifiManager.WIFI_STATE_CHANGED_ACTION));
            check(sticky != null && sticky.getIntExtra(WifiManager.EXTRA_WIFI_STATE, -1) == WifiManager.WIFI_STATE_DISABLED,
                    "WIFI_STATE_CHANGED is sticky");
            int states = ownerState.values.size();
            h.radio.events.onWifiStateChanged(WifiManager.WIFI_STATE_DISABLED, WifiManager.WIFI_STATE_UNKNOWN);
            check(h.peer.thread.broadcasts == before + 1 && ownerState.values.size() == states + 1,
                    "UNKNOWN is reported to listeners but never broadcast");
            h.owner.process.dead = true;
            h.radio.events.onWifiStateChanged(WifiManager.WIFI_STATE_UNKNOWN, WifiManager.WIFI_STATE_ENABLED);
            check(ownerState.values.size() == states + 1, "A retired process's registrations are dropped");
        }
    }
    private static void await(BooleanSupplier condition, String message) throws Exception {
        long deadline = System.nanoTime() + 10_000_000_000L;
        while (!condition.getAsBoolean() && System.nanoTime() < deadline) Thread.sleep(5);
        check(condition.getAsBoolean(), message);
    }
    /** A request's client end: records NETWORK_* events and their extras. */
    private static final class Req extends com.gesher.net.IWindowsWifi.Callback {
        final List<Integer> events = new java.util.concurrent.CopyOnWriteArrayList<>();
        volatile Bundle available;
        @Override protected void onEvent(int kind, int value, Bundle extras) {
            if (value == com.gesher.net.IWindowsWifi.NETWORK_AVAILABLE) available = extras;
            events.add(value);
        }
    }
    private static final class Failures extends com.gesher.net.IWindowsWifi.Callback {
        final List<Integer> codes = new java.util.concurrent.CopyOnWriteArrayList<>();
        volatile android.net.wifi.WifiNetworkSpecifier specifier;
        @Override protected void onEvent(int kind, int value, Bundle extras) {
            if (extras != null) specifier = extras.getParcelable(com.gesher.net.IWindowsWifi.EXTRA_SPECIFIER,
                    android.net.wifi.WifiNetworkSpecifier.class);
            codes.add(value);
        }
    }
    private static android.net.wifi.WifiNetworkSpecifier open(String ssid) {
        return new android.net.wifi.WifiNetworkSpecifier.Builder().setSsid(ssid).build();
    }
    private static void specifierRequests() throws Exception {
        WifiConnections.ipWaitMs = 300;
        try (Harness h = new Harness(36, false)) {
            h.owner.process.inProcessActivityState = ActivityTaskManagerService.State.RESUMED;
            Failures failures = new Failures();
            h.owner.call(() -> h.wifi.registerCallback(h.owner.name(), null,
                    com.gesher.net.IWindowsWifi.CALLBACK_LOCAL_ONLY_FAILURE, failures));
            h.noAccess.call(() -> rejects(() -> h.wifi.requestLocalNetwork(h.noAccess.name(), null, open("Fixture AP"),
                    new Req()), "Requests need CHANGE_NETWORK_STATE"));
            // Happy path: dialog, Windows profile, real completion, IPv4, then AVAILABLE.
            Req first = new Req();
            h.owner.call(() -> h.wifi.requestLocalNetwork(h.owner.name(), "wifi", open("Fixture AP"), first));
            await(() -> first.events.size() == 1, "Approved request completes");
            check(first.events.get(0) == com.gesher.net.IWindowsWifi.NETWORK_AVAILABLE
                    && first.available.getInt(com.gesher.net.IWindowsWifi.EXTRA_INTERFACE) == 12,
                    "AVAILABLE names the joined interface");
            check(h.ui.shown.equals(List.of("choose " + h.owner.name())) && h.ui.labels.length == 1
                    && h.ui.labels[0].startsWith("Fixture AP"), "The user chose among real matching networks");
            check(h.radio.calls.contains("connect 12 true")
                    && h.radio.lastXml.contains("<connectionMode>manual</connectionMode>")
                    && h.radio.lastXml.contains("<authentication>open</authentication>"),
                    "A manual, open, BSSID-pinned temporary profile was used");
            check(h.radio.lastXml.contains("<hex>46697874757265204150</hex>"),
                    "The SSID's bytes reach Windows unchanged");
            h.owner.call(() -> h.wifi.releaseLocalNetwork(h.owner.name(), first));
            check(h.radio.calls.contains("disconnect true Home") && "Home".equals(h.radio.current)
                    && h.radio.stored.isEmpty(), "Release deletes the profile and returns to the previous network");
            // An open network approved before still asks when the request does not name the
            // approved BSSID (WifiNetworkFactory requires the exact BSSID for open networks).
            Req openAgain = new Req();
            h.owner.call(() -> h.wifi.requestLocalNetwork(h.owner.name(), "wifi", open("Fixture AP"), openAgain));
            await(() -> openAgain.events.size() == 1, "Open re-request completes");
            check(h.ui.shown.size() == 2, "An open network without the approved BSSID asks the user again");
            h.owner.call(() -> h.wifi.releaseLocalNetwork(h.owner.name(), openAgain));
            // A request naming the approved access point is joined without asking again.
            Req again = new Req();
            h.owner.call(() -> h.wifi.requestLocalNetwork(h.owner.name(), "wifi", new android.net.wifi.WifiNetworkSpecifier
                    .Builder().setSsid("Fixture AP").setBssid(android.net.MacAddress.fromString("02:10:20:30:40:50")).build(),
                    again));
            await(() -> again.events.size() == 1, "Approved network re-request completes");
            check(h.ui.shown.size() == 2, "The approved access point skips the dialog");
            // A background app cannot end another app's connection: its request is unfulfillable.
            Req intruder = new Req();
            h.peer.process.inProcessActivityState = null;
            h.peer.call(() -> h.wifi.requestLocalNetwork(h.peer.name(), null, open("Fixture AP"), intruder));
            await(() -> intruder.events.size() == 1, "Background request completes");
            check(intruder.events.get(0) == com.gesher.net.IWindowsWifi.NETWORK_UNAVAILABLE
                    && again.events.equals(List.of(com.gesher.net.IWindowsWifi.NETWORK_AVAILABLE)),
                    "A background request leaves the active request connected");
            // A new request replaces the connected one, which loses its network.
            h.ui.choice = -1;
            Req rejected = new Req();
            h.owner.call(() -> h.wifi.requestLocalNetwork(h.owner.name(), "wifi",
                    new android.net.wifi.WifiNetworkSpecifier.Builder().setSsidPattern(
                            new android.os.PatternMatcher("Fixture", android.os.PatternMatcher.PATTERN_PREFIX)).build(),
                    rejected));
            await(() -> rejected.events.size() == 1, "Rejected request completes");
            check(again.events.equals(List.of(com.gesher.net.IWindowsWifi.NETWORK_AVAILABLE,
                    com.gesher.net.IWindowsWifi.NETWORK_LOST)), "The replaced connected request loses its network");
            check(rejected.events.get(0) == com.gesher.net.IWindowsWifi.NETWORK_UNAVAILABLE
                    && failures.codes.contains(WifiManager.STATUS_LOCAL_ONLY_CONNECTION_FAILURE_USER_REJECT)
                    && failures.specifier != null, "A pattern request always asks; rejection is USER_REJECT");
            // Authentication failure: not retried, profile removed, previous network restored.
            h.ui.choice = 0; h.radio.outcome = WifiManager.STATUS_LOCAL_ONLY_CONNECTION_FAILURE_AUTHENTICATION;
            int connects = (int) h.radio.calls.stream().filter(c -> c.startsWith("connect")).count();
            Req auth = new Req();
            h.owner.call(() -> h.wifi.requestLocalNetwork(h.owner.name(), "wifi", open("Fixture AP"), auth));
            await(() -> auth.events.size() == 1, "Failed request completes");
            check(auth.events.get(0) == com.gesher.net.IWindowsWifi.NETWORK_UNAVAILABLE
                    && failures.codes.get(failures.codes.size() - 1) == WifiManager.STATUS_LOCAL_ONLY_CONNECTION_FAILURE_AUTHENTICATION
                    && h.radio.calls.stream().filter(c -> c.startsWith("connect")).count() == connects + 1
                    && h.radio.stored.isEmpty(), "Wrong credentials fail once, undone, never AVAILABLE");
            // Association failures are retried three times.
            h.radio.outcome = WifiManager.STATUS_LOCAL_ONLY_CONNECTION_FAILURE_ASSOCIATION;
            Req association = new Req();
            h.owner.call(() -> h.wifi.requestLocalNetwork(h.owner.name(), "wifi", open("Fixture AP"), association));
            await(() -> association.events.size() == 1, "Association failure completes");
            check(h.radio.calls.stream().filter(c -> c.startsWith("connect")).count() == connects + 4,
                    "Association failures are retried up to three attempts");
            // Associated without an address: IP provisioning failure.
            h.radio.outcome = com.gesher.net.WindowsWifi.CONNECTED; h.radio.ipv4 = false;
            Req noIp = new Req();
            h.owner.call(() -> h.wifi.requestLocalNetwork(h.owner.name(), "wifi", open("Fixture AP"), noIp));
            await(() -> noIp.events.size() == 1, "IP failure completes");
            check(noIp.events.get(0) == com.gesher.net.IWindowsWifi.NETWORK_UNAVAILABLE
                    && failures.codes.get(failures.codes.size() - 1) == WifiManager.STATUS_LOCAL_ONLY_CONNECTION_FAILURE_IP_PROVISIONING
                    && "Home".equals(h.radio.current), "No IPv4 address is IP_PROVISIONING, not availability");
            h.radio.ipv4 = true;
            // A dropped connection is lost.
            Req drop = new Req();
            h.owner.call(() -> h.wifi.requestLocalNetwork(h.owner.name(), "wifi", open("Fixture AP"), drop));
            await(() -> drop.events.size() == 1, "Connection for drop test");
            long disconnectsBefore = h.radio.calls.stream().filter(c -> c.startsWith("disconnect")).count();
            h.radio.current = "Other"; h.radio.events.onConnectionChanged();
            await(() -> drop.events.size() == 2, "Drop is reported");
            check(drop.events.get(1) == com.gesher.net.IWindowsWifi.NETWORK_LOST, "Windows dropping the network is LOST");
            await(() -> h.radio.stored.isEmpty(), "The lost request's profile is deleted");
            check("Other".equals(h.radio.current) && h.radio.calls.stream().filter(c -> c.startsWith("disconnect")).count() == disconnectsBefore,
                    "A network the user chose meanwhile is kept, not replaced by the previous one");
            // No matching network: WifiNetworkFactory keeps scanning periodically until the
            // request ends; nothing is reported, asked or joined meanwhile.
            h.radio.autoComplete = true;
            WifiConnections.scanIntervalMs = 20;
            int shown = h.ui.shown.size();
            int failuresBefore = failures.codes.size();
            int scansBefore = h.radio.starts;
            Req missing = new Req();
            h.owner.call(() -> h.wifi.requestLocalNetwork(h.owner.name(), "wifi", open("Missing"), missing));
            await(() -> h.radio.starts >= scansBefore + 5, "The search keeps scanning past three scans");
            check(missing.events.isEmpty() && failures.codes.size() == failuresBefore && h.ui.shown.size() == shown,
                    "A request still searching reports nothing and asks nothing");
            h.owner.call(() -> h.wifi.releaseLocalNetwork(h.owner.name(), missing));
            Thread.sleep(150);
            int scansAfter = h.radio.starts;
            Thread.sleep(150);
            check(h.radio.starts == scansAfter && missing.events.isEmpty(), "Releasing the request ends the search");
            // An approved network that is out of range: the dialog would be skipped, so the
            // request gives up after USER_APPROVED_SCAN_RETRY_MAX scans, as unfulfillable.
            WifiSsid visible = h.radio.ap.getWifiSsid();
            h.radio.ap.setWifiSsid(WifiSsid.fromBytes("Gone".getBytes(java.nio.charset.StandardCharsets.UTF_8)));
            int approvedScans = h.radio.starts;
            Req away = new Req();
            h.owner.call(() -> h.wifi.requestLocalNetwork(h.owner.name(), "wifi", new android.net.wifi.WifiNetworkSpecifier
                    .Builder().setSsid("Fixture AP").setBssid(android.net.MacAddress.fromString("02:10:20:30:40:50")).build(), away));
            await(() -> away.events.size() == 1, "Approved but absent network completes");
            check(away.events.get(0) == com.gesher.net.IWindowsWifi.NETWORK_UNAVAILABLE
                    && h.radio.starts - approvedScans == WifiConnections.SCAN_ROUNDS
                    && failures.codes.size() == failuresBefore && h.ui.shown.size() == shown,
                    "An approved, absent network is unfulfillable after three scans, without a failure code");
            h.radio.ap.setWifiSsid(visible);
            WifiConnections.scanIntervalMs = 10_000;
            // Unfulfillable: Wi-Fi off, or a background requester.
            h.radio.state = WifiManager.WIFI_STATE_DISABLED;
            Req off = new Req();
            h.owner.call(() -> h.wifi.requestLocalNetwork(h.owner.name(), "wifi", open("Fixture AP"), off));
            await(() -> off.events.size() == 1, "Wi-Fi off completes");
            check(off.events.get(0) == com.gesher.net.IWindowsWifi.NETWORK_UNAVAILABLE, "Wi-Fi off is unavailable");
            h.radio.state = WifiManager.WIFI_STATE_ENABLED;
            h.owner.process.inProcessActivityState = null;
            int calls = h.radio.calls.size();
            Req background = new Req();
            h.owner.call(() -> h.wifi.requestLocalNetwork(h.owner.name(), "wifi", open("Fixture AP"), background));
            await(() -> background.events.size() == 1, "Background request completes");
            check(background.events.get(0) == com.gesher.net.IWindowsWifi.NETWORK_UNAVAILABLE
                    && h.radio.calls.size() == calls, "A background app's request never touches Windows");
            // Without a host dialog nothing can be approved.
            h.owner.process.inProcessActivityState = ActivityTaskManagerService.State.RESUMED;
            h.ui.available = false;
            Req noUi = new Req();
            h.owner.call(() -> h.wifi.requestLocalNetwork(h.owner.name(), "wifi", new android.net.wifi.WifiNetworkSpecifier
                    .Builder().setSsidPattern(new android.os.PatternMatcher("Fix", android.os.PatternMatcher.PATTERN_PREFIX))
                    .build(), noUi));
            await(() -> noUi.events.size() == 1, "No-UI request completes");
            check(noUi.events.get(0) == com.gesher.net.IWindowsWifi.NETWORK_UNAVAILABLE && h.radio.calls.size() == calls,
                    "No approval surface: unavailable, Windows unchanged");
        } finally {
            WifiConnections.ipWaitMs = 20_000;
        }
    }
    private static void suggestions() throws Exception {
        try (Harness h = new Harness(36, false)) {
            Events approval = new Events();
            h.owner.call(() -> h.wifi.registerCallback(h.owner.name(), null,
                    com.gesher.net.IWindowsWifi.CALLBACK_SUGGESTION_APPROVAL, approval));
            check(approval.values.equals(List.of(300 + WifiManager.STATUS_SUGGESTION_APPROVAL_UNKNOWN)),
                    "No suggestions: UNKNOWN on registration");
            android.net.wifi.WifiNetworkSuggestion wpa2 = new android.net.wifi.WifiNetworkSuggestion.Builder()
                    .setSsid("Fixture AP").setWpa2Passphrase("fixture-passphrase").setIsAppInteractionRequired(true).build();
            h.noChange.call(() -> rejects(() -> h.wifi.addNetworkSuggestions(h.noChange.name(), null, List.of(wpa2)),
                    "Suggestions need CHANGE_WIFI_STATE"));
            h.owner.call(() -> check(h.wifi.addNetworkSuggestions(h.owner.name(), "wifi", List.of(wpa2))
                    == WifiManager.STATUS_NETWORK_SUGGESTIONS_SUCCESS, "Valid suggestion accepted"));
            await(() -> approval.values.contains(300 + WifiManager.STATUS_SUGGESTION_APPROVAL_APPROVED_BY_USER),
                    "User approval reported");
            check(approval.values.contains(300 + WifiManager.STATUS_SUGGESTION_APPROVAL_PENDING),
                    "Pending until the user answers");
            await(() -> h.radio.calls.contains("store Gesher-s-"), "Approved suggestion becomes a Windows profile");
            check(h.radio.lastXml.contains("<connectionMode>auto</connectionMode>")
                    && h.radio.lastXml.contains("<authentication>WPA2PSK</authentication>")
                    && h.radio.lastXml.contains("<keyMaterial>fixture-passphrase</keyMaterial>"),
                    "Windows may join it automatically with its own credentials");
            h.owner.call(() -> check(h.wifi.getNetworkSuggestions(h.owner.name()).equals(List.of(wpa2)),
                    "The app reads back its own suggestions"));
            h.peer.call(() -> check(h.wifi.getNetworkSuggestions(h.peer.name()).isEmpty(), "Other apps see none"));
            // A non-UTF-8 SSID (WifiSsid bytes, Android's unquoted hex form) is a valid suggestion.
            android.net.wifi.WifiNetworkSuggestion raw = new android.net.wifi.WifiNetworkSuggestion.Builder()
                    .setWifiSsid(WifiSsid.fromBytes(new byte[] {(byte) 0xff, (byte) 0xfe, 0x41}))
                    .setWpa2Passphrase("fixture-passphrase").build();
            h.owner.call(() -> check(h.wifi.addNetworkSuggestions(h.owner.name(), "wifi", List.of(raw))
                    == WifiManager.STATUS_NETWORK_SUGGESTIONS_SUCCESS, "A non-UTF-8 SSID suggestion is accepted"));
            await(() -> h.radio.lastXml != null && h.radio.lastXml.contains("<hex>FFFE41</hex>"),
                    "Its exact bytes reach the Windows profile");
            h.owner.call(() -> check(h.wifi.removeNetworkSuggestions(h.owner.name(), null, List.of(raw),
                    WifiManager.ACTION_REMOVE_SUGGESTION_DISCONNECT) == WifiManager.STATUS_NETWORK_SUGGESTIONS_SUCCESS,
                    "The non-UTF-8 suggestion is removed again"));
            List<android.net.wifi.WifiNetworkSuggestion> invalid = new ArrayList<>(); invalid.add(null);
            h.owner.call(() -> check(h.wifi.addNetworkSuggestions(h.owner.name(), null, invalid)
                    == WifiManager.STATUS_NETWORK_SUGGESTIONS_ERROR_ADD_INVALID, "Null entry is ADD_INVALID"));
            // Post-connection broadcast when Windows joins an app-interaction suggestion.
            h.listen(h.owner, WifiManager.ACTION_WIFI_NETWORK_SUGGESTION_POST_CONNECTION);
            String profile = h.radio.stored.iterator().next();
            // Without the fine-location app op (enforceCanAccessScanResults) nothing is sent.
            h.pm.appOps().setUserRestriction(AppOpsManager.OP_FINE_LOCATION, true, h.restriction, 0, null);
            h.radio.current = profile; h.radio.events.onConnectionChanged();
            Thread.sleep(300);
            check(h.owner.thread.latest == null || !WifiManager.ACTION_WIFI_NETWORK_SUGGESTION_POST_CONNECTION
                    .equals(h.owner.thread.latest.getAction()), "No post-connection broadcast without the location app op");
            h.pm.appOps().setUserRestriction(AppOpsManager.OP_FINE_LOCATION, false, h.restriction, 0, null);
            h.radio.current = "Home"; h.radio.events.onConnectionChanged();
            Thread.sleep(200);
            h.radio.current = profile; h.radio.events.onConnectionChanged();
            await(() -> h.owner.thread.latest != null && WifiManager.ACTION_WIFI_NETWORK_SUGGESTION_POST_CONNECTION
                    .equals(h.owner.thread.latest.getAction()), "Post-connection broadcast delivered");
            check(wpa2.equals(h.owner.thread.latest.getParcelableExtra(WifiManager.EXTRA_NETWORK_SUGGESTION,
                    android.net.wifi.WifiNetworkSuggestion.class)) && h.owner.name().equals(h.owner.thread.latest.getPackage()),
                    "Explicit to the suggesting app with its suggestion");
            // disallowCurrentSuggestedNetwork disconnects and blocks the network.
            h.owner.call(() -> h.wifi.disallowCurrentSuggestedNetwork(h.owner.name(),
                    new android.net.wifi.BlockingOption.Builder(3600).build()));
            check(h.radio.calls.contains("disconnect true null") && !h.radio.stored.contains(profile),
                    "The suggesting app can disallow its current network");
            h.owner.call(() -> {
                check(h.wifi.removeNetworkSuggestions(h.owner.name(), null, List.of(new android.net.wifi.WifiNetworkSuggestion
                        .Builder().setSsid("Unknown").build()), WifiManager.ACTION_REMOVE_SUGGESTION_DISCONNECT)
                        == WifiManager.STATUS_NETWORK_SUGGESTIONS_ERROR_REMOVE_INVALID, "Unknown removal is REMOVE_INVALID");
                check(h.wifi.removeNetworkSuggestions(h.owner.name(), null, List.of(), 9)
                        == WifiManager.STATUS_NETWORK_SUGGESTIONS_ERROR_REMOVE_INVALID, "Unknown action is REMOVE_INVALID");
                check(h.wifi.removeNetworkSuggestions(h.owner.name(), null, List.of(),
                        WifiManager.ACTION_REMOVE_SUGGESTION_DISCONNECT) == WifiManager.STATUS_NETWORK_SUGGESTIONS_SUCCESS
                        && h.wifi.getNetworkSuggestions(h.owner.name()).isEmpty(), "Empty list removes all");
            });
            // "Don't allow": the app's CHANGE_WIFI_STATE operation is ignored afterwards.
            h.ui.decision = PermissionRequests.DENIED;
            Events peerApproval = new Events();
            h.peer.call(() -> {
                h.wifi.registerCallback(h.peer.name(), null, com.gesher.net.IWindowsWifi.CALLBACK_SUGGESTION_APPROVAL, peerApproval);
                check(h.wifi.addNetworkSuggestions(h.peer.name(), null, List.of(wpa2))
                        == WifiManager.STATUS_NETWORK_SUGGESTIONS_SUCCESS, "Peer suggestion accepted pending approval");
            });
            await(() -> peerApproval.values.contains(300 + WifiManager.STATUS_SUGGESTION_APPROVAL_REJECTED_BY_USER),
                    "Rejection reported");
            h.peer.call(() -> {
                check(h.wifi.addNetworkSuggestions(h.peer.name(), null, List.of(wpa2))
                        == WifiManager.STATUS_NETWORK_SUGGESTIONS_ERROR_APP_DISALLOWED, "Disallowed app cannot suggest");
                check(h.wifi.getNetworkSuggestions(h.peer.name()).isEmpty(), "Rejected app's suggestions are removed");
            });
            check(h.radio.stored.stream().noneMatch(name -> name.startsWith("Gesher-s-")),
                    "No Windows profile exists for removed or rejected suggestions");
            // WLAN event bursts are coalesced: at most one queued pass at a time.
            Thread.sleep(200);
            int reads = h.radio.profileReads.get();
            for (int i = 0; i < 500; i++) h.radio.events.onConnectionChanged();
            Thread.sleep(500);
            check(h.radio.profileReads.get() - reads < 50, "A burst of WLAN events runs a bounded number of passes");
            // Uninstall drops the app's suggestions and Windows profiles at once.
            h.owner.call(() -> check(h.wifi.addNetworkSuggestions(h.owner.name(), "wifi", List.of(wpa2))
                    == WifiManager.STATUS_NETWORK_SUGGESTIONS_SUCCESS, "Suggestion added again"));
            await(() -> h.radio.stored.stream().anyMatch(name -> name.startsWith("Gesher-s-")), "Profile stored again");
            h.pm.packageRemoved(h.owner.code, new Object());
            await(() -> h.radio.stored.stream().noneMatch(name -> name.startsWith("Gesher-s-")),
                    "An uninstalled app's suggestion profiles are deleted at once");
            h.owner.call(() -> check(h.wifi.getNetworkSuggestions(h.owner.name()).isEmpty(),
                    "An uninstalled app's suggestions are dropped"));
        }
    }
    public static void main(String[] args) throws Exception {
        System.load(args[0]); setOwnUid(Process.SYSTEM_UID);
        android.app.PropertyInvalidatedCache.setTestMode(true);
        android.app.PropertyInvalidatedCache.disableForTestMode();
        field(Class.forName("com.gesher.app.locale.SystemLocale"), "sIs24Hour").set(null, Boolean.TRUE);
        identityAndData(); scanPolicy(); queuedAuthority(); rejectedAdmission(); standalone(); privilegedPolicies(); networkObservations();
        capabilitiesAndEvents(); specifierRequests(); suggestions();
        System.out.println("WifiServiceHost passed: " + checks + " checks; real permission/AppOps/broadcast Java, radio=fixture");
    }
}
