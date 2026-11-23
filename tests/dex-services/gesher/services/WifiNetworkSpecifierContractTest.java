/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package gesher.services;

import android.net.MacAddress;
import android.net.NetworkCapabilities;
import android.net.NetworkRequest;
import android.net.NetworkSpecifier;
import android.net.wifi.WifiConfiguration;
import android.net.wifi.WifiEnterpriseConfig;
import android.net.wifi.WifiNetworkSpecifier;
import android.os.Parcel;
import android.os.Parcelable;
import android.os.PatternMatcher;
import android.util.Pair;
import java.lang.reflect.InvocationTargetException;
import java.security.MessageDigest;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;
import java.util.Objects;

/** SDK36-compiled callers; hidden reflection inspects the genuine value/matcher providers. */
public final class WifiNetworkSpecifierContractTest {
    // Pinned ScanResult band values; their public argument annotations do not expose SDK fields.
    private static final int UNSPECIFIED=-1, BAND24=1, BAND5=2, BAND6=8, BAND60=16,
            BAND5_LOW=1<<29, BAND5_HIGH=1<<30;
    private static final int[] BANDS={BAND24,BAND5,BAND6,BAND60,BAND5_LOW,BAND5_HIGH};
    private static final int SENTINEL=0x56a0259;
    private static final String MAC="12:34:56:78:9a:bc";
    private static int checks;
    private interface Action { void run() throws Exception; }
    private static void check(boolean value,String message) {
        if(!value)throw new AssertionError(message);checks++;
    }
    private static void equal(Object expected,Object actual,String message) {
        check(Objects.equals(expected,actual),message+" expected="+expected+" actual="+actual);
    }
    private static Throwable expect(Class<? extends Throwable> type,Action action,String message)
            throws Exception {
        try { action.run(); } catch(Throwable failure) {
            while(failure instanceof InvocationTargetException)failure=((InvocationTargetException)failure).getCause();
            if(!type.isInstance(failure))throw new AssertionError(message,failure);
            checks++;return failure;
        }
        throw new AssertionError(message+": expected "+type.getSimpleName());
    }
    private static WifiNetworkSpecifier.Builder named(String name) {
        return new WifiNetworkSpecifier.Builder().setSsid(name);
    }
    private static Object field(Object value,String name) throws Exception {
        return value.getClass().getField(name).get(value);
    }
    private static WifiConfiguration config(WifiNetworkSpecifier value) throws Exception {
        return (WifiConfiguration)field(value,"wifiConfiguration");
    }
    private static PatternMatcher pattern(WifiNetworkSpecifier value) throws Exception {
        return (PatternMatcher)field(value,"ssidPatternMatcher");
    }
    @SuppressWarnings("unchecked")
    private static Pair<MacAddress,MacAddress> macPattern(WifiNetworkSpecifier value) throws Exception {
        return (Pair<MacAddress,MacAddress>)field(value,"bssidPatternMatcher");
    }
    private static Class<?> agentClass() throws Exception {
        return Class.forName("android.net.wifi.WifiNetworkAgentSpecifier",true,
                WifiNetworkSpecifier.class.getClassLoader());
    }
    private static boolean agentMatches(NetworkSpecifier agent,NetworkSpecifier request)
            throws Exception {
        return (Boolean)agentClass().getMethod("canBeSatisfiedBy",NetworkSpecifier.class)
                .invoke(agent,request);
    }
    private static WifiNetworkSpecifier.Builder secondary(WifiNetworkSpecifier.Builder builder,boolean value)
            throws Exception {
        return (WifiNetworkSpecifier.Builder)WifiNetworkSpecifier.Builder.class
                .getMethod("setPreferSecondarySta",boolean.class).invoke(builder,value);
    }
    private static boolean secondary(WifiNetworkSpecifier value) throws Exception {
        return (Boolean)WifiNetworkSpecifier.class.getMethod("isPreferSecondarySta").invoke(value);
    }
    private static NetworkSpecifier agent(String ssid,String bssid,int security,int band,boolean local)
            throws Exception {
        WifiConfiguration configuration=new WifiConfiguration();
        configuration.SSID=ssid;configuration.BSSID=bssid;configuration.setSecurityParams(security);
        return (NetworkSpecifier)agentClass().getConstructor(WifiConfiguration.class,int.class,boolean.class)
                .newInstance(configuration,band,local);
    }
    private static void security(WifiNetworkSpecifier value,int type,String message) throws Exception {
        WifiConfiguration expected=new WifiConfiguration();expected.setSecurityParams(type);
        equal(expected.allowedKeyManagement,config(value).allowedKeyManagement,message+" key management");
        equal(expected.allowedProtocols,config(value).allowedProtocols,message+" protocols");
        equal(expected.allowedPairwiseCiphers,config(value).allowedPairwiseCiphers,message+" pairwise ciphers");
    }

