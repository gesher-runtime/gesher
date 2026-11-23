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
    private static final String[] EXEMPTIONS = {
        "android.permission.NETWORK_SETTINGS", "android.permission.NETWORK_SETUP_WIZARD",
        "android.permission.NETWORK_MANAGED_PROVISIONING", "android.permission.NETWORK_STACK",
        "android.permission.MAINLINE_NETWORK_STACK", "android.permission.RADIO_SCAN_WITHOUT_LOCATION"
    };
    private static final List<String> ORDINARY = List.of(ACCESS, CHANGE, NETWORK, FINE, COARSE, BACKGROUND);
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
            dhcp.ipAddress = 0x0101a8c0; dhcp.gateway = 0x0100a8c0;
            ap.SSID = "Fixture AP"; ap.BSSID = "02:10:20:30:40:50";
            ap.capabilities = "[ESS]"; ap.frequency = 5180; ap.level = -49; ap.timestamp = 123456;
        }
        private void read() { reads++; if (duringRead != null) duringRead.run(); }
        @Override public int wifiState() { read(); return WifiManager.WIFI_STATE_ENABLED; }
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
            starts++; guard = eligible; completion = completed; return accepted;
        }
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
            wifi = new WifiService(am, atm, pm, () -> location, radio);
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
        void listen(App app) throws Exception {
            IIntentReceiver receiver = new IIntentReceiver.Default() {
                final Binder token = new Binder(); @Override public IBinder asBinder() { return token; }
            };
            app.call(() -> am.registerReceiverWithFeature(app.thread, app.name(), null, null, receiver,
                    new IntentFilter(WifiManager.SCAN_RESULTS_AVAILABLE_ACTION), null, 0, 0));
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
    public static void main(String[] args) throws Exception {
        System.load(args[0]); setOwnUid(Process.SYSTEM_UID);
        android.app.PropertyInvalidatedCache.setTestMode(true);
        android.app.PropertyInvalidatedCache.disableForTestMode();
        field(Class.forName("com.gesher.app.locale.SystemLocale"), "sIs24Hour").set(null, Boolean.TRUE);
        identityAndData(); scanPolicy(); queuedAuthority(); rejectedAdmission(); standalone(); privilegedPolicies(); networkObservations();
        System.out.println("WifiServiceHost passed: " + checks + " checks; real permission/AppOps/broadcast Java, radio=fixture");
    }
}
