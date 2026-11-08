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
    private static void unsupported(Runnable action) {
        try { action.run(); }
        catch (UnsupportedOperationException expected) { checks++; return; }
        throw new AssertionError("Unsupported Wi-Fi operation reported success");
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
        unsupported(() -> config.setSecurityParams(0));
        WifiManager.WifiLock lock = wifi.createWifiLock("synthetic-test");
        lock.setReferenceCounted(false);
        require(!lock.isHeld(), "Unavailable power lock reported held");
        unsupported(lock::acquire);
        require(!lock.isHeld(), "Failed lock acquisition reported held");
        unsupported(lock::release);
        WifiManager.MulticastLock multicast = wifi.createMulticastLock("synthetic-test");
        require(!multicast.isHeld(), "Unavailable multicast lock reported held");
        unsupported(multicast::acquire);
        unsupported(multicast::release);
        require(WifiManager.calculateSignalLevel(-110, 5) == 0, "Low RSSI clamp incorrect");
        require(WifiManager.calculateSignalLevel(-55, 5) == 4, "High RSSI clamp incorrect");
        require(WifiManager.calculateSignalLevel(-77, 5) == 2, "Legacy RSSI interpolation incorrect");
        require(WifiManager.calculateSignalLevel(-77, 1) == 0, "Single signal level incorrect");
        Parcel parcel = Parcel.obtain();
        try {
            unsupported(() -> info.writeToParcel(parcel, 0));
            unsupported(() -> config.writeToParcel(parcel, 0));
            unsupported(() -> new ScanResult().writeToParcel(parcel, 0));
            require(parcel.dataSize() == 0, "Unsupported parcel transport published a partial invented wire format");
        } finally { parcel.recycle(); }
        System.out.println("WifiUnsupportedTest all passed: " + checks);
    }
}
