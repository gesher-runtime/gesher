/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package gesher.services;

import java.net.URL;
import java.net.URLClassLoader;
import java.nio.file.Path;

/** The host boundary initializes framework natives; it implements no Parcel/value behavior. */
public final class WifiP2pValuesHost {
    private static final String[] FAMILY = {
        "wifi/p2p/WifiP2pConfig", "wifi/p2p/WifiP2pPairingBootstrappingConfig", "wifi/p2p/WifiP2pDevice",
        "wifi/p2p/WifiP2pWfdInfo", "wifi/p2p/WifiP2pDeviceList", "wifi/p2p/WifiP2pGroup", "wifi/p2p/WifiP2pInfo",
        "wifi/WpsInfo", "wifi/WifiInfo", "wifi/WifiConfiguration", "wifi/WifiEnterpriseConfig",
        "wifi/SecurityParams", "wifi/ParcelUtil", "wifi/OuiKeyedData", "wifi/util/PersistableBundleUtils",
        "wifi/WifiSsid", "wifi/util/HexEncoding", "wifi/SupplicantState", "wifi/MloLink", "wifi/ScanResult",
        "wifi/ScanResultSecurity", "wifi/AnqpInformationElement", "MacAddress", "TransportInfo"
    };
    private static final class BaselineLoader extends URLClassLoader {
        BaselineLoader(URL[] urls, ClassLoader parent) { super(urls, parent); }
        @Override protected synchronized Class<?> loadClass(String name, boolean resolve) throws ClassNotFoundException {
            if (!name.startsWith("android.net.wifi.p2p.") && !name.startsWith("gesher.services.WifiP2pValuesContractTest"))
                return super.loadClass(name, resolve);
            Class<?> type = findLoadedClass(name);
            if (type == null) type = findClass(name);
            if (resolve) resolveClass(type);
            return type;
        }
    }
    public static void main(String[] args) throws Exception {
        System.load(args[0]);
        Path expected = Path.of(args[1]).toRealPath();
        if (args[3].equals("baseline")) {
            try (BaselineLoader loader = new BaselineLoader(new URL[]{expected.toUri().toURL(),
                    Path.of(args[2]).toRealPath().toUri().toURL()}, WifiP2pValuesHost.class.getClassLoader())) {
                Class.forName("gesher.services.WifiP2pValuesContractTest", true, loader).getMethod("baselineControl").invoke(null);
            }
            return;
        }
        for (String relative : FAMILY) {
            String name = "android.net." + relative.replace('/', '.');
            Class<?> type = Class.forName(name);
            Path actual = Path.of(type.getProtectionDomain().getCodeSource().getLocation().toURI()).toRealPath();
            if (!actual.equals(expected)) throw new AssertionError("Unexpected production provider: " + name + ": " + actual);
        }
        System.out.println("P2P host providers: 24 freshly compiled production families");
        WifiP2pValuesContractTest.run(false, true, false);
        Class.forName("gesher.services.WifiP2pValuesExtendedTest").getMethod("run", boolean.class, boolean.class).invoke(null, false, false);
    }
}
