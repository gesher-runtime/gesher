/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package com.gesher.server;

import android.app.AppOpsManager;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.net.DhcpInfo;
import android.net.wifi.ScanResult;
import android.net.wifi.WifiConfiguration;
import android.net.wifi.WifiInfo;
import android.net.wifi.WifiManager;
import android.os.Binder;
import android.os.Process;
import com.gesher.net.IWindowsWifi;
import com.gesher.net.WindowsWifi;
import java.util.ArrayList;
import java.util.List;
import java.util.Objects;
import java.util.function.BooleanSupplier;
import java.util.function.Consumer;

/** Device-owned Wi-Fi observations and protected scan-completion broadcasts. */
public final class WifiService extends IWindowsWifi.Stub {
    private static final String ACCESS = "android.permission.ACCESS_WIFI_STATE";
    private static final String NETWORK = "android.permission.ACCESS_NETWORK_STATE";
    private static final String CHANGE = "android.permission.CHANGE_WIFI_STATE";
    private static final String FINE = "android.permission.ACCESS_FINE_LOCATION";
    private static final String COARSE = "android.permission.ACCESS_COARSE_LOCATION";
    private static final int MAX_SCAN_REQUESTERS = 32;

    /** Explicit server-local test boundary; applications cannot select a backend. */
    interface Backend {
        int wifiState();
        WifiInfo connectionInfo();
        WifiInfo connectionInfo(int interfaceIndex);
        DhcpInfo dhcpInfo();
        List<ScanResult> scanResults();
        boolean startScan(BooleanSupplier eligible, Consumer<Boolean> completion);
    }

    private static final Backend WINDOWS = new Backend() {
        @Override public int wifiState() { return WindowsWifi.wifiState(); }
        @Override public WifiInfo connectionInfo() { return WindowsWifi.connectionInfo(); }
        @Override public WifiInfo connectionInfo(int index) { return WindowsWifi.connectionInfo(index); }
        @Override public DhcpInfo dhcpInfo() { return WindowsWifi.dhcpInfo(); }
        @Override public List<ScanResult> scanResults() { return WindowsWifi.scanResults(); }
        @Override public boolean startScan(BooleanSupplier eligible, Consumer<Boolean> completion) {
            return WindowsWifi.startScan(eligible, completion);
        }
    };

    private final ActivityManagerService mAm;
    private final ActivityTaskManagerService mAtm;
    private final PackageManagerService mPm;
    private final BooleanSupplier mLocationEnabled;
    private final Backend mBackend;
    private final Object mScanLock = new Object();
    private ScanGroup mScan;

    WifiService(ActivityManagerService am, ActivityTaskManagerService atm,
            PackageManagerService pm, BooleanSupplier locationEnabled) {
        this(am, atm, pm, locationEnabled, WINDOWS);
    }

    WifiService(ActivityManagerService am, ActivityTaskManagerService atm,
            PackageManagerService pm, BooleanSupplier locationEnabled, Backend backend) {
        mAm = Objects.requireNonNull(am); mAtm = Objects.requireNonNull(atm);
        mPm = Objects.requireNonNull(pm); mLocationEnabled = Objects.requireNonNull(locationEnabled);
        mBackend = Objects.requireNonNull(backend);
    }

    private record Owner(ApkPackage code, ActivityTaskManagerService.ProcessRecord process,
            int uid, int pid, String attributionTag) {
        boolean system() { return code == null; }
        String name() { return system() ? "android" : code.packageName; }
    }

    private Owner caller(String claimed, String tag) {
        int uid = Binder.getCallingUid(), pid = Binder.getCallingPid();
        ActivityTaskManagerService.ProcessRecord process = mAtm.callerProcess(pid);
        if (process != null) {
            ApkPackage code = process.packageCode;
            Owner owner = new Owner(code, process, uid, pid, tag);
            if (code == null || !current(owner) || code.uid() != uid
                    || pid == Process.myPid() && process.inProcessActivityState == null) {
                throw new SecurityException("Wi-Fi requires its live application admission");
            }
            ApkPackage claimedCode = claimed == null ? null : mPm.get(claimed);
            if (!code.packageName.equals(claimed)
                    && (claimedCode == null || claimedCode.uid() != uid)) {
                throw new SecurityException("Wi-Fi caller cannot act for another package");
            }
            return owner;
        }
        // CallerIdentity's control-plane shortcut must not elevate a retired
        // standalone APK merely because its services have the same Windows PID.
        if (pid == Process.myPid() && uid >= Process.FIRST_APPLICATION_UID
                || !mAm.callers().isSystem() || claimed != null && !"android".equals(claimed)) {
            throw new SecurityException("Unrecognized Wi-Fi caller");
        }
        return new Owner(null, null, uid, pid, tag);
    }

