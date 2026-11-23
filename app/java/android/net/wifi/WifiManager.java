/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package android.net.wifi;

import android.content.Context;
import android.content.pm.PackageManager;
import android.net.DhcpInfo;
import android.net.wifi.hotspot2.PasspointConfiguration;
import android.os.Bundle;
import android.os.Handler;
import android.os.Parcel;
import android.os.Parcelable;
import android.os.RemoteException;
import android.os.ServiceManager;
import android.os.WorkSource;
import com.gesher.net.IWindowsWifi;
import com.gesher.net.WifiAccess;
import java.net.InetAddress;
import java.util.ArrayList;
import java.util.Collections;
import java.util.HashMap;
import java.util.IdentityHashMap;
import java.util.List;
import java.util.Map;
import java.util.Objects;
import java.util.Set;
import java.util.concurrent.Executor;
import java.util.function.BiConsumer;
import java.util.function.Consumer;
import java.util.function.IntConsumer;

/**
 * Android API 36 WifiManager backed by Windows' WLAN service through the authenticated
 * system Wi-Fi service ({@code gesher.wifi}, docs/wifi.md). Every public SDK member
 * exists. State, scans, connection information, adapter capabilities and the scan and
 * state callbacks come from real Windows observations. Facilities Windows does not
 * expose to Gesher (SoftAP, WPS, TDLS, Passpoint, P2P, saved Android networks) answer
 * with Android's own documented unsupported or failure results, after the same
 * argument and permission checks Android applies.
 */
public class WifiManager {
    public static final String ACTION_PICK_WIFI_NETWORK = "android.net.wifi.PICK_WIFI_NETWORK";
    public static final int ACTION_REMOVE_SUGGESTION_DISCONNECT = 2;
    public static final int ACTION_REMOVE_SUGGESTION_LINGER = 1;
    public static final String ACTION_REQUEST_SCAN_ALWAYS_AVAILABLE =
            "android.net.wifi.action.REQUEST_SCAN_ALWAYS_AVAILABLE";
    public static final String ACTION_WIFI_NETWORK_SUGGESTION_POST_CONNECTION =
            "android.net.wifi.action.WIFI_NETWORK_SUGGESTION_POST_CONNECTION";
    public static final String ACTION_WIFI_SCAN_AVAILABILITY_CHANGED =
            "android.net.wifi.action.WIFI_SCAN_AVAILABILITY_CHANGED";
    public static final String CHANNEL_DATA_KEY_FREQUENCY_MHZ = "CHANNEL_DATA_KEY_FREQUENCY_MHZ";
    public static final String CHANNEL_DATA_KEY_NUM_AP = "CHANNEL_DATA_KEY_NUM_AP";
    public static final int ERROR_AUTHENTICATING = 1;
    public static final String EXTRA_BSSID = "bssid";
    public static final String EXTRA_NETWORK_INFO = "networkInfo";
    public static final String EXTRA_NETWORK_SUGGESTION = "android.net.wifi.extra.NETWORK_SUGGESTION";
    public static final String EXTRA_NEW_RSSI = "newRssi";
    public static final String EXTRA_NEW_STATE = "newState";
    public static final String EXTRA_PREVIOUS_WIFI_STATE = "previous_wifi_state";
    public static final String EXTRA_RESULTS_UPDATED = "resultsUpdated";
    public static final String EXTRA_SCAN_AVAILABLE = "android.net.wifi.extra.SCAN_AVAILABLE";
    public static final String EXTRA_SUPPLICANT_CONNECTED = "connected";
    public static final String EXTRA_SUPPLICANT_ERROR = "supplicantError";
    public static final String EXTRA_WIFI_INFO = "wifiInfo";
    public static final String EXTRA_WIFI_STATE = "wifi_state";
    public static final int FLAG_SEND_DHCP_HOSTNAME_RESTRICTION_OPEN = 1;
    public static final int FLAG_SEND_DHCP_HOSTNAME_RESTRICTION_SECURE = 2;
    public static final String NETWORK_IDS_CHANGED_ACTION = "android.net.wifi.NETWORK_IDS_CHANGED";
    public static final String NETWORK_STATE_CHANGED_ACTION = "android.net.wifi.STATE_CHANGE";
    public static final int ROAMING_MODE_AGGRESSIVE = 2;
    public static final int ROAMING_MODE_NONE = 0;
    public static final int ROAMING_MODE_NORMAL = 1;
    public static final String RSSI_CHANGED_ACTION = "android.net.wifi.RSSI_CHANGED";
    public static final String SCAN_RESULTS_AVAILABLE_ACTION = "android.net.wifi.SCAN_RESULTS";
    public static final int STATUS_LOCAL_ONLY_CONNECTION_FAILURE_ASSOCIATION = 1;
    public static final int STATUS_LOCAL_ONLY_CONNECTION_FAILURE_AUTHENTICATION = 2;
    public static final int STATUS_LOCAL_ONLY_CONNECTION_FAILURE_IP_PROVISIONING = 3;
    public static final int STATUS_LOCAL_ONLY_CONNECTION_FAILURE_NOT_FOUND = 4;
    public static final int STATUS_LOCAL_ONLY_CONNECTION_FAILURE_NO_RESPONSE = 5;
    public static final int STATUS_LOCAL_ONLY_CONNECTION_FAILURE_UNKNOWN = 0;
    public static final int STATUS_LOCAL_ONLY_CONNECTION_FAILURE_USER_REJECT = 6;
    public static final int STATUS_NETWORK_SUGGESTIONS_ERROR_ADD_DUPLICATE = 3;
    public static final int STATUS_NETWORK_SUGGESTIONS_ERROR_ADD_EXCEEDS_MAX_PER_APP = 4;
    public static final int STATUS_NETWORK_SUGGESTIONS_ERROR_ADD_INVALID = 7;
    public static final int STATUS_NETWORK_SUGGESTIONS_ERROR_ADD_NOT_ALLOWED = 6;
    public static final int STATUS_NETWORK_SUGGESTIONS_ERROR_APP_DISALLOWED = 2;
    public static final int STATUS_NETWORK_SUGGESTIONS_ERROR_INTERNAL = 1;
    public static final int STATUS_NETWORK_SUGGESTIONS_ERROR_REMOVE_INVALID = 5;
    public static final int STATUS_NETWORK_SUGGESTIONS_ERROR_RESTRICTED_BY_ADMIN = 8;
    public static final int STATUS_NETWORK_SUGGESTIONS_SUCCESS = 0;
    public static final int STATUS_SUGGESTION_APPROVAL_APPROVED_BY_CARRIER_PRIVILEGE = 4;
    public static final int STATUS_SUGGESTION_APPROVAL_APPROVED_BY_USER = 2;
    public static final int STATUS_SUGGESTION_APPROVAL_PENDING = 1;
    public static final int STATUS_SUGGESTION_APPROVAL_REJECTED_BY_USER = 3;
    public static final int STATUS_SUGGESTION_APPROVAL_UNKNOWN = 0;
    public static final int STATUS_SUGGESTION_CONNECTION_FAILURE_ASSOCIATION = 1;
    public static final int STATUS_SUGGESTION_CONNECTION_FAILURE_AUTHENTICATION = 2;
    public static final int STATUS_SUGGESTION_CONNECTION_FAILURE_IP_PROVISIONING = 3;
    public static final int STATUS_SUGGESTION_CONNECTION_FAILURE_UNKNOWN = 0;
    public static final String SUPPLICANT_CONNECTION_CHANGE_ACTION =
            "android.net.wifi.supplicant.CONNECTION_CHANGE";
    public static final String SUPPLICANT_STATE_CHANGED_ACTION = "android.net.wifi.supplicant.STATE_CHANGE";
    public static final String UNKNOWN_SSID = "<unknown ssid>";
    public static final int WIFI_INTERFACE_TYPE_AP = 1;
    public static final int WIFI_INTERFACE_TYPE_AWARE = 2;
    public static final int WIFI_INTERFACE_TYPE_DIRECT = 3;
    public static final int WIFI_INTERFACE_TYPE_STA = 0;
    public static final int WIFI_MODE_FULL = 1;
    public static final int WIFI_MODE_FULL_HIGH_PERF = 3;
    public static final int WIFI_MODE_FULL_LOW_LATENCY = 4;
    public static final int WIFI_MODE_SCAN_ONLY = 2;
    public static final int WIFI_MULTI_INTERNET_MODE_DBS_AP = 1;
    public static final int WIFI_MULTI_INTERNET_MODE_DISABLED = 0;
    public static final int WIFI_MULTI_INTERNET_MODE_MULTI_AP = 2;
    public static final String WIFI_STATE_CHANGED_ACTION = "android.net.wifi.WIFI_STATE_CHANGED";
    public static final int WIFI_STATE_DISABLED = 1;
    public static final int WIFI_STATE_DISABLING = 0;
    public static final int WIFI_STATE_ENABLED = 3;
    public static final int WIFI_STATE_ENABLING = 2;
    public static final int WIFI_STATE_UNKNOWN = 4;
    public static final int WPS_AUTH_FAILURE = 6;
    public static final int WPS_OVERLAP_ERROR = 3;
    public static final int WPS_TIMED_OUT = 7;
    public static final int WPS_TKIP_ONLY_PROHIBITED = 5;
    public static final int WPS_WEP_PROHIBITED = 4;

