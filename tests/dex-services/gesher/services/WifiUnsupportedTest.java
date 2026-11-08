/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package gesher.services;

import android.content.Context;
import android.content.pm.PackageManager;
import android.content.pm.FeatureInfo;
import android.content.pm.SystemFeaturesCache;
import android.net.wifi.ScanResult;
import android.net.wifi.WifiConfiguration;
import android.net.wifi.WifiInfo;
import android.net.wifi.WifiManager;
import android.os.Parcel;
import java.lang.reflect.Constructor;
import java.lang.reflect.Field;
import java.lang.reflect.Method;
import java.util.List;

/** Real Context service registration, with explicit unsupported hardware behavior. */
public final class WifiUnsupportedTest {
    private static int checks;
    private static void require(boolean value, String reason) {
        if (!value) throw new AssertionError(reason);
        checks++;
    }
    private static <T extends android.os.Parcelable> T roundTrip(T value, android.os.Parcelable.Creator<T> creator) {
        Parcel parcel = Parcel.obtain();
        try {
            value.writeToParcel(parcel, 0); parcel.setDataPosition(0);
            T result = creator.createFromParcel(parcel);
            require(parcel.dataAvail() == 0, "Wi-Fi parcel did not consume its complete wire representation");
            return result;
        } finally { parcel.recycle(); }
    }
    public static void main(String[] args) throws Exception {
        String root = System.getenv("ANDROID_ROOT");
        com.gesher.app.Main.bootstrap("wifi-test", root + "/build.prop", root + "/framework/framework-res.apk");
        com.gesher.server.Services.installInProcess(com.gesher.server.Services.create(List.of()));
        // bindApplication normally installs this cache from system_server's feature list.
        Field features = Class.forName("com.gesher.server.PackageManagerService").getDeclaredField("FEATURES");
        features.setAccessible(true);
        SystemFeaturesCache.setInstance(new SystemFeaturesCache(
                (android.util.ArrayMap<String, FeatureInfo>) features.get(null)));
        Class<?> threadType = Class.forName("android.app.ActivityThread");
        Constructor<?> constructor = threadType.getDeclaredConstructor(); constructor.setAccessible(true);
        Object thread = constructor.newInstance();
        Class<?> contextType = Class.forName("android.app.ContextImpl");
        Method create = contextType.getDeclaredMethod("createSystemContext", threadType); create.setAccessible(true);
        Context context = (Context) create.invoke(null, thread);
        WifiManager wifi = context.getSystemService(WifiManager.class);
        require(wifi != null, "Typed Context lookup did not create the Wi-Fi wrapper");
        require(wifi == context.getSystemService(Context.WIFI_SERVICE), "String and typed service lookup differ");
        require(!context.getPackageManager().hasSystemFeature(PackageManager.FEATURE_WIFI), "Wrapper declared Wi-Fi hardware");
        require(!wifi.isWifiEnabled() && wifi.getWifiState() == WifiManager.WIFI_STATE_DISABLED, "No-controller Wi-Fi state incorrect");
        require(wifi.getWifiApState() == WifiManager.WIFI_AP_STATE_DISABLED, "Unsupported access point reported enabled");
        require(!wifi.isWpa3SaeSupported(), "Unsupported WPA3 reported supported");
        require(wifi.getScanResults().isEmpty(), "Unsupported controller exposed scan entries");
        require(!wifi.startScan() && wifi.getScanResults().isEmpty(), "Unsupported legacy scan reported success or data");
        WifiInfo info = wifi.getConnectionInfo();
        require(info != null && WifiManager.UNKNOWN_SSID.equals(info.getSSID()), "Disconnected SSID incorrect");
        require(info.getBSSID() == null && info.getNetworkId() == -1, "Disconnected snapshot invented a network identity");
        require(info.getRssi() == -127 && info.getLinkSpeed() == -1, "Unknown signal and speed differ from API 36 defaults");
        require(info != wifi.getConnectionInfo(), "Connection snapshots unexpectedly share mutable identity");
        require(!wifi.setWifiEnabled(true) && !wifi.setWifiEnabled(false), "Unsupported radio control reported success");
        WifiConfiguration config = new WifiConfiguration();
        config.SSID = "synthetic-test";
        require(config.networkId == -1 && config.wepKeys.length == 4, "Configuration defaults differ from API 36");
        require(config.allowedKeyManagement.isEmpty() && config.allowedGroupCiphers.isEmpty(), "Configuration invented security parameters");
        require(wifi.addNetwork(config) == -1 && !wifi.enableNetwork(0, true), "Unsupported network mutation reported success");
        require(!wifi.isWifiEnabled() && wifi.getScanResults().isEmpty(), "Rejected mutation changed controller state");
        config.setSecurityParams(0);
        require(config.allowedKeyManagement.get(0), "Open security data was not represented");
        config.setSecurityParams(2);
        require(config.allowedKeyManagement.get(1) && !config.allowedKeyManagement.get(0), "PSK did not replace open security data");
        try { config.setSecurityParams(99); throw new AssertionError("Unknown security type accepted"); }
        catch (IllegalArgumentException expected) { checks++; }
        WifiManager.WifiLock lock = wifi.createWifiLock("synthetic-test");
        require(!lock.isHeld(), "Unacquired compatibility token reported held");
        lock.acquire(); lock.acquire(); lock.release();
        require(lock.isHeld(), "Reference-counted Wi-Fi token released too early");
        lock.release(); require(!lock.isHeld(), "Wi-Fi token leaked after final release");
        try { lock.release(); throw new AssertionError("Wi-Fi token underflow accepted"); }
        catch (RuntimeException expected) { checks++; }
        lock.setReferenceCounted(false); lock.acquire(); lock.acquire(); lock.release();
        require(!lock.isHeld(), "Non-reference-counted token retained stale acquisitions");
        lock.acquire(); lock.setReferenceCounted(true); lock.release();
        require(!lock.isHeld(), "Changing token mode retained stale reference counts");
        WifiManager.MulticastLock multicast = wifi.createMulticastLock("synthetic-test");
        multicast.acquire(); multicast.acquire(); multicast.release();
        require(multicast.isHeld(), "Multicast token lost a live reference");
        multicast.release(); require(!multicast.isHeld(), "Multicast token leaked");
        require(!wifi.isWifiEnabled() && wifi.getScanResults().isEmpty(), "Logical tokens fabricated a Wi-Fi controller");
        require(WifiManager.calculateSignalLevel(-110, 5) == 0, "Low RSSI clamp incorrect");
        require(WifiManager.calculateSignalLevel(-55, 5) == 4, "High RSSI clamp incorrect");
        require(WifiManager.calculateSignalLevel(-77, 5) == 2, "Legacy RSSI interpolation incorrect");
        require(WifiManager.calculateSignalLevel(-77, 1) == 0, "Single signal level incorrect");
        WifiInfo infoCopy = roundTrip(info, WifiInfo.CREATOR);
        require(WifiManager.UNKNOWN_SSID.equals(infoCopy.getSSID()) && infoCopy.getNetworkId() == -1
                && infoCopy.getRssi() == -127, "Disconnected parcel invented a network");
        config.networkId = 7; config.BSSID = "02:00:00:00:00:01"; config.preSharedKey = "synthetic-secret";
        config.hiddenSSID = true; config.wepKeys[2] = "synthetic-wep"; config.wepTxKeyIndex = 2;
        config.allowedProtocols.set(31); config.allowedAuthAlgorithms.set(3);
        config.allowedPairwiseCiphers.set(7); config.allowedGroupCiphers.set(5);
        WifiConfiguration copy = roundTrip(config, WifiConfiguration.CREATOR);
        require(copy.networkId == 7 && config.SSID.equals(copy.SSID) && config.BSSID.equals(copy.BSSID)
                && config.preSharedKey.equals(copy.preSharedKey) && copy.hiddenSSID && copy.wepTxKeyIndex == 2
                && java.util.Arrays.equals(config.wepKeys, copy.wepKeys), "Wi-Fi configuration parcel lost identity/key fields");
        require(config.allowedKeyManagement.equals(copy.allowedKeyManagement) && config.allowedProtocols.equals(copy.allowedProtocols)
                && config.allowedAuthAlgorithms.equals(copy.allowedAuthAlgorithms) && config.allowedPairwiseCiphers.equals(copy.allowedPairwiseCiphers)
                && config.allowedGroupCiphers.equals(copy.allowedGroupCiphers), "Wi-Fi parcel dropped security bitsets");
        config.wepKeys = null; config.allowedProtocols = null;
        copy = roundTrip(config, WifiConfiguration.CREATOR);
        require(copy.wepKeys == null && copy.allowedProtocols == null, "Wi-Fi parcel lost nullable fields");
        ScanResult scan = new ScanResult(); scan.SSID = "רשת סינתטית"; scan.BSSID = "02:00:00:00:00:02";
        scan.capabilities = "[synthetic]"; scan.frequency = 5180; scan.level = -77; scan.timestamp = 1234567890123L;
        ScanResult scanCopy = roundTrip(scan, ScanResult.CREATOR);
        require(scan.SSID.equals(scanCopy.SSID) && scan.BSSID.equals(scanCopy.BSSID)
                && scan.capabilities.equals(scanCopy.capabilities) && scan.frequency == scanCopy.frequency
                && scan.level == scanCopy.level && scan.timestamp == scanCopy.timestamp, "Scan data parcel lost supplied values");
        System.out.println("WifiUnsupportedTest all passed: " + checks);
    }
}