    private static void selectorsAndErrors() throws Exception {
        expect(IllegalStateException.class,()->new WifiNetworkSpecifier.Builder().build(),"Missing selector");
        expect(NullPointerException.class,()->named(null),"Null SSID");
        expect(NullPointerException.class,()->new WifiNetworkSpecifier.Builder().setSsidPattern(null),"Null pattern");
        expect(NullPointerException.class,()->new WifiNetworkSpecifier.Builder().setBssid(null),"Null BSSID");
        expect(NullPointerException.class,()->new WifiNetworkSpecifier.Builder().setBssidPattern(null,MacAddress.BROADCAST_ADDRESS),"Null MAC base");
        expect(NullPointerException.class,()->new WifiNetworkSpecifier.Builder().setBssidPattern(MacAddress.fromString(MAC),null),"Null MAC mask");
        expect(IllegalArgumentException.class,()->named("\ud800"),"Unpaired SSID surrogate");
        expect(IllegalStateException.class,()->named("").build(),"Empty literal is match-none");
        for(String ssid:new String[]{"Network","quoted\"value","caf\u00e9","\u05e9\u05dc\u05d5\u05dd","\ud83d\ude00","abcdefghijklmnopqrstuvwxyz0123456789"}) {
            WifiNetworkSpecifier.Builder builder=new WifiNetworkSpecifier.Builder();
            check(builder.setSsid(ssid)==builder,"SSID setter fluent");
            WifiNetworkSpecifier value=builder.build();
            equal(ssid,pattern(value).getPath(),"Literal SSID preserved");
            check(pattern(value).getType()==PatternMatcher.PATTERN_LITERAL,"Literal pattern kind");
            equal("\""+ssid+"\"",config(value).SSID,"Quoted configuration SSID");
            check(value.getBand()==UNSPECIFIED && value.getPreferredChannelFrequenciesMhz().length==0,"Default band/channels");
        }
        for(String bssid:new String[]{MAC,"01:02:03:04:05:06"}) {
            WifiNetworkSpecifier.Builder builder=new WifiNetworkSpecifier.Builder();
            check(builder.setBssid(MacAddress.fromString(bssid))==builder,"BSSID setter fluent");
            WifiNetworkSpecifier value=builder.build();equal(bssid,config(value).BSSID,"Exact BSSID configuration");
            check(macPattern(value).first.equals(MacAddress.fromString(bssid)),"Exact MAC pattern");
        }
        for(String bssid:new String[]{"00:00:00:00:00:00","ff:ff:ff:ff:ff:ff"})
            expect(IllegalStateException.class,()->new WifiNetworkSpecifier.Builder().setBssid(MacAddress.fromString(bssid)).build(),"Match-none MAC "+bssid);
        WifiNetworkSpecifier viaPattern=new WifiNetworkSpecifier.Builder().setBssidPattern(
                MacAddress.fromString(MAC),MacAddress.fromString("ff:ff:ff:ff:ff:ff")).build();
        equal(null,config(viaPattern).BSSID,"Equal-but-distinct exact mask retains upstream identity behavior");
        check(viaPattern.equals(new WifiNetworkSpecifier.Builder().setBssid(MacAddress.fromString(MAC)).build()),"Equal mask patterns still compare by value");
        for(int kind:new int[]{PatternMatcher.PATTERN_LITERAL,PatternMatcher.PATTERN_SIMPLE_GLOB,PatternMatcher.PATTERN_ADVANCED_GLOB,PatternMatcher.PATTERN_SUFFIX}) {
            expect(IllegalStateException.class,()->new WifiNetworkSpecifier.Builder().setSsidPattern(new PatternMatcher("",kind)).build(),"Empty non-prefix pattern "+kind);
        }
        expect(IllegalStateException.class,()->new WifiNetworkSpecifier.Builder().setSsidPattern(new PatternMatcher("",PatternMatcher.PATTERN_PREFIX)).build(),"Unrestricted empty prefix");
        WifiNetworkSpecifier prefix=new WifiNetworkSpecifier.Builder().setSsidPattern(new PatternMatcher("",PatternMatcher.PATTERN_PREFIX))
                .setBssidPattern(MacAddress.fromString("12:34:56:00:00:00"),MacAddress.fromString("ff:ff:ff:00:00:00")).build();
        check(pattern(prefix).match("anything"),"Empty prefix is valid with constrained MAC");
        expect(IllegalStateException.class,()->new WifiNetworkSpecifier.Builder().setSsidPattern(new PatternMatcher(".*",PatternMatcher.PATTERN_SIMPLE_GLOB)).build(),"Unrestricted glob");
        WifiNetworkSpecifier.Builder hidden=named("hidden");check(hidden.setIsHiddenSsid(true)==hidden,"Hidden setter fluent");
        check(config(hidden.build()).hiddenSSID,"Hidden literal stored");
        expect(IllegalStateException.class,()->new WifiNetworkSpecifier.Builder().setSsidPattern(new PatternMatcher("hidden",PatternMatcher.PATTERN_PREFIX)).setIsHiddenSsid(true).build(),"Hidden prefix rejected");
        check(!config(named("visible").setIsHiddenSsid(false).build()).hiddenSSID,"False hidden value");
    }

