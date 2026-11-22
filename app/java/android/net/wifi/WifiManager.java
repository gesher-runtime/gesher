/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package android.net.wifi;

import android.content.Context;
import android.net.DhcpInfo;
import android.os.RemoteException;
import android.os.ServiceManager;
import com.gesher.net.IWindowsWifi;
import com.gesher.net.WifiAccess;
import java.util.List;

/**
 * Android API 36 compatibility backed by Windows' Wi-Fi status, scan cache, and
 * IPv4 configuration through the authenticated system Wi-Fi service.
 * Radio configuration mutations remain unsupported.
 */
public class WifiManager {
    public static final int WIFI_STATE_DISABLING = 0;
    public static final int WIFI_STATE_DISABLED = 1;
    public static final int WIFI_STATE_ENABLING = 2;
    public static final int WIFI_STATE_ENABLED = 3;
    public static final int WIFI_STATE_UNKNOWN = 4;
    public static final int WIFI_AP_STATE_DISABLING = 10;
    public static final int WIFI_AP_STATE_DISABLED = 11;
    public static final int WIFI_AP_STATE_ENABLING = 12;
    public static final int WIFI_AP_STATE_ENABLED = 13;
    public static final int WIFI_AP_STATE_FAILED = 14;
    public static final int WIFI_MODE_FULL = 1;
    public static final int WIFI_MODE_SCAN_ONLY = 2;
    public static final int WIFI_MODE_FULL_HIGH_PERF = 3;
    public static final int WIFI_MODE_FULL_LOW_LATENCY = 4;
    public static final String UNKNOWN_SSID = "<unknown ssid>";
    public static final String WIFI_STATE_CHANGED_ACTION = "android.net.wifi.WIFI_STATE_CHANGED";
    public static final String SCAN_RESULTS_AVAILABLE_ACTION = "android.net.wifi.SCAN_RESULTS";
    public static final String EXTRA_RESULTS_UPDATED = "resultsUpdated";
    public static final String NETWORK_STATE_CHANGED_ACTION = "android.net.wifi.STATE_CHANGE";

    private final Context mContext;

    WifiManager(Context context) {
        mContext = WifiAccess.context(context);
    }

    private static IWindowsWifi service() {
        return IWindowsWifi.Stub.asInterface(ServiceManager.getService(IWindowsWifi.NAME));
    }

    private String packageName() { return mContext.getOpPackageName(); }

    public int getWifiState() {
        WifiAccess.enforceAccess(mContext);
        IWindowsWifi service = service();
        try {
            return service != null ? service.getWifiState(packageName()) : WIFI_STATE_UNKNOWN;
        } catch (RemoteException e) {
            throw e.rethrowFromSystemServer();
        }
    }
    public int getWifiApState() { return WIFI_AP_STATE_DISABLED; }
    public boolean isWifiEnabled() { return getWifiState() == WIFI_STATE_ENABLED; }
    public boolean isWpa3SaeSupported() { return false; }
    public WifiInfo getConnectionInfo() {
        WifiAccess.enforceAccess(mContext);
        IWindowsWifi service = service();
        try {
            return service != null ? service.getConnectionInfo(packageName(), mContext.getAttributionTag())
                    : new WifiInfo();
        } catch (RemoteException e) {
            throw e.rethrowFromSystemServer();
        }
    }
    public DhcpInfo getDhcpInfo() {
        WifiAccess.enforceAccess(mContext);
        IWindowsWifi service = service();
        try {
            return service != null ? service.getDhcpInfo(packageName()) : new DhcpInfo();
        } catch (RemoteException e) {
            throw e.rethrowFromSystemServer();
        }
    }
    public List<WifiConfiguration> getConfiguredNetworks() {
        WifiAccess.enforceAccess(mContext);
        IWindowsWifi service = service();
        try {
            return service != null ? service.getConfiguredNetworks(packageName(), mContext.getAttributionTag())
                    : List.of();
        } catch (RemoteException e) {
            throw e.rethrowFromSystemServer();
        }
    }
    public List<ScanResult> getScanResults() {
        WifiAccess.enforceAccess(mContext);
        // The service checks location and AppOps using the authenticated caller.
        IWindowsWifi service = service();
        try {
            return service != null ? service.getScanResults(packageName(), mContext.getAttributionTag())
                    : List.of();
        } catch (RemoteException e) {
            throw e.rethrowFromSystemServer();
        }
    }
    public boolean startScan() {
        WifiAccess.enforceChange(mContext);
        IWindowsWifi service = service();
        try {
            return service != null && service.startScan(packageName(), mContext.getAttributionTag());
        } catch (RemoteException e) {
            throw e.rethrowFromSystemServer();
        }
    }
    public boolean setWifiEnabled(boolean enabled) { WifiAccess.enforceChange(mContext); return false; }
    public boolean enableNetwork(int networkId, boolean disableOthers) { WifiAccess.enforceChange(mContext); return false; }
    public boolean disableNetwork(int netId) { WifiAccess.enforceChange(mContext); return false; }
    public boolean disconnect() { WifiAccess.enforceChange(mContext); return false; }
    public boolean reconnect() { WifiAccess.enforceChange(mContext); return false; }
    public int addNetwork(WifiConfiguration config) { WifiAccess.enforceChange(mContext); return -1; }
    public boolean removeNetwork(int netId) { WifiAccess.enforceChange(mContext); return false; }
    public boolean saveConfiguration() { WifiAccess.enforceChange(mContext); return false; }

