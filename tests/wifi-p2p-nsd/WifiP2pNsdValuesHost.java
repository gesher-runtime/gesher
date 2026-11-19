/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package gesher.services;

import java.net.URL;
import java.net.URLClassLoader;
import java.nio.file.Path;

/** Explicit framework initialization boundary; no value or Parcel implementation. */
public final class WifiP2pNsdValuesHost {
    private static final String[] NAMES = {"WifiP2pServiceInfo", "WifiP2pServiceRequest", "WifiP2pDnsSdServiceInfo",
            "WifiP2pDnsSdServiceRequest", "WifiP2pUpnpServiceInfo", "WifiP2pUpnpServiceRequest",
            "WifiP2pUsdBasedServiceConfig", "WifiP2pUsdBasedServiceConfig$Builder"};
    private static final class BaselineLoader extends URLClassLoader {
        BaselineLoader(URL[] urls, ClassLoader parent) { super(urls, parent); }
        @Override protected synchronized Class<?> loadClass(String name, boolean resolve) throws ClassNotFoundException {
            if (!name.startsWith("android.net.wifi.p2p.nsd.") && !name.startsWith("gesher.services.WifiP2pNsdValuesContractTest"))
                return super.loadClass(name, resolve);
            Class<?> type = findLoadedClass(name);
            if (type == null) type = findClass(name);
            if (resolve) resolveClass(type);
            return type;
        }
    }
    private static void provider(String name, Path expected) throws Exception {
        Class<?> type = Class.forName(name);
        Path actual = Path.of(type.getProtectionDomain().getCodeSource().getLocation().toURI()).toRealPath();
        if (!actual.equals(expected)) throw new AssertionError("Unexpected provider for " + name + ": " + actual);
    }
    public static void main(String[] args) throws Exception {
        System.load(args[0]);
        Path expected = Path.of(args[1]).toRealPath();
        if (args[4].equals("baseline")) {
            try (BaselineLoader loader = new BaselineLoader(new URL[]{expected.toUri().toURL(),
                    Path.of(args[2]).toRealPath().toUri().toURL()}, WifiP2pNsdValuesHost.class.getClassLoader())) {
                Class.forName("gesher.services.WifiP2pNsdValuesContractTest", true, loader).getMethod("baselineControl").invoke(null);
            }
            return;
        }
        for (String name : NAMES) provider("android.net.wifi.p2p.nsd." + name, expected);
        provider("com.android.net.module.util.DnsSdTxtRecord", Path.of(args[3]).toRealPath());
        provider("com.android.net.module.util.DnsSdTxtRecord$1", Path.of(args[3]).toRealPath());
        System.out.println("P2P_NSD_HOST_PROVIDERS eight fresh SDK types; genuine existing framework TXT helper");
        WifiP2pNsdValuesContractTest.run(false, true);
        Class.forName("gesher.services.WifiP2pNsdValuesExtendedTest").getMethod("run", boolean.class, boolean.class).invoke(null, false, true);
    }
}
