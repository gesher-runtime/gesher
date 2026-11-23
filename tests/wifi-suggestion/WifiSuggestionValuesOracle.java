/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package gesher.services;

import android.os.Build;
import dalvik.system.PathClassLoader;

/** One source owner per type graph, with genuine Android native Parcel throughout. */
public final class WifiSuggestionValuesOracle {
    private static final String[] NEW_FAMILY = {
        "android.net.wifi.WifiNetworkSuggestion", "android.net.wifi.hotspot2.PasspointConfiguration",
        "android.net.wifi.hotspot2.pps.HomeSp", "android.net.wifi.hotspot2.pps.Credential",
        "android.net.wifi.hotspot2.pps.Policy", "android.net.wifi.hotspot2.pps.UpdateParameter",
        "android.net.wifi.EAPConstants", "android.net.wifi.util.BuildProperties"
    };
    private static final String[] CANDIDATE_DEPENDENCIES = {
        "android.net.wifi.WifiInfo", "android.net.wifi.WifiConfiguration", "android.net.wifi.WifiEnterpriseConfig",
        "android.net.wifi.SecurityParams", "android.net.wifi.ParcelUtil", "android.net.wifi.OuiKeyedData",
        "android.net.wifi.util.PersistableBundleUtils", "android.net.wifi.WifiSsid", "android.net.wifi.util.HexEncoding",
        "android.net.wifi.SupplicantState", "android.net.wifi.MloLink", "android.net.wifi.ScanResult",
        "android.net.wifi.ScanResultSecurity", "android.net.wifi.AnqpInformationElement",
        "android.net.MacAddress", "android.net.TransportInfo"
    };
    private static final String[] PINNED_DEPENDENCIES = {
        "android.net.wifi.WifiConfiguration", "android.net.wifi.SecurityParams", "android.net.wifi.util.Environment"
    };
    private static final String[] PLATFORM_HELPERS = {
        "android.net.wifi.WifiEnterpriseConfig", "android.net.wifi.WifiInfo", "android.net.wifi.WifiSsid",
        "android.net.wifi.ParcelUtil", "android.net.wifi.OuiKeyedData", "android.net.MacAddress"
    };
    private static final class OwnedLoader extends PathClassLoader {
        private final String[] dependencies;
        OwnedLoader(String path, ClassLoader parent, String[] dependencies) {
            super(path, parent); this.dependencies = dependencies;
        }
        private boolean owned(String name) {
            if (name.startsWith("gesher.services.WifiSuggestion")) return true;
            for (String prefix : NEW_FAMILY) if (name.equals(prefix) || name.startsWith(prefix + "$")) return true;
            for (String prefix : dependencies) if (name.equals(prefix) || name.startsWith(prefix + "$")) return true;
            return false;
        }
        @Override protected synchronized Class<?> loadClass(String name, boolean resolve) throws ClassNotFoundException {
            if (!owned(name)) return super.loadClass(name, resolve);
            Class<?> type = findLoadedClass(name);
            if (type == null) type = findClass(name);
            if (resolve) resolveClass(type);
            return type;
        }
    }
    private static void owner(String name, ClassLoader loader, ClassLoader parent, boolean owned) throws Exception {
        Class<?> value = Class.forName(name, true, loader);
        if (owned ? value.getClassLoader() != loader : value != Class.forName(name, true, parent))
            throw new AssertionError("Wrong defining provider for " + name);
        System.out.println("provider " + name + "=" + value.getClassLoader());
    }
    private static void stockInternalHelper(String name, ClassLoader loader, ClassLoader parent) throws Exception {
        try { owner(name, loader, parent, false); }
        catch (ClassNotFoundException absent) {
            System.out.println("STOCK_PROFILE internal helper absent=" + name);
        }
    }
    public static void main(String[] args) {
        try { execute(args); }
        catch (Throwable failure) {
            while (failure instanceof java.lang.reflect.InvocationTargetException)
                failure = ((java.lang.reflect.InvocationTargetException) failure).getCause();
            System.err.println("WIFI_SUGGESTION_ORACLE_FAILURE");
            failure.printStackTrace(System.err);
            System.exit(1);
        }
    }
    private static void execute(String[] args) throws Exception {
        if (args.length != 2 || !(args[0].equals("reference") || args[0].equals("pinned-r4") || args[0].equals("candidate")))
            throw new IllegalArgumentException("reference|pinned-r4|candidate <dex-jar>");
        if (Build.VERSION.SDK_INT != 36) throw new AssertionError("API36 image required");
        boolean stock = args[0].equals("reference"), candidate = args[0].equals("candidate");
        ClassLoader parent = WifiSuggestionValuesOracle.class.getClassLoader();
        String[] dependencies = candidate ? CANDIDATE_DEPENDENCIES : PINNED_DEPENDENCIES;
        ClassLoader loader = stock ? new PathClassLoader(args[1], parent) : new OwnedLoader(args[1], parent, dependencies);
        System.out.println("Suggestion oracle mode=" + args[0] + "; SDK=" + Build.VERSION.SDK_INT
                + "; build-type=" + Build.TYPE + "; fingerprint=" + Build.FINGERPRINT);
        for (String name : NEW_FAMILY) {
            if (stock && name.equals("android.net.wifi.util.BuildProperties")) stockInternalHelper(name, loader, parent);
            else owner(name, loader, parent, !stock);
        }
        for (String name : dependencies) {
            if (stock && name.equals("android.net.wifi.util.Environment")) stockInternalHelper(name, loader, parent);
            else owner(name, loader, parent, !stock);
        }
        if (!candidate) for (String name : PLATFORM_HELPERS) owner(name, loader, parent, false);
        if (args[0].equals("pinned-r4")) {
            // Build-only WifiManager declarations are excluded; this is the actual boot constant.
            Class<?> manager = Class.forName("android.net.wifi.WifiManager", true, loader);
            Class<?> mac = Class.forName("android.net.MacAddress", true, loader);
            if (manager != Class.forName("android.net.wifi.WifiManager", true, parent)
                    || manager.getField("ALL_ZEROS_MAC_ADDRESS").getType() != mac
                    || !manager.getField("ALL_ZEROS_MAC_ADDRESS").get(null).toString().equals("00:00:00:00:00:00"))
                throw new AssertionError("Pinned source requires real boot manager constant and matching MacAddress");
        }
        Class<?> fixture = Class.forName("gesher.services.WifiSuggestionValuesContractTest", true, loader);
        Class<?> certificates = Class.forName("gesher.services.WifiSuggestionCertificates", true, loader);
        if (fixture.getClassLoader() != loader || certificates.getClassLoader() != loader)
            throw new AssertionError("Fixture provider mismatch");
        fixture.getMethod("verifyGraph").invoke(null);
        fixture.getMethod("run", boolean.class, boolean.class).invoke(null, true, !stock);
        System.out.println("Suggestion oracle " + args[0] + " complete with real Android native Parcel");
    }
}