    /** The API 36 legacy RSSI mapping, independent of hardware availability. */
    public static int calculateSignalLevel(int rssi, int numLevels) {
        if (rssi <= -100) return 0;
        if (rssi >= -55) return numLevels - 1;
        return (int) ((float) (rssi + 100) * (numLevels - 1) / 45);
    }

    public WifiLock createWifiLock(String tag) { return createWifiLock(WIFI_MODE_FULL, tag); }
    public WifiLock createWifiLock(int lockType, String tag) { return new WifiLock(); }
    public MulticastLock createMulticastLock(String tag) { return new MulticastLock(); }

    public class WifiLock {
        private boolean mHeld;
        private boolean mRefCounted = true;
        private int mRefCount;

        private WifiLock() { }
        public synchronized void setReferenceCounted(boolean value) {
            if (mRefCounted != value) { mRefCounted = value; mRefCount = mHeld ? 1 : 0; }
        }
        public synchronized boolean isHeld() { return mHeld; }
        public synchronized void acquire() {
            if (mRefCounted) {
                if (mRefCount == Integer.MAX_VALUE) throw new IllegalStateException("Wi-Fi lock count overflow");
                mRefCount++;
                mHeld = true;
            } else {
                mRefCount = 1;
                mHeld = true;
            }
        }
        public synchronized void release() {
            if (mRefCounted) {
                if (mRefCount == 0) throw new RuntimeException("Wi-Fi lock under-locked");
                if (--mRefCount <= 0) {
                    mRefCount = 0;
                    mHeld = false;
                }
            } else {
                mRefCount = 0;
                mHeld = false;
            }
        }
    }

    public class MulticastLock {
        private boolean mHeld;
        private boolean mRefCounted = true;
        private int mRefCount;

        private MulticastLock() { }
        public synchronized void setReferenceCounted(boolean value) {
            if (mRefCounted != value) { mRefCounted = value; mRefCount = mHeld ? 1 : 0; }
        }
        public synchronized boolean isHeld() { return mHeld; }
        public synchronized void acquire() {
            if (mRefCounted) {
                if (mRefCount == Integer.MAX_VALUE) throw new IllegalStateException("Multicast lock count overflow");
                mRefCount++;
                mHeld = true;
            } else {
                mRefCount = 1;
                mHeld = true;
            }
        }
        public synchronized void release() {
            if (mRefCounted) {
                if (mRefCount == 0) throw new RuntimeException("Multicast lock under-locked");
                if (--mRefCount <= 0) {
                    mRefCount = 0;
                    mHeld = false;
                }
            } else {
                mRefCount = 0;
                mHeld = false;
            }
        }
    }
}
