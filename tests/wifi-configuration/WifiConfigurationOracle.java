/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package gesher.services;

import android.os.Build;
import dalvik.system.PathClassLoader;

/** app_process harness: authentic Android Parcel with explicit reference/candidate providers. */
public final class WifiConfigurationOracle {
    private static final String[] FAMILY = {"WifiConfiguration", "WifiEnterpriseConfig",
            "SecurityParams", "ParcelUtil", "OuiKeyedData", "util.PersistableBundleUtils"};

    private static final class CandidateLoader extends PathClassLoader {
        CandidateLoader(String path, ClassLoader parent) { super(path, parent); }
        private static boolean owned(String name) {
            if (name.startsWith("gesher.services.WifiConfigurationContractTest")
                    || name.equals("gesher.services.WifiConfigurationCertificates")) return true;
            for (String family : FAMILY) {
                String prefix = "android.net.wifi." + family;
                if (name.equals(prefix) || name.startsWith(prefix + "$")) return true;
            }
            return false;
        }
        @Override protected synchronized Class<?> loadClass(String name, boolean resolve)
                throws ClassNotFoundException {
            if (!owned(name)) return super.loadClass(name, resolve);
            Class<?> found = findLoadedClass(name);
            if (found == null) found = findClass(name); // Never fall back to a boot Wi-Fi provider.
            if (resolve) resolveClass(found);
            return found;
        }
    }

    public static void main(String[] args) throws Exception {
        if (args.length != 2 || !(args[0].equals("reference") || args[0].equals("candidate"))) {
            throw new IllegalArgumentException("reference|candidate <dex-jar>");
        }
        ClassLoader parent = WifiConfigurationOracle.class.getClassLoader();
        boolean candidate = args[0].equals("candidate");
        ClassLoader loader = candidate ? new CandidateLoader(args[1], parent) : new PathClassLoader(args[1], parent);
        System.out.println("Wi-Fi oracle mode=" + args[0] + "; SDK=" + Build.VERSION.SDK_INT + "; fingerprint=" + Build.FINGERPRINT);
        for (String family : FAMILY) {
            Class<?> type = Class.forName("android.net.wifi." + family, true, loader);
            Class<?> reference = Class.forName(type.getName(), true, parent);
            if (candidate && (type.getClassLoader() != loader || type == reference)) {
                throw new AssertionError("Candidate family resolved to reference: " + type.getName());
            }
            if (!candidate && type != reference) throw new AssertionError("Reference provider mismatch: " + type.getName());
            System.out.println("provider " + type.getName() + "=" + type.getClassLoader());
        }
        Class<?> fixture = Class.forName("gesher.services.WifiConfigurationContractTest", true, loader);
        if (fixture.getClassLoader() != loader) throw new AssertionError("Fixture loader mismatch");
        fixture.getMethod("run", boolean.class).invoke(null, true);
        System.out.println("Wi-Fi oracle " + args[0] + " complete with real Android native Parcel");
    }
}
