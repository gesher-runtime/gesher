/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package gesher.services;

import android.os.Build;
import dalvik.system.PathClassLoader;

/** Strict provider ownership; every execution uses genuine Android native Parcel. */
public final class SoftApValuesOracle {
    private static final String[] CANDIDATE_FAMILY = {
        "android.net.wifi.SoftApConfiguration", "android.net.wifi.WpsInfo", "android.net.wifi.WifiInfo",
        "android.net.wifi.WifiConfiguration", "android.net.wifi.WifiEnterpriseConfig", "android.net.wifi.SecurityParams",
        "android.net.wifi.ParcelUtil", "android.net.wifi.OuiKeyedData", "android.net.wifi.util.PersistableBundleUtils",
        "android.net.wifi.WifiSsid", "android.net.wifi.util.HexEncoding", "android.net.wifi.SupplicantState",
        "android.net.wifi.MloLink", "android.net.wifi.ScanResult", "android.net.wifi.ScanResultSecurity",
        "android.net.wifi.AnqpInformationElement", "android.net.MacAddress", "android.net.TransportInfo"
    };
    private static final String[] PINNED_FAMILY = {
        "android.net.wifi.SoftApConfiguration", "android.net.wifi.WpsInfo", "android.net.wifi.util.Environment"
    };
    private static final String[] REFERENCE_FAMILY = {
        "android.net.wifi.SoftApConfiguration", "android.net.wifi.WpsInfo", "android.net.wifi.WifiSsid",
        "android.net.wifi.ScanResult", "android.net.MacAddress"
    };
    private static final class OwnedLoader extends PathClassLoader {
        private final String[] family;
        OwnedLoader(String path, ClassLoader parent, String[] family) {
            super(path, parent);
            this.family = family;
        }
        private boolean owned(String name) {
            if (name.startsWith("gesher.services.SoftApValuesContractTest")) return true;
            for (String prefix : family)
                if (name.equals(prefix) || name.startsWith(prefix + "$")) return true;
            return false;
        }
        @Override protected synchronized Class<?> loadClass(String name, boolean resolve)
                throws ClassNotFoundException {
            if (!owned(name)) return super.loadClass(name, resolve);
            Class<?> type = findLoadedClass(name);
            if (type == null) type = findClass(name);
            if (resolve) resolveClass(type);
            return type;
        }
    }
    public static void main(String[] args) throws Exception {
        if (args.length != 2 || !(args[0].equals("reference") || args[0].equals("candidate")
                || args[0].equals("pinned-r4"))) throw new IllegalArgumentException("reference|candidate|pinned-r4 <dex-jar>");
        if (Build.VERSION.SDK_INT != 36) throw new AssertionError("API36 reference required");
        boolean stock = args[0].equals("reference");
        String[] family = args[0].equals("candidate") ? CANDIDATE_FAMILY : PINNED_FAMILY;
        ClassLoader parent = SoftApValuesOracle.class.getClassLoader();
        ClassLoader loader = stock ? new PathClassLoader(args[1], parent)
                : new OwnedLoader(args[1], parent, family);
        System.out.println("SoftAP/WPS oracle mode=" + args[0] + "; SDK=" + Build.VERSION.SDK_INT
                + "; fingerprint=" + Build.FINGERPRINT);
        for (String name : stock ? REFERENCE_FAMILY : family) {
            Class<?> type = Class.forName(name, true, loader);
            if (stock && type != Class.forName(name, true, parent))
                throw new AssertionError("Reference provider mismatch: " + name);
            if (!stock && type.getClassLoader() != loader)
                throw new AssertionError("Owned provider fell back: " + name);
            System.out.println("provider " + name + "=" + type.getClassLoader());
        }
        // All parameter identities used by the source-owned SoftAP graph must agree.
        Class<?> soft = Class.forName("android.net.wifi.SoftApConfiguration", true, loader);
        Class<?> builder = Class.forName("android.net.wifi.SoftApConfiguration$Builder", true, loader);
        Class<?> ssid = Class.forName("android.net.wifi.WifiSsid", true, loader);
        Class<?> mac = Class.forName("android.net.MacAddress", true, loader);
        if (builder.getMethod("setWifiSsid", ssid).getParameterTypes()[0] != ssid
                || builder.getMethod("setBssid", mac).getParameterTypes()[0] != mac
                || soft.getMethod("getWifiSsid").getReturnType() != ssid)
            throw new AssertionError("Incoherent value graph");
        if (args[0].equals("pinned-r4")) {
            Class<?> manager = Class.forName("android.net.wifi.WifiManager", true, loader);
            if (manager != Class.forName("android.net.wifi.WifiManager", true, parent)
                    || manager.getField("ALL_ZEROS_MAC_ADDRESS").getType() != mac
                    || !manager.getField("ALL_ZEROS_MAC_ADDRESS").get(null).toString().equals("00:00:00:00:00:00"))
                throw new AssertionError("Pinned source requires the actual Android manager constant");
        }
        Class<?> fixture = Class.forName("gesher.services.SoftApValuesContractTest", true, loader);
        if (fixture.getClassLoader() != loader) throw new AssertionError("Fixture provider mismatch");
        fixture.getMethod("run", boolean.class, boolean.class).invoke(null, true, !stock);
        System.out.println("SoftAP/WPS oracle " + args[0] + " complete with real Android native Parcel");
    }
}
