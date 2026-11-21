/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package gesher.services;

import java.net.URL;
import java.net.URLClassLoader;
import java.nio.file.Path;

/** No Wi-Fi or Parcel substitute: only framework native initialization is adapted. */
public final class SoftApValuesHost {
    private static final String[] FAMILY = {
        "wifi/SoftApConfiguration", "wifi/WpsInfo", "wifi/WifiInfo",
        "wifi/WifiConfiguration", "wifi/WifiEnterpriseConfig", "wifi/SecurityParams",
        "wifi/ParcelUtil", "wifi/OuiKeyedData", "wifi/util/PersistableBundleUtils",
        "wifi/WifiSsid", "wifi/util/HexEncoding", "wifi/SupplicantState", "wifi/MloLink",
        "wifi/ScanResult", "wifi/ScanResultSecurity", "wifi/AnqpInformationElement",
        "MacAddress", "TransportInfo"
    };
    private static final class BaselineLoader extends URLClassLoader {
        BaselineLoader(URL[] urls, ClassLoader parent) { super(urls, parent); }
        @Override protected synchronized Class<?> loadClass(String name, boolean resolve)
                throws ClassNotFoundException {
            boolean absent = name.equals("android.net.wifi.SoftApConfiguration")
                    || name.startsWith("android.net.wifi.SoftApConfiguration$")
                    || name.equals("android.net.wifi.WpsInfo")
                    || name.startsWith("android.net.wifi.WpsInfo$");
            if (!absent && !name.startsWith("gesher.services.SoftApValuesContractTest"))
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
        if (args.length > 3 && !args[3].equals("candidate")) {
            try (BaselineLoader loader = new BaselineLoader(
                    new URL[]{expected.toUri().toURL(), Path.of(args[2]).toRealPath().toUri().toURL()},
                    SoftApValuesHost.class.getClassLoader())) {
                Class<?> fixture = Class.forName("gesher.services.SoftApValuesContractTest", true, loader);
                fixture.getMethod(args[3].equals("softap")
                        ? "baselineSoftApControl" : "baselineWpsControl").invoke(null);
            }
            return;
        }
        for (String relative : FAMILY) {
            String name = "android.net." + relative.replace('/', '.');
            Class<?> type = Class.forName(name);
            Path actual = Path.of(type.getProtectionDomain().getCodeSource().getLocation().toURI()).toRealPath();
            if (!actual.equals(expected)) throw new AssertionError("Unexpected value provider " + name + ": " + actual);
        }
        System.out.println("SoftAP/WPS providers: 18 freshly compiled production families");
        SoftApValuesContractTest.verifyGeneratedCompatTable();
        SoftApValuesContractTest.run(false, true);
    }
}
