/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package gesher.services;

import java.net.URL;
import java.net.URLClassLoader;
import java.nio.file.Path;

/** Host-only native initialization boundary; no Parcel or value implementation. */
public final class WifiNetworkSpecifierHost {
    private static final String[] FAMILY={"android.net.wifi.WifiInfo","android.net.wifi.WifiConfiguration",
            "android.net.wifi.WifiEnterpriseConfig","android.net.wifi.SecurityParams","android.net.wifi.ParcelUtil",
            "android.net.wifi.OuiKeyedData","android.net.wifi.util.PersistableBundleUtils","android.net.wifi.WifiSsid",
            "android.net.wifi.util.HexEncoding","android.net.wifi.SupplicantState","android.net.wifi.MloLink",
            "android.net.wifi.ScanResult","android.net.wifi.ScanResultSecurity","android.net.wifi.AnqpInformationElement",
            "android.net.MacAddress","android.net.TransportInfo"};
    private static boolean owned(String name) {
        return name.equals("android.net.wifi.WifiNetworkSpecifier") || name.startsWith("android.net.wifi.WifiNetworkSpecifier$")
                || name.equals("android.net.wifi.WifiNetworkAgentSpecifier") || name.startsWith("android.net.wifi.WifiNetworkAgentSpecifier$")
                || name.startsWith("gesher.services.WifiNetworkSpecifierContractTest") || name.equals("gesher.services.WifiSpecifierCertificates");
    }
    private static final class NegativeLoader extends URLClassLoader {
        NegativeLoader(URL[] paths,ClassLoader parent){super(paths,parent);}
        @Override protected synchronized Class<?> loadClass(String name,boolean resolve) throws ClassNotFoundException {
            if(!owned(name))return super.loadClass(name,resolve);
            Class<?> type=findLoadedClass(name);if(type==null)type=findClass(name);
            if(resolve)resolveClass(type);return type;
        }
    }
    private static void requireProvider(String name,Path expected) throws Exception {
        Class<?> type=Class.forName(name);Path actual=Path.of(type.getProtectionDomain().getCodeSource().getLocation().toURI()).toRealPath();
        if(!actual.equals(expected))throw new AssertionError(name+" provider "+actual+" != "+expected);
    }
    public static void main(String[] args) throws Exception {
        System.load(args[0]);Path expected=Path.of(args[1]).toRealPath();
        for(String name:FAMILY)requireProvider(name,expected);
        for(String name:new String[]{"android.net.NetworkSpecifier","android.net.MatchAllNetworkSpecifier",
                "android.os.PatternMatcher","android.util.Pair","com.android.internal.util.Preconditions",
                "android.internal.modules.utils.build.SdkLevel"})requireProvider(name,Path.of(args[2]).toRealPath());
        if(args[3].equals("baseline")) {
            try(NegativeLoader loader=new NegativeLoader(new URL[]{expected.toUri().toURL(),Path.of(args[4]).toRealPath().toUri().toURL()},WifiNetworkSpecifierHost.class.getClassLoader())) {
                Class.forName("gesher.services.WifiNetworkSpecifierContractTest",true,loader)
                        .getMethod("baselineLinkageControl").invoke(null);
            }
        } else {
            for(String name:new String[]{"android.net.wifi.WifiNetworkSpecifier","android.net.wifi.WifiNetworkSpecifier$Builder","android.net.wifi.WifiNetworkAgentSpecifier"})requireProvider(name,expected);
            System.out.println("WifiNetworkSpecifier providers:18 fresh production value families; genuine framework matching dependencies");
            WifiNetworkSpecifierContractTest.run(false);
        }
    }
}
