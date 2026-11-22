/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package gesher.services;

import java.net.URL;
import java.net.URLClassLoader;
import java.nio.file.Path;

/** Native initialization boundary only: no Parcel or Wi-Fi value substitute. */
public final class WifiSuggestionValuesHost {
    private static final String[] FAMILY = {
        "wifi/WifiNetworkSuggestion", "wifi/hotspot2/PasspointConfiguration", "wifi/hotspot2/pps/HomeSp",
        "wifi/hotspot2/pps/Credential", "wifi/hotspot2/pps/Policy", "wifi/hotspot2/pps/UpdateParameter",
        "wifi/EAPConstants", "wifi/util/BuildProperties", "wifi/WifiInfo", "wifi/WifiConfiguration",
        "wifi/WifiEnterpriseConfig", "wifi/SecurityParams", "wifi/ParcelUtil", "wifi/OuiKeyedData",
        "wifi/util/PersistableBundleUtils", "wifi/WifiSsid", "wifi/util/HexEncoding", "wifi/SupplicantState",
        "wifi/MloLink", "wifi/ScanResult", "wifi/ScanResultSecurity", "wifi/AnqpInformationElement",
        "MacAddress", "TransportInfo"
    };
    private static final class BaselineLoader extends URLClassLoader {
        BaselineLoader(URL[] urls, ClassLoader parent) { super(urls, parent); }
        @Override protected synchronized Class<?> loadClass(String name, boolean resolve)
                throws ClassNotFoundException {
            boolean absent = name.startsWith("android.net.wifi.WifiNetworkSuggestion")
                    || name.startsWith("android.net.wifi.hotspot2.");
            if (!absent && !name.startsWith("gesher.services.WifiSuggestion")) return super.loadClass(name, resolve);
            Class<?> result = findLoadedClass(name);
            if (result == null) result = findClass(name);
            if (resolve) resolveClass(result);
            return result;
        }
    }
    public static void main(String[] args) throws Exception {
        System.load(args[0]);
        Path expected = Path.of(args[1]).toRealPath();
        if (args.length > 3 && !args[3].equals("candidate")) {
            try (BaselineLoader loader = new BaselineLoader(new URL[]{expected.toUri().toURL(),
                    Path.of(args[2]).toRealPath().toUri().toURL()}, WifiSuggestionValuesHost.class.getClassLoader())) {
                Class<?> test = Class.forName("gesher.services.WifiSuggestionValuesContractTest", true, loader);
                test.getMethod(args[3].equals("suggestion") ? "baselineSuggestionControl" : "baselinePasspointControl").invoke(null);
            }
            return;
        }
        for (String relative : FAMILY) {
            String name = "android.net." + relative.replace('/', '.');
            Class<?> type = Class.forName(name);
            Path actual = Path.of(type.getProtectionDomain().getCodeSource().getLocation().toURI()).toRealPath();
            if (!actual.equals(expected)) throw new AssertionError("Unexpected production provider " + name + ": " + actual);
        }
        System.out.println("Suggestion providers: " + FAMILY.length + " freshly compiled production families");
        WifiSuggestionValuesContractTest.verifyGraph();
        WifiSuggestionValuesContractTest.run(false, true);
    }
}
