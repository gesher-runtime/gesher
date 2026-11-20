/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package gesher.services;

import android.net.wifi.p2p.nsd.WifiP2pDnsSdServiceInfo;
import android.net.wifi.p2p.nsd.WifiP2pDnsSdServiceRequest;
import android.net.wifi.p2p.nsd.WifiP2pServiceInfo;
import android.net.wifi.p2p.nsd.WifiP2pServiceRequest;
import android.net.wifi.p2p.nsd.WifiP2pUpnpServiceInfo;
import android.net.wifi.p2p.nsd.WifiP2pUpnpServiceRequest;
import android.net.wifi.p2p.nsd.WifiP2pUsdBasedServiceConfig;
import android.os.Parcel;
import android.os.Parcelable;
import java.security.MessageDigest;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collections;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Locale;
import java.util.Objects;

/** SDK36-only public callers. Values and ordinary native records; no network operations. */
public final class WifiP2pNsdValuesContractTest {
    static final String UUID = "6859dede-8574-59ab-9332-123456789012";
    static final String DEVICE = "urn:schemas-upnp-org:device:MediaServer:1";
    static final String SERVICE = "urn:schemas-upnp-org:service:ContentDirectory:1";
    static int checks;
    interface Action { void run() throws Exception; }

    static void check(boolean result, String label) {
        checks++;
        if (!result) throw new AssertionError(label);
    }
    static void equal(Object expected, Object actual, String label) {
        check(Objects.equals(expected, actual), label + ": " + expected + " != " + actual);
    }
    static void rejects(Class<? extends Throwable> expected, Action action, String label) {
        try { action.run(); }
        catch (Throwable actual) {
            check(actual.getClass() == expected, label + ": expected " + expected.getName() + ", got " + actual);
            return;
        }
        throw new AssertionError(label + ": missing " + expected.getName());
    }
    static String repeated(String value, int count) {
        StringBuilder result = new StringBuilder(value.length() * count);
        for (int i = 0; i < count; i++) result.append(value);
        return result.toString();
    }
    static LinkedHashMap<String, String> deterministicMap() {
        LinkedHashMap<String, String> result = new LinkedHashMap<>();
        result.put("txtvers", "1");
        result.put("note", "two words");
        result.put("flag", null);
        result.put("empty", "");
        return result;
    }
    static WifiP2pUsdBasedServiceConfig config(String name, byte[] info) {
        return new WifiP2pUsdBasedServiceConfig.Builder(name).setServiceSpecificInfo(info).build();
    }
    static void constants(boolean usd) throws Exception {
        equal(WifiP2pServiceInfo.SERVICE_TYPE_ALL,
                WifiP2pServiceInfo.class.getField("SERVICE_TYPE_ALL").getInt(null), "ALL constant");
        equal(WifiP2pServiceInfo.SERVICE_TYPE_BONJOUR,
                WifiP2pServiceInfo.class.getField("SERVICE_TYPE_BONJOUR").getInt(null), "BONJOUR constant");
        equal(WifiP2pServiceInfo.SERVICE_TYPE_UPNP,
                WifiP2pServiceInfo.class.getField("SERVICE_TYPE_UPNP").getInt(null), "UPNP constant");
        equal(WifiP2pServiceInfo.SERVICE_TYPE_VENDOR_SPECIFIC,
                WifiP2pServiceInfo.class.getField("SERVICE_TYPE_VENDOR_SPECIFIC").getInt(null), "VENDOR_SPECIFIC constant");
        if (usd) {
            equal(WifiP2pUsdBasedServiceConfig.SERVICE_PROTOCOL_TYPE_BONJOUR,
                    WifiP2pUsdBasedServiceConfig.class.getField("SERVICE_PROTOCOL_TYPE_BONJOUR").getInt(null),
                    "USD BONJOUR constant");
            equal(WifiP2pUsdBasedServiceConfig.SERVICE_PROTOCOL_TYPE_GENERIC,
                    WifiP2pUsdBasedServiceConfig.class.getField("SERVICE_PROTOCOL_TYPE_GENERIC").getInt(null),
                    "USD GENERIC constant");
        }
    }
    static void requestValues(boolean usd) {
        WifiP2pServiceRequest all = WifiP2pServiceRequest.newInstance(WifiP2pServiceInfo.SERVICE_TYPE_ALL);
        WifiP2pServiceRequest explicitNull = WifiP2pServiceRequest.newInstance(0, null);
        equal(all, explicitNull, "implicit and explicit null query");
        equal(all.hashCode(), explicitNull.hashCode(), "null query hash");
        equal(0, all.describeContents(), "request contents");
        check(all.equals(all), "request reflexive equality");
        check(!all.equals(null) && !all.equals(new Object()), "request equality types");
        WifiP2pServiceRequest empty = WifiP2pServiceRequest.newInstance(0, "");
        check(!all.equals(empty), "null and empty queries remain distinct");
        equal(all.hashCode(), empty.hashCode(), "null and empty query hash collision preserved");
        check(!all.equals(WifiP2pServiceRequest.newInstance(255)), "different protocols unequal");
        WifiP2pServiceRequest mixed = WifiP2pServiceRequest.newInstance(255, "AbCd00");
        equal(mixed, WifiP2pServiceRequest.newInstance(255, "AbCd00"), "same hexadecimal query");
        equal(mixed.hashCode(), WifiP2pServiceRequest.newInstance(255, "AbCd00").hashCode(), "same query hash");
        check(!mixed.equals(WifiP2pServiceRequest.newInstance(255, "abcd00")), "query case is retained");
        check(!mixed.equals(WifiP2pServiceRequest.newInstance(255, "AbCd01")), "query content equality");
        equal(WifiP2pServiceRequest.newInstance(7, "0011"), WifiP2pServiceRequest.newInstance(7, "0011"),
                "generic protocol retained");
        rejects(IllegalArgumentException.class, () -> WifiP2pServiceRequest.newInstance(255, "a"), "odd query");
        rejects(IllegalArgumentException.class, () -> WifiP2pServiceRequest.newInstance(255, "0z"), "non-hex query");
        rejects(IllegalArgumentException.class, () -> WifiP2pServiceRequest.newInstance(255, "00 1"), "query whitespace");
        // Constructor rejection only; never written to Parcel or sent.
        rejects(IllegalArgumentException.class,
                () -> WifiP2pServiceRequest.newInstance(255, repeated("00", 65536)), "query byte-length limit");
        if (usd) equal(null, all.getWifiP2pUsdBasedServiceConfig(), "legacy request has no USD config");
    }
    static void dnsValues(boolean usd) {
        LinkedHashMap<String, String> txt = deterministicMap();
        LinkedHashMap<String, String> snapshot = new LinkedHashMap<>(txt);
        WifiP2pDnsSdServiceInfo info = WifiP2pDnsSdServiceInfo.newInstance("Printer", "_ipp._tcp", txt);
        WifiP2pDnsSdServiceInfo same = WifiP2pDnsSdServiceInfo.newInstance("Printer", "_ipp._tcp", snapshot);
        equal(info, same, "DNS-SD identical ordered TXT");
        equal(info.hashCode(), same.hashCode(), "DNS-SD value hash");
        equal(0, info.describeContents(), "DNS-SD contents");
        check(info.equals(info), "DNS-SD reflexive equality");
        check(!info.equals(null) && !info.equals(new Object()), "DNS-SD equality types");
        txt.put("txtvers", "2"); txt.put("later", "entry");
        equal(info, same, "DNS-SD snapshots map");
        check(!info.equals(WifiP2pDnsSdServiceInfo.newInstance("Printer", "_ipp._tcp", txt)), "changed TXT value");
        LinkedHashMap<String, String> reverse = new LinkedHashMap<>();
        List<String> keys = new ArrayList<>(snapshot.keySet());
        Collections.reverse(keys);
        for (String key : keys) reverse.put(key, snapshot.get(key));
        check(!info.equals(WifiP2pDnsSdServiceInfo.newInstance("Printer", "_ipp._tcp", reverse)), "TXT order retained");
        WifiP2pDnsSdServiceInfo absent = WifiP2pDnsSdServiceInfo.newInstance("Printer", "_ipp._tcp", null);
        equal(absent, WifiP2pDnsSdServiceInfo.newInstance("Printer", "_ipp._tcp", Collections.emptyMap()),
                "null and empty TXT maps");
        check(!absent.equals(WifiP2pDnsSdServiceInfo.newInstance("printer", "_ipp._tcp", null)), "PTR instance case retained");
        LinkedHashMap<String, String> nullValue = new LinkedHashMap<>(); nullValue.put("key", null);
        LinkedHashMap<String, String> emptyValue = new LinkedHashMap<>(); emptyValue.put("key", "");
        check(!WifiP2pDnsSdServiceInfo.newInstance("Printer", "_ipp._tcp", nullValue).equals(
                WifiP2pDnsSdServiceInfo.newInstance("Printer", "_ipp._tcp", emptyValue)), "null and empty TXT values differ");
        LinkedHashMap<String, String> replaced = new LinkedHashMap<>();
        replaced.put("Key", "first"); replaced.put("key", "second");
        equal(WifiP2pDnsSdServiceInfo.newInstance("Printer", "_ipp._tcp", Collections.singletonMap("key", "second")),
                WifiP2pDnsSdServiceInfo.newInstance("Printer", "_ipp._tcp", replaced), "TXT replacement ignores key case");
        rejects(IllegalArgumentException.class, () -> WifiP2pDnsSdServiceInfo.newInstance(null, "_ipp._tcp", null), "null advertised instance");
        rejects(IllegalArgumentException.class, () -> WifiP2pDnsSdServiceInfo.newInstance("", "_ipp._tcp", null), "empty advertised instance");
        rejects(IllegalArgumentException.class, () -> WifiP2pDnsSdServiceInfo.newInstance("Printer", null, null), "null advertised type");
        rejects(IllegalArgumentException.class, () -> WifiP2pDnsSdServiceInfo.newInstance("Printer", "", null), "empty advertised type");
        rejects(IllegalArgumentException.class, () -> WifiP2pDnsSdServiceInfo.newInstance("Printer", "_ipp._tcp",
                Collections.singletonMap("bad=key", "value")), "TXT key separator");
        rejects(NullPointerException.class, () -> WifiP2pDnsSdServiceInfo.newInstance("Printer", "_ipp._tcp",
                Collections.singletonMap(null, "value")), "null TXT key");
        rejects(IllegalArgumentException.class, () -> WifiP2pDnsSdServiceInfo.newInstance("Printer", "_ipp._tcp",
                Collections.singletonMap("k", repeated("v", 254))), "TXT entry length limit");
        equal(WifiP2pServiceRequest.newInstance(1), WifiP2pDnsSdServiceRequest.newInstance(), "DNS-SD wildcard protocol");
        equal(WifiP2pServiceRequest.newInstance(1, "045f697070c00c000c01"),
                WifiP2pDnsSdServiceRequest.newInstance("_ipp._tcp"), "DNS-SD PTR bytes");
        equal(WifiP2pServiceRequest.newInstance(1, "077072696e746572045f697070c00c001001"),
                WifiP2pDnsSdServiceRequest.newInstance("Printer", "_ipp._tcp"), "DNS-SD TXT bytes");
        equal(WifiP2pDnsSdServiceRequest.newInstance("Printer", "_ipp._tcp"),
                WifiP2pDnsSdServiceRequest.newInstance("printer", "_ipp._tcp"), "TXT lookup lowercases instance");
        check(!WifiP2pDnsSdServiceRequest.newInstance("_IPP._tcp").equals(
                WifiP2pDnsSdServiceRequest.newInstance("_ipp._tcp")), "PTR lookup preserves case");
        Locale previous = Locale.getDefault();
        try {
            Locale.setDefault(Locale.forLanguageTag("tr-TR"));
            equal(WifiP2pDnsSdServiceRequest.newInstance("PRINTI", "_ipp._tcp"),
                    WifiP2pDnsSdServiceRequest.newInstance("printi", "_ipp._tcp"), "TXT root locale");
        } finally { Locale.setDefault(previous); }
        check(WifiP2pDnsSdServiceRequest.newInstance("") != null, "empty request type accepted");
        check(WifiP2pDnsSdServiceRequest.newInstance("", "") != null, "empty request instance/type accepted");
        rejects(IllegalArgumentException.class, () -> WifiP2pDnsSdServiceRequest.newInstance((String) null), "null requested type");
        rejects(IllegalArgumentException.class, () -> WifiP2pDnsSdServiceRequest.newInstance(null, "_ipp._tcp"), "null requested instance");
        rejects(IllegalArgumentException.class, () -> WifiP2pDnsSdServiceRequest.newInstance("Printer", null), "null requested TXT type");
        if (usd) equal(null, info.getWifiP2pUsdBasedServiceConfig(), "DNS-SD has no USD config");
    }
    static void upnpValues(boolean usd) {
        ArrayList<String> services = new ArrayList<>(Arrays.asList(SERVICE, SERVICE + ":next"));
        WifiP2pUpnpServiceInfo info = WifiP2pUpnpServiceInfo.newInstance(UUID, DEVICE, services);
        WifiP2pUpnpServiceInfo same = WifiP2pUpnpServiceInfo.newInstance(UUID, DEVICE, new ArrayList<>(services));
        equal(info, same, "UPnP ordered services");
        equal(info.hashCode(), same.hashCode(), "UPnP value hash");
        equal(0, info.describeContents(), "UPnP contents");
        services.add("later");
        equal(info, same, "UPnP snapshots service list");
        check(!info.equals(WifiP2pUpnpServiceInfo.newInstance(UUID, DEVICE, services)), "UPnP additional service");
        equal(WifiP2pUpnpServiceInfo.newInstance(UUID, DEVICE, null),
                WifiP2pUpnpServiceInfo.newInstance(UUID, DEVICE, Collections.emptyList()), "null and empty UPnP services");
        check(!WifiP2pUpnpServiceInfo.newInstance(UUID, DEVICE, Collections.singletonList(SERVICE)).equals(
                WifiP2pUpnpServiceInfo.newInstance(UUID, DEVICE, Arrays.asList(SERVICE, SERVICE))), "UPnP duplicate services retained");
        check(!WifiP2pUpnpServiceInfo.newInstance(UUID, DEVICE, null).equals(
                WifiP2pUpnpServiceInfo.newInstance(UUID.toUpperCase(Locale.ROOT), DEVICE, null)), "UUID spelling retained");
        check(WifiP2pUpnpServiceInfo.newInstance(UUID, "", null) != null, "empty UPnP device accepted");
        rejects(IllegalArgumentException.class, () -> WifiP2pUpnpServiceInfo.newInstance(null, DEVICE, null), "null UPnP UUID");
        rejects(IllegalArgumentException.class, () -> WifiP2pUpnpServiceInfo.newInstance(UUID, null, null), "null UPnP device");
        rejects(IllegalArgumentException.class, () -> WifiP2pUpnpServiceInfo.newInstance("not-a-uuid", DEVICE, null), "invalid UUID");
        equal(WifiP2pServiceRequest.newInstance(2), WifiP2pUpnpServiceRequest.newInstance(), "UPnP wildcard protocol");
        equal(WifiP2pServiceRequest.newInstance(2, "10737364703a616c6c"),
                WifiP2pUpnpServiceRequest.newInstance("ssdp:all"), "UPnP target bytes");
        equal(WifiP2pServiceRequest.newInstance(2, "10"), WifiP2pUpnpServiceRequest.newInstance(""), "empty UPnP target version");
        check(!WifiP2pUpnpServiceRequest.newInstance().equals(WifiP2pUpnpServiceRequest.newInstance("")), "wildcard and empty UPnP targets differ");
        rejects(IllegalArgumentException.class, () -> WifiP2pUpnpServiceRequest.newInstance(null), "null UPnP target");
        if (usd) equal(null, info.getWifiP2pUsdBasedServiceConfig(), "UPnP has no USD config");
    }
    static void configValues() {
        WifiP2pUsdBasedServiceConfig empty = new WifiP2pUsdBasedServiceConfig();
        equal(0, empty.getServiceProtocolType(), "no-arg USD protocol");
        equal(null, empty.getServiceName(), "no-arg USD name");
        equal(null, empty.getServiceSpecificInfo(), "no-arg USD info");
        equal(0, empty.describeContents(), "USD contents");
        equal("WifiP2pUsdBasedServiceConfig:\n Protocol type: 0\n Service name : null\n Service specific info : <null>",
                empty.toString(), "no-arg USD text");
        equal(1024, WifiP2pUsdBasedServiceConfig.getMaxAllowedServiceSpecificInfoLength(), "USD info limit");
        WifiP2pUsdBasedServiceConfig generic = new WifiP2pUsdBasedServiceConfig.Builder("Printer").build();
        equal(2, generic.getServiceProtocolType(), "builder generic protocol");
        equal("Printer", generic.getServiceName(), "builder name");
        equal(null, generic.getServiceSpecificInfo(), "builder default info null");
        equal("WifiP2pUsdBasedServiceConfig:\n Protocol type: 2\n Service name : Printer\n Service specific info : <null>",
                generic.toString(), "generic USD text");
        check(generic.equals(generic), "Config reflexive identity");
        check(!generic.equals(new WifiP2pUsdBasedServiceConfig.Builder("Printer").build()), "Config identity equality");
        check(!generic.equals(null) && !generic.equals(new Object()), "Config equality types");
        equal(System.identityHashCode(generic), generic.hashCode(), "Config identity hash");
        WifiP2pUsdBasedServiceConfig.Builder builder = new WifiP2pUsdBasedServiceConfig.Builder("Printer");
        check(builder.setServiceProtocolType(1) == builder, "protocol setter chaining");
        byte[] bytes = new byte[] {0, 1, 127, (byte) 255};
        check(builder.setServiceSpecificInfo(bytes) == builder, "info setter chaining");
        WifiP2pUsdBasedServiceConfig first = builder.build(), second = builder.build();
        check(first != second, "build makes new Config");
        check(first.getServiceSpecificInfo() == bytes && second.getServiceSpecificInfo() == bytes, "array alias retained");
        equal(1, first.getServiceProtocolType(), "Bonjour protocol");
        bytes[1] = 9;
        equal((byte) 9, first.getServiceSpecificInfo()[1], "input array mutation visible");
        first.getServiceSpecificInfo()[2] = 10;
        equal((byte) 10, second.getServiceSpecificInfo()[2], "getter array alias visible");
        builder.setServiceSpecificInfo(new byte[] {5}).setServiceProtocolType(255);
        check(first.getServiceSpecificInfo() == bytes, "builder replacement preserves earlier array");
        equal(1, first.getServiceProtocolType(), "builder replacement preserves earlier protocol");
        equal(255, builder.build().getServiceProtocolType(), "maximum protocol accepted");
        equal(0, builder.setServiceProtocolType(0).build().getServiceProtocolType(), "minimum protocol accepted");
        equal(null, builder.setServiceSpecificInfo(null).build().getServiceSpecificInfo(), "null info accepted");
        byte[] zero = new byte[0];
        check(builder.setServiceSpecificInfo(zero).build().getServiceSpecificInfo() == zero, "empty info shared");
        byte[] maximum = new byte[1024];
        check(builder.setServiceSpecificInfo(maximum).build().getServiceSpecificInfo() == maximum, "maximum info shared");
        equal(100, new WifiP2pUsdBasedServiceConfig.Builder(repeated("x", 100)).build().getServiceName().length(), "maximum name accepted");
        String supplementaryName = repeated("\ud83d\udce1", 50);
        equal(supplementaryName, new WifiP2pUsdBasedServiceConfig.Builder(supplementaryName).build().getServiceName(), "name limit counts UTF-16 units");
        rejects(NullPointerException.class, () -> new WifiP2pUsdBasedServiceConfig.Builder(null), "null USD name");
        rejects(IllegalArgumentException.class, () -> new WifiP2pUsdBasedServiceConfig.Builder(""), "empty USD name");
        rejects(IllegalArgumentException.class, () -> new WifiP2pUsdBasedServiceConfig.Builder(repeated("x", 101)), "USD name length limit");
        rejects(IllegalArgumentException.class, () -> new WifiP2pUsdBasedServiceConfig.Builder(repeated("\ud83d\udce1", 51)), "UTF-16 name length limit");
        rejects(IllegalArgumentException.class, () -> builder.setServiceProtocolType(-1), "negative USD protocol");
        rejects(IllegalArgumentException.class, () -> builder.setServiceProtocolType(256), "USD protocol over maximum");
        rejects(IllegalArgumentException.class, () -> builder.setServiceSpecificInfo(new byte[1025]), "USD info over maximum");
    }
    static void usdWrappers() {
        WifiP2pUsdBasedServiceConfig config = config("Printer", new byte[] {1, 2, 3});
        WifiP2pUsdBasedServiceConfig separate = config("Printer", new byte[] {1, 2, 3});
        WifiP2pServiceInfo info = new WifiP2pServiceInfo(config), sameInfo = new WifiP2pServiceInfo(config);
        check(info.getWifiP2pUsdBasedServiceConfig() == config, "Info Config identity");
        equal(info, sameInfo, "Info shared Config equality");
        equal(info.hashCode(), sameInfo.hashCode(), "Info shared Config hash");
        check(!info.equals(new WifiP2pServiceInfo(separate)), "Info Config not structural");
        equal(0, info.describeContents(), "USD Info contents");
        WifiP2pServiceRequest request = new WifiP2pServiceRequest(config), sameRequest = new WifiP2pServiceRequest(config);
        check(request.getWifiP2pUsdBasedServiceConfig() == config, "Request Config identity");
        equal(request, sameRequest, "Request shared Config equality");
        equal(request.hashCode(), sameRequest.hashCode(), "Request shared Config hash");
        check(!request.equals(new WifiP2pServiceRequest(separate)), "Request Config not structural");
        equal(0, request.describeContents(), "USD Request contents");
        check(!info.equals(request) && !request.equals(info), "Info and Request are distinct value families");
        config.getServiceSpecificInfo()[0] = 4;
        equal((byte) 4, info.getWifiP2pUsdBasedServiceConfig().getServiceSpecificInfo()[0], "Info shared Config array");
        equal((byte) 4, request.getWifiP2pUsdBasedServiceConfig().getServiceSpecificInfo()[0], "Request shared Config array");
        rejects(NullPointerException.class, () -> new WifiP2pServiceInfo((WifiP2pUsdBasedServiceConfig) null), "null Info Config");
        rejects(NullPointerException.class, () -> new WifiP2pServiceRequest((WifiP2pUsdBasedServiceConfig) null), "null Request Config");
    }
    static String sha256(byte[] data) {
        try {
            byte[] digest = MessageDigest.getInstance("SHA-256").digest(data);
            StringBuilder result = new StringBuilder(64);
            for (byte b : digest) result.append(String.format(Locale.ROOT, "%02x", b & 255));
            return result.toString();
        } catch (Exception error) { throw new AssertionError("SHA-256 unavailable", error); }
    }