    private static void bandsChannelsAndReuse() throws Exception {
        for(int band:BANDS) {
            WifiNetworkSpecifier.Builder builder=new WifiNetworkSpecifier.Builder();
            check(builder.setBand(band)==builder,"Band setter fluent");
            WifiNetworkSpecifier value=builder.build();check(value.getBand()==band,"Band-only request accepted");
            check(pattern(value).match("") && value.getPreferredChannelFrequenciesMhz().length==0,"Band-only wildcard defaults");
        }
        check(named("x").setBand(UNSPECIFIED).build().getBand()==UNSPECIFIED,"Explicit unspecified with selector");
        expect(IllegalStateException.class,()->new WifiNetworkSpecifier.Builder().setBand(UNSPECIFIED).build(),"Unspecified is not a selector");
        for(int invalid:new int[]{Integer.MIN_VALUE,-2,0,3,4,7,9,32,Integer.MAX_VALUE})
            expect(IllegalArgumentException.class,()->named("x").setBand(invalid),"Invalid band "+invalid);
        int[] input={2412,2484,5180,5955,60480};WifiNetworkSpecifier.Builder builder=named("channels");
        check(builder.setPreferredChannelsFrequenciesMhz(input)==builder,"Channel setter fluent");
        input[0]=1;WifiNetworkSpecifier value=builder.build();
        check(Arrays.equals(value.getPreferredChannelFrequenciesMhz(),new int[]{2412,2484,5180,5955,60480}),"Input channel array copied");
        int[] result=value.getPreferredChannelFrequenciesMhz();result[0]=2;
        check(value.getPreferredChannelFrequenciesMhz()[0]==2412,"Output channel array copied");
        builder.setPreferredChannelsFrequenciesMhz(new int[]{5180});
        check(value.getPreferredChannelFrequenciesMhz().length==5 && builder.build().getPreferredChannelFrequenciesMhz().length==1,"Later channel setter leaves earlier snapshot");
        check(Arrays.equals(named("duplicate").setPreferredChannelsFrequenciesMhz(new int[]{2412,2412}).build().getPreferredChannelFrequenciesMhz(),new int[]{2412,2412}),"Duplicate/order retained");
        expect(NullPointerException.class,()->named("x").setPreferredChannelsFrequenciesMhz(null),"Null channels");
        // The pinned converter accepts in-band frequencies between channel centers.
        check(named("in-band").setPreferredChannelsFrequenciesMhz(new int[]{2413}).build()
                .getPreferredChannelFrequenciesMhz()[0]==2413,"Pinned in-band non-center frequency retained");
        for(int invalid:new int[]{-1,0,1,100000,Integer.MAX_VALUE})
            expect(IllegalArgumentException.class,()->named("x").setPreferredChannelsFrequenciesMhz(new int[]{2412,invalid}),"Invalid frequency "+invalid);
        WifiNetworkSpecifier.Builder retry=new WifiNetworkSpecifier.Builder().setBand(BAND5).setPreferredChannelsFrequenciesMhz(new int[]{5180});
        expect(IllegalStateException.class,retry::build,"Band and channels conflict");
        Throwable second=expect(IllegalStateException.class,()->retry.setBand(UNSPECIFIED).build(),"Failed build has filled wildcard patterns");
        check(second.getMessage().contains("match-all"),"Observable failed-build mutation retained");
        WifiNetworkSpecifier repaired=retry.setSsid("retry").build();
        check(repaired.getPreferredChannelFrequenciesMhz()[0]==5180,"Reuse after failed build retains channels");
        WifiNetworkSpecifier.Builder empty=named("");expect(IllegalStateException.class,empty::build,"Failed empty selector");
        equal("fixed",pattern(empty.setSsid("fixed").build()).getPath(),"Reuse after replacing invalid selector");
        WifiNetworkSpecifier.Builder preferred=named("secondary");
        check(secondary(preferred,true)==preferred,"Secondary preference setter fluent");
        WifiNetworkSpecifier before=preferred.build();
        check(secondary(before) && !secondary(secondary(preferred,false).build()) && secondary(before),
                "Secondary preference snapshots remain independent");
        check(!before.equals(preferred.build()),"Secondary preference enters equality");
    }

