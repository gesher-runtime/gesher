/* Copyright (C) 2026 The Gesher Authors. SPDX-License-Identifier: Apache-2.0 */
package gesher.services;

import android.net.IpConfiguration;
import android.net.LinkAddress;
import android.net.MacAddress;
import android.net.ProxyInfo;
import android.net.RouteInfo;
import android.net.StaticIpConfiguration;
import android.net.Uri;
import android.os.Parcel;
import android.os.Parcelable;

import java.net.InetAddress;
import java.util.Arrays;
import java.util.List;

/** Runs without a Binder server; tests actual Android value and Parcel contracts. */
public final class NetworkValueContractTest {
    private static int checks;
    private static void check(boolean condition, String name) {
        if (!condition) throw new AssertionError(name);
        checks++;
    }
    private static void rejects(Runnable action, String name) {
        try { action.run(); } catch (IllegalArgumentException expected) { checks++; return; }
        throw new AssertionError(name);
    }
    private static <T extends Parcelable> T roundTrip(T value, Parcelable.Creator<T> creator) {
        Parcel p = Parcel.obtain();
        try {
            value.writeToParcel(p, 0);
            p.setDataPosition(0);
            T copy = creator.createFromParcel(p);
            check(p.dataAvail() == 0, "complete parcel consumed");
            return copy;
        } finally { p.recycle(); }
    }
    public static void main(String[] args) throws Exception {
        System.loadLibrary("android_runtime");
        MacAddress mac = MacAddress.fromString("02:1a:2b:80:fe:ff");
        check(mac.toString().equals("02:1a:2b:80:fe:ff"), "MAC unsigned formatting");
        check(mac.toOuiString().equals("02:1a:2b"), "MAC OUI");
        check(mac.getAddressType() == MacAddress.TYPE_UNICAST && mac.isLocallyAssigned(), "local unicast");
        check(MacAddress.BROADCAST_ADDRESS.getAddressType() == MacAddress.TYPE_BROADCAST, "broadcast");
        check(MacAddress.fromString("01:00:5e:00:00:01").getAddressType() == MacAddress.TYPE_MULTICAST, "multicast");
        byte[] bytes = mac.toByteArray(); bytes[0] = 0;
        check(mac.toByteArray()[0] == 2, "MAC defensive copy");
        check(mac.matches(MacAddress.fromString("02:1a:2b:00:00:00"), MacAddress.fromString("ff:ff:ff:00:00:00")), "masked MAC match");
        check(!mac.matches(MacAddress.fromString("02:1a:2c:00:00:00"), MacAddress.fromString("ff:ff:ff:00:00:00")), "masked MAC mismatch");
        check(mac.getLinkLocalIpv6FromEui48Mac().equals(InetAddress.getByName("fe80::1a:2bff:fe80:feff")), "EUI64 link-local");
        check(mac.equals(roundTrip(mac, MacAddress.CREATOR)), "MAC parcel");
        rejects(() -> MacAddress.fromString("00:11:22:33:44"), "short MAC rejected");
        rejects(() -> MacAddress.fromBytes(new byte[8]), "eight-byte MAC rejected");

        LinkAddress address = new LinkAddress(InetAddress.getByName("192.0.2.4"), 24);
        StaticIpConfiguration config = new StaticIpConfiguration.Builder().setIpAddress(address)
                .setGateway(InetAddress.getByName("198.51.100.1"))
                .setDnsServers(Arrays.asList(InetAddress.getByName("192.0.2.53")))
                .setDomains("example.test").build();
        List<RouteInfo> routes = config.getRoutes("eth0");
        check(routes.size() == 3, "off-link gateway needs host route");
        check(routes.get(0).matches(InetAddress.getByName("192.0.2.99")), "connected route");
        check(routes.get(1).getDestination().getPrefixLength() == 32
                && routes.get(1).matches(config.getGateway()) && !routes.get(1).hasGateway(), "gateway host route");
        check(routes.get(2).isDefaultRoute() && routes.get(2).getGateway().equals(config.getGateway()), "default route");
        check(config.toLinkProperties("eth0").getRoutes().size() == 3, "link properties routes");
        check(config.equals(roundTrip(config, StaticIpConfiguration.CREATOR)), "static config parcel");
        StaticIpConfiguration clone = new StaticIpConfiguration(config);
        clone.dnsServers.clear();
        check(config.getDnsServers().size() == 1, "static config copy owns DNS list");
        rejects(() -> new StaticIpConfiguration.Builder().setIpAddress(new LinkAddress("2001:db8::1/64")), "IPv6 static config rejected");

        IpConfiguration dhcp = new IpConfiguration.Builder().build();
        check(dhcp.getIpAssignment() == IpConfiguration.IpAssignment.DHCP
                && dhcp.getProxySettings() == IpConfiguration.ProxySettings.NONE, "DHCP defaults");
        check(dhcp.equals(roundTrip(dhcp, IpConfiguration.CREATOR)), "DHCP parcel");
        check(dhcp.hashCode() == new IpConfiguration.Builder().build().hashCode(), "null proxy hash");
        IpConfiguration direct = new IpConfiguration.Builder().setStaticIpConfiguration(config)
                .setHttpProxy(ProxyInfo.buildDirectProxy("proxy.example", 8080)).build();
        check(direct.getIpAssignment() == IpConfiguration.IpAssignment.STATIC
                && direct.getProxySettings() == IpConfiguration.ProxySettings.STATIC, "direct proxy classification");
        check(direct.equals(roundTrip(direct, IpConfiguration.CREATOR)), "direct proxy config parcel");
        IpConfiguration pac = new IpConfiguration.Builder().setHttpProxy(
                ProxyInfo.buildPacProxy(Uri.parse("https://example.test/proxy.pac"))).build();
        check(pac.getProxySettings() == IpConfiguration.ProxySettings.PAC, "PAC proxy classification");
        System.out.println("NetworkValueContractTest all passed: " + checks);
    }
}