    /** @hide System API soft-AP states. */
    public static final int WIFI_AP_STATE_DISABLING = 10;
    /** @hide */ public static final int WIFI_AP_STATE_DISABLED = 11;
    /** @hide */ public static final int WIFI_AP_STATE_ENABLING = 12;
    /** @hide */ public static final int WIFI_AP_STATE_ENABLED = 13;
    /** @hide */ public static final int WIFI_AP_STATE_FAILED = 14;

    // ActionListener.FAILURE_INTERNAL_ERROR, which the deprecated WPS entry points report.
    private static final int FAILURE_INTERNAL_ERROR = 0;
    // config_wifiRssiLevelThresholds (RssiUtil): levels 0..4.
    private static final int[] RSSI_THRESHOLDS = {-88, -77, -66, -55};
    private static final int MIN_RSSI = -100;
    private static final int MAX_RSSI = -55;
    private static final int MAX_ACTIVE_LOCKS = 50;
    private static final int NETWORK_SUGGESTIONS_MAX_PER_APP_LOW_RAM = 256;
    private static final int NETWORK_SUGGESTIONS_MAX_PER_APP_HIGH_RAM = 1024;
    // config_wifiNetworkSpecifierMaxPreferredChannels.
    private static final int MAX_CHANNELS_PER_NETWORK_SPECIFIER_REQUEST = 5;

    private static final String WAKE_LOCK = "android.permission.WAKE_LOCK";
    private static final String MULTICAST = "android.permission.CHANGE_WIFI_MULTICAST_STATE";
    private static final String NEARBY = "android.permission.NEARBY_WIFI_DEVICES";
    private static final String FINE = "android.permission.ACCESS_FINE_LOCATION";
    private static final String COARSE = "android.permission.ACCESS_COARSE_LOCATION";
    private static final String NETWORK_SETTINGS = "android.permission.NETWORK_SETTINGS";
    private static final String NETWORK_SETUP_WIZARD = "android.permission.NETWORK_SETUP_WIZARD";

    private final Context mContext;
    private final Object mLockCountLock = new Object();
    private int mActiveLockCount; // guarded by mLockCountLock

    WifiManager(Context context) {
        mContext = WifiAccess.context(context);
    }

    private static IWindowsWifi service() {
        return IWindowsWifi.Stub.asInterface(ServiceManager.getService(IWindowsWifi.NAME));
    }

    private String packageName() { return mContext.getOpPackageName(); }
    private String attributionTag() { return mContext.getAttributionTag(); }
    private static int myUid() { return android.os.Process.myUid(); }

    private boolean granted(String permission) {
        if (mContext == null) return false;
        try {
            return mContext.checkSelfPermission(permission) == PackageManager.PERMISSION_GRANTED;
        } catch (RuntimeException e) {
            return false;
        }
    }

    private boolean privileged() { return granted(NETWORK_SETTINGS) || granted(NETWORK_SETUP_WIZARD); }

    private int targetSdk() {
        try {
            return mContext.getApplicationInfo().targetSdkVersion;
        } catch (RuntimeException e) {
            return android.os.Build.VERSION_CODES.CUR_DEVELOPMENT;
        }
    }

    private boolean locationEnabled() {
        try {
            android.location.LocationManager location =
                    mContext.getSystemService(android.location.LocationManager.class);
            return location != null && location.isLocationEnabled();
        } catch (RuntimeException e) {
            return false;
        }
    }

    private static void post(Executor executor, Runnable action) {
        long identity = android.os.Binder.clearCallingIdentity();
        try {
            executor.execute(action);
        } finally {
            android.os.Binder.restoreCallingIdentity(identity);
        }
    }

    // ---- State, scans and connection ------------------------------------------

