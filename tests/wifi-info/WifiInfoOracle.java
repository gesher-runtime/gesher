/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package gesher.services;

import android.os.Build;
import dalvik.system.PathClassLoader;

/** Authentic Android Parcel and strict provider separation for one shared SDK corpus. */
public final class WifiInfoOracle {
    private static final String[] FAMILY={"android.net.wifi.WifiInfo","android.net.wifi.WifiConfiguration",
            "android.net.wifi.WifiEnterpriseConfig","android.net.wifi.SecurityParams","android.net.wifi.ParcelUtil",
            "android.net.wifi.OuiKeyedData","android.net.wifi.util.PersistableBundleUtils","android.net.wifi.WifiSsid",
            "android.net.wifi.util.HexEncoding","android.net.wifi.SupplicantState","android.net.wifi.MloLink",
            "android.net.wifi.ScanResult","android.net.wifi.ScanResultSecurity","android.net.wifi.AnqpInformationElement",
            "android.net.MacAddress","android.net.TransportInfo"};
    private static final String[] PUBLIC_REFERENCE={"android.net.wifi.WifiInfo","android.net.wifi.WifiConfiguration",
            "android.net.wifi.WifiEnterpriseConfig","android.net.wifi.WifiSsid","android.net.wifi.SupplicantState",
            "android.net.wifi.MloLink","android.net.wifi.ScanResult","android.net.MacAddress","android.net.TransportInfo"};
    private static final String IPV4_HELPER="com.android.net.module.util.Inet4AddressUtils";
    private static final class CandidateLoader extends PathClassLoader {
        CandidateLoader(String path,ClassLoader parent){super(path,parent);}
        private static boolean owned(String name) {
            if(name.startsWith("gesher.services.WifiInfoContractTest") || name.equals(IPV4_HELPER))return true;
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
        ClassLoader parent=WifiInfoOracle.class.getClassLoader();boolean candidate=args[0].equals("candidate");
        ClassLoader loader=candidate?new CandidateLoader(args[1],parent):new PathClassLoader(args[1],parent);
        if(Build.VERSION.SDK_INT!=36)throw new AssertionError("API36 reference required");
        System.out.println("WifiInfo oracle mode="+args[0]+"; SDK="+Build.VERSION.SDK_INT+"; fingerprint="+Build.FINGERPRINT);
        for(String name:candidate?FAMILY:PUBLIC_REFERENCE) {
            Class<?> type=Class.forName(name,true,loader);
            if(candidate && type.getClassLoader()!=loader)throw new AssertionError("Candidate provider fallback: "+name);
            if(!candidate && type!=Class.forName(name,true,parent))throw new AssertionError("Reference provider mismatch: "+name);
            System.out.println("provider "+name+"="+type.getClassLoader());
        }
        if(candidate && Class.forName(IPV4_HELPER,true,loader).getClassLoader()!=loader)
            throw new AssertionError("Actual production IPv4 helper not loaded");
        Class<?> fixture=Class.forName("gesher.services.WifiInfoContractTest",true,loader);
        if(fixture.getClassLoader()!=loader)throw new AssertionError("Fixture loader mismatch");
        fixture.getMethod("run",boolean.class).invoke(null,true);
        System.out.println("WifiInfo oracle "+args[0]+" complete with real Android native Parcel");
    }
}
