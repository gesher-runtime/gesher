/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package gesher.services;

import android.net.MacAddress;
import android.net.wifi.WifiConfiguration;
import android.net.wifi.WifiNetworkSpecifier;
import android.os.Build;
import android.os.Parcel;
import android.os.Parcelable;
import android.os.PatternMatcher;
import dalvik.system.PathClassLoader;
import java.security.MessageDigest;

/** Executes the pinned upstream writers on Android; never normalizes candidate bytes. */
public final class PinnedWifiSpecifierWireOracle {
    private static final String[] OWNED={"android.net.wifi.WifiNetworkSpecifier",
            "android.net.wifi.WifiNetworkAgentSpecifier","android.net.wifi.WifiConfiguration",
            "android.net.wifi.SecurityParams",
            "gesher.services.PinnedWifiSpecifierWireOracle$Corpus","gesher.services.WifiSpecifierCertificates"};
    private static final class SourceLoader extends PathClassLoader {
        SourceLoader(String path,ClassLoader parent){super(path,parent);}
        @Override protected synchronized Class<?> loadClass(String name,boolean resolve) throws ClassNotFoundException {
            boolean owned=false;
            for(String prefix:OWNED)if(name.equals(prefix)||name.startsWith(prefix+"$")){owned=true;break;}
            if(!owned)return super.loadClass(name,resolve);
            Class<?> type=findLoadedClass(name);if(type==null)type=findClass(name);
            if(resolve)resolveClass(type);return type;
        }
    }
    public static final class Corpus {
        private static WifiNetworkSpecifier.Builder named(String ssid) {
            return new WifiNetworkSpecifier.Builder().setSsid(ssid);
        }
        private static void wire(String name,Parcelable value) throws Exception {
            if (name.equals("prefix_owe") || name.equals("enterprise_192")) {
                Object config=value.getClass().getField("wifiConfiguration").get(value);
                if (!config.getClass().getField("requirePmf").getBoolean(config)) {
                    throw new AssertionError("Pinned source security fields did not reach configuration: "+name);
                }
            }
            Parcel p=Parcel.obtain();try {
                value.writeToParcel(p,0);byte[] bytes=p.marshall();
                StringBuilder hash=new StringBuilder();
                for(byte b:MessageDigest.getInstance("SHA-256").digest(bytes))
                    hash.append(Character.forDigit((b>>>4)&15,16)).append(Character.forDigit(b&15,16));
                System.out.println("WIFI_SPECIFIER_PARCEL "+name+" bytes="+bytes.length+" sha256="+hash);
            }finally{p.recycle();}
        }
        public static void run() throws Exception {
            wire("plain",named("plain").build());
            wire("unicode_psk_channels",named("caf\u00e9\ud83d\ude00").setWpa2Passphrase("parcel-secret")
                    .setPreferredChannelsFrequenciesMhz(new int[]{2412,5180}).build());
            wire("exact_bssid",new WifiNetworkSpecifier.Builder().setBssid(MacAddress.fromString("12:34:56:78:9a:bc")).build());
            wire("prefix_owe",new WifiNetworkSpecifier.Builder().setSsidPattern(new PatternMatcher("Net",PatternMatcher.PATTERN_PREFIX))
                    .setIsEnhancedOpen(true).build());
            wire("hidden_ssid",named("hidden").setIsHiddenSsid(true).build());
            for(int band:new int[]{1,2,8,16})wire("band_"+band,new WifiNetworkSpecifier.Builder().setBand(band).build());
            WifiNetworkSpecifier.Builder secondary=named("secondary");
            WifiNetworkSpecifier.Builder.class.getMethod("setPreferSecondarySta",boolean.class).invoke(secondary,true);
            wire("secondary_sta",secondary.build());
            wire("enterprise_192",named("enterprise").setWpa3Enterprise192BitModeConfig(WifiSpecifierCertificates.config("EC384")).build());
            WifiConfiguration config=new WifiConfiguration();config.SSID="\"Net42\"";config.BSSID="12:34:56:78:9a:bc";
            config.setSecurityParams(WifiConfiguration.SECURITY_TYPE_PSK);
            Class<?> agent=Class.forName("android.net.wifi.WifiNetworkAgentSpecifier",true,WifiNetworkSpecifier.class.getClassLoader());
            wire("agent",(Parcelable)agent.getConstructor(WifiConfiguration.class,int.class,boolean.class).newInstance(config,2,true));
            System.out.println("Pinned AOSP r4 WifiNetworkSpecifier native writer reference complete: 12 cases");
        }
    }
    public static void main(String[] args) throws Exception {
        if(args.length!=1 || Build.VERSION.SDK_INT!=36)throw new IllegalArgumentException("API36 and one pinned source payload required");
        ClassLoader parent=PinnedWifiSpecifierWireOracle.class.getClassLoader();
        ClassLoader source=new SourceLoader(args[0],parent);
        for(String name:OWNED) {
            Class<?> type=Class.forName(name,true,source);
            if(type.getClassLoader()!=source)throw new AssertionError("Pinned source fallback: "+name);
            System.out.println("pinned provider "+name+"="+type.getClassLoader());
        }
        // SecurityParams writes directly into WifiConfiguration, so both must have
        // the same defining loader. A boot helper is not a valid mixed-source reference.
        Class<?> config=Class.forName("android.net.wifi.WifiConfiguration",true,source);
        Class.forName("android.net.wifi.SecurityParams",true,source)
                .getMethod("updateLegacyWifiConfiguration",config);
        for(String name:new String[]{"android.os.Parcel","android.net.wifi.WifiEnterpriseConfig",
                "android.net.wifi.WifiManager","android.net.MacAddress",
                "android.internal.modules.utils.build.SdkLevel"}) {
            Class<?> type=Class.forName(name,true,source);
            if(type!=Class.forName(name,true,parent))throw new AssertionError("Platform dependency replaced: "+name);
            System.out.println("platform provider "+name+"="+type.getClassLoader());
        }
        Class.forName("gesher.services.PinnedWifiSpecifierWireOracle$Corpus",true,source).getMethod("run").invoke(null);
    }
}