    private boolean current(Owner owner) {
        if (owner.system()) return true;
        ActivityTaskManagerService.ProcessRecord process = owner.process();
        return process != null && mAtm.callerProcess(owner.pid()) == process
                && process.ready && process.thread != null && process.uid == owner.uid()
                && !process.dead && !process.exiting
                && !process.pidSuperseded && process.packageCode == owner.code()
                && owner.code().uid() == owner.uid() && mPm.get(owner.name()) == owner.code()
                && !mPm.isRemovalPending(owner.name()) && owner.code().matchesEnabledApplication(0)
                && (process.enabledProcessLifetime == null || process.enabledProcessLifetime.getAsBoolean());
    }

    private boolean granted(Owner owner, String permission) {
        return current(owner) && (owner.system() || mPm.permissions().checkPermission(owner.name(), permission)
                == PackageManager.PERMISSION_GRANTED);
    }

    private void enforce(Owner owner, String permission) {
        if (!granted(owner, permission)) throw new SecurityException("Wi-Fi requires " + permission);
    }

    private boolean note(Owner owner, int operation) {
        if (!current(owner)) return false;
        if (owner.system()) return true;
        long identity = Binder.clearCallingIdentity();
        try {
            return mPm.appOps().noteOperation(operation, owner.uid(), owner.name(),
                    owner.attributionTag(), false, null, false).getOpMode() == AppOpsManager.MODE_ALLOWED;
        } finally { Binder.restoreCallingIdentity(identity); }
    }

    private boolean privilegedScan(Owner owner) {
        return granted(owner, "android.permission.NETWORK_SETTINGS")
                || granted(owner, "android.permission.NETWORK_SETUP_WIZARD")
                || granted(owner, "android.permission.NETWORK_MANAGED_PROVISIONING")
                || granted(owner, "android.permission.NETWORK_STACK")
                || granted(owner, "android.permission.MAINLINE_NETWORK_STACK")
                || granted(owner, "android.permission.RADIO_SCAN_WITHOUT_LOCATION");
    }

    private boolean mayScan(Owner owner) {
        if (!current(owner)) return false;
        if (privilegedScan(owner)) return true;
        if (!mLocationEnabled.getAsBoolean()) return false;
        boolean location = granted(owner, FINE) && note(owner, AppOpsManager.OP_FINE_LOCATION);
        if (!location && owner.code().parsed.getTargetSdkVersion() < 29) {
            location = granted(owner, COARSE) && note(owner, AppOpsManager.OP_COARSE_LOCATION);
        }
        return (location || granted(owner, "android.permission.PEERS_MAC_ADDRESS"))
                && note(owner, AppOpsManager.OP_WIFI_SCAN) && current(owner);
    }

    private boolean mayStartScan(Owner owner) {
        return granted(owner, CHANGE)
                && (granted(owner, "android.permission.NETWORK_SETTINGS")
                    || note(owner, AppOpsManager.OP_CHANGE_WIFI_STATE))
                && mayScan(owner);
    }

    private boolean mayAccessNetworkLocation(Owner owner) {
        if (!current(owner)) return false;
        if (granted(owner, "android.permission.NETWORK_SETTINGS")
                || granted(owner, "android.permission.NETWORK_SETUP_WIZARD")
                || granted(owner, "android.permission.NETWORK_STACK")
                || granted(owner, "android.permission.MAINLINE_NETWORK_STACK")) return true;
        if (!mLocationEnabled.getAsBoolean()) return false;
        boolean location = granted(owner, FINE) && note(owner, AppOpsManager.OP_FINE_LOCATION);
        if (!location && owner.code().parsed.getTargetSdkVersion() < 29) {
            location = granted(owner, COARSE) && note(owner, AppOpsManager.OP_COARSE_LOCATION);
        }
        return location && current(owner);
    }

    private WifiInfo redact(Owner owner, WifiInfo info, boolean includeLocation) {
        long redactions = info.getApplicableRedactions();
        if (includeLocation) redactions &= ~1L;
        if (granted(owner, "android.permission.LOCAL_MAC_ADDRESS")) redactions &= ~(1L << 1);
        if (granted(owner, "android.permission.NETWORK_SETTINGS")
                || granted(owner, "android.permission.NETWORK_SETUP_WIZARD")) redactions &= ~(1L << 2);
        return info.makeCopy(redactions);
    }

    @Override public int getWifiState(String name) {
        Owner owner = caller(name, null); enforce(owner, ACCESS);
        int state = mBackend.wifiState(); enforce(owner, ACCESS); return state;
    }

