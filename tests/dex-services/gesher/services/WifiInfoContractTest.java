/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package gesher.services;

import android.net.MacAddress;
import android.net.NetworkInfo.DetailedState;
import android.net.TransportInfo;
import android.net.wifi.MloLink;
import android.net.wifi.ScanResult;
import android.net.wifi.SupplicantState;
import android.net.wifi.WifiConfiguration;
import android.net.wifi.WifiInfo;
import android.net.wifi.WifiSsid;
import android.os.Parcel;
import android.os.Parcelable;
import android.os.PersistableBundle;
import android.os.SystemClock;
import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Method;
import java.net.InetAddress;
import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collections;
import java.util.List;
import java.util.Objects;

/** Public call sites compile against SDK36; hidden reflection only seeds value state. */
public final class WifiInfoContractTest {
    private static int checks;
    private static final int SENTINEL = 0x56a02f1;
    private static final String MASKED_MAC = "02:00:00:00:00:00";
    private static final String UNKNOWN_SSID = "<unknown ssid>";
    private interface Action { void run() throws Exception; }
    private static void check(boolean condition, String message) {
        if (!condition) throw new AssertionError(message);
        checks++;
    }
    private static void expect(Class<? extends Throwable> type, Action action, String message)
            throws Exception {
        try { action.run(); } catch (Throwable failure) {
            while (failure instanceof InvocationTargetException) failure = ((InvocationTargetException) failure).getCause();
            if (!type.isInstance(failure)) throw new AssertionError(message, failure);
            checks++; return;
        }
        throw new AssertionError(message + ": expected " + type.getSimpleName());
    }
    private static Object call(Object target, String name, Class<?>[] types, Object... args) throws Exception {
        return target.getClass().getMethod(name, types).invoke(target, args);
    }
    private static Object get(Object target, String name) throws Exception {
        return call(target, name, new Class<?>[0]);
    }
    private static void set(Object target, String name, Class<?> type, Object value) throws Exception {
        call(target, name, new Class<?>[]{type}, value);
    }
    private static WifiInfo fresh() throws Exception { return WifiInfo.class.getConstructor().newInstance(); }
    private static void constant(String name, Object expected) throws Exception {
        check(Objects.equals(WifiInfo.class.getField(name).get(null), expected), "SDK constant " + name);
    }
    private static void constants() throws Exception {
        constant("FREQUENCY_UNITS", WifiInfo.FREQUENCY_UNITS);
        constant("LINK_SPEED_UNITS", WifiInfo.LINK_SPEED_UNITS);
        constant("LINK_SPEED_UNKNOWN", WifiInfo.LINK_SPEED_UNKNOWN);
        constant("SECURITY_TYPE_DPP", WifiInfo.SECURITY_TYPE_DPP);
        constant("SECURITY_TYPE_EAP", WifiInfo.SECURITY_TYPE_EAP);
        constant("SECURITY_TYPE_EAP_WPA3_ENTERPRISE", WifiInfo.SECURITY_TYPE_EAP_WPA3_ENTERPRISE);
        constant("SECURITY_TYPE_EAP_WPA3_ENTERPRISE_192_BIT", WifiInfo.SECURITY_TYPE_EAP_WPA3_ENTERPRISE_192_BIT);
        constant("SECURITY_TYPE_OPEN", WifiInfo.SECURITY_TYPE_OPEN);
        constant("SECURITY_TYPE_OSEN", WifiInfo.SECURITY_TYPE_OSEN);
        constant("SECURITY_TYPE_OWE", WifiInfo.SECURITY_TYPE_OWE);
        constant("SECURITY_TYPE_PASSPOINT_R1_R2", WifiInfo.SECURITY_TYPE_PASSPOINT_R1_R2);
        constant("SECURITY_TYPE_PASSPOINT_R3", WifiInfo.SECURITY_TYPE_PASSPOINT_R3);
        constant("SECURITY_TYPE_PSK", WifiInfo.SECURITY_TYPE_PSK);
        constant("SECURITY_TYPE_SAE", WifiInfo.SECURITY_TYPE_SAE);
        constant("SECURITY_TYPE_UNKNOWN", WifiInfo.SECURITY_TYPE_UNKNOWN);
        constant("SECURITY_TYPE_WAPI_CERT", WifiInfo.SECURITY_TYPE_WAPI_CERT);
        constant("SECURITY_TYPE_WAPI_PSK", WifiInfo.SECURITY_TYPE_WAPI_PSK);
        constant("SECURITY_TYPE_WEP", WifiInfo.SECURITY_TYPE_WEP);
    }
    private static void defaultsAndReset() throws Exception {
        WifiInfo value = fresh();
        check(UNKNOWN_SSID.equals(value.getSSID()), "Default SSID");
        check(value.getBSSID() == null, "Default BSSID");
        check(MASKED_MAC.equals(value.getMacAddress()), "Default local MAC");
        check(value.getNetworkId() == -1 && value.getSubscriptionId() == -1, "Default identifiers");
        check(value.getCurrentSecurityType() == WifiInfo.SECURITY_TYPE_UNKNOWN, "Default security");
        check(value.getRssi() == -127 && value.getLinkSpeed() == -1 && value.getFrequency() == -1, "Default signal values");
        check(value.getTxLinkSpeedMbps() == 0 && value.getRxLinkSpeedMbps() == 0
                && value.getMaxSupportedTxLinkSpeedMbps() == 0 && value.getMaxSupportedRxLinkSpeedMbps() == 0,
                "Constructor directional rates");
        check(value.getSupplicantState() == SupplicantState.UNINITIALIZED, "Default supplicant");
        check(value.getWifiStandard() == ScanResult.WIFI_STANDARD_UNKNOWN, "Default standard");
        check(value.getIpAddress() == 0 && !value.getHiddenSSID(), "Default address and hidden flag");
        check(value.getApMldMacAddress() == null && value.getApMloLinkId() == -1, "Default MLO identity");
        check(value.getAffiliatedMloLinks().isEmpty() && value.getAssociatedMloLinks().isEmpty(), "Default MLO lists");
        check(value.getInformationElements() == null, "Default information elements");
        check(value.getPasspointFqdn() == null && value.getPasspointProviderFriendlyName() == null
                && value.getPasspointUniqueId() == null, "Default Passpoint identity");
        check(!value.isRestricted() && value.describeContents() == 0, "Default restriction and contents");
        check(value.getApplicableRedactions() == 7L, "Exact applicable redaction mask");
        check((Long) get(value, "getLastRssiUpdateMillis") == 0L, "Default RSSI timestamp");
        check(!(Boolean) get(value, "isPrimary"), "Default primary value");
        WifiInfo same = new WifiInfo.Builder().build();
        check(value != same && value.equals(same) && same.equals(value), "API36 value equality");
        check(value.hashCode() == same.hashCode(), "Equal defaults hash equally");
        check(!value.equals(null) && !value.equals("WifiInfo"), "Equality type boundary");
        check(value.toString().startsWith("SSID: <unknown ssid>, BSSID: <none>"), "Value diagnostic prefix");
        set(value, "setSupplicantState", SupplicantState.class, SupplicantState.COMPLETED);
        set(value, "setWifiStandard", int.class, ScanResult.WIFI_STANDARD_11BE);
        set(value, "setHiddenSSID", boolean.class, true);
        set(value, "setMeteredHint", boolean.class, true);
        set(value, "setRestricted", boolean.class, true);
        long before = SystemClock.elapsedRealtime(); get(value, "reset"); long after = SystemClock.elapsedRealtime();
        check(value.getTxLinkSpeedMbps() == -1 && value.getRxLinkSpeedMbps() == -1
                && value.getMaxSupportedTxLinkSpeedMbps() == -1 && value.getMaxSupportedRxLinkSpeedMbps() == -1,
                "Reset directional rates differ from constructor");
        check(!value.getHiddenSSID() && !value.isRestricted() && !(Boolean) get(value, "getMeteredHint"), "Reset mutable flags");
        check(value.getSupplicantState() == SupplicantState.COMPLETED && value.getWifiStandard() == ScanResult.WIFI_STANDARD_11BE,
                "Pinned reset retains supplicant state and Wi-Fi standard");
        long timestamp = (Long) get(value, "getLastRssiUpdateMillis");
        check(timestamp >= before && timestamp <= after, "Reset RSSI uses real elapsed clock");
    }
    private static void builders() throws Exception {
        WifiInfo.Builder builder = new WifiInfo.Builder(); byte[] ssid = "initial".getBytes(StandardCharsets.UTF_8);
        check(builder.setSsid(ssid) == builder, "SSID fluent return");
        check(builder.setBssid("not-validated-as-a-mac") == builder, "BSSID fluent return");
        check(builder.setRssi(-61) == builder, "RSSI fluent return");
        check(builder.setNetworkId(-42) == builder, "Network fluent return");
        check(builder.setSubscriptionId(51) == builder, "Subscription fluent return");
        check(builder.setCurrentSecurityType(WifiConfiguration.SECURITY_TYPE_SAE) == builder, "Security fluent return");
        WifiInfo first = builder.build();
        check(first.getSSID().equals("\"initial\"") && first.getBSSID().equals("not-validated-as-a-mac"),
                "Builder value and absence of invented BSSID validation");
        check(first.getNetworkId() == -42 && first.getSubscriptionId() == 51 && first.getRssi() == -61
                && first.getCurrentSecurityType() == WifiInfo.SECURITY_TYPE_SAE, "Builder supplied scalar values");
        check((Long) get(first, "getLastRssiUpdateMillis") == 0L, "Pinned copy constructor omits producer RSSI timestamp");
        ssid[0] = 'I'; check(first.getSSID().equals("\"Initial\""), "Pinned WifiSsid input-byte ownership");
        WifiSsid retainedSsid = (WifiSsid) get(first, "getWifiSsid"); byte[] bytes = retainedSsid.getBytes(); bytes[0] = 'X';
        check(first.getSSID().equals("\"Initial\""), "SSID output bytes are defensive");
        builder.setSsid("next".getBytes(StandardCharsets.UTF_8)).setNetworkId(9).setBssid(null); WifiInfo second = builder.build();
        check(first != second && first.getNetworkId() == -42 && second.getNetworkId() == 9, "Builder snapshots retain prior scalar state");
        check(first.getSSID().equals("\"Initial\"") && second.getSSID().equals("\"next\"") && second.getBSSID() == null,
                "Builder replacement SSID and null BSSID");
        for (int[] item : new int[][]{{Integer.MIN_VALUE,-127},{-128,-127},{-127,-127},{-1,-1},{0,0},{200,200},{201,200},{Integer.MAX_VALUE,200}})
            check(new WifiInfo.Builder().setRssi(item[0]).build().getRssi() == item[1], "RSSI clamp " + item[0]);
        check(UNKNOWN_SSID.equals(new WifiInfo.Builder().setSsid(null).build().getSSID()), "Null SSID value");
        check(UNKNOWN_SSID.equals(new WifiInfo.Builder().setSsid(new byte[0]).build().getSSID()), "Empty SSID value");
        check("ff80".equals(new WifiInfo.Builder().setSsid(new byte[]{-1,-128}).build().getSSID()), "Non-UTF8 SSID hex value");
        check("\"שלום\"".equals(new WifiInfo.Builder().setSsid("שלום".getBytes(StandardCharsets.UTF_8)).build().getSSID()), "UTF8 SSID");
        for (int security : new int[]{-1,0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,Integer.MIN_VALUE,Integer.MAX_VALUE}) {
            int expected = (security >= 0 && security <= 13 && security != 10) ? security : -1;
            check(new WifiInfo.Builder().setCurrentSecurityType(security).build().getCurrentSecurityType() == expected,
                    "Security conversion " + security);
        }
        for (int id : new int[]{Integer.MIN_VALUE,-1,0,17,Integer.MAX_VALUE}) {
            WifiInfo value = new WifiInfo.Builder().setNetworkId(id).setSubscriptionId(id).build();
            check(value.getNetworkId() == id && value.getSubscriptionId() == id, "Identifier preservation " + id);
        }
    }
    private static ScanResult.InformationElement ie(int id, int extension, byte[] bytes) throws Exception {
        ScanResult.InformationElement value = ScanResult.InformationElement.class.getConstructor().newInstance();
        value.getClass().getField("id").setInt(value,id); value.getClass().getField("idExt").setInt(value,extension);
        value.getClass().getField("bytes").set(value,bytes); return value;
    }
    private static MloLink link(int id, int state) throws Exception {
        MloLink value = MloLink.class.getConstructor().newInstance();
        set(value,"setLinkId",int.class,id); set(value,"setState",int.class,state);
        set(value,"setBand",int.class,ScanResult.WIFI_BAND_6_GHZ); set(value,"setChannel",int.class,37);
        set(value,"setRssi",int.class,-49); set(value,"setRxLinkSpeedMbps",int.class,1200);
        set(value,"setTxLinkSpeedMbps",int.class,600);
        set(value,"setApMacAddress",MacAddress.class,MacAddress.fromString("12:34:56:78:90:01"));
        set(value,"setStaMacAddress",MacAddress.class,MacAddress.fromString("22:34:56:78:90:02")); return value;
    }
    private static WifiInfo populated() throws Exception {
        WifiInfo value = new WifiInfo.Builder().setSsid("value-network".getBytes(StandardCharsets.UTF_8))
                .setBssid("12:34:56:78:90:ab").setNetworkId(47).setSubscriptionId(13).setRssi(-43)
                .setCurrentSecurityType(WifiConfiguration.SECURITY_TYPE_EAP_WPA3_ENTERPRISE).build();
        set(value,"setSupplicantState",SupplicantState.class,SupplicantState.COMPLETED);
        set(value,"setLinkSpeed",int.class,433); set(value,"setTxLinkSpeedMbps",int.class,288);
        set(value,"setRxLinkSpeedMbps",int.class,577); set(value,"setMaxSupportedTxLinkSpeedMbps",int.class,1200);
        set(value,"setMaxSupportedRxLinkSpeedMbps",int.class,2400); set(value,"setFrequency",int.class,6135);
        set(value,"setWifiStandard",int.class,ScanResult.WIFI_STANDARD_11BE);
        set(value,"setInetAddress",InetAddress.class,InetAddress.getByAddress(new byte[]{10,20,30,40}));
        set(value,"setMacAddress",String.class,"32:34:56:78:90:cd");
        for (String flag : new String[]{"MeteredHint","Ephemeral","Trusted","Restricted","OemPaid",
                "OemPrivate","CarrierMerged","OsuAp","HiddenSSID","IsPrimary"}) set(value,"set"+flag,boolean.class,true);
        set(value,"setFQDN",String.class,"example.test"); set(value,"setProviderFriendlyName",String.class,"Contract provider");
        set(value,"setPasspointUniqueId",String.class,"passpoint-identity");
        set(value,"setRequestingPackageName",String.class,"example.contract"); set(value,"setNetworkKey",String.class,"value-network-WPA3");
        set(value,"setApMldMacAddress",MacAddress.class,MacAddress.fromString("42:34:56:78:90:ef")); set(value,"setApMloLinkId",int.class,3);
        set(value,"setAffiliatedMloLinks",List.class,Arrays.asList(link(3,MloLink.MLO_LINK_STATE_ACTIVE),
                link(7,MloLink.MLO_LINK_STATE_IDLE),link(9,MloLink.MLO_LINK_STATE_UNASSOCIATED)));
        set(value,"setInformationElements",List.class,Arrays.asList(ie(48,0,new byte[]{1,2,3}),ie(255,35,new byte[]{4,5})));
        set(value,"enableApTidToLinkMappingNegotiationSupport",boolean.class,true);
        String[] counters={"txSuccess","txRetries","txBad","rxSuccess"};
        for(int i=0;i<counters.length;i++)value.getClass().getField(counters[i]).setLong(value,100+i);
        set(value,"setSuccessfulTxPacketsPerSecond",double.class,1.25); set(value,"setRetriedTxPacketsRate",double.class,2.5);
        set(value,"setLostTxPacketsPerSecond",double.class,3.75); set(value,"setSuccessfulRxPacketsPerSecond",double.class,5.0);
        set(value,"setScore",int.class,57); set(value,"setUsable",boolean.class,false); return value;
    }
    private static void compareVisible(WifiInfo a, WifiInfo b, String scope) throws Exception {
        check(Objects.equals(a.getSSID(),b.getSSID()) && Objects.equals(a.getBSSID(),b.getBSSID())
                && Objects.equals(a.getMacAddress(),b.getMacAddress()),scope+" identity");
        check(a.getNetworkId()==b.getNetworkId() && a.getSubscriptionId()==b.getSubscriptionId()
                && a.getCurrentSecurityType()==b.getCurrentSecurityType(),scope+" identifiers/security");
        check(a.getRssi()==b.getRssi() && a.getLinkSpeed()==b.getLinkSpeed() && a.getTxLinkSpeedMbps()==b.getTxLinkSpeedMbps()
                && a.getRxLinkSpeedMbps()==b.getRxLinkSpeedMbps() && a.getMaxSupportedTxLinkSpeedMbps()==b.getMaxSupportedTxLinkSpeedMbps()
                && a.getMaxSupportedRxLinkSpeedMbps()==b.getMaxSupportedRxLinkSpeedMbps(),scope+" signal/rates");
        check(a.getFrequency()==b.getFrequency() && a.getWifiStandard()==b.getWifiStandard()
                && a.getSupplicantState()==b.getSupplicantState() && a.getIpAddress()==b.getIpAddress(),scope+" frequency/state/address");
        check(Objects.equals(a.getPasspointFqdn(),b.getPasspointFqdn())
                && Objects.equals(a.getPasspointProviderFriendlyName(),b.getPasspointProviderFriendlyName())
                && Objects.equals(a.getPasspointUniqueId(),b.getPasspointUniqueId()),scope+" Passpoint");
        check(Objects.equals(a.getApMldMacAddress(),b.getApMldMacAddress()) && a.getApMloLinkId()==b.getApMloLinkId()
                && a.getAffiliatedMloLinks().equals(b.getAffiliatedMloLinks())
                && a.getAssociatedMloLinks().equals(b.getAssociatedMloLinks()),scope+" MLO");
        check(Objects.equals(a.getInformationElements(),b.getInformationElements()) && a.isRestricted()==b.isRestricted(),scope+" IE/restriction");
        for(String getter:new String[]{"getMeteredHint","isEphemeral","isTrusted","isOemPaid","isOemPrivate","isCarrierMerged",
                "isOsuAp","getRequestingPackageName","getNetworkKey","getScore","isUsable","getSuccessfulTxPacketsPerSecond",
                "getRetriedTxPacketsPerSecond","getLostTxPacketsPerSecond","getSuccessfulRxPacketsPerSecond",
                "isApTidToLinkMappingNegotiationSupported","getVendorData"})
            check(Objects.equals(get(a,getter),get(b,getter)),scope+" "+getter);
        for(String field:new String[]{"txSuccess","txRetries","txBad","rxSuccess"})
            check(a.getClass().getField(field).getLong(a)==b.getClass().getField(field).getLong(b),scope+" "+field);
    }
    private static void copiesAndRedactions() throws Exception {
        WifiInfo original=populated(), copy=original.makeCopy(0); compareVisible(original,copy,"Plain copy");
        check(copy!=original && copy.equals(original) && copy.hashCode()==original.hashCode(),"Content copy equality and hash");
        check(original.getHiddenSSID() && !copy.getHiddenSSID(),"Pinned copy omits hidden-SSID flag");
        Method bridge=TransportInfo.class.getMethod("makeCopy",long.class);
        check(((WifiInfo)bridge.invoke(original,0L)).equals(copy),"TransportInfo covariant bridge dispatch");
        for(long mask:new long[]{0,1,2,3,4,5,6,7,8,Long.MIN_VALUE,-1L}) {
            WifiInfo redacted=original.makeCopy(mask);
            boolean location=(mask&1)!=0, mac=location||(mask&2)!=0, settings=(mask&4)!=0;
            check(redacted.getSSID().equals(location?UNKNOWN_SSID:original.getSSID()),"SSID redaction "+mask);
            check(Objects.equals(redacted.getBSSID(),location?MASKED_MAC:original.getBSSID()),"BSSID redaction "+mask);
            check(redacted.getMacAddress().equals(mac?MASKED_MAC:original.getMacAddress()),"MAC redaction "+mask);
            check(redacted.getNetworkId()==(location?-1:original.getNetworkId()),"Network ID redaction "+mask);
            check(Objects.equals(redacted.getApMldMacAddress(),location?null:original.getApMldMacAddress()),"AP MLD redaction "+mask);
            check(Objects.equals(redacted.getPasspointFqdn(),location?null:original.getPasspointFqdn())
                    && Objects.equals(redacted.getPasspointProviderFriendlyName(),location?null:original.getPasspointProviderFriendlyName())
                    && Objects.equals(redacted.getPasspointUniqueId(),location?null:original.getPasspointUniqueId()),"Passpoint redaction "+mask);
            check(Objects.equals(get(redacted,"getNetworkKey"),location?null:get(original,"getNetworkKey")),"Network key redaction "+mask);
            check(location?redacted.getInformationElements()==null:redacted.getInformationElements().equals(original.getInformationElements()),"IE redaction "+mask);
            check(Objects.equals(get(redacted,"getRequestingPackageName"),settings?null:"example.contract"),"Requesting package redaction "+mask);
            if(settings)expect(SecurityException.class,()->get(redacted,"isPrimary"),"Primary redaction "+mask);
            else check((Boolean)get(redacted,"isPrimary"),"Retained primary "+mask);
            check(redacted.getRssi()==-43 && redacted.getFrequency()==6135 && redacted.getSubscriptionId()==13
                    && redacted.getCurrentSecurityType()==WifiInfo.SECURITY_TYPE_EAP_WPA3_ENTERPRISE
                    && redacted.getApMloLinkId()==3 && redacted.getIpAddress()==0x281e140a && redacted.isRestricted(),"Nonredacted values "+mask);
            check(redacted.getAffiliatedMloLinks().size()==3 && redacted.getAssociatedMloLinks().size()==2,"MLO lists retained under redaction "+mask);
            MloLink ml=redacted.getAffiliatedMloLinks().get(0);
            check(Objects.equals(ml.getApMacAddress(),location?null:original.getAffiliatedMloLinks().get(0).getApMacAddress()),"MLO AP MAC redaction "+mask);
            check(Objects.equals(ml.getStaMacAddress(),(mask&2)!=0?null:original.getAffiliatedMloLinks().get(0).getStaMacAddress()),"MLO STA MAC redaction "+mask);
        }
        check(original.getNetworkId()==47 && original.getInformationElements().size()==2 && (Boolean)get(original,"isPrimary"),"Redaction does not mutate source");
    }
    private static void listsAndEquality() throws Exception {
        WifiInfo value=populated(); List<MloLink> affiliated=value.getAffiliatedMloLinks(); affiliated.clear();
        check(value.getAffiliatedMloLinks().size()==3,"Affiliated getter list isolation");
        List<MloLink> associated=value.getAssociatedMloLinks();
        check(associated.size()==2 && associated.get(0).getState()==MloLink.MLO_LINK_STATE_ACTIVE
                && associated.get(1).getState()==MloLink.MLO_LINK_STATE_IDLE,"Associated state filter and order");
        set(associated.get(0),"setRssi",int.class,-88);
        check(value.getAffiliatedMloLinks().get(0).getRssi()==-88,"Associated elements share stored values");
        WifiInfo copy=value.makeCopy(0); set(value.getAffiliatedMloLinks().get(0),"setRssi",int.class,-21);
        check(copy.getAffiliatedMloLinks().get(0).getRssi()==-88,"MLO copy deep-copies link values");
        ScanResult.InformationElement first=value.getInformationElements().get(0);
        List<ScanResult.InformationElement> ies=value.getInformationElements(); ies.clear();
        check(value.getInformationElements().size()==2,"IE getter list isolation");
        first.getClass().getField("id").setInt(first,221);
        check(copy.getInformationElements().get(0).getId()==221,"Pinned IE copy retains element aliasing");
        ArrayList<ScanResult.InformationElement> supplied=new ArrayList<>();supplied.add(first);
        set(value,"setInformationElements",List.class,supplied);supplied.clear();
        check(value.getInformationElements().size()==1,"IE setter copies list");
        set(value,"setInformationElements",List.class,null);check(value.getInformationElements()==null,"Null IE list");
        expect(NullPointerException.class,()->set(value,"setAffiliatedMloLinks",List.class,null),"Null MLO list");
        WifiInfo ordered=populated(),permuted=ordered.makeCopy(0);
        ArrayList<MloLink> reversed=new ArrayList<>(permuted.getAffiliatedMloLinks());Collections.reverse(reversed);
        set(permuted,"setAffiliatedMloLinks",List.class,reversed);
        check(ordered.equals(permuted) && permuted.equals(ordered),"MLO equality ignores order");
        check(ordered.hashCode()!=permuted.hashCode(),"Pinned AOSP MLO hash retains order despite equality");
        check(ordered.getAffiliatedMloLinks().get(0).getLinkId()==3 && permuted.getAffiliatedMloLinks().get(0).getLinkId()==9,
                "Equality does not reorder stored lists");
        set(permuted,"setRssi",int.class,-99);check(!ordered.equals(permuted),"Scalar difference affects equality");
        WifiInfo noMld=populated();set(noMld,"setApMldMacAddress",MacAddress.class,null);
        check(noMld.getAffiliatedMloLinks().size()==3 && noMld.makeCopy(0).getAffiliatedMloLinks().isEmpty(),
                "Pinned copy only retains MLO links with an AP MLD identity");
    }
    private static void addressesStatesAndMetering() throws Exception {
        WifiInfo info=fresh();
        for(byte[] address:new byte[][]{{1,2,3,4},{-1,-128,0,127},{0,0,0,0}}) {
            set(info,"setInetAddress",InetAddress.class,InetAddress.getByAddress(address));
            int expected=(address[0]&255)|((address[1]&255)<<8)|((address[2]&255)<<16)|((address[3]&255)<<24);
            check(info.getIpAddress()==expected,"IPv4 host-to-little-endian integer");
        }
        set(info,"setInetAddress",InetAddress.class,InetAddress.getByAddress(new byte[16]));
        check(info.getIpAddress()==0,"IPv6 has no legacy IPv4 integer");
        set(info,"setInetAddress",InetAddress.class,null);check(info.getIpAddress()==0,"No address");
        SupplicantState[] states={SupplicantState.DISCONNECTED,SupplicantState.INTERFACE_DISABLED,
                SupplicantState.INACTIVE,SupplicantState.SCANNING,SupplicantState.AUTHENTICATING,
                SupplicantState.ASSOCIATING,SupplicantState.ASSOCIATED,SupplicantState.FOUR_WAY_HANDSHAKE,
                SupplicantState.GROUP_HANDSHAKE,SupplicantState.COMPLETED,SupplicantState.DORMANT,
                SupplicantState.UNINITIALIZED,SupplicantState.INVALID};
        DetailedState[] expected={DetailedState.DISCONNECTED,DetailedState.DISCONNECTED,DetailedState.IDLE,
                DetailedState.SCANNING,DetailedState.CONNECTING,DetailedState.CONNECTING,DetailedState.CONNECTING,
                DetailedState.AUTHENTICATING,DetailedState.AUTHENTICATING,DetailedState.OBTAINING_IPADDR,
                DetailedState.DISCONNECTED,DetailedState.IDLE,DetailedState.FAILED};
        for(int i=0;i<states.length;i++)check(WifiInfo.getDetailedStateOf(states[i])==expected[i],"Supplicant state "+states[i]);
        check(WifiInfo.getDetailedStateOf(null)==null,"Null supplicant mapping");
        Method blend=WifiConfiguration.class.getMethod("isMetered",WifiConfiguration.class,WifiInfo.class);
        check(!(Boolean)blend.invoke(null,null,null),"Null metered inputs"); WifiConfiguration config=new WifiConfiguration();
        for(boolean infoHint:new boolean[]{false,true})for(boolean configHint:new boolean[]{false,true}) {
            set(info,"setMeteredHint",boolean.class,infoHint);WifiConfiguration.class.getField("meteredHint").setBoolean(config,configHint);
            for(int override:new int[]{0,1,2,99}) {
                WifiConfiguration.class.getField("meteredOverride").setInt(config,override);boolean metered=override==2?false:override==1||infoHint||configHint;
                check((Boolean)blend.invoke(null,config,info)==metered,"Metered hint/override precedence");
            }
        }
        WifiConfiguration.class.getField("meteredOverride").setInt(config,0);
        WifiConfiguration.class.getField("meteredHint").setBoolean(config,true);
        check((Boolean)blend.invoke(null,config,null),"Configuration-only metered hint");
        set(info,"setMeteredHint",boolean.class,true);check((Boolean)blend.invoke(null,null,info),"Info-only metered hint");
    }
    private static void vendorValues(boolean nativeParcel) throws Exception {
        ClassLoader loader=WifiInfo.class.getClassLoader();
        Class<?> type=Class.forName("android.net.wifi.OuiKeyedData$Builder",true,loader);
        PersistableBundle bundle=new PersistableBundle();bundle.putString("kind","wifi-info");
        Object builder=type.getConstructor(int.class,PersistableBundle.class).newInstance(0x112233,bundle);
        Object vendor=get(builder,"build");WifiInfo value=populated();ArrayList<Object> source=new ArrayList<>();source.add(vendor);
        set(value,"setVendorData",List.class,source);source.clear();
        check(((List<?>)get(value,"getVendorData")).size()==1,"Vendor setter list isolation");
        WifiInfo copy=value.makeCopy(0);check(get(copy,"getVendorData").equals(get(value,"getVendorData")),"Vendor values copied");
        ((List<?>)get(value,"getVendorData")).clear();check(((List<?>)get(copy,"getVendorData")).size()==1,"Vendor copy has independent list");
        expect(IllegalArgumentException.class,()->set(value,"setVendorData",List.class,null),"Null vendor list");
        if(nativeParcel)roundTrip(copy,"Vendor-bearing Parcel");
    }
    @SuppressWarnings("unchecked")
    private static WifiInfo roundTrip(WifiInfo value,String scope) throws Exception {
        Parcel p=Parcel.obtain();
        try {
            value.writeToParcel(p,0);int end=p.dataPosition();
            byte[] wire=p.marshall();byte[] digest=MessageDigest.getInstance("SHA-256").digest(wire);
            StringBuilder hex=new StringBuilder();
            for(byte item:digest)hex.append(Character.forDigit((item>>>4)&15,16)).append(Character.forDigit(item&15,16));
            System.out.println("WIFI_INFO_PARCEL "+scope.replace(' ','_')+" bytes="+wire.length+" sha256="+hex);
            p.writeInt(SENTINEL);p.setDataPosition(0);
            check(p.readInt()==value.getNetworkId() && p.readInt()==value.getRssi() && p.readInt()==value.getLinkSpeed()
                    && p.readInt()==value.getTxLinkSpeedMbps() && p.readInt()==value.getRxLinkSpeedMbps()
                    && p.readInt()==value.getFrequency(),scope+" independent native prefix");
            p.setDataPosition(0);
            Parcelable.Creator<WifiInfo> creator=(Parcelable.Creator<WifiInfo>)WifiInfo.class.getField("CREATOR").get(null);
            WifiInfo result=creator.createFromParcel(p);
            check(p.dataPosition()==end && p.readInt()==SENTINEL && p.dataAvail()==0,scope+" exact native consumption");
            check(creator.newArray(3).length==3,scope+" creator array");compareVisible(value,result,scope);
            check(value.equals(result) && value.hashCode()==result.hashCode(),scope+" full equality and hash");
            check(!result.getHiddenSSID(),scope+" pinned hidden-SSID omission");return result;
        } finally { p.recycle(); }
    }
    private static void parcels() throws Exception {
        roundTrip(fresh(),"Default Parcel");WifiInfo full=populated();roundTrip(full,"Populated Parcel");
        for(long mask:new long[]{1,2,4,7})roundTrip(full.makeCopy(mask),"Redacted Parcel "+mask);
        set(full,"setInetAddress",InetAddress.class,InetAddress.getByAddress(new byte[16]));roundTrip(full,"IPv6 Parcel");
        set(full,"setInformationElements",List.class,Collections.emptyList());
        set(full,"setAffiliatedMloLinks",List.class,Collections.emptyList());roundTrip(full,"Empty-list Parcel");
    }
    public static void baselineValueControl() throws Exception {
        WifiInfo value=fresh();check(value.getApplicableRedactions()==7L,"Exact applicable redaction mask");
        throw new AssertionError("Baseline control unexpectedly passed");
    }
    public static void baselineLinkageControl() throws Exception {
        new WifiInfo.Builder().setRssi(-50).build();
        throw new AssertionError("Baseline linkage control unexpectedly passed");
    }
    public static void run(boolean nativeParcel) throws Exception {
        checks=0;constants();defaultsAndReset();builders();copiesAndRedactions();listsAndEquality();
        addressesStatesAndMetering();vendorValues(nativeParcel);if(nativeParcel)parcels();
        System.out.println("WifiInfoContractTest all passed: "+checks+" checks; parcel="+nativeParcel);
    }
    public static void main(String[] args) throws Exception {
        // Standalone dalvikvm does not perform the normal AndroidRuntime bootstrap.
        // Load the production registrations before the corpus calls SystemClock or Parcel.
        System.loadLibrary("android_runtime");
        run(true);
    }
}
