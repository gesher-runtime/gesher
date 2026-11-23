/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package gesher.services;

import java.net.URL;
import java.net.URLClassLoader;
import java.nio.file.Path;

/** Genuine framework initialization only; no value or Parcel implementation. */
public final class WifiP2pDiscoveryValuesHost {
    private static final String PREFIX = "android.net.wifi.p2p.";
    private static final String[] TYPES = {"WifiP2pDiscoveryConfig", "WifiP2pDiscoveryConfig$Builder",
            "WifiP2pUsdBasedLocalServiceAdvertisementConfig", "WifiP2pUsdBasedLocalServiceAdvertisementConfig$Builder",
            "WifiP2pUsdBasedServiceDiscoveryConfig", "WifiP2pUsdBasedServiceDiscoveryConfig$Builder",
            "WifiP2pDirInfo", "nsd.WifiP2pUsdBasedServiceResponse"};
    private static final class BaselineLoader extends URLClassLoader {
        BaselineLoader(URL[] urls, ClassLoader parent) { super(urls, parent); }
        @Override protected synchronized Class<?> loadClass(String name, boolean resolve) throws ClassNotFoundException {
            boolean selected = name.startsWith("gesher.services.WifiP2pDiscoveryValuesContractTest");
            for (String type : TYPES) selected |= name.equals(PREFIX + type) || name.startsWith(PREFIX + type + "$");
            if (!selected) return super.loadClass(name, resolve);
            Class<?> value = findLoadedClass(name);
            if (value == null) value = findClass(name); // absent selected class cannot fall back to a cached stub
            if (resolve) resolveClass(value);
            return value;
        }
    }
    private static void provider(String name, Path expected) throws Exception {
        Class<?> type = Class.forName(name, false, WifiP2pDiscoveryValuesHost.class.getClassLoader());
        Path actual = Path.of(type.getProtectionDomain().getCodeSource().getLocation().toURI()).toRealPath();
        if (!actual.equals(expected)) throw new AssertionError("Unexpected provider for " + name + ": " + actual);
        System.out.println("P2P_DISCOVERY_HOST_PROVIDER " + name + "=" + actual);
    }
    public static void main(String[] args) throws Exception {
        System.load(args[0]);
        Path classes = Path.of(args[1]).toRealPath();
        if (args[5].equals("baseline")) {
            try (BaselineLoader loader = new BaselineLoader(new URL[] {classes.toUri().toURL(),
                    Path.of(args[2]).toRealPath().toUri().toURL()}, WifiP2pDiscoveryValuesHost.class.getClassLoader())) {
                Class.forName("gesher.services.WifiP2pDiscoveryValuesContractTest", true, loader)
                        .getMethod("baselineControl").invoke(null);
            }
            return;
        }
        for (String type : TYPES) provider(PREFIX + type, classes);
        for (String type : new String[] {"WifiP2pDiscoveryConfig", "WifiP2pUsdBasedLocalServiceAdvertisementConfig",
                "WifiP2pUsdBasedServiceDiscoveryConfig", "WifiP2pDirInfo", "nsd.WifiP2pUsdBasedServiceResponse"})
            provider(PREFIX + type + "$1", classes);
        for (String type : new String[] {"android.net.wifi.OuiKeyedData", "android.net.wifi.OuiKeyedData$1",
                "android.net.wifi.OuiKeyedData$Builder", "android.net.wifi.util.PersistableBundleUtils",
                "android.internal.modules.utils.build.SdkLevel"}) provider(type, classes);
        for (String type : new String[] {"android.os.Parcel", "android.os.Parcelable", "android.os.PersistableBundle"})
            provider(type, Path.of(args[3]).toRealPath());
        provider("android.net.MacAddress", Path.of(args[4]).toRealPath());
        System.out.println("P2P_DISCOVERY_HOST_PROVIDERS eight fresh SDK types; complete fresh helpers; no Parcel replacement");
        WifiP2pDiscoveryValuesContractTest.run(false, true);
        Class.forName("gesher.services.WifiP2pDiscoveryValuesExtendedTest")
                .getMethod("run", boolean.class, boolean.class).invoke(null, false, true);
        WifiP2pDiscoveryValuesContractTest.finish(false, true);
    }
}