    @SuppressWarnings("deprecation")
    private static void securityAndCertificates() throws Exception {
        for(int which:new int[]{0,1}) {
            expect(NullPointerException.class,()->{if(which==0)named("n").setWpa2Passphrase(null);else named("n").setWpa3Passphrase(null);},"Null passphrase "+which);
            expect(IllegalArgumentException.class,()->{if(which==0)named("n").setWpa2Passphrase("caf\u00e9");else named("n").setWpa3Passphrase("caf\u00e9");},"Non-ASCII passphrase "+which);
            for(String pass:new String[]{"","x","password","abcdefghijklmnopqrstuvwxyz0123456789abcdefghijklmnopqrstuvwxyz0123456789"}) {
                WifiNetworkSpecifier.Builder builder=named("n");
                check((which==0?builder.setWpa2Passphrase(pass):builder.setWpa3Passphrase(pass))==builder,"Passphrase setter fluent");
                WifiNetworkSpecifier value=builder.build();
                security(value,pass.isEmpty()?WifiConfiguration.SECURITY_TYPE_OPEN:which==0?WifiConfiguration.SECURITY_TYPE_PSK:WifiConfiguration.SECURITY_TYPE_SAE,"Passphrase mode");
                equal(pass.isEmpty()?null:"\""+pass+"\"",config(value).preSharedKey,"Quoted passphrase value");
            }
        }
        WifiNetworkSpecifier.Builder owe=named("owe");check(owe.setIsEnhancedOpen(true)==owe,"OWE setter fluent");
        security(owe.build(),WifiConfiguration.SECURITY_TYPE_OWE,"Enhanced open");
        security(owe.setIsEnhancedOpen(false).build(),WifiConfiguration.SECURITY_TYPE_OPEN,"Disable enhanced open");
        WifiNetworkSpecifier.Builder mixed=named("mixed").setWpa2Passphrase("a").setWpa3Passphrase("b");
        expect(IllegalStateException.class,mixed::build,"Mixed passphrase modes");
        security(mixed.setWpa2Passphrase("").build(),WifiConfiguration.SECURITY_TYPE_SAE,"Empty passphrase disables its mode");
        expect(IllegalStateException.class,()->named("mixed").setIsEnhancedOpen(true).setWpa2Passphrase("a").build(),"OWE and PSK conflict");
        WifiEnterpriseConfig enterprise=new WifiEnterpriseConfig();enterprise.setEapMethod(WifiEnterpriseConfig.Eap.PEAP);enterprise.setIdentity("before");
        WifiNetworkSpecifier.Builder eap=named("enterprise");check(eap.setWpa2EnterpriseConfig(enterprise)==eap,"WPA2 enterprise setter fluent");
        enterprise.setIdentity("after");WifiNetworkSpecifier one=eap.build(),two=eap.build();
        equal("before",config(one).enterpriseConfig.getIdentity(),"Setter copied enterprise input");
        check(config(one).enterpriseConfig==config(two).enterpriseConfig,"Build snapshots retain shared copied enterprise value");
        config(one).enterpriseConfig.setIdentity("shared");equal("shared",config(two).enterpriseConfig.getIdentity(),"Shared enterprise ownership is observable");
        security(one,WifiConfiguration.SECURITY_TYPE_EAP,"WPA2 enterprise mode");
        for(int which=0;which<4;which++) {
            final int mode=which;
            expect(NullPointerException.class,()->{WifiNetworkSpecifier.Builder b=named("x");switch(mode){case 0:b.setWpa2EnterpriseConfig(null);break;case 1:b.setWpa3EnterpriseConfig(null);break;case 2:b.setWpa3EnterpriseStandardModeConfig(null);break;default:b.setWpa3Enterprise192BitModeConfig(null);}},"Null enterprise mode "+which);
        }
        expect(IllegalArgumentException.class,()->named("x").setWpa3Enterprise192BitModeConfig(enterprise),"192-bit requires TLS");
        WifiEnterpriseConfig tls=new WifiEnterpriseConfig();tls.setEapMethod(WifiEnterpriseConfig.Eap.TLS);
        expect(IllegalArgumentException.class,()->named("x").setWpa3Enterprise192BitModeConfig(tls),"192-bit requires client certificate");
        for(String kind:new String[]{"EC384","RSA3072","RSA2048","RSA3072SHA256"}) {
            WifiEnterpriseConfig material=WifiSpecifierCertificates.config(kind);boolean strong=kind.equals("EC384")||kind.equals("RSA3072");
            WifiNetworkSpecifier.Builder auto=named(kind);check(auto.setWpa3EnterpriseConfig(material)==auto,"Automatic WPA3 enterprise fluent");
            security(auto.build(),strong?WifiConfiguration.SECURITY_TYPE_EAP_WPA3_ENTERPRISE_192_BIT:WifiConfiguration.SECURITY_TYPE_EAP_WPA3_ENTERPRISE,"Automatic certificate mode "+kind);
            WifiNetworkSpecifier.Builder standard=named(kind);check(standard.setWpa3EnterpriseStandardModeConfig(material)==standard,"Standard WPA3 fluent");
            security(standard.build(),WifiConfiguration.SECURITY_TYPE_EAP_WPA3_ENTERPRISE,"Explicit standard mode "+kind);
            if(strong) {
                WifiNetworkSpecifier.Builder explicit=named(kind);check(explicit.setWpa3Enterprise192BitModeConfig(material)==explicit,"192-bit setter fluent");
                security(explicit.build(),WifiConfiguration.SECURITY_TYPE_EAP_WPA3_ENTERPRISE_192_BIT,"Explicit 192-bit "+kind);
                WifiEnterpriseConfig noCa=new WifiEnterpriseConfig(material);noCa.setCaCertificate(null);
                expect(IllegalArgumentException.class,()->named("x").setWpa3Enterprise192BitModeConfig(noCa),"192-bit requires qualifying CA "+kind);
            } else expect(IllegalArgumentException.class,()->named("x").setWpa3Enterprise192BitModeConfig(material),"Reject insufficient certificate "+kind);
        }
        expect(IllegalStateException.class,()->named("x").setWpa2EnterpriseConfig(enterprise).setWpa3EnterpriseStandardModeConfig(enterprise).build(),"Mixed enterprise modes");
    }

