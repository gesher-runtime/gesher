/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package gesher.services;

import android.net.wifi.p2p.nsd.WifiP2pDnsSdServiceInfo;
import android.net.wifi.p2p.nsd.WifiP2pDnsSdServiceRequest;
import android.net.wifi.p2p.nsd.WifiP2pServiceInfo;
import android.net.wifi.p2p.nsd.WifiP2pServiceRequest;
import android.net.wifi.p2p.nsd.WifiP2pUpnpServiceInfo;
import android.net.wifi.p2p.nsd.WifiP2pUpnpServiceRequest;
import android.net.wifi.p2p.nsd.WifiP2pUsdBasedServiceConfig;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.Objects;

/** Source-level hidden value observers and original CREATORs; no service or radio. */
public final class WifiP2pNsdValuesExtendedTest {
    private static int checks;
    private static final String UUID = "6859dede-8574-59ab-9332-123456789012";
    private static void check(boolean ok, String label) {
        checks++; if (!ok) throw new AssertionError(label);
    }
    private static void equal(Object a, Object b, String label) {
        check(Objects.equals(a, b), label + ": " + a + " != " + b);
    }
    private static final class Info extends WifiP2pServiceInfo {
        Info(List<String> query) { super(query); }
    }
    private static final class Request extends WifiP2pServiceRequest {
        Request(int protocol, String query) { super(protocol, query); }
    }
    private static final class UpnpRequest extends WifiP2pUpnpServiceRequest {
        UpnpRequest() { super(); }
        UpnpRequest(String query) { super(query); }
    }
    private static Map<String, String> txt() {
        Map<String, String> result = new LinkedHashMap<>();
        result.put("txtvers", "1"); result.put("note", "ab");
        return result;
    }
    private static WifiP2pDnsSdServiceInfo dns(Map<String, String> data) {
        return WifiP2pDnsSdServiceInfo.newInstance("MyPrinter", "_ipp._tcp", data);
    }
    private static void queryContracts(boolean usd) {
        WifiP2pServiceRequest all = WifiP2pServiceRequest.newInstance(0);
        equal("02000000", all.getSupplicantQuery(), "all-protocol query");
        equal(0, all.getTransactionId(), "initial transaction");
        WifiP2pServiceRequest vendor = new Request(255, "A0b1");
        vendor.setTransactionId(0x23);
        equal("0400ff23A0b1", vendor.getSupplicantQuery(), "little endian length and original hex case");
        equal(0x23, vendor.getTransactionId(), "transaction retained");
        WifiP2pServiceRequest same = WifiP2pServiceRequest.newInstance(255, "A0b1");
        equal(vendor, same, "transaction omitted from equality");
        equal(vendor.hashCode(), same.hashCode(), "transaction omitted from hash");
        WifiP2pServiceRequest empty = new Request(0, "");
        check(!empty.equals(all), "null and empty query unequal");
        equal(empty.hashCode(), all.hashCode(), "null and empty query equal hash");
        equal(empty.getSupplicantQuery(), all.getSupplicantQuery(), "null and empty query equal supplicant text");
        equal("020010000", new Request(256, null).getSupplicantQuery(), "protocol has no undocumented clamp");
        equal("02000200", new UpnpRequest().getSupplicantQuery(), "protected empty UPnP constructor");
        equal("0300020010", new UpnpRequest("10").getSupplicantQuery(), "protected UPnP query constructor");
        equal("02000100", WifiP2pDnsSdServiceRequest.newInstance().getSupplicantQuery(), "all Bonjour");
        equal("0c000100045f697070c00c000c01",
                WifiP2pDnsSdServiceRequest.newInstance("_ipp._tcp").getSupplicantQuery(), "PTR compression");
        equal("16000100096d797072696e746572045f697070c00c001001",
                WifiP2pDnsSdServiceRequest.newInstance("MyPrinter", "_ipp._tcp").getSupplicantQuery(), "TXT compression");
        equal("0d000100055f64656d6fc01c000c01",
                WifiP2pDnsSdServiceRequest.newInstance("_demo._udp").getSupplicantQuery(), "UDP compression pointer");
        equal("0b00020010737364703a616c6c",
                WifiP2pUpnpServiceRequest.newInstance("ssdp:all").getSupplicantQuery(), "UPnP target bytes");
        equal("0300020010", WifiP2pUpnpServiceRequest.newInstance("").getSupplicantQuery(), "empty UPnP target accepted");

        Map<String, String> data = txt();
        WifiP2pServiceInfo info = dns(data);
        equal(Arrays.asList("bonjour 045f697070c00c000c01 094d795072696e746572c027",
                "bonjour 096d797072696e746572045f697070c00c001001 09747874766572733d31076e6f74653d6162"),
                info.getSupplicantQueryList(), "DNS-SD PTR and ordered TXT bytes");
        data.put("note", "changed"); equal(dns(txt()), info, "factory consumes map into immutable strings");
        equal("bonjour 096d797072696e746572045f697070c00c001001 00",
                dns(null).getSupplicantQueryList().get(1), "empty TXT sentinel");
        Map<String, String> flags = new LinkedHashMap<>(); flags.put("flag", null); flags.put("empty", "");
        check(dns(flags).getSupplicantQueryList().get(1).endsWith(" 04666c616706656d7074793d"),
                "null and empty TXT value encoding");
        Map<String, String> reversed = new LinkedHashMap<>(); reversed.put("note", "ab"); reversed.put("txtvers", "1");
        check(!dns(txt()).equals(dns(reversed)), "map order is value state");
        Locale previous = Locale.getDefault();
        try {
            Locale.setDefault(Locale.forLanguageTag("tr-TR"));
            equal("16000100096d797072696e746572045f697070c00c001001",
                    WifiP2pDnsSdServiceRequest.newInstance("MYPRINTER", "_IPP._TCP").getSupplicantQuery(),
                    "TXT lowercasing uses ROOT locale");
            check(WifiP2pDnsSdServiceRequest.newInstance("_IPP._tcp").getSupplicantQuery().contains("045f495050c00c"),
                    "PTR retains supplied case");
        } finally { Locale.setDefault(previous); }

        ArrayList<String> queries = new ArrayList<>(Arrays.asList("owned first"));
        Info aliased = new Info(queries);
        check(aliased.getSupplicantQueryList() == queries, "protected constructor shares supplied list");
        queries.add("owned second"); equal(2, aliased.getSupplicantQueryList().size(), "list alias is live");
        Info copied = new Info(new ArrayList<>(queries)); equal(aliased, copied, "list content equality");
        aliased.getSupplicantQueryList().remove(0); check(!aliased.equals(copied), "getter mutations affect equality");
        try { new Info(null); throw new AssertionError("missing null-list rejection"); }
        catch (IllegalArgumentException expected) { check(true, "null protected query list rejected"); }
        List<String> services = new ArrayList<>(Arrays.asList("urn:owned:service:One:1", "urn:owned:service:One:1", null));
        WifiP2pServiceInfo upnp = WifiP2pUpnpServiceInfo.newInstance(UUID, "urn:owned:device:One:1", services);
        List<String> supplied = upnp.getSupplicantQueryList();
        equal(6, supplied.size(), "UPnP query count keeps duplicates and null element");
        equal("upnp 10 uuid:" + UUID, supplied.get(0), "UUID advertisement");
        equal("upnp 10 uuid:" + UUID + "::upnp:rootdevice", supplied.get(1), "root-device advertisement");
        equal(supplied.get(3), supplied.get(4), "duplicate services retained");
        equal(supplied.get(0), supplied.get(5), "null service encodes bare UUID");
        services.clear(); equal(6, supplied.size(), "UPnP factory consumes input list");
        equal("upnp 10 uuid:" + UUID.toUpperCase(Locale.ROOT),
                WifiP2pUpnpServiceInfo.newInstance(UUID.toUpperCase(Locale.ROOT), "", null)
                        .getSupplicantQueryList().get(0), "UUID validation retains spelling");
        equal(5, WifiP2pServiceInfo.CREATOR.newArray(5).length, "hidden Info CREATOR array");
        equal(4, WifiP2pServiceRequest.CREATOR.newArray(4).length, "hidden Request CREATOR array");
        if (usd) {
            same.setUsdSessionId(9); vendor.setUsdSessionId(17);
            equal(same, vendor, "request session omitted from equality");
            equal(17, vendor.getUsdSessionId(), "request session retained");
            Info first = new Info(new ArrayList<>(Arrays.asList("one")));
            Info second = new Info(new ArrayList<>(Arrays.asList("one")));
            first.setUsdSessionId(19); second.setUsdSessionId(20);
            equal(first, second, "info session omitted from equality");
            equal(first.hashCode(), second.hashCode(), "info session omitted from hash");
            equal(19, first.getUsdSessionId(), "info session retained");
        }
    }
    private static void requestWire(String name, WifiP2pServiceRequest request, boolean usd) throws Exception {
        request.setTransactionId(0x2a);
        WifiP2pServiceRequest restored = WifiP2pNsdValuesContractTest.wire(name, request, WifiP2pServiceRequest.CREATOR, false);
        equal(WifiP2pServiceRequest.class, restored.getClass(), "original request CREATOR returns base class");
        equal(request.getSupplicantQuery(), restored.getSupplicantQuery(), "complete request query restored");
        equal(0x2a, restored.getTransactionId(), "request transaction restored");
        if (usd) equal(0, restored.getUsdSessionId(), "default request session restored");
        equal(request, restored, "legacy request value equality after read");
    }
    private static void infoWire(String name, WifiP2pServiceInfo info, boolean usd) throws Exception {
        WifiP2pServiceInfo restored = WifiP2pNsdValuesContractTest.wire(name, info, WifiP2pServiceInfo.CREATOR, false);
        equal(WifiP2pServiceInfo.class, restored.getClass(), "original Info CREATOR returns base class");
        equal(info.getSupplicantQueryList(), restored.getSupplicantQueryList(), "query list restored");
        check(info.getSupplicantQueryList() != restored.getSupplicantQueryList(), "reader allocates query list");
        if (usd) equal(0, restored.getUsdSessionId(), "default Info session restored");
        equal(info, restored, "legacy Info value equality after read");
    }
    private static void normalParcels(boolean usd) throws Exception {
        requestWire("request_all", WifiP2pServiceRequest.newInstance(0), usd);
        requestWire("request_vendor_upper", WifiP2pServiceRequest.newInstance(255, "A0b1"), usd);
        requestWire("request_empty", WifiP2pServiceRequest.newInstance(0, ""), usd);
        requestWire("request_dns_all", WifiP2pDnsSdServiceRequest.newInstance(), usd);
        requestWire("request_dns_ptr", WifiP2pDnsSdServiceRequest.newInstance("_ipp._tcp"), usd);
        requestWire("request_dns_txt", WifiP2pDnsSdServiceRequest.newInstance("MyPrinter", "_ipp._tcp"), usd);
        requestWire("request_upnp_all", WifiP2pUpnpServiceRequest.newInstance(), usd);
        requestWire("request_upnp_target", WifiP2pUpnpServiceRequest.newInstance("ssdp:all"), usd);
        infoWire("info_dns_empty", dns(null), usd);
        infoWire("info_dns_txt", dns(txt()), usd);
        infoWire("info_upnp_base", WifiP2pUpnpServiceInfo.newInstance(UUID, "urn:owned:device:One:1", null), usd);
        infoWire("info_upnp_services", WifiP2pUpnpServiceInfo.newInstance(UUID, "urn:owned:device:One:1",
                Arrays.asList("urn:owned:service:One:1", "urn:owned:service:Two:1")), usd);
        infoWire("info_list", new Info(new ArrayList<>(Arrays.asList("owned query 1", "owned query 2"))), usd);
        if (usd) {
            byte[] bytes = {0, 1, 0x7f, (byte) 0x80, (byte) 0xff};
            WifiP2pUsdBasedServiceConfig config = new WifiP2pUsdBasedServiceConfig.Builder("owned-\u05e9\u05dc\u05d5\u05dd")
                    .setServiceProtocolType(1).setServiceSpecificInfo(bytes).build();
            WifiP2pServiceRequest request = new WifiP2pServiceRequest(config);
            request.setTransactionId(37); request.setUsdSessionId(15);
            WifiP2pServiceRequest restored = WifiP2pNsdValuesContractTest.wire("request_usd", request,
                    WifiP2pServiceRequest.CREATOR, false);
            equal(37, restored.getTransactionId(), "USD transaction preserved");
            equal(15, restored.getUsdSessionId(), "USD request session preserved");
            equal(request.getSupplicantQuery(), restored.getSupplicantQuery(), "USD default legacy fields preserved");
            configFields(config, restored.getWifiP2pUsdBasedServiceConfig());
            check(!request.equals(restored), "USD request preserves identity-sensitive Config equality");
            WifiP2pServiceInfo info = new WifiP2pServiceInfo(config); info.setUsdSessionId(16);
            check(info.getSupplicantQueryList() == null, "USD Info begins with null query list");
            WifiP2pServiceInfo infoCopy = WifiP2pNsdValuesContractTest.wire("info_usd", info,
                    WifiP2pServiceInfo.CREATOR, true);
            check(infoCopy.getSupplicantQueryList().isEmpty(), "genuine Info reader normalizes null list to empty");
            equal(16, infoCopy.getUsdSessionId(), "USD Info session preserved");
            configFields(config, infoCopy.getWifiP2pUsdBasedServiceConfig());
            check(!info.equals(infoCopy), "USD Info equality reflects source reader state and new Config identity");
        }
    }
    private static void configFields(WifiP2pUsdBasedServiceConfig original, WifiP2pUsdBasedServiceConfig restored) {
        check(original != restored, "reader creates new Config identity");
        equal(original.getServiceName(), restored.getServiceName(), "nested Config name");
        equal(original.getServiceProtocolType(), restored.getServiceProtocolType(), "nested Config protocol");
        check(Arrays.equals(original.getServiceSpecificInfo(), restored.getServiceSpecificInfo()), "nested Config bytes");
        check(original.getServiceSpecificInfo() != restored.getServiceSpecificInfo(), "native Config array is detached");
    }
    public static void run(boolean nativeParcel, boolean usd) throws Exception {
        checks = 0;
        queryContracts(usd);
        if (nativeParcel) normalParcels(usd);
        System.out.println("WifiP2pNsdValuesExtendedTest passed: " + checks + " checks; parcel=" + nativeParcel + "; usd=" + usd);
    }
}
