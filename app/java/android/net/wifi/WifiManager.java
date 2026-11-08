/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package android.net.wifi;

import android.content.Context;
import java.util.List;

/**
 * Android API 36 compatibility for a runtime without an Android Wi-Fi controller.
 * Windows connectivity is provided separately by ConnectivityManager. This wrapper
 * exposes no scans, configured networks, radio controls, multicast or power locks.
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
    public static final String NETWORK_STATE_CHANGED_ACTION = "android.net.wifi.STATE_CHANGE";

    WifiManager(Context context) { }

    public int getWifiState() { return WIFI_STATE_DISABLED; }
    public int getWifiApState() { return WIFI_AP_STATE_DISABLED; }
    public boolean isWifiEnabled() { return false; }
    public boolean isWpa3SaeSupported() { return false; }
    public WifiInfo getConnectionInfo() { return new WifiInfo(); }
    public List<ScanResult> getScanResults() { return List.of(); }
    public boolean startScan() { return false; }
    public boolean setWifiEnabled(boolean enabled) { return false; }
    public boolean enableNetwork(int networkId, boolean disableOthers) { return false; }
    public int addNetwork(WifiConfiguration config) { return -1; }

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