    private static void equalityAndRedaction() throws Exception {
        WifiNetworkSpecifier a=named("same").setWpa2Passphrase("first").build(),b=named("same").setWpa2Passphrase("second").build();
        check(a!=b && a.equals(b) && b.equals(a) && a.hashCode()==b.hashCode(),"Credentials do not enter selector equality");
        check(a.equals(a) && !a.equals(null) && !a.equals("same"),"Equality type/self/null");
        check(!a.equals(named("different").setWpa2Passphrase("first").build()),"SSID differentiates equality");
        check(!a.equals(named("same").setWpa3Passphrase("first").build()),"Security differentiates equality");
        WifiNetworkSpecifier x=named("channels").setPreferredChannelsFrequenciesMhz(new int[]{2412,5180}).build();
        WifiNetworkSpecifier y=named("channels").setPreferredChannelsFrequenciesMhz(new int[]{5180,2412}).build();
        check(!x.equals(y),"Channel order enters equality");
        check(x.equals(named("channels").setPreferredChannelsFrequenciesMhz(new int[]{2412,5180}).build()),"Independent channel arrays compare by value");
        for(int band:BANDS) {
            WifiNetworkSpecifier source=named("redact").setBand(band).setWpa2Passphrase("secret-value").build();
            NetworkSpecifier redacted=source.redact();
            check(redacted instanceof WifiNetworkSpecifier && redacted!=source,"Redaction produces fresh specifier");
            WifiNetworkSpecifier value=(WifiNetworkSpecifier)redacted;
            check(value.getBand()==band && value.equals(new WifiNetworkSpecifier.Builder().setBand(band).build()),"Redaction retains only band");
            equal(null,config(value).SSID,"Redacted SSID");equal(null,config(value).preSharedKey,"Redacted passphrase");
            check(!source.toString().contains("secret-value") && source.toString().contains("redact"),"Diagnostics omit credential and retain selector");
        }
        expect(IllegalStateException.class,()->named("unspecified").build().redact(),"Pinned unspecified-band redaction exception");
        check(a.canBeSatisfiedBy(b) && !a.canBeSatisfiedBy(null),"Request matching equality and null");
        check(a.describeContents()==0,"Parcelable content flags");
        NetworkRequest request=new NetworkRequest.Builder().addTransportType(NetworkCapabilities.TRANSPORT_WIFI).setNetworkSpecifier(a).build();
        check(request.getNetworkSpecifier()==a,"NetworkRequest retains genuine supplied value");
    }