    /** Full original reader and complete rewrite, without a replacement Parcel implementation. */
    public static <T extends Parcelable> T wire(String name, T value,
            Parcelable.Creator<T> creator, boolean nullListNormalization) {
        Parcel writer = Parcel.obtain(), reader = Parcel.obtain(), rewritten = Parcel.obtain();
        try {
            value.writeToParcel(writer, 0);
            byte[] original = writer.marshall();
            equal(writer.dataSize(), original.length, name + " writer size");
            reader.unmarshall(original, 0, original.length);
            reader.setDataPosition(0);
            T restored = creator.createFromParcel(reader);
            check(restored != null, name + " reader result");
            equal(reader.dataSize(), reader.dataPosition(), name + " complete read consumption");
            equal(0, reader.dataAvail(), name + " no unread data");
            restored.writeToParcel(rewritten, 0);
            byte[] next = rewritten.marshall();
            equal(rewritten.dataSize(), next.length, name + " rewritten size");
            if (nullListNormalization) {
                check(value instanceof WifiP2pServiceInfo, name + " ServiceInfo null-list normalization");
                check(((WifiP2pServiceInfo) value).getWifiP2pUsdBasedServiceConfig() != null, name + " USD Config present");
                check(original.length >= 4, name + " list length present");
                check(original[0] == (byte) 255 && original[1] == (byte) 255
                        && original[2] == (byte) 255 && original[3] == (byte) 255, name + " original list null");
                byte[] expected = original.clone();
                Arrays.fill(expected, 0, 4, (byte) 0);
                check(Arrays.equals(expected, next), name + " only null-to-empty list normalization");
            } else {
                check(Arrays.equals(original, next), name + " stable rewritten wire");
            }
            System.out.println("P2P_NSD_PARCEL " + name + " bytes=" + original.length
                    + " sha256=" + sha256(original) + " rewrittenBytes=" + next.length
                    + " rewrittenSha256=" + sha256(next) + " roundtrip=true");
            return restored;
        } finally {
            rewritten.recycle(); reader.recycle(); writer.recycle();
        }
    }
    static void configWire(String name, WifiP2pUsdBasedServiceConfig value) {
        WifiP2pUsdBasedServiceConfig restored = wire(name, value, WifiP2pUsdBasedServiceConfig.CREATOR, false);
        check(restored != value, name + " new object identity");
        check(!value.equals(restored), name + " identity equality preserved");
        equal(value.getServiceProtocolType(), restored.getServiceProtocolType(), name + " protocol");
        equal(value.getServiceName(), restored.getServiceName(), name + " name");
        check(Arrays.equals(value.getServiceSpecificInfo(), restored.getServiceSpecificInfo()), name + " info bytes");
        if (value.getServiceSpecificInfo() != null)
            check(value.getServiceSpecificInfo() != restored.getServiceSpecificInfo(), name + " fresh reader array");
        equal(value.toString(), restored.toString(), name + " text fields");
    }
    static void nativeConfigs() {
        equal(0, WifiP2pUsdBasedServiceConfig.CREATOR.newArray(0).length, "Config empty creator array");
        WifiP2pUsdBasedServiceConfig[] values = WifiP2pUsdBasedServiceConfig.CREATOR.newArray(2);
        check(values.length == 2 && values[0] == null && values[1] == null, "Config creator array slots");
        configWire("usd_config_noarg", new WifiP2pUsdBasedServiceConfig());
        configWire("usd_config_generic", new WifiP2pUsdBasedServiceConfig.Builder("Generic").build());
        configWire("usd_config_null_info", config("Null", null));
        configWire("usd_config_empty_info", config("Empty", new byte[0]));
        configWire("usd_config_bytes_info", new WifiP2pUsdBasedServiceConfig.Builder("Bytes")
                .setServiceProtocolType(WifiP2pUsdBasedServiceConfig.SERVICE_PROTOCOL_TYPE_BONJOUR)
                .setServiceSpecificInfo(new byte[] {0, 1, 2, 127, (byte) 128, (byte) 255}).build());
    }
    public static void run(boolean nativeParcel, boolean usd) throws Exception {
        checks = 0;
        constants(usd); requestValues(usd); dnsValues(usd); upnpValues(usd);
        if (usd) { configValues(); usdWrappers(); if (nativeParcel) nativeConfigs(); }
        System.out.println("WifiP2pNsdValuesContractTest passed: " + checks
                + " checks; parcel=" + nativeParcel + "; usd=" + usd);
    }
    /** Exact-source absence control; its loader forbids cached framework stub fallback. */
    public static void baselineControl() { WifiP2pServiceRequest.newInstance(0); }

