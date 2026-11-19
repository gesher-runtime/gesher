/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package gesher.services;

import java.nio.file.Path;
import java.net.URL;
import java.net.URLClassLoader;

/** Host initialization boundary only; all Wi-Fi providers are fresh production output. */
public final class WifiInfoHost {
    private static final class NegativeLoader extends URLClassLoader {
        NegativeLoader(URL[] paths, ClassLoader parent) { super(paths,parent); }
        @Override protected synchronized Class<?> loadClass(String name,boolean resolve)
                throws ClassNotFoundException {
            if(!(name.equals("android.net.wifi.WifiInfo") || name.startsWith("android.net.wifi.WifiInfo$")
                    || name.startsWith("gesher.services.WifiInfoContractTest"))) return super.loadClass(name,resolve);
            Class<?> type=findLoadedClass(name);
            if(type==null)type=findClass(name); // Never fill a missing baseline class from cached framework/app output.
            if(resolve)resolveClass(type);return type;
        }
    }
    public static void main(String[] args) throws Exception {
        System.load(args[0]);
        Path expected=Path.of(args[1]).toRealPath();
        for(String name:new String[]{"android.net.wifi.WifiInfo","android.net.wifi.WifiConfiguration",
                "android.net.wifi.WifiEnterpriseConfig","android.net.wifi.SecurityParams","android.net.wifi.ParcelUtil",
                "android.net.wifi.OuiKeyedData","android.net.wifi.util.PersistableBundleUtils","android.net.wifi.WifiSsid",
                "android.net.wifi.util.HexEncoding","android.net.wifi.SupplicantState","android.net.wifi.MloLink",
                "android.net.wifi.ScanResult","android.net.wifi.ScanResultSecurity","android.net.wifi.AnqpInformationElement",
                "android.net.MacAddress","android.net.TransportInfo"}) {
            Class<?> type=Class.forName(name);
            Path actual=Path.of(type.getProtectionDomain().getCodeSource().getLocation().toURI()).toRealPath();
            if(!actual.equals(expected))throw new AssertionError(name+" provider "+actual+" != "+expected);
        }
        Class<?> helper=Class.forName("com.android.net.module.util.Inet4AddressUtils");
        Path helperPath=Path.of(helper.getProtectionDomain().getCodeSource().getLocation().toURI()).toRealPath();
        if(!helperPath.equals(Path.of(args[2]).toRealPath()))throw new AssertionError("Unexpected framework IPv4 helper provider "+helperPath);
        System.out.println("WifiInfo providers:16 fresh production families; actual framework IPv4 helper");
        if(args.length>4 && !args[3].equals("candidate")) {
            try(NegativeLoader loader=new NegativeLoader(new URL[]{expected.toUri().toURL(),
                    Path.of(args[4]).toRealPath().toUri().toURL()},WifiInfoHost.class.getClassLoader())) {
                Class<?> fixture=Class.forName("gesher.services.WifiInfoContractTest",true,loader);
                fixture.getMethod(args[3].equals("values")?"baselineValueControl":"baselineLinkageControl").invoke(null);
            }
        } else {
            Class<?> builder=Class.forName("android.net.wifi.WifiInfo$Builder");
            Path actual=Path.of(builder.getProtectionDomain().getCodeSource().getLocation().toURI()).toRealPath();
            if(!actual.equals(expected))throw new AssertionError("Builder did not come from fresh production output");
            WifiInfoContractTest.run(false);
        }
    }
}