    private static void matching() throws Exception {
        String[] paths={"Net42","Net","Net.*","Net[0-9]{2}","42"};
        int[] kinds={PatternMatcher.PATTERN_LITERAL,PatternMatcher.PATTERN_PREFIX,PatternMatcher.PATTERN_SIMPLE_GLOB,PatternMatcher.PATTERN_ADVANCED_GLOB,PatternMatcher.PATTERN_SUFFIX};
        String[] actual={"Net42","Net7","Other42","Other"};
        boolean[][] matches={{true,false,false,false},{true,true,false,false},{true,true,false,false},{true,false,false,false},{true,false,true,false}};
        for(int p=0;p<paths.length;p++) {
            WifiNetworkSpecifier request=new WifiNetworkSpecifier.Builder().setSsidPattern(new PatternMatcher(paths[p],kinds[p]))
                    .setBssidPattern(MacAddress.fromString("12:34:56:00:00:00"),MacAddress.fromString("ff:ff:ff:00:00:00")).build();
            for(int s=0;s<actual.length;s++)for(boolean rightMac:new boolean[]{false,true}) {
                NetworkSpecifier source=agent("\""+actual[s]+"\"",rightMac?MAC:"12:34:57:78:9a:bc",WifiConfiguration.SECURITY_TYPE_OPEN,BAND5,true);
                boolean expected=matches[p][s]&&rightMac;
                check(request.canBeSatisfiedBy(source)==expected,"Request pattern/mask match "+p+"/"+s+"/"+rightMac);
                check(agentMatches(source,request)==expected,"Agent pattern/mask match "+p+"/"+s+"/"+rightMac);
            }
        }
        NetworkSpecifier open=agent("\"Net42\"",MAC,WifiConfiguration.SECURITY_TYPE_OPEN,BAND5,true);
        NetworkSpecifier psk=agent("\"Net42\"",MAC,WifiConfiguration.SECURITY_TYPE_PSK,BAND5,true);
        check(!named("Net42").setWpa2Passphrase("x").build().canBeSatisfiedBy(open),"Key-management mismatch");
        check(named("Net42").setWpa2Passphrase("x").build().canBeSatisfiedBy(psk),"Key-management match");
        check(!named("Net42").setBand(BAND24).build().canBeSatisfiedBy(open),"Local request band mismatch");
        check(named("Net42").setBand(BAND5).build().canBeSatisfiedBy(open),"Local request band match");
        check(agentMatches(open,null) && agentMatches(open,open),"Agent null and self behavior");
        NetworkSpecifier all=(NetworkSpecifier)Class.forName("android.net.MatchAllNetworkSpecifier",true,NetworkSpecifier.class.getClassLoader()).getConstructor().newInstance();
        check(agentMatches(open,all) && !named("Net42").build().canBeSatisfiedBy(all),"Directional MatchAll behavior");
        check(agentClass().getMethod("redact").invoke(open)==null,"Agent redaction");
        NetworkSpecifier copy=agent("\"Net42\"",MAC,WifiConfiguration.SECURITY_TYPE_OPEN,BAND5,true);
        check(open.equals(copy) && open.hashCode()==copy.hashCode(),"Agent equality/hash");
        check(!open.equals(agent("\"Other\"",MAC,WifiConfiguration.SECURITY_TYPE_OPEN,BAND5,true)),"Agent SSID inequality");
        check(!open.equals(agent("\"Net42\"",MAC,WifiConfiguration.SECURITY_TYPE_OPEN,BAND5,false)),"Agent mode inequality");
        for(int band:new int[]{BAND24,BAND5,BAND6,BAND60}) {
            NetworkSpecifier system=agent("\"unrelated\"","98:76:54:32:10:aa",WifiConfiguration.SECURITY_TYPE_PSK,band,false);
            for(int requestBand:new int[]{BAND24,BAND5,BAND6,BAND60})
                check(named("not-the-agent-ssid").setBand(requestBand).build().canBeSatisfiedBy(system)==(band==requestBand),"System matcher uses band only");
            check(!named("unrestricted-band").build().canBeSatisfiedBy(system),"System matcher unspecified band differs");
        }
        for(int split:new int[]{BAND5_LOW,BAND5_HIGH}) {
            NetworkSpecifier system=agent("\"unrelated\"",MAC,WifiConfiguration.SECURITY_TYPE_OPEN,split,false);
            for(int requested:BANDS)check(named("selector").setBand(requested).build().canBeSatisfiedBy(system)
                    ==(requested==BAND5 || requested==split),"System 5GHz sub-band matching");
            NetworkSpecifier local=agent("\"selector\"",MAC,WifiConfiguration.SECURITY_TYPE_OPEN,split,true);
            for(int requested:new int[]{BAND5,BAND5_LOW,BAND5_HIGH,UNSPECIFIED})
                check(named("selector").setBand(requested).build().canBeSatisfiedBy(local)
                        ==(requested==UNSPECIFIED || requested==split),"Local 5GHz band remains exact");
        }
        NetworkSpecifier invalid=agent("not-quoted",MAC,WifiConfiguration.SECURITY_TYPE_OPEN,BAND5,true);
        expect(IllegalStateException.class,()->named("not-quoted").build().canBeSatisfiedBy(invalid),"Agent requires quoted internal SSID");
        NetworkSpecifier absent=agent(null,MAC,WifiConfiguration.SECURITY_TYPE_OPEN,BAND5,false);
        expect(NullPointerException.class,()->named("x").setBand(BAND5).build().canBeSatisfiedBy(absent),"Agent validates configuration before system match");
    }