    /** Production Windows boot topology, checked before any ordinary native record. */
    public static void main(String[] args) throws Exception {
        ClassLoader boot = Parcel.class.getClassLoader();
        check(boot == null || boot.getClass().getName().endsWith("BootClassLoader"), "genuine boot Parcel provider");
        for (String name : new String[] {"WifiP2pServiceInfo", "WifiP2pServiceRequest", "WifiP2pDnsSdServiceInfo",
                "WifiP2pDnsSdServiceRequest", "WifiP2pUpnpServiceInfo", "WifiP2pUpnpServiceRequest",
                "WifiP2pUsdBasedServiceConfig", "WifiP2pUsdBasedServiceConfig$Builder"}) {
            Class<?> type = Class.forName("android.net.wifi.p2p.nsd." + name);
            check(type.getClassLoader() == boot, "production boot value provider: " + name);
        }
        for (String name : new String[] {"WifiP2pServiceInfo", "WifiP2pServiceRequest", "WifiP2pUsdBasedServiceConfig"}) {
            Object creator = Class.forName("android.net.wifi.p2p.nsd." + name).getField("CREATOR").get(null);
            check(creator.getClass().getClassLoader() == boot, "production boot original CREATOR: " + name);
        }
        for (String name : new String[] {"com.android.net.module.util.DnsSdTxtRecord", "com.android.net.module.util.DnsSdTxtRecord$1"})
            check(Class.forName(name).getClassLoader() == boot, "existing framework TXT provider: " + name);
        check(WifiP2pServiceInfo.class.getMethod("getWifiP2pUsdBasedServiceConfig").getReturnType()
                == WifiP2pUsdBasedServiceConfig.class, "Info Config identity");
        check(WifiP2pServiceRequest.class.getMethod("getWifiP2pUsdBasedServiceConfig").getReturnType()
                == WifiP2pUsdBasedServiceConfig.class, "Request Config identity");
        check(android.os.Build.VERSION.SDK_INT == 36, "pinned API36 runtime profile");
        System.out.println("P2P_NSD_PROVIDERS coherent production boot graph; complete original readers");
        System.out.println("P2P_NSD_PROFILE android16-r4; bp4a; sdk36; declarations=40");
        run(true, true);
        Class.forName("gesher.services.WifiP2pNsdValuesExtendedTest")
                .getMethod("run", boolean.class, boolean.class).invoke(null, true, true);
    }
}
