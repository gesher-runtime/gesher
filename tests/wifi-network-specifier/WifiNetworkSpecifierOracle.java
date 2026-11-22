/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package gesher.services;

import android.os.Build;
import dalvik.system.PathClassLoader;

/** Real Android Parcel, with candidate value providers strictly isolated from boot classes. */
public final class WifiNetworkSpecifierOracle {
    private static final String[] FAMILY={"android.net.wifi.WifiInfo","android.net.wifi.WifiConfiguration",
            "android.net.wifi.WifiEnterpriseConfig","android.net.wifi.SecurityParams","android.net.wifi.ParcelUtil",
            "android.net.wifi.OuiKeyedData","android.net.wifi.util.PersistableBundleUtils","android.net.wifi.WifiSsid",
            "android.net.wifi.util.HexEncoding","android.net.wifi.SupplicantState","android.net.wifi.MloLink",
            "android.net.wifi.ScanResult","android.net.wifi.ScanResultSecurity","android.net.wifi.AnqpInformationElement",
            "android.net.MacAddress","android.net.TransportInfo","android.net.wifi.WifiNetworkSpecifier",
            "android.net.wifi.WifiNetworkAgentSpecifier"};
    private static final String[] REFERENCE={"android.net.wifi.WifiNetworkSpecifier","android.net.wifi.WifiNetworkSpecifier$Builder",
            "android.net.wifi.WifiNetworkAgentSpecifier","android.net.wifi.WifiConfiguration","android.net.wifi.WifiEnterpriseConfig",
            "android.net.wifi.ScanResult","android.net.MacAddress","android.net.NetworkSpecifier","android.net.MatchAllNetworkSpecifier"};
    private static final class CandidateLoader extends PathClassLoader {
        CandidateLoader(String path,ClassLoader parent){super(path,parent);}
        private static boolean owned(String name) {
            if(name.startsWith("gesher.services.WifiNetworkSpecifierContractTest") || name.equals("gesher.services.WifiSpecifierCertificates"))return true;
            for(String prefix:FAMILY)if(name.equals(prefix)||name.startsWith(prefix+"$"))return true;
            return false;
        }
        @Override protected synchronized Class<?> loadClass(String name,boolean resolve) throws ClassNotFoundException {
            if(!owned(name))return super.loadClass(name,resolve);
            Class<?> type=findLoadedClass(name);if(type==null)type=findClass(name);
            if(resolve)resolveClass(type);return type;
        }
    }
    public static void main(String[] args) throws Exception {
        if(args.length!=2 || !(args[0].equals("reference") || args[0].equals("candidate")))
            throw new IllegalArgumentException("reference|candidate <dex-jar>");
        if(Build.VERSION.SDK_INT!=36)throw new AssertionError("API36 reference required");
        boolean candidate=args[0].equals("candidate");ClassLoader parent=WifiNetworkSpecifierOracle.class.getClassLoader();
        ClassLoader loader=candidate?new CandidateLoader(args[1],parent):new PathClassLoader(args[1],parent);
        System.out.println("WifiNetworkSpecifier oracle mode="+args[0]+"; SDK="+Build.VERSION.SDK_INT+"; fingerprint="+Build.FINGERPRINT);
        for(String name:candidate?FAMILY:REFERENCE) {
            Class<?> type=Class.forName(name,true,loader);
            if(candidate && type.getClassLoader()!=loader)throw new AssertionError("Candidate fallback: "+name);
            if(!candidate && type!=Class.forName(name,true,parent))throw new AssertionError("Reference provider mismatch: "+name);
            System.out.println("provider "+name+"="+type.getClassLoader());
        }
        if(candidate && Class.forName("android.net.wifi.WifiNetworkSpecifier$Builder",true,loader).getClassLoader()!=loader)
            throw new AssertionError("Builder fallback");
        Class<?> fixture=Class.forName("gesher.services.WifiNetworkSpecifierContractTest",true,loader);
        if(fixture.getClassLoader()!=loader)throw new AssertionError("Fixture loader mismatch");
        fixture.getMethod("run",boolean.class).invoke(null,true);
        if(!candidate)fixture.getMethod("bootRequestParcel").invoke(null);
        System.out.println("WifiNetworkSpecifier oracle "+args[0]+" complete with real Android native Parcel");
        if(candidate)System.out.println("Nested boot NetworkRequest creator remains a separate Windows ART gate; isolated Android boot classes cannot load candidate values.");
    }
}