    private static String digest(byte[] bytes) throws Exception {
        StringBuilder out=new StringBuilder();for(byte b:MessageDigest.getInstance("SHA-256").digest(bytes))
            out.append(Character.forDigit((b>>>4)&15,16)).append(Character.forDigit(b&15,16));return out.toString();
    }
    private static void roundTrip(WifiNetworkSpecifier value,String label) throws Exception {
        Parcel p=Parcel.obtain();try {
            value.writeToParcel(p,0);int end=p.dataPosition();byte[] wire=p.marshall();
            System.out.println("WIFI_SPECIFIER_PARCEL "+label+" bytes="+wire.length+" sha256="+digest(wire));
            p.writeInt(SENTINEL);p.setDataPosition(0);
            PatternMatcher observed=p.readParcelable(PatternMatcher.class.getClassLoader());
            MacAddress base=p.readParcelable(MacAddress.class.getClassLoader()),mask=p.readParcelable(MacAddress.class.getClassLoader());
            check(observed.getType()==pattern(value).getType() && observed.getPath().equals(pattern(value).getPath()),label+" independent pattern prefix");
            equal(macPattern(value),Pair.create(base,mask),label+" independent MAC prefix");
            check(p.readInt()==value.getBand(),label+" independent band prefix");
            p.setDataPosition(0);WifiNetworkSpecifier copy=WifiNetworkSpecifier.CREATOR.createFromParcel(p);
            check(p.dataPosition()==end && p.readInt()==SENTINEL && p.dataAvail()==0,label+" exact native Parcel consumption");
            check(copy!=value && value.equals(copy) && value.hashCode()==copy.hashCode(),label+" value equality/hash");
            check(copy.getClass()==value.getClass() && config(copy).getClass()==config(value).getClass(),label+" real selected value providers");
            equal(config(value).SSID,config(copy).SSID,label+" nested configuration SSID");
            equal(config(value).BSSID,config(copy).BSSID,label+" nested configuration BSSID");
            equal(config(value).preSharedKey,config(copy).preSharedKey,label+" nested credential");
            equal(config(value).enterpriseConfig.getIdentity(),config(copy).enterpriseConfig.getIdentity(),label+" nested enterprise identity");
            if(config(value).enterpriseConfig.getClientCertificate()!=null)
                check(Arrays.equals(config(value).enterpriseConfig.getClientCertificate().getEncoded(),config(copy).enterpriseConfig.getClientCertificate().getEncoded()),label+" actual nested DER certificate");
            check(Arrays.equals(value.getPreferredChannelFrequenciesMhz(),copy.getPreferredChannelFrequenciesMhz()),label+" channels survive");
            check(secondary(value)==secondary(copy),label+" secondary preference tail survives");
        } finally {p.recycle();}
    }
    private static void parcels() throws Exception {
        check(WifiNetworkSpecifier.CREATOR.newArray(3).length==3 && WifiNetworkSpecifier.CREATOR.newArray(0).length==0,"Creator array lengths");
        expect(NegativeArraySizeException.class,()->WifiNetworkSpecifier.CREATOR.newArray(-1),"Negative creator size");
        roundTrip(named("plain").build(),"plain");
        roundTrip(named("caf\u00e9\ud83d\ude00").setWpa2Passphrase("parcel-secret").setPreferredChannelsFrequenciesMhz(new int[]{2412,5180}).build(),"unicode_psk_channels");
        roundTrip(new WifiNetworkSpecifier.Builder().setBssid(MacAddress.fromString(MAC)).build(),"exact_bssid");
        roundTrip(new WifiNetworkSpecifier.Builder().setSsidPattern(new PatternMatcher("Net",PatternMatcher.PATTERN_PREFIX)).setIsEnhancedOpen(true).build(),"prefix_owe");
        roundTrip(named("hidden").setIsHiddenSsid(true).build(),"hidden_ssid");
        for(int band:new int[]{BAND24,BAND5,BAND6,BAND60})roundTrip(new WifiNetworkSpecifier.Builder().setBand(band).build(),"band_"+band);
        roundTrip(secondary(named("secondary"),true).build(),"secondary_sta");
        roundTrip(named("enterprise").setWpa3Enterprise192BitModeConfig(WifiSpecifierCertificates.config("EC384")).build(),"enterprise_192");
        NetworkSpecifier value=agent("\"Net42\"",MAC,WifiConfiguration.SECURITY_TYPE_PSK,BAND5,true);
        Parcel p=Parcel.obtain();try {
            ((Parcelable)value).writeToParcel(p,0);int end=p.dataPosition();byte[] wire=p.marshall();
            System.out.println("WIFI_SPECIFIER_PARCEL agent bytes="+wire.length+" sha256="+digest(wire));
            p.writeInt(SENTINEL);p.setDataPosition(0);
            Parcelable.Creator<?> creator=(Parcelable.Creator<?>)agentClass().getField("CREATOR").get(null);
            Object copy=creator.createFromParcel(p);
            check(p.dataPosition()==end && p.readInt()==SENTINEL && p.dataAvail()==0,"Agent full native Parcel consumption");
            check(value.equals(copy) && value.hashCode()==copy.hashCode() && copy.getClass()==value.getClass(),"Agent real provider round trip");
            check(((Parcelable)copy).describeContents()==0 && creator.newArray(2).length==2,"Agent creator/content contract");
        }finally{p.recycle();}
    }