    @Override public WifiInfo getConnectionInfo(String name, String tag) {
        Owner owner = caller(name, tag); enforce(owner, ACCESS);
        WifiInfo info = mBackend.connectionInfo(); enforce(owner, ACCESS);
        WifiInfo result = redact(owner, info, mayScan(owner));
        enforce(owner, ACCESS);
        return result;
    }

    @Override public WifiInfo getConnectionInfoForNetwork(String name, String tag,
            int index, boolean includeLocation) {
        Owner owner = caller(name, tag); enforce(owner, NETWORK);
        WifiInfo info = mBackend.connectionInfo(index); enforce(owner, NETWORK);
        WifiInfo result = redact(owner, info, includeLocation && mayAccessNetworkLocation(owner));
        enforce(owner, NETWORK);
        return result;
    }

    @Override public DhcpInfo getDhcpInfo(String name) {
        Owner owner = caller(name, null); enforce(owner, ACCESS);
        DhcpInfo data = mBackend.dhcpInfo(); enforce(owner, ACCESS);
        DhcpInfo result = new DhcpInfo();
        result.ipAddress = data.ipAddress; result.gateway = data.gateway; result.netmask = data.netmask;
        result.dns1 = data.dns1; result.dns2 = data.dns2; result.serverAddress = data.serverAddress;
        result.leaseDuration = data.leaseDuration;
        return result;
    }

    @Override public List<WifiConfiguration> getConfiguredNetworks(String name, String tag) {
        Owner owner = caller(name, tag); enforce(owner, ACCESS);
        // Windows connection observations are not Android saved configurations.
        return new ArrayList<>();
    }

    @Override public List<ScanResult> getScanResults(String name, String tag) {
        Owner owner = caller(name, tag); enforce(owner, ACCESS);
        if (!mayScan(owner)) return new ArrayList<>();
        List<ScanResult> scans = mBackend.scanResults();
        enforce(owner, ACCESS);
        if (!mayScan(owner)) return new ArrayList<>();
        List<ScanResult> result = new ArrayList<>(scans.size());
        for (ScanResult scan : scans) result.add(new ScanResult(scan));
        return result;
    }

    private final class ScanGroup {
        final List<Owner> owners = new ArrayList<>(); // guarded by mScanLock
        boolean finished;

        boolean eligible() {
            List<Owner> snapshot;
            synchronized (mScanLock) {
                if (finished || mScan != this) return false;
                snapshot = new ArrayList<>(owners);
            }
            for (Owner owner : snapshot) {
                try { if (mayStartScan(owner)) return true; }
                catch (RuntimeException denied) { /* Revoked/retired identity is ineligible. */ }
            }
            return false;
        }

        private boolean finish() {
            synchronized (mScanLock) {
                if (finished) return false;
                finished = true;
                if (mScan == this) mScan = null;
                return true;
            }
        }

        void complete(boolean updated) {
            if (!finish()) return;
            // ScanRequestProxy publishes every completed device scan globally.
            // This contains only status; each subsequent data read rechecks its own
            // caller. Retirement of an initiator cannot suppress other apps' result.
            mAm.sendBroadcastFor("android", resultIntent(updated), null);
        }

        void rejected() {
            if (!finish()) return;
            // A refused request did not complete a device scan. Android reports
            // that failure to its requester, without waking passive listeners.
            java.util.Set<String> notified = new java.util.HashSet<>();
            for (Owner owner : owners) {
                if (current(owner) && notified.add(owner.name())) {
                    mAm.sendBroadcastFor("android", resultIntent(false).setPackage(owner.name()),
                            null, null, () -> current(owner));
                }
            }
        }
    }

    private static Intent resultIntent(boolean updated) {
        return new Intent(WifiManager.SCAN_RESULTS_AVAILABLE_ACTION)
                .addFlags(Intent.FLAG_RECEIVER_REGISTERED_ONLY_BEFORE_BOOT)
                .putExtra(WifiManager.EXTRA_RESULTS_UPDATED, updated);
    }

    @Override public boolean startScan(String name, String tag) {
        Owner owner = caller(name, tag); enforce(owner, CHANGE);
        if (!mayStartScan(owner)) return false;
        ScanGroup scan;
        synchronized (mScanLock) {
            if (mScan != null) {
                if (mScan.owners.size() >= MAX_SCAN_REQUESTERS) return false;
                mScan.owners.add(owner);
                return true;
            }
            scan = new ScanGroup(); scan.owners.add(owner); mScan = scan;
        }
        boolean accepted;
        try { accepted = mBackend.startScan(scan::eligible, scan::complete); }
        catch (RuntimeException | LinkageError failed) {
            SystemServer.log("Wi-Fi scan could not start: " + failed);
            accepted = false;
        }
        if (!accepted) scan.rejected();
        return accepted;
    }
}