    public int getWifiState() {
        WifiAccess.enforceAccess(mContext);
        IWindowsWifi service = service();
        try {
            return service != null ? service.getWifiState(packageName()) : WIFI_STATE_UNKNOWN;
        } catch (RemoteException e) {
            throw e.rethrowFromSystemServer();
        }
    }
    /** @hide No SoftAP: the access point is always disabled. */
    public int getWifiApState() { return WIFI_AP_STATE_DISABLED; }
    public boolean isWifiEnabled() { return getWifiState() == WIFI_STATE_ENABLED; }
    public WifiInfo getConnectionInfo() {
        WifiAccess.enforceAccess(mContext);
        IWindowsWifi service = service();
        try {
            return service != null ? service.getConnectionInfo(packageName(), attributionTag()) : new WifiInfo();
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
    public List<ScanResult> getScanResults() {
        WifiAccess.enforceAccess(mContext);
        // The service checks location and AppOps using the authenticated caller.
        IWindowsWifi service = service();
        try {
            List<ScanResult> result = service != null
                    ? service.getScanResults(packageName(), attributionTag()) : null;
            return result != null ? result : new ArrayList<>();
        } catch (RemoteException e) {
            throw e.rethrowFromSystemServer();
        }
    }
    public boolean startScan() {
        WifiAccess.enforceChange(mContext);
        IWindowsWifi service = service();
        try {
            return service != null && service.startScan(packageName(), attributionTag());
        } catch (RemoteException e) {
            throw e.rethrowFromSystemServer();
        }
    }
    /** As Android: {@code isWifiEnabled()}. */
    public boolean pingSupplicant() { return isWifiEnabled(); }

    /** Windows scans only with the radio on; scan-always mode does not exist. */
    public boolean isScanAlwaysAvailable() { WifiAccess.enforceAccess(mContext); return false; }
    /** Scan requests are coalesced, never throttled. */
    public boolean isScanThrottleEnabled() { WifiAccess.enforceAccess(mContext); return false; }
    /** No Android Wi-Fi wake controller exists. */
    public boolean isAutoWakeupEnabled() { WifiAccess.enforceAccess(mContext); return false; }
    /** No carrier-merged networks exist without telephony. */
    public boolean isCarrierNetworkOffloadEnabled(int subscriptionId, boolean merged) {
        WifiAccess.enforceAccess(mContext);
        return false;
    }
    /** Passpoint is not supported (no FEATURE_WIFI_PASSPOINT). */
    public boolean isWifiPasspointEnabled() { WifiAccess.enforceAccess(mContext); return false; }
    public int getStaConcurrencyForMultiInternetMode() {
        WifiAccess.enforceAccess(mContext);
        return WIFI_MULTI_INTERNET_MODE_DISABLED;
    }

    /** Per-frequency AP counts of the cached real scan (NEARBY_WIFI_DEVICES). */
    public void getChannelData(Executor executor, Consumer<List<Bundle>> resultsCallback) {
        Objects.requireNonNull(executor, "executor cannot be null");
        Objects.requireNonNull(resultsCallback, "resultsCallback cannot be null");
        IWindowsWifi service = service();
        List<Bundle> result;
        try {
            result = service != null ? service.getChannelData(packageName(), attributionTag()) : null;
        } catch (RemoteException e) {
            throw e.rethrowFromSystemServer();
        }
        List<Bundle> delivered = result != null ? result : new ArrayList<>();
        post(executor, () -> resultsCallback.accept(delivered));
    }

    private void enforceChannelQuery(int band) {
        // WifiServiceImpl.getUsableChannels: before U, LOCATION_HARDWARE; from U,
        // NEARBY_WIFI_DEVICES plus location unless declared neverForLocation.
        if (targetSdk() < 34) {
            if (!locationEnabled()) throw new SecurityException("Location mode is disabled for the device");
            if (!granted("android.permission.LOCATION_HARDWARE")) {
                throw new SecurityException("UID " + myUid() + " does not have location h/w permission");
            }
        } else if (!privileged()) {
            if (!granted(NEARBY)) {
                throw new SecurityException("package=" + packageName() + " UID=" + myUid()
                        + " does not have nearby devices permission.");
            }
            if (!neverForLocation() && (!locationEnabled() || !granted(FINE))) {
                throw new SecurityException("package=" + packageName() + " UID=" + myUid()
                        + " does not have location permission.");
            }
        }
        switch (band) {
            case 0: case 1: case 6: case 7: case 8: case 15: case 16: case 31:
                break; // UNSPECIFIED, 24, 5_WITH_DFS, BOTH_WITH_DFS, 6, 24_5_DFS_6, 60, 24_5_DFS_6_60
            default:
                throw new IllegalArgumentException("Unsupported band: " + band);
        }
    }

    private boolean neverForLocation() {
        try {
            android.content.pm.PackageInfo info = mContext.getPackageManager().getPackageInfo(
                    mContext.getPackageName(), PackageManager.GET_PERMISSIONS);
            if (info.requestedPermissions == null || info.requestedPermissionsFlags == null) return false;
            for (int i = 0; i < info.requestedPermissions.length; i++) {
                if (NEARBY.equals(info.requestedPermissions[i])) {
                    return (info.requestedPermissionsFlags[i]
                            & android.content.pm.PackageInfo.REQUESTED_PERMISSION_NEVER_FOR_LOCATION) != 0;
                }
            }
        } catch (PackageManager.NameNotFoundException | RuntimeException e) {
            // Undeclared: location is required.
        }
        return false;
    }

    /** Windows exposes no regulatory channel table: Android's unsupported result. */
    public List<WifiAvailableChannel> getAllowedChannels(int band, int mode) {
        enforceChannelQuery(band);
        throw new UnsupportedOperationException();
    }
    public List<WifiAvailableChannel> getUsableChannels(int band, int mode) {
        enforceChannelQuery(band);
        throw new UnsupportedOperationException();
    }

    // ---- Capabilities -----------------------------------------------------------

    private long capabilities(int group) {
        if (group == IWindowsWifi.CAPABILITIES_FEATURES) WifiAccess.enforceAccess(mContext);
        IWindowsWifi service = service();
        try {
            return service != null ? service.getCapabilities(packageName(), group) : 0;
        } catch (RemoteException e) {
            throw e.rethrowFromSystemServer();
        }
    }
    private boolean feature(long bit) { return (capabilities(IWindowsWifi.CAPABILITIES_FEATURES) & bit) != 0; }
    /** A feature Windows does not report: false, after Android's ACCESS_WIFI_STATE check. */
    private boolean unsupportedFeature() { WifiAccess.enforceAccess(mContext); return false; }
    private boolean band(long bit) { return (capabilities(IWindowsWifi.CAPABILITIES_BANDS) & bit) != 0; }

    public boolean is24GHzBandSupported() { return band(IWindowsWifi.BAND_24_GHZ); }
    public boolean is5GHzBandSupported() { return band(IWindowsWifi.BAND_5_GHZ); }
    public boolean is6GHzBandSupported() { return band(IWindowsWifi.BAND_6_GHZ); }
    public boolean is60GHzBandSupported() { return band(IWindowsWifi.BAND_60_GHZ); }
    public boolean isWifiStandardSupported(int standard) {
        if (standard < 0 || standard + IWindowsWifi.STANDARD_SHIFT >= 63) return false;
        return band(1L << (IWindowsWifi.STANDARD_SHIFT + standard));
    }
    public boolean isWpa3SaeSupported() { return feature(IWindowsWifi.FEATURE_WPA3_SAE); }
    public boolean isWpa3SuiteBSupported() { return feature(IWindowsWifi.FEATURE_WPA3_SUITE_B); }
    public boolean isEnhancedOpenSupported() { return feature(IWindowsWifi.FEATURE_OWE); }
    public boolean isWpaPersonalSupported() { return feature(IWindowsWifi.FEATURE_WPA_PERSONAL); }
    public boolean isWepSupported() { return feature(IWindowsWifi.FEATURE_WEP); }
    /** Windows reports no H2E capability, so support cannot be established. */
    public boolean isWpa3SaeH2eSupported() { return unsupportedFeature(); }
    /** Android reports this as unsupported without a service call. */
    public boolean isWpa3SaePublicKeySupported() { return false; }
    public boolean isWapiSupported() { return unsupportedFeature(); }
    public boolean isStaApConcurrencySupported() { return unsupportedFeature(); }
    public boolean isStaConcurrencyForLocalOnlyConnectionsSupported() { return unsupportedFeature(); }
    public boolean isMakeBeforeBreakWifiSwitchingSupported() { return unsupportedFeature(); }
    public boolean isStaConcurrencyForMultiInternetSupported() { return unsupportedFeature(); }
    public boolean isDeviceToApRttSupported() { return unsupportedFeature(); }
    public boolean isTdlsSupported() { return unsupportedFeature(); }
    public boolean isEnhancedPowerReportingSupported() { return unsupportedFeature(); }
    public boolean isStaBridgedApConcurrencySupported() { return unsupportedFeature(); }
    public boolean isBridgedApConcurrencySupported() { return unsupportedFeature(); }
    public boolean isD2dSupportedWhenInfraStaDisabled() { return unsupportedFeature(); }
    public boolean isEasyConnectSupported() { return unsupportedFeature(); }
    public boolean isEasyConnectEnrolleeResponderModeSupported() { return unsupportedFeature(); }
    public boolean isEasyConnectDppAkmSupported() { return unsupportedFeature(); }
    public boolean isPasspointTermsAndConditionsSupported() { return unsupportedFeature(); }
    public boolean isWifiDisplayR2Supported() { return unsupportedFeature(); }
    public boolean isDecoratedIdentitySupported() { return unsupportedFeature(); }
    public boolean isTrustOnFirstUseSupported() { return unsupportedFeature(); }
    public boolean isTlsMinimumVersionSupported() { return unsupportedFeature(); }
    public boolean isTlsV13Supported() { return unsupportedFeature(); }
    public boolean isDualBandSimultaneousSupported() { return unsupportedFeature(); }
    public boolean isTidToLinkMappingNegotiationSupported() { return unsupportedFeature(); }
    public boolean isAggressiveRoamingModeSupported() { return unsupportedFeature(); }
    public boolean isPreferredNetworkOffloadSupported() { return unsupportedFeature(); }
    /** Android checks FEATURE_WIFI_DIRECT first; Gesher does not declare it. */
    public boolean isP2pSupported() {
        try {
            if (!mContext.getPackageManager().hasSystemFeature(PackageManager.FEATURE_WIFI_DIRECT)) return false;
        } catch (RuntimeException e) {
            return false;
        }
        return unsupportedFeature();
    }

    // ---- Signal levels -----------------------------------------------------------

    /** The API 36 legacy RSSI mapping, independent of hardware availability. */
    public static int calculateSignalLevel(int rssi, int numLevels) {
        if (rssi <= MIN_RSSI) return 0;
        if (rssi >= MAX_RSSI) return numLevels - 1;
        return (int) ((float) (rssi - MIN_RSSI) * (numLevels - 1) / (MAX_RSSI - MIN_RSSI));
    }
    /** RssiUtil with the default {@code config_wifiRssiLevelThresholds}. */
    public int calculateSignalLevel(int rssi) {
        for (int level = 0; level < RSSI_THRESHOLDS.length; level++) {
            if (rssi < RSSI_THRESHOLDS[level]) return level;
        }
        return RSSI_THRESHOLDS.length;
    }
    public int getMaxSignalLevel() { return calculateSignalLevel(Integer.MAX_VALUE); }
    public static int compareSignalLevel(int rssiA, int rssiB) { return rssiA - rssiB; }

    // ---- Saved networks (legacy configuration API) ------------------------------

    public List<WifiConfiguration> getConfiguredNetworks() {
        WifiAccess.enforceAccess(mContext);
        IWindowsWifi service = service();
        try {
            List<WifiConfiguration> result = service != null
                    ? service.getConfiguredNetworks(packageName(), attributionTag()) : null;
            return result != null ? result : new ArrayList<>();
        } catch (RemoteException e) {
            throw e.rethrowFromSystemServer();
        }
    }
    public List<WifiConfiguration> getCallerConfiguredNetworks() {
        WifiAccess.enforceAccess(mContext);
        // Only device/profile owners, carrier or privileged apps may list their own.
        throw new SecurityException("Not a DO, PO, carrier or privileged app");
    }
    /**
     * Gesher keeps no Android saved networks: an addition is refused with -1, the result
     * Android returns to apps targeting API 29+ and for any failed addition.
     */
    public int addNetwork(WifiConfiguration config) {
        if (config == null) return -1;
        config.networkId = -1;
        WifiAccess.enforceChange(mContext);
        return -1;
    }
    public int updateNetwork(WifiConfiguration config) {
        if (config == null || config.networkId < 0) return -1;
        WifiAccess.enforceChange(mContext);
        return -1;
    }
    public AddNetworkResult addNetworkPrivileged(WifiConfiguration config) {
        if (config == null) throw new IllegalArgumentException("config cannot be null");
        if (config.isSecurityType(WifiInfo.SECURITY_TYPE_DPP) && !isEasyConnectDppAkmSupported()) {
            throw new IllegalArgumentException("dpp akm is not supported");
        }
        config.networkId = -1;
        if (!privileged()) {
            throw new SecurityException("Caller is not a device owner, profile owner, system app, or privileged app");
        }
        return new AddNetworkResult(AddNetworkResult.STATUS_ADD_WIFI_CONFIG_FAILURE, -1);
    }
    public boolean removeNetwork(int netId) { WifiAccess.enforceChange(mContext); return false; }
    public boolean enableNetwork(int netId, boolean attemptConnect) { WifiAccess.enforceChange(mContext); return false; }
    public boolean disableNetwork(int netId) { WifiAccess.enforceChange(mContext); return false; }
    public boolean disconnect() { WifiAccess.enforceChange(mContext); return false; }
    public boolean reconnect() { WifiAccess.enforceChange(mContext); return false; }
    public boolean reassociate() { WifiAccess.enforceChange(mContext); return false; }
    /** Radio power stays under Windows' control. */
    public boolean setWifiEnabled(boolean enabled) { WifiAccess.enforceChange(mContext); return false; }
    /** As Android: always false, no service call. */
    public boolean saveConfiguration() { return false; }
    public boolean removeNonCallerConfiguredNetworks() {
        WifiAccess.enforceChange(mContext);
        throw new SecurityException("Caller is not device owner or profile owner of an organization owned device");
    }

    // ---- Network suggestions -------------------------------------------------

    /** No suggestion store: Android's internal-error status, never a false success. */
    public int addNetworkSuggestions(List<WifiNetworkSuggestion> networkSuggestions) {
        WifiAccess.enforceChange(mContext);
        if (networkSuggestions == null) return STATUS_NETWORK_SUGGESTIONS_SUCCESS;
        return STATUS_NETWORK_SUGGESTIONS_ERROR_INTERNAL;
    }
    public int removeNetworkSuggestions(List<WifiNetworkSuggestion> networkSuggestions) {
        return removeNetworkSuggestions(networkSuggestions, ACTION_REMOVE_SUGGESTION_DISCONNECT);
    }
    public int removeNetworkSuggestions(List<WifiNetworkSuggestion> networkSuggestions, int action) {
        WifiAccess.enforceChange(mContext);
        if (action != ACTION_REMOVE_SUGGESTION_LINGER && action != ACTION_REMOVE_SUGGESTION_DISCONNECT) {
            return STATUS_NETWORK_SUGGESTIONS_ERROR_REMOVE_INVALID;
        }
        // An empty list removes all of the app's (none); unknown entries are invalid.
        if (networkSuggestions == null || networkSuggestions.isEmpty()) return STATUS_NETWORK_SUGGESTIONS_SUCCESS;
        return STATUS_NETWORK_SUGGESTIONS_ERROR_REMOVE_INVALID;
    }
    public List<WifiNetworkSuggestion> getNetworkSuggestions() {
        WifiAccess.enforceAccess(mContext);
        return new ArrayList<>();
    }
    public int getMaxNumberOfNetworkSuggestionsPerApp() {
        boolean lowRam = false;
        try {
            android.app.ActivityManager am = mContext.getSystemService(android.app.ActivityManager.class);
            lowRam = am != null && am.isLowRamDevice();
        } catch (RuntimeException ignored) { }
        return lowRam ? NETWORK_SUGGESTIONS_MAX_PER_APP_LOW_RAM : NETWORK_SUGGESTIONS_MAX_PER_APP_HIGH_RAM;
    }
    public int getMaxNumberOfChannelsPerNetworkSpecifierRequest() {
        return MAX_CHANNELS_PER_NETWORK_SPECIFIER_REQUEST;
    }
    /** Affects only a current network this app suggested; there is none to block. */
    public void disallowCurrentSuggestedNetwork(BlockingOption blockingOption) {
        Objects.requireNonNull(blockingOption, "blockingOption cannot be null");
        if (!granted("android.permission.CHANGE_WIFI_STATE")) {
            throw new SecurityException("Caller does not hold CHANGE_WIFI_STATE permission");
        }
    }

    // ---- Listener registrations --------------------------------------------------

    /** A registration delivered by the service to an app executor. */
    private static final class Remote extends IWindowsWifi.Callback {
        final Executor executor;
        final IntConsumer target;
        volatile boolean active = true;
        Remote(Executor executor, IntConsumer target) { this.executor = executor; this.target = target; }
        @Override protected void onEvent(int kind, int value, Bundle extras) {
            if (!active) return;
            post(executor, () -> { if (active) target.accept(value); });
        }
    }

    // Keyed by identity, as Android keys listener proxies by System.identityHashCode.
    private static final Map<Integer, Map<Object, Remote>> sRemotes = new HashMap<>();

    private static Map<Object, Remote> remotes(int kind) {
        return sRemotes.computeIfAbsent(kind, ignored -> new IdentityHashMap<>());
    }

    private void register(int kind, Object key, Executor executor, IntConsumer target) {
        Remote remote = new Remote(executor, target);
        synchronized (sRemotes) {
            if (remotes(kind).containsKey(key)) return;
            remotes(kind).put(key, remote);
        }
        IWindowsWifi service = service();
        if (service == null) return;
        try {
            service.registerCallback(packageName(), attributionTag(), kind, remote);
        } catch (RemoteException e) {
            synchronized (sRemotes) { remotes(kind).remove(key); }
            throw e.rethrowFromSystemServer();
        } catch (RuntimeException e) {
            synchronized (sRemotes) { remotes(kind).remove(key); }
            throw e;
        }
    }

    private void unregister(int kind, Object key) {
        Remote remote;
        synchronized (sRemotes) { remote = remotes(kind).remove(key); }
        if (remote == null) return;
        remote.active = false;
        IWindowsWifi service = service();
        if (service == null) return;
        try {
            service.unregisterCallback(packageName(), kind, remote);
        } catch (RemoteException e) {
            throw e.rethrowFromSystemServer();
        }
    }

    /** Fired on every completed real scan, including Windows' own background scans. */
    public void registerScanResultsCallback(Executor executor, ScanResultsCallback callback) {
        if (executor == null) throw new IllegalArgumentException("executor cannot be null");
        if (callback == null) throw new IllegalArgumentException("callback cannot be null");
        WifiAccess.enforceAccess(mContext);
        register(IWindowsWifi.CALLBACK_SCAN_RESULTS, callback, executor, value -> callback.onScanResultsAvailable());
    }
    public void unregisterScanResultsCallback(ScanResultsCallback callback) {
        if (callback == null) throw new IllegalArgumentException("callback cannot be null");
        WifiAccess.enforceAccess(mContext);
        unregister(IWindowsWifi.CALLBACK_SCAN_RESULTS, callback);
    }
    /** Called once on registration and on each observed radio state change. */
    public void addWifiStateChangedListener(Executor executor, WifiStateChangedListener listener) {
        Objects.requireNonNull(executor);
        Objects.requireNonNull(listener);
        WifiAccess.enforceAccess(mContext);
        register(IWindowsWifi.CALLBACK_WIFI_STATE, listener, executor, value -> listener.onWifiStateChanged());
    }
    public void removeWifiStateChangedListener(WifiStateChangedListener listener) {
        Objects.requireNonNull(listener);
        WifiAccess.enforceAccess(mContext);
        unregister(IWindowsWifi.CALLBACK_WIFI_STATE, listener);
    }
    public void addSuggestionConnectionStatusListener(Executor executor, SuggestionConnectionStatusListener listener) {
        if (listener == null) throw new IllegalArgumentException("Listener cannot be null");
        if (executor == null) throw new IllegalArgumentException("Executor cannot be null");
        WifiAccess.enforceAccess(mContext);
        // From T, WifiServiceImpl requires ACCESS_FINE_LOCATION declared in the manifest.
        if (!granted(FINE)) throw new SecurityException("UID " + myUid() + " has no location permission");
        // Failures are reported only for suggested-network connection attempts.
        register(IWindowsWifi.CALLBACK_SUGGESTION_CONNECTION, listener, executor, value -> { });
    }
    public void removeSuggestionConnectionStatusListener(SuggestionConnectionStatusListener listener) {
        if (listener == null) throw new IllegalArgumentException("Listener cannot be null");
        WifiAccess.enforceAccess(mContext);
        unregister(IWindowsWifi.CALLBACK_SUGGESTION_CONNECTION, listener);
    }
    /** Called once on registration with the app's current approval status. */
    public void addSuggestionUserApprovalStatusListener(Executor executor,
            SuggestionUserApprovalStatusListener listener) {
        if (listener == null) throw new NullPointerException("Listener cannot be null");
        if (executor == null) throw new NullPointerException("Executor cannot be null");
        WifiAccess.enforceAccess(mContext);
        register(IWindowsWifi.CALLBACK_SUGGESTION_APPROVAL, listener, executor, listener::onUserApprovalStatusChange);
    }
    public void removeSuggestionUserApprovalStatusListener(SuggestionUserApprovalStatusListener listener) {
        if (listener == null) throw new IllegalArgumentException("Listener cannot be null");
        WifiAccess.enforceAccess(mContext);
        unregister(IWindowsWifi.CALLBACK_SUGGESTION_APPROVAL, listener);
    }
    public void addLocalOnlyConnectionFailureListener(Executor executor, LocalOnlyConnectionFailureListener listener) {
        if (listener == null) throw new IllegalArgumentException("Listener cannot be null");
        if (executor == null) throw new IllegalArgumentException("Executor cannot be null");
        WifiAccess.enforceAccess(mContext);
        // Failures are reported only for this app's network-specifier requests.
        register(IWindowsWifi.CALLBACK_LOCAL_ONLY_FAILURE, listener, executor, value -> { });
    }
    public void removeLocalOnlyConnectionFailureListener(LocalOnlyConnectionFailureListener listener) {
        if (listener == null) throw new IllegalArgumentException("Listener cannot be null");
        WifiAccess.enforceAccess(mContext);
        unregister(IWindowsWifi.CALLBACK_LOCAL_ONLY_FAILURE, listener);
    }
    /** Gesher has no Wi-Fi subsystem restart; registered callbacks are never invoked. */
    public void registerSubsystemRestartTrackingCallback(Executor executor, SubsystemRestartTrackingCallback callback) {
        if (executor == null) throw new IllegalArgumentException("executor must not be null");
        if (callback == null) throw new IllegalArgumentException("callback must not be null");
        WifiAccess.enforceAccess(mContext);
    }
    public void unregisterSubsystemRestartTrackingCallback(SubsystemRestartTrackingCallback callback) {
        if (callback == null) throw new IllegalArgumentException("callback must not be null");
        WifiAccess.enforceAccess(mContext);
    }

    private boolean declares(String permission) {
        try {
            android.content.pm.PackageInfo info = mContext.getPackageManager().getPackageInfo(
                    mContext.getPackageName(), PackageManager.GET_PERMISSIONS);
            if (info.requestedPermissions != null) {
                for (String requested : info.requestedPermissions) if (permission.equals(requested)) return true;
            }
            return false;
        } catch (PackageManager.NameNotFoundException | RuntimeException e) {
            // The system context has no package record of its own grants.
            return granted(permission);
        }
    }

    // ---- Hotspot, WPS, TDLS, Passpoint and policy APIs -----------------------------

    private void localOnlyHotspot(Executor executor, LocalOnlyHotspotCallback callback) {
        WifiAccess.enforceChange(mContext);
        if (targetSdk() < 33) {
            if (!granted(FINE) && !granted(COARSE)) {
                throw new SecurityException("UID " + myUid() + " has no location permission");
            }
            if (!locationEnabled()) throw new SecurityException("Location mode is not enabled.");
        } else if (!privileged() && !granted(NEARBY)) {
            throw new SecurityException("package=" + packageName() + " UID=" + myUid()
                    + " does not have nearby devices permission.");
        }
        // No SoftAP: the request fails as a soft-AP start failure does on Android.
        if (callback != null) post(executor, () -> callback.onFailed(LocalOnlyHotspotCallback.ERROR_GENERIC));
    }
    public void startLocalOnlyHotspot(LocalOnlyHotspotCallback callback, Handler handler) {
        Executor executor = handler == null ? mContext.getMainExecutor() : handler::post;
        localOnlyHotspot(executor, callback);
    }
    public void startLocalOnlyHotspotWithConfiguration(SoftApConfiguration config, Executor executor,
            LocalOnlyHotspotCallback callback) {
        Objects.requireNonNull(config);
        Objects.requireNonNull(executor);
        Objects.requireNonNull(callback);
        localOnlyHotspot(executor, callback);
    }
    /** No SoftAP capability exists, so no configuration is valid for this device. */
    public boolean validateSoftApConfiguration(SoftApConfiguration config) {
        if (config == null) throw new IllegalArgumentException("WifiManager: config can not be null");
        return false;
    }

    /** WPS is deprecated: Android reports an internal failure synchronously. */
    public void startWps(WpsInfo config, WpsCallback listener) {
        if (listener != null) listener.onFailed(FAILURE_INTERNAL_ERROR);
    }
    public void cancelWps(WpsCallback listener) {
        if (listener != null) listener.onFailed(FAILURE_INTERNAL_ERROR);
    }

    /** No TDLS: Android accepts the fire-and-forget request; nothing is enabled. */
    public void setTdlsEnabled(InetAddress remoteIPAddress, boolean enable) {
        remoteIPAddress.getHostAddress();
    }
    public void setTdlsEnabled(InetAddress remoteIPAddress, boolean enable, Executor executor,
            Consumer<Boolean> resultsCallback) {
        Objects.requireNonNull(executor, "executor cannot be null");
        Objects.requireNonNull(resultsCallback, "resultsCallback cannot be null");
        Objects.requireNonNull(remoteIPAddress, "remote IP address cannot be null");
        post(executor, () -> resultsCallback.accept(false));
    }
    public void setTdlsEnabledWithMacAddress(String remoteMacAddress, boolean enable) {
        if (remoteMacAddress == null) throw new IllegalArgumentException("remoteMacAddress cannot be null");
    }
    public void setTdlsEnabledWithMacAddress(String remoteMacAddress, boolean enable, Executor executor,
            Consumer<Boolean> resultsCallback) {
        Objects.requireNonNull(executor, "executor cannot be null");
        Objects.requireNonNull(resultsCallback, "resultsCallback cannot be null");
        Objects.requireNonNull(remoteMacAddress, "remote Mac address cannot be null");
        post(executor, () -> resultsCallback.accept(false));
    }
    public void isTdlsOperationCurrentlyAvailable(Executor executor, Consumer<Boolean> resultsCallback) {
        Objects.requireNonNull(executor, "executor cannot be null");
        Objects.requireNonNull(resultsCallback, "resultsCallback cannot be null");
        post(executor, () -> resultsCallback.accept(false));
    }
    public void getMaxSupportedConcurrentTdlsSessions(Executor executor, Consumer<Integer> resultsCallback) {
        Objects.requireNonNull(executor, "executor cannot be null");
        Objects.requireNonNull(resultsCallback, "resultsCallback cannot be null");
        post(executor, () -> resultsCallback.accept(-1));
    }
    public void getNumberOfEnabledTdlsSessions(Executor executor, Consumer<Integer> resultsCallback) {
        Objects.requireNonNull(executor, "executor cannot be null");
        Objects.requireNonNull(resultsCallback, "resultsCallback cannot be null");
        post(executor, () -> resultsCallback.accept(0));
    }

    /** Passpoint is unsupported: the service refuses and Android throws IAE. */
    public void addOrUpdatePasspointConfiguration(PasspointConfiguration config) {
        WifiAccess.enforceChange(mContext);
        throw new IllegalArgumentException();
    }
    public void removePasspointConfiguration(String fqdn) { throw new IllegalArgumentException(); }
    public List<PasspointConfiguration> getPasspointConfigurations() { return new ArrayList<>(); }
    public void flushPasspointAnqpCache() {
        if (!granted(NETWORK_SETTINGS) && !granted("android.permission.NETWORK_MANAGED_PROVISIONING")
                && !granted("android.permission.NETWORK_CARRIER_PROVISIONING")) {
            throw new SecurityException("Caller does not have permission to flush the Passpoint ANQP cache");
        }
        // There is no Passpoint ANQP cache to flush.
    }

    public void reportCreateInterfaceImpact(int interfaceType, boolean requireNewInterface, Executor executor,
            BiConsumer<Boolean, Set<InterfaceCreationImpact>> resultCallback) {
        Objects.requireNonNull(executor, "Non-null executor required");
        Objects.requireNonNull(resultCallback, "Non-null resultCallback required");
        if (interfaceType < WIFI_INTERFACE_TYPE_STA || interfaceType > WIFI_INTERFACE_TYPE_DIRECT) {
            throw new IllegalArgumentException("Invalid interfaceType");
        }
        WifiAccess.enforceAccess(mContext);
        if (!granted("android.permission.MANAGE_WIFI_INTERFACES")) {
            throw new SecurityException("WifiService Uid " + myUid() + " Missing MANAGE_WIFI_INTERFACES permission");
        }
        // Gesher creates no Wi-Fi interfaces: creation is not possible.
        post(executor, () -> resultCallback.accept(false, Collections.emptySet()));
    }

    private boolean manageNetworkSelection() {
        return granted(NETWORK_SETTINGS) || granted("android.permission.MANAGE_WIFI_NETWORK_SELECTION");
    }
    public void allowAutojoinGlobal(boolean allowAutojoin) {
        if (!manageNetworkSelection()) {
            throw new SecurityException("Uid " + myUid() + " is not allowed to set wifi global autojoin");
        }
        throw new UnsupportedOperationException("Windows owns automatic network selection");
    }
    public void queryAutojoinGlobal(Executor executor, Consumer<Boolean> resultsCallback) {
        Objects.requireNonNull(executor, "executor cannot be null");
        Objects.requireNonNull(resultsCallback, "resultsCallback cannot be null");
        if (!manageNetworkSelection()) {
            throw new SecurityException("Uid " + myUid() + " is not allowed to get wifi global autojoin");
        }
        throw new UnsupportedOperationException("Windows owns automatic network selection");
    }
    public void setSendDhcpHostnameRestriction(int restriction) {
        if ((restriction & ~(FLAG_SEND_DHCP_HOSTNAME_RESTRICTION_OPEN | FLAG_SEND_DHCP_HOSTNAME_RESTRICTION_SECURE)) != 0) {
            throw new IllegalArgumentException("Unknown dhcp hostname restriction flags: " + restriction);
        }
        if (!privileged()) {
            throw new SecurityException("Uid " + myUid() + " is not allowed to query the global dhcp hostname restriction");
        }
        throw new UnsupportedOperationException("Windows owns the DHCP client");
    }
    public void querySendDhcpHostnameRestriction(Executor executor, IntConsumer resultsCallback) {
        Objects.requireNonNull(executor, "executor cannot be null");
        Objects.requireNonNull(resultsCallback, "resultsCallback cannot be null");
        if (!privileged()) {
            throw new SecurityException("Uid " + myUid() + " is not allowed to query the global dhcp hostname restriction");
        }
        throw new UnsupportedOperationException("Windows owns the DHCP client");
    }
    public void setPerSsidRoamingMode(WifiSsid ssid, int roamingMode) {
        if (roamingMode < ROAMING_MODE_NONE || roamingMode > ROAMING_MODE_AGGRESSIVE) {
            throw new IllegalArgumentException("invalid roaming mode: " + roamingMode);
        }
        Objects.requireNonNull(ssid, "ssid cannot be null");
        if (roamingMode == ROAMING_MODE_AGGRESSIVE) {
            throw new UnsupportedOperationException("Aggressive roaming mode not supported");
        }
        if (!manageNetworkSelection()) {
            throw new SecurityException("Uid=" + myUid() + " is not allowed to add roaming policies");
        }
        throw new UnsupportedOperationException("Windows owns roaming policy");
    }
    public void removePerSsidRoamingMode(WifiSsid ssid) {
        Objects.requireNonNull(ssid, "ssid cannot be null");
        if (!manageNetworkSelection()) {
            throw new SecurityException("Uid=" + myUid() + " is not allowed to remove roaming policies");
        }
        throw new UnsupportedOperationException("Windows owns roaming policy");
    }
    public void getPerSsidRoamingModes(Executor executor, Consumer<Map<String, Integer>> resultsCallback) {
        Objects.requireNonNull(executor, "executor cannot be null");
        Objects.requireNonNull(resultsCallback, "resultsCallback cannot be null");
        if (!manageNetworkSelection()) {
            throw new SecurityException("Uid=" + myUid() + " is not allowed to get roaming policies");
        }
        // No per-SSID policy has been set.
        post(executor, () -> resultsCallback.accept(new HashMap<>()));
    }

    // ---- Locks -------------------------------------------------------------------

    public WifiLock createWifiLock(String tag) { return createWifiLock(WIFI_MODE_FULL, tag); }
    public WifiLock createWifiLock(int lockType, String tag) { return new WifiLock(lockType, tag); }
    public MulticastLock createMulticastLock(String tag) { return new MulticastLock(tag); }

    private void enforceLockPermission(String permission) {
        if (!granted(permission)) {
            throw new SecurityException("Neither user " + myUid() + " nor current process has " + permission + ".");
        }
    }

    private void lockAcquired() {
        synchronized (mLockCountLock) {
            if (mActiveLockCount >= MAX_ACTIVE_LOCKS) {
                throw new UnsupportedOperationException("Exceeded maximum number of wifi locks");
            }
            mActiveLockCount++;
        }
    }

    private void lockReleased() {
        synchronized (mLockCountLock) { if (mActiveLockCount > 0) mActiveLockCount--; }
    }

    /**
     * Windows keeps an associated adapter connected without an Android-style lock; the
     * lock records the request with Android's permission, mode and count contracts.
     */
    public class WifiLock {
        private final int mLockType;
        private final String mTag;
        private boolean mHeld;
        private boolean mRefCounted = true;
        private int mRefCount;
        private WorkSource mWorkSource;

        private WifiLock(int lockType, String tag) { mLockType = lockType; mTag = tag; }

        // As Android: the count moves before the checks, so a failed first acquire leaves
        // it at 1 (a retry is then a no-op) and an over-release leaves it negative.
        public void acquire() {
            synchronized (this) {
                if (mRefCounted ? ++mRefCount == 1 : !mHeld) {
                    enforceLockPermission(WAKE_LOCK);
                    if (mLockType < WIFI_MODE_FULL || mLockType > WIFI_MODE_FULL_LOW_LATENCY) {
                        throw new IllegalArgumentException("lockMode =" + mLockType);
                    }
                    lockAcquired();
                    mHeld = true;
                }
            }
        }
        public void release() {
            synchronized (this) {
                if (mRefCounted ? --mRefCount == 0 : mHeld) {
                    enforceLockPermission(WAKE_LOCK);
                    lockReleased();
                    mHeld = false;
                }
                if (mRefCount < 0) throw new RuntimeException("WifiLock under-locked " + mTag);
            }
        }
        /** A held lock that is garbage collected stops counting toward the 50-lock limit. */
        @Override protected void finalize() throws Throwable {
            try {
                synchronized (this) {
                    if (mHeld) { lockReleased(); mHeld = false; }
                }
            } finally {
                super.finalize();
            }
        }
        public void setReferenceCounted(boolean refCounted) {
            synchronized (this) { mRefCounted = refCounted; }
        }
        public boolean isHeld() {
            synchronized (this) { return mHeld; }
        }
        public void setWorkSource(WorkSource ws) {
            synchronized (this) {
                if (ws != null && ws.isEmpty()) ws = null;
                // As Android: clearing always counts as a change (and is applied first);
                // a held lock's change needs UPDATE_DEVICE_STATS.
                boolean changed = true;
                if (ws == null) {
                    mWorkSource = null;
                } else {
                    ws = ws.withoutNames();
                    if (mWorkSource == null) {
                        changed = false;
                        mWorkSource = new WorkSource(ws);
                    } else {
                        changed = !mWorkSource.equals(ws);
                        if (changed) mWorkSource.set(ws);
                    }
                }
                if (changed && mHeld && !granted("android.permission.UPDATE_DEVICE_STATS")) {
                    throw new SecurityException("Neither user " + myUid()
                            + " nor current process has android.permission.UPDATE_DEVICE_STATS.");
                }
            }
        }
        @Override public String toString() {
            synchronized (this) {
                return "WifiLock{ " + Integer.toHexString(System.identityHashCode(this)) + "; "
                        + (mHeld ? "held; " : "")
                        + (mRefCounted ? "refcounted: refcount = " + mRefCount : "not refcounted") + " }";
            }
        }
    }

    /** Windows does not filter multicast for an associated adapter; the lock is accounted. */
    public class MulticastLock {
        private final String mTag;
        private boolean mHeld;
        private boolean mRefCounted = true;
        private int mRefCount;

        private MulticastLock(String tag) { mTag = tag; }

        public void acquire() {
            synchronized (this) {
                if (mRefCounted ? ++mRefCount == 1 : !mHeld) {
                    enforceLockPermission(MULTICAST);
                    lockAcquired();
                    mHeld = true;
                }
            }
        }
        public void release() {
            synchronized (this) {
                if (mRefCounted ? --mRefCount == 0 : mHeld) {
                    enforceLockPermission(MULTICAST);
                    lockReleased();
                    mHeld = false;
                }
                if (mRefCount < 0) throw new RuntimeException("MulticastLock under-locked " + mTag);
            }
        }
        /** As Android: a held lock that is garbage collected is released. */
        @Override protected void finalize() throws Throwable {
            try {
                synchronized (this) {
                    if (mHeld) { lockReleased(); mHeld = false; }
                }
            } finally {
                super.finalize();
            }
        }
        public void setReferenceCounted(boolean refCounted) {
            synchronized (this) { mRefCounted = refCounted; }
        }
        public boolean isHeld() {
            synchronized (this) { return mHeld; }
        }
        @Override public String toString() {
            synchronized (this) {
                return "MulticastLock{ " + Integer.toHexString(System.identityHashCode(this)) + "; "
                        + (mHeld ? "held; " : "")
                        + (mRefCounted ? "refcounted: refcount = " + mRefCount : "not refcounted") + " }";
            }
        }
    }

    // ---- Nested public types -------------------------------------------------------

    public abstract static class ScanResultsCallback {
        public ScanResultsCallback() { }
        public abstract void onScanResultsAvailable();
    }

    public interface WifiStateChangedListener {
        void onWifiStateChanged();
    }

    public interface SuggestionConnectionStatusListener {
        void onConnectionStatus(WifiNetworkSuggestion wifiNetworkSuggestion, int failureReason);
    }

    public interface SuggestionUserApprovalStatusListener {
        void onUserApprovalStatusChange(int status);
    }

    public interface LocalOnlyConnectionFailureListener {
        void onConnectionFailed(WifiNetworkSpecifier wifiNetworkSpecifier, int failureReason);
    }

    public abstract static class SubsystemRestartTrackingCallback {
        public SubsystemRestartTrackingCallback() { }
        public abstract void onSubsystemRestarting();
        public abstract void onSubsystemRestarted();
    }

    public abstract static class WpsCallback {
        public WpsCallback() { }
        public abstract void onStarted(String pin);
        public abstract void onSucceeded();
        public abstract void onFailed(int reason);
    }

    public static class LocalOnlyHotspotCallback {
        /** @hide */ public static final int REQUEST_REGISTERED = 0;
        public static final int ERROR_NO_CHANNEL = 1;
        public static final int ERROR_GENERIC = 2;
        public static final int ERROR_INCOMPATIBLE_MODE = 3;
        public static final int ERROR_TETHERING_DISALLOWED = 4;
        public LocalOnlyHotspotCallback() { }
        public void onStarted(LocalOnlyHotspotReservation reservation) { }
        public void onStopped() { }
        public void onFailed(int reason) { }
    }

    /** Never handed out here: no local-only hotspot can start. */
    public class LocalOnlyHotspotReservation implements AutoCloseable {
        private final SoftApConfiguration mConfig;
        private LocalOnlyHotspotReservation(SoftApConfiguration config) { mConfig = config; }
        public WifiConfiguration getWifiConfiguration() {
            return mConfig == null ? null : mConfig.toWifiConfiguration();
        }
        public SoftApConfiguration getSoftApConfiguration() { return mConfig; }
        @Override public void close() { }
    }

    public static class InterfaceCreationImpact {
        private final int mInterfaceType;
        private final Set<String> mPackages;
        public InterfaceCreationImpact(int interfaceType, Set<String> packages) {
            mInterfaceType = interfaceType;
            mPackages = packages;
        }
        public int getInterfaceType() { return mInterfaceType; }
        public Set<String> getPackages() { return mPackages; }
        @Override public boolean equals(Object other) {
            if (this == other) return true;
            if (!(other instanceof InterfaceCreationImpact that)) return false;
            return mInterfaceType == that.mInterfaceType && Objects.equals(mPackages, that.mPackages);
        }
        @Override public int hashCode() { return Objects.hash(mInterfaceType, mPackages); }
    }

    public static final class AddNetworkResult implements Parcelable {
        public static final int STATUS_SUCCESS = 0;
        public static final int STATUS_FAILURE_UNKNOWN = 1;
        public static final int STATUS_NO_PERMISSION = 2;
        public static final int STATUS_ADD_PASSPOINT_FAILURE = 3;
        public static final int STATUS_ADD_WIFI_CONFIG_FAILURE = 4;
        public static final int STATUS_INVALID_CONFIGURATION = 5;
        public static final int STATUS_NO_PERMISSION_MODIFY_CONFIG = 6;
        public static final int STATUS_NO_PERMISSION_MODIFY_PROXY_SETTING = 7;
        public static final int STATUS_NO_PERMISSION_MODIFY_MAC_RANDOMIZATION = 8;
        public static final int STATUS_FAILURE_UPDATE_NETWORK_KEYS = 9;
        public static final int STATUS_INVALID_CONFIGURATION_ENTERPRISE = 10;

        public final int statusCode;
        public final int networkId;

        public AddNetworkResult(int statusCode, int networkId) {
            this.statusCode = statusCode;
            this.networkId = networkId;
        }
        @Override public int describeContents() { return 0; }
        @Override public void writeToParcel(Parcel dest, int flags) {
            dest.writeInt(statusCode);
            dest.writeInt(networkId);
        }
        public static final Creator<AddNetworkResult> CREATOR = new Creator<AddNetworkResult>() {
            @Override public AddNetworkResult createFromParcel(Parcel in) {
                return new AddNetworkResult(in.readInt(), in.readInt());
            }
            @Override public AddNetworkResult[] newArray(int size) { return new AddNetworkResult[size]; }
        };
    }
}