    /** The real boot-loader path; Android isolated candidate mode is qualified separately. */
    public static void bootRequestParcel() throws Exception {
        WifiNetworkSpecifier value=named("boot-nested").setWpa2Passphrase("boot-secret").build();
        NetworkRequest request=new NetworkRequest.Builder().addTransportType(NetworkCapabilities.TRANSPORT_WIFI).setNetworkSpecifier(value).build();
        Parcel p=Parcel.obtain();try {
            request.writeToParcel(p,0);int end=p.dataPosition();p.writeInt(SENTINEL);p.setDataPosition(0);
            NetworkRequest copy=NetworkRequest.CREATOR.createFromParcel(p);
            check(p.dataPosition()==end && p.readInt()==SENTINEL && p.dataAvail()==0,"Boot NetworkRequest exact native consumption");
            check(copy.getNetworkSpecifier().getClass()==WifiNetworkSpecifier.class,"Boot request selects real WifiNetworkSpecifier provider");
            check(copy.getNetworkSpecifier().equals(value),"Boot nested specifier equality");
            equal("\"boot-secret\"",config((WifiNetworkSpecifier)copy.getNetworkSpecifier()).preSharedKey,"Boot nested credential preserved");
            System.out.println("WifiNetworkSpecifier boot NetworkRequest Parcel passed: 4 checks");
        }finally{p.recycle();}
    }
    public static void baselineLinkageControl() throws Exception {
        named("baseline").build();throw new AssertionError("Absent baseline specifier unexpectedly linked");
    }
    public static void run(boolean nativeParcel) throws Exception {
        checks=0;selectorsAndErrors();bandsChannelsAndReuse();securityAndCertificates();equalityAndRedaction();matching();
        if(nativeParcel)parcels();
        System.out.println("WifiNetworkSpecifierContractTest all passed: "+checks+" checks; parcel="+nativeParcel);
    }
    public static void main(String[] args) throws Exception {
        System.loadLibrary("android_runtime");
        run(true);bootRequestParcel();
    }
}
