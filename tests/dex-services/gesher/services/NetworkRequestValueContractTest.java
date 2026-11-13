/* Copyright (C) 2026 The Gesher Authors. SPDX-License-Identifier: Apache-2.0 */
package gesher.services;

import android.net.DhcpInfo;
import android.net.L2capNetworkSpecifier;
import android.net.MacAddress;
import android.net.Network;
import android.net.NetworkCapabilities;
import android.net.NetworkRequest;
import android.net.ParseException;
import android.net.VpnTransportInfo;
import android.net.nsd.AdvertisingRequest;
import android.net.nsd.DiscoveryRequest;
import android.net.nsd.NsdServiceInfo;
import android.os.Parcel;
import android.os.Parcelable;

import java.time.Duration;

/** Pure DHCP, network-specifier and DNS-SD request contracts; no discovery is started. */
public final class NetworkRequestValueContractTest {
    private static int checks;
    private static void check(boolean ok, String name) {
        if (!ok) throw new AssertionError(name);
        checks++;
    }
    private static void rejects(Runnable action, String name) {
        try { action.run(); } catch (IllegalArgumentException expected) { checks++; return; }
        throw new AssertionError(name);
    }
    private static <T extends Parcelable> T roundTrip(T value, Parcelable.Creator<T> creator) {
        Parcel p = Parcel.obtain();
        try {
            value.writeToParcel(p, 0); p.setDataPosition(0);
            T result = creator.createFromParcel(p);
            check(p.dataAvail() == 0, "complete parcel consumed");
            return result;
        } finally { p.recycle(); }
    }
    public static void main(String[] args) {
        System.loadLibrary("android_runtime");
        checks += NetworkRequestClearContractTest.contracts();
        DhcpInfo dhcp = new DhcpInfo();
        dhcp.ipAddress = 0x040200c0; dhcp.gateway = 0x010200c0;
        dhcp.netmask = 0x00ffffff; dhcp.dns1 = 0x350200c0;
        dhcp.dns2 = 0x08080808; dhcp.serverAddress = dhcp.gateway; dhcp.leaseDuration = 3600;
        check(dhcp.toString().equals("ipaddr 192.0.2.4 gateway 192.0.2.1 netmask 255.255.255.0 dns1 192.0.2.53 dns2 8.8.8.8 DHCP server 192.0.2.1 lease 3600 seconds"), "DHCP little-endian addresses");
        DhcpInfo dhcpCopy = roundTrip(dhcp, DhcpInfo.CREATOR);
        check(dhcpCopy.toString().equals(dhcp.toString()) && dhcpCopy.leaseDuration == 3600, "DHCP parcel");
        check(new DhcpInfo(dhcp).toString().equals(dhcp.toString()), "DHCP copy");
        ParseException error = new ParseException("bad address");
        check(error.response.equals("bad address") && error.getMessage().equals("bad address"), "parse error response and message");

        MacAddress remote = MacAddress.fromString("02:11:22:33:44:55");
        L2capNetworkSpecifier client = new L2capNetworkSpecifier.Builder()
                .setRole(L2capNetworkSpecifier.ROLE_CLIENT)
                .setHeaderCompression(L2capNetworkSpecifier.HEADER_COMPRESSION_6LOWPAN)
                .setRemoteAddress(remote).setPsm(128).build();
        check(client.isValidClientRequestSpecifier(), "L2CAP client request");
        check(!client.isValidServerReservationSpecifier(), "L2CAP client is not reservation");
        check(client.equals(roundTrip(client, L2capNetworkSpecifier.CREATOR)), "L2CAP parcel");
        check(new L2capNetworkSpecifier.Builder().build().canBeSatisfiedBy(client), "L2CAP wildcards");
        check(!new L2capNetworkSpecifier.Builder().setPsm(129).build().canBeSatisfiedBy(client), "L2CAP PSM mismatch");
        L2capNetworkSpecifier redacted = (L2capNetworkSpecifier) client.redact();
        check(redacted.getRemoteAddress() == null && redacted.getPsm() == 128 && client.getRemoteAddress().equals(remote), "L2CAP address redaction without mutation");
        check(new L2capNetworkSpecifier.Builder().setRole(L2capNetworkSpecifier.ROLE_SERVER)
                .setHeaderCompression(L2capNetworkSpecifier.HEADER_COMPRESSION_NONE).build()
                .isValidServerReservationSpecifier(), "L2CAP server reservation");
        rejects(() -> new L2capNetworkSpecifier.Builder().setPsm(256), "PSM upper bound");
        rejects(() -> new L2capNetworkSpecifier.Builder().setRole(L2capNetworkSpecifier.ROLE_SERVER)
                .setRemoteAddress(remote).build(), "server cannot specify remote");

        DiscoveryRequest discovery = new DiscoveryRequest.Builder("_ipp._tcp").setSubtype("_printer")
                .setNetwork(Network.fromNetworkHandle(0xcafed00dL + (7L << 32))).build();
        check(discovery.equals(roundTrip(discovery, DiscoveryRequest.CREATOR)), "discovery request parcel");
        check(discovery.getServiceType().equals("_ipp._tcp") && discovery.getSubtype().equals("_printer"), "discovery request fields");
        rejects(() -> new DiscoveryRequest.Builder(""), "empty discovery service type");
        rejects(() -> new DiscoveryRequest.Builder(2, "_ipp._tcp"), "unsupported discovery protocol");
        NsdServiceInfo info = new NsdServiceInfo();
        info.setServiceName("printer"); info.setServiceType("_ipp._tcp"); info.setPort(631);
        AdvertisingRequest advertising = new AdvertisingRequest.Builder(info)
                .setFlags(AdvertisingRequest.FLAG_SKIP_PROBING).setTtl(Duration.ofSeconds(120, 123)).build();
        AdvertisingRequest advertisingCopy = roundTrip(advertising, AdvertisingRequest.CREATOR);
        check(advertisingCopy.getFlags() == AdvertisingRequest.FLAG_SKIP_PROBING
                && advertisingCopy.getTtl().equals(Duration.ofSeconds(120))
                && advertisingCopy.getServiceInfo().getPort() == 631
                && advertisingCopy.getServiceInfo().getServiceName().equals("printer"), "advertising request payload and TTL truncation");
        check(new AdvertisingRequest.Builder(info).build().getTtl() == null, "default advertisement TTL");
        rejects(() -> new AdvertisingRequest.Builder(info).setTtl(Duration.ofSeconds(-1)), "negative TTL");
        rejects(() -> new AdvertisingRequest.Builder(info).setTtl(Duration.ofSeconds(0x100000000L)), "TTL upper bound");

        java.util.Set<Integer> subscriptions = new java.util.HashSet<>(java.util.Arrays.asList(1, 2));
        NetworkRequest request = new NetworkRequest.Builder().addTransportType(NetworkCapabilities.TRANSPORT_BLUETOOTH)
                .setNetworkSpecifier(new L2capNetworkSpecifier.Builder().setPsm(128).build())
                .setSubscriptionIds(subscriptions).build();
        subscriptions.clear();
        check(request.getSubscriptionIds().size() == 2, "request copies subscription filter");
        request.getSubscriptionIds().clear();
        check(request.getSubscriptionIds().size() == 2, "request getter copies subscription filter");
        NetworkCapabilities offered = new NetworkCapabilities().addTransportType(NetworkCapabilities.TRANSPORT_BLUETOOTH)
                .setNetworkSpecifier(client).setSubscriptionIds(java.util.Collections.singleton(2));
        check(request.canBeSatisfiedBy(offered), "request honors wildcard specifier and intersecting subscription");
        offered.setSubscriptionIds(java.util.Collections.singleton(3));
        check(!request.canBeSatisfiedBy(offered), "request rejects disjoint subscription");
        offered.setSubscriptionIds(java.util.Collections.emptySet());
        check(!request.canBeSatisfiedBy(offered), "request rejects absent subscription");
        NetworkRequest copiedRequest = roundTrip(request, NetworkRequest.CREATOR);
        check(copiedRequest.equals(request) && copiedRequest.hashCode() == request.hashCode(), "request parcel preserves specifier and subscriptions");
        VpnTransportInfo vpn = new VpnTransportInfo(1, "test-session", true, true);
        offered.setTransportInfo(vpn).setSubscriptionIds(java.util.Collections.singleton(2));
        NetworkCapabilities offeredCopy = roundTrip(offered, NetworkCapabilities.CREATOR);
        check(offeredCopy.equals(offered) && offeredCopy.getTransportInfo().equals(vpn), "capabilities parcel preserves transport value");
        check(request.canBeSatisfiedBy(offeredCopy), "matching preserved after parcel");
        NetworkCapabilities redactedCapabilities = new NetworkCapabilities(offered, NetworkCapabilities.REDACT_FOR_NETWORK_SETTINGS);
        check(((VpnTransportInfo)redactedCapabilities.getTransportInfo()).getSessionId() == null
                && vpn.getSessionId().equals("test-session"), "transport session redaction without mutation");
        check(vpn.isBypassable() && vpn.areLongLivedTcpConnectionsExpensive(), "VPN transport attributes");
        offeredCopy.clearAll();
        check(offeredCopy.getSubscriptionIds().isEmpty() && offeredCopy.getTransportInfo() == null
                && offeredCopy.getNetworkSpecifier() == null, "clear removes value filters");
        System.out.println("NetworkRequestValueContractTest all passed: " + checks);
    }
}
