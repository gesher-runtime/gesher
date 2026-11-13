/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package gesher.services;

import android.net.IpConfiguration;
import android.net.IpPrefix;
import android.net.LinkAddress;
import android.net.LinkProperties;
import android.net.ProxyInfo;
import android.net.RouteInfo;
import android.net.Uri;
import android.net.wifi.WifiConfiguration;
import android.os.Parcel;
import android.os.Parcelable;
import java.lang.reflect.InvocationTargetException;
import java.net.Inet4Address;
import java.net.Inet6Address;
import java.net.InetAddress;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collection;
import java.util.HashSet;
import java.util.List;
import java.util.Objects;

/**
 * Public call sites compile separately against SDK 36. Android's hidden value
 * constructors and setters are called reflectively to create platform snapshots;
 * no network connection, Wi-Fi radio or replacement framework class is used.
 */
public final class NetworkSnapshotContractTest {
    private static int checks;
    private interface Action { void run() throws Exception; }

    private static void check(boolean condition, String message) {
        if (!condition) throw new AssertionError(message);
        checks++;
    }

    private static void expect(Class<? extends Throwable> type, Action action, String message)
            throws Exception {
        try { action.run(); }
        catch (Throwable error) {
            while (error instanceof InvocationTargetException) error = error.getCause();
            if (!type.isInstance(error)) throw new AssertionError(message, error);
            checks++;
            return;
        }
        throw new AssertionError(message);
    }

    private static InetAddress ip(String address) throws Exception {
        // Every input is a numeric literal; the fixture never queries DNS.
        return InetAddress.getByName(address);
    }

    private static IpPrefix prefix(String address, int length) throws Exception {
        return new IpPrefix(ip(address), length);
    }

    private static IpPrefix mappedIpv4Prefix() throws Exception {
        byte[] bytes = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, (byte)0xff, (byte)0xff,
                (byte)192, 0, 2, 4};
        // Preserve the mapped IPv6 bytes at construction; IpPrefix.getAddress()
        // subsequently resolves the mapped address to Inet4Address, as on Android.
        return new IpPrefix(Inet6Address.getByAddress(null, bytes, 0), 120);
    }

    private static LinkAddress address(String text, int prefix) throws Exception {
        return LinkAddress.class.getConstructor(InetAddress.class, int.class)
                .newInstance(text == null ? null : ip(text), prefix);
    }

    private static LinkAddress address(String text, int prefix, int flags, int scope) throws Exception {
        return LinkAddress.class.getConstructor(InetAddress.class, int.class, int.class, int.class)
                .newInstance(text == null ? null : ip(text), prefix, flags, scope);
    }

    private static boolean preferred(LinkAddress address, String method) throws Exception {
        return (Boolean)LinkAddress.class.getMethod(method).invoke(address);
    }

    private static RouteInfo route(IpPrefix destination, InetAddress gateway, String iface, int type)
            throws Exception {
        return RouteInfo.class.getConstructor(IpPrefix.class, InetAddress.class, String.class, int.class)
                .newInstance(destination, gateway, iface, type);
    }

    private static RouteInfo route(IpPrefix destination, InetAddress gateway, String iface,
            int type, int mtu) throws Exception {
        return RouteInfo.class.getConstructor(IpPrefix.class, InetAddress.class, String.class,
                int.class, int.class).newInstance(destination, gateway, iface, type, mtu);
    }

    private static int mtu(RouteInfo route) throws Exception {
        return (Integer)RouteInfo.class.getMethod("getMtu").invoke(route);
    }

    private static LinkProperties copy(LinkProperties source) throws Exception {
        return LinkProperties.class.getConstructor(LinkProperties.class).newInstance(source);
    }

    private static void hidden(LinkProperties link, String name, Class<?> type, Object value)
            throws Exception {
        LinkProperties.class.getMethod(name, type).invoke(link, value);
    }

    private static void addresses() throws Exception {
        LinkAddress ordinary = address("192.0.2.1", 24);
        check(ordinary.getFlags() == 0, "Default LinkAddress flags are zero");
        String[] texts = {"0.0.0.0", "::", "127.0.0.1", "::1", "169.254.1.1", "fe80::1",
                "fec0::1", "10.1.2.3", "172.16.1.2", "192.168.1.1", "2001:db8::1", "fc00::1", "fd00::1"};
        int[] scopes = {254, 254, 253, 253, 253, 253, 200, 0, 0, 0, 0, 0, 0};
        boolean[] global = {false, false, false, false, false, false, false, true, true, true, true,
                false, false};
        for (int i = 0; i < texts.length; i++) {
            LinkAddress value = address(texts[i], 0);
            check(value.getScope() == scopes[i], "Address scope " + texts[i]);
            check(preferred(value, "isGlobalPreferred") == global[i], "Global preference " + texts[i]);
            check(preferred(value, "isPreferred"), "Unflagged address is preferred " + texts[i]);
            check(value.getFlags() == 0, "Scope inference does not add flags " + texts[i]);
        }
        // DADFAILED, DEPRECATED, TENTATIVE and optimistic DAD combinations.
        int[] flags = {0, 0x80, 0x08, 0x20, 0x40, 0x04, 0x44, 0x4c, 0x64, 0x84};
        boolean[] isPreferred = {true, true, false, false, false, true, true, false, false, true};
        for (int i = 0; i < flags.length; i++) {
            LinkAddress value = address("2001:db8::1", 64, flags[i], 0);
            check(value.getFlags() == flags[i], "Explicit flags retained " + flags[i]);
            check(preferred(value, "isPreferred") == isPreferred[i], "Preference flags " + flags[i]);
            check(preferred(value, "isGlobalPreferred") == isPreferred[i], "Global preference flags " + flags[i]);
        }
        check(!preferred(address("2001:db8::1", 64, 0, 253), "isGlobalPreferred"),
                "Explicit local scope remains non-global");
        check(ordinary.equals(address("192.0.2.1", 24, 0, 0)), "Default flags equal explicit zero");
        expect(IllegalArgumentException.class, () -> address(null, 0), "Null address rejected consistently");
        expect(IllegalArgumentException.class, () -> address("224.0.0.1", 4), "IPv4 multicast rejected");
        expect(IllegalArgumentException.class, () -> address("ff02::1", 64), "IPv6 multicast rejected");
        expect(IllegalArgumentException.class, () -> address("192.0.2.1", 33), "IPv4 prefix bound");
        expect(IllegalArgumentException.class, () -> address("2001:db8::1", 129), "IPv6 prefix bound");
        expect(IllegalArgumentException.class, () -> address("2001:db8::1", -1), "Negative prefix bound");
    }

    private static void routes() throws Exception {
        LinkProperties link = new LinkProperties();
        link.setInterfaceName("eth0");
        IpPrefix destination = prefix("192.0.2.0", 24);
        RouteInfo unnamed = route(destination, null, null, RouteInfo.RTN_UNICAST);
        check(link.addRoute(unnamed), "Add unnamed route");
        check("eth0".equals(link.getRoutes().get(0).getInterface()), "Route adopts link interface");
        check(unnamed.getInterface() == null, "Caller route remains unchanged");
        check(!link.addRoute(unnamed), "Same normalized route is not duplicated");
        link.setInterfaceName("eth1");
        check("eth1".equals(link.getRoutes().get(0).getInterface()), "Route tracks renamed interface");
        RouteInfo before = link.getRoutes().get(0);
        expect(IllegalArgumentException.class,
                () -> link.addRoute(route(destination, null, "wlan0", RouteInfo.RTN_UNICAST)),
                "Mismatched route interface rejected");
        check(link.getRoutes().equals(List.of(before)), "Rejected route preserves prior state");
        expect(NullPointerException.class, () -> link.addRoute(null), "Null route follows Android contract");

        RouteInfo changed = route(destination, null, "eth1", RouteInfo.RTN_UNREACHABLE, 1400);
        check(link.addRoute(changed), "Same route key updates non-key properties");
        check(link.getRoutes().size() == 1 && link.getRoutes().get(0).equals(changed),
                "Key replacement retains one route");
        check(!link.addRoute(changed), "Repeated replacement reports no change");
        RouteInfo next = route(prefix("2001:db8::", 64), null, null, RouteInfo.RTN_UNICAST, 1280);
        check(link.addRoute(next), "Distinct route key appended");
        RouteInfo mtuChanged = route(destination, null, "eth1", RouteInfo.RTN_UNREACHABLE, 1450);
        check(!changed.equals(mtuChanged) && changed.hashCode() != mtuChanged.hashCode(),
                "Route MTU participates in value semantics");
        check(link.addRoute(mtuChanged) && link.getRoutes().get(0).equals(mtuChanged)
                && link.getRoutes().size() == 2, "MTU-only replacement retains route position");
        link.setInterfaceName("eth2");
        check(link.getRoutes().stream().allMatch(r -> "eth2".equals(r.getInterface())),
                "All routes rebind together");
        check(mtu(link.getRoutes().get(0)) == 1450 && mtu(link.getRoutes().get(1)) == 1280,
                "Rebinding preserves each MTU");
        check(link.getRoutes().get(0).getType() == RouteInfo.RTN_UNREACHABLE,
                "Rebinding preserves route type");
        check(!(Boolean)LinkProperties.class.getMethod("removeRoute", RouteInfo.class)
                .invoke(link, mtuChanged), "Removal requires current interface");
        RouteInfo rebound = link.getRoutes().get(0);
        check((Boolean)LinkProperties.class.getMethod("removeRoute", RouteInfo.class)
                .invoke(link, rebound), "Matching route removed");
        link.setInterfaceName(null);
        check(link.getRoutes().get(0).getInterface() == null && mtu(link.getRoutes().get(0)) == 1280,
                "Clearing interface also rebinds routes");
        RouteInfo defaultV6 = route(null, ip("2001:db8::1"), "eth0", RouteInfo.RTN_UNICAST, 1300);
        check(defaultV6.isDefaultRoute() && defaultV6.getDestination().getRawAddress().length == 16,
                "Null destination follows gateway family");
        expect(IllegalArgumentException.class, () -> route(null, null, null, RouteInfo.RTN_UNICAST),
                "Route requires destination or gateway");
        expect(IllegalArgumentException.class, () -> route(destination, ip("2001:db8::1"), null,
                RouteInfo.RTN_UNICAST), "Route address family mismatch rejected");
        expect(IllegalArgumentException.class, () -> route(destination, null, null, 999),
                "Unknown route type rejected");

        IpPrefix mappedPrefix = mappedIpv4Prefix();
        check(mappedPrefix.getRawAddress().length == 16
                && mappedPrefix.getAddress() instanceof Inet4Address,
                "Mapped prefix retains IPv6 bytes with an IPv4 address family");
        RouteInfo mapped = route(mappedPrefix, null, "eth0", RouteInfo.RTN_UNICAST, 1460);
        check(mapped.getGateway() instanceof Inet4Address && mapped.getGateway().isAnyLocalAddress()
                && !mapped.hasGateway(), "Mapped prefix uses the IPv4 unspecified gateway");
        check(mapped.getDestination().equals(mappedPrefix) && "eth0".equals(mapped.getInterface())
                && mapped.getType() == RouteInfo.RTN_UNICAST && mtu(mapped) == 1460,
                "Mapped route preserves prefix, interface, type and MTU");
        expect(IllegalArgumentException.class, () -> route(mappedPrefix, ip("2001:db8::1"),
                "eth0", RouteInfo.RTN_UNICAST), "Mapped prefix still rejects an IPv6 gateway");
    }

    private static LinkProperties populated(boolean reversed) throws Exception {
        LinkProperties value = new LinkProperties();
        value.setInterfaceName("eth0");
        List<LinkAddress> addresses = new ArrayList<>(List.of(address("192.0.2.5", 24),
                address("2001:db8::5", 64)));
        List<InetAddress> dns = new ArrayList<>(List.of(ip("192.0.2.53"), ip("2001:db8::53")));
        List<RouteInfo> routes = new ArrayList<>(List.of(
                route(prefix("0.0.0.0", 0), ip("192.0.2.1"), null, RouteInfo.RTN_UNICAST, 1400),
                route(prefix("::", 0), ip("2001:db8::1"), null, RouteInfo.RTN_UNICAST, 1280)));
        if (reversed) {
            java.util.Collections.reverse(addresses);
            java.util.Collections.reverse(dns);
            java.util.Collections.reverse(routes);
        }
        value.setLinkAddresses(addresses);
        value.setDnsServers(dns);
        for (RouteInfo route : routes) value.addRoute(route);
        hidden(value, "setValidatedPrivateDnsServers", Collection.class, dns);
        hidden(value, "setUsePrivateDns", boolean.class, true);
        hidden(value, "setPrivateDnsServerName", String.class, "dns.example.test");
        hidden(value, "setWakeOnLanSupported", boolean.class, true);
        value.setDhcpServerAddress((Inet4Address)ip("192.0.2.2"));
        value.setNat64Prefix(prefix("64:ff9b::", 96));
        value.setDomains("example.test");
        value.setHttpProxy(ProxyInfo.buildDirectProxy("proxy.example.test", 8080, List.of("localhost")));
        value.setMtu(1500);
        return value;
    }

    private static void values() throws Exception {
        LinkProperties empty = new LinkProperties();
        LinkProperties dhcp = new LinkProperties();
        dhcp.setDhcpServerAddress((Inet4Address)ip("192.0.2.1"));
        check(!dhcp.equals(empty) && !empty.equals(dhcp), "DHCP address changes equality");
        LinkProperties wol = new LinkProperties();
        hidden(wol, "setWakeOnLanSupported", boolean.class, true);
        check(!wol.equals(empty) && !empty.equals(wol), "Wake-on-LAN changes equality");
        LinkProperties first = populated(false), reordered = populated(true), copied = copy(first);
        check(first.equals(reordered) && reordered.equals(copied) && first.equals(copied),
                "Address/DNS/route order is not value state");
        check(first.hashCode() == reordered.hashCode() && first.hashCode() == copied.hashCode(),
                "Equal unordered snapshots have equal hashes");
        check(new HashSet<>(List.of(first, reordered, copied)).size() == 1,
                "Hash-based collections deduplicate equal snapshots");
        LinkProperties addressMetadata = copy(first);
        addressMetadata.setLinkAddresses(List.of(address("192.0.2.5", 16, 0x20, 253),
                address("2001:db8::5", 48, 0x44, 0)));
        check(first.equals(addressMetadata) && addressMetadata.equals(first),
                "LinkProperties compares address identity independently of prefix/flags/scope");
        check(first.hashCode() == addressMetadata.hashCode(), "Address metadata does not change equal snapshot hash");
        check(!first.getLinkAddresses().get(0).equals(addressMetadata.getLinkAddresses().get(0)),
                "LinkAddress itself still distinguishes prefix/flags/scope");
        // AOSP calls its address helper with (target, this). Distinct prefixes can
        // repeat an IP in getAddresses(); preserve the pinned asymmetric edge case.
        LinkProperties duplicateIps = new LinkProperties(), distinctIps = new LinkProperties();
        duplicateIps.setLinkAddresses(List.of(address("192.0.2.1", 24), address("192.0.2.1", 16)));
        distinctIps.setLinkAddresses(List.of(address("192.0.2.1", 24), address("192.0.2.2", 24)));
        check(duplicateIps.equals(distinctIps), "Pinned address comparison uses target contains receiver");
        check(!distinctIps.equals(duplicateIps), "Pinned duplicate-address comparison direction is retained");
        check(first.equals(first) && !first.equals(null) && !first.equals("other"), "Equality basic contract");
        for (int i = 0; i < 13; i++) {
            LinkProperties changed = copy(first);
            switch (i) {
                case 0: changed.setInterfaceName("eth1"); break;
                case 1: changed.setLinkAddresses(List.of(address("192.0.2.6", 24))); break;
                case 2: changed.setDnsServers(List.of(ip("192.0.2.54"))); break;
                case 3: hidden(changed, "setValidatedPrivateDnsServers", Collection.class,
                        List.of(ip("192.0.2.54"))); break;
                case 4: hidden(changed, "setUsePrivateDns", boolean.class, false); break;
                case 5: hidden(changed, "setPrivateDnsServerName", String.class, "other.example.test"); break;
                case 6: changed.setDomains("other.example.test"); break;
                case 7: changed.addRoute(route(prefix("192.0.2.0", 24), null, null, RouteInfo.RTN_UNICAST)); break;
                case 8: changed.setHttpProxy(null); break;
                case 9: changed.setMtu(1400); break;
                case 10: changed.setNat64Prefix(null); break;
                case 11: changed.setDhcpServerAddress((Inet4Address)ip("192.0.2.3")); break;
                case 12: hidden(changed, "setWakeOnLanSupported", boolean.class, false); break;
                default: throw new AssertionError(i);
            }
            check(!first.equals(changed) && !changed.equals(first), "Supported snapshot field differs " + i);
        }
        check(first.equals(copied), "Copy changes do not affect original snapshot");
        copied.clear();
        check(copied.equals(empty) && copied.hashCode() == empty.hashCode(), "Clear resets every supported field");
        hidden(empty, "setValidatedPrivateDnsServers", Collection.class,
                Arrays.asList(ip("192.0.2.53"), null, ip("192.0.2.53")));
        check(((List<?>)LinkProperties.class.getMethod("getValidatedPrivateDnsServers")
                .invoke(empty)).size() == 1, "Validated DNS ignores nulls and duplicates");
        check((Boolean)LinkProperties.class.getMethod("removeValidatedPrivateDnsServer", InetAddress.class)
                .invoke(empty, ip("192.0.2.53")), "Validated DNS removal reports change");
        check(empty.equals(new LinkProperties()), "Removing final validated DNS restores empty value");
    }

    private static void nat64() throws Exception {
        LinkProperties link = new LinkProperties();
        IpPrefix good = prefix("64:ff9b::", 96);
        link.setNat64Prefix(good);
        for (int length : new int[]{0, 32, 64, 95, 97, 128}) {
            expect(IllegalArgumentException.class, () -> link.setNat64Prefix(prefix("64:ff9b::", length)),
                    "Reject unsupported NAT64 length " + length);
            check(link.getNat64Prefix().equals(good), "Rejected NAT64 prefix preserves previous value");
        }
        link.setNat64Prefix(null);
        check(link.getNat64Prefix() == null, "NAT64 null clears prefix");
    }

    private static void proxies() throws Exception {
        WifiConfiguration wifi = new WifiConfiguration();
        check(wifi.getHttpProxy() != null && wifi.getHttpProxy().getHost() == null
                && wifi.getHttpProxy().getPort() == 0, "New Android configuration uses UNASSIGNED proxy");
        ProxyInfo direct = ProxyInfo.buildDirectProxy("proxy.example.test", 8181, List.of("localhost", "*.test"));
        wifi.setHttpProxy(direct);
        ProxyInfo first = wifi.getHttpProxy(), second = wifi.getHttpProxy();
        check(first.equals(direct) && second.equals(direct), "Direct proxy retains host/port/exclusions");
        check(first != direct && first != second, "Proxy getter returns detached copies");
        String[] exclusions = first.getExclusionList();
        exclusions[0] = "changed";
        check("localhost".equals(wifi.getHttpProxy().getExclusionList()[0]), "Returned exclusions do not mutate stored proxy");
        ProxyInfo pac = ProxyInfo.buildPacProxy(Uri.parse("https://proxy.example.test/config.pac"), 3128);
        wifi.setHttpProxy(pac);
        check(wifi.getHttpProxy().equals(pac) && wifi.getHttpProxy() != pac, "PAC URI and port retained in copy");
        ProxyInfo unresolvedPac = ProxyInfo.buildPacProxy(Uri.parse("https://proxy.example.test/config.pac"));
        wifi.setHttpProxy(unresolvedPac);
        check(wifi.getHttpProxy().getPort() == -1, "Unresolved PAC local port remains -1");
        ProxyInfo invalid = ProxyInfo.buildDirectProxy("proxy.example.test", -1);
        check(!invalid.isValid(), "Invalid direct proxy fixture");
        wifi.setHttpProxy(invalid);
        check(wifi.getHttpProxy().equals(invalid), "Pinned Android warns but retains invalid proxy");
        wifi.setHttpProxy(null);
        check(wifi.getHttpProxy() == null, "Explicit NONE differs from UNASSIGNED");
        IpConfiguration ip = new IpConfiguration.Builder().setHttpProxy(direct).build();
        wifi.setIpConfiguration(ip);
        check(wifi.getHttpProxy().equals(direct), "Public IP configuration supplies proxy");
        IpConfiguration detached = (IpConfiguration)WifiConfiguration.class.getMethod("getIpConfiguration")
                .invoke(wifi);
        check(detached != ip && detached.equals(ip), "IP configuration getter copies");
        IpConfiguration.class.getMethod("setHttpProxy", ProxyInfo.class).invoke(ip, pac);
        check(wifi.getHttpProxy().equals(pac), "Pinned IP configuration setter retains supplied state object");
        check(detached.getHttpProxy().equals(direct), "Earlier IP configuration copy retains its proxy");
        wifi.setIpConfiguration(null);
        check(wifi.getHttpProxy() != null && wifi.getHttpProxy().getHost() == null,
                "Null IP configuration restores default UNASSIGNED state");
    }

    private static <T extends Parcelable> T roundTrip(T value, Parcelable.Creator<T> creator)
            throws Exception {
        Parcel parcel = Parcel.obtain();
        try {
            value.writeToParcel(parcel, 0);
            parcel.writeInt(0x26a91);
            parcel.setDataPosition(0);
            T result = creator.createFromParcel(parcel);
            check(parcel.readInt() == 0x26a91 && parcel.dataPosition() == parcel.dataSize(),
                    "Parcel consumes exact value bytes " + value.getClass().getSimpleName());
            return result;
        } finally { parcel.recycle(); }
    }

    @SuppressWarnings("unchecked")
    private static void parcels() throws Exception {
        LinkProperties expected = populated(false);
        LinkProperties restored = roundTrip(expected, LinkProperties.CREATOR);
        check(expected.equals(restored) && expected.hashCode() == restored.hashCode(), "Complete LinkProperties parcel");
        check(expected.getDhcpServerAddress().equals(restored.getDhcpServerAddress()), "DHCP server survives parcel");
        check(restored.isWakeOnLanSupported() && restored.isPrivateDnsActive(), "Boolean link state survives parcel");
        check(mtu(restored.getRoutes().get(0)) == 1400 && mtu(restored.getRoutes().get(1)) == 1280,
                "Route MTUs survive nested parcel");
        check(new LinkProperties().equals(roundTrip(new LinkProperties(), LinkProperties.CREATOR)),
                "Empty LinkProperties parcel");
        LinkAddress linkAddress = address("2001:db8::4", 64, 0x44, 0);
        check(linkAddress.equals(roundTrip(linkAddress, LinkAddress.CREATOR)), "Address flags/scope parcel");
        RouteInfo route = route(prefix("192.0.2.0", 24), ip("192.0.2.1"), "eth1", RouteInfo.RTN_THROW, 1492);
        check(route.equals(roundTrip(route, RouteInfo.CREATOR)), "Route type/MTU parcel");
        RouteInfo mapped = route(mappedIpv4Prefix(), null, "eth2", RouteInfo.RTN_THROW, 1460);
        RouteInfo mappedCopy = roundTrip(mapped, RouteInfo.CREATOR);
        check(mapped.equals(mappedCopy) && mappedCopy.getDestination().getRawAddress().length == 16
                && mappedCopy.getGateway() instanceof Inet4Address
                && mappedCopy.getGateway().isAnyLocalAddress(),
                "Mapped route retains raw prefix, IPv4 gateway and route data through Parcel");
        Parcelable.Creator<WifiConfiguration> creator = (Parcelable.Creator<WifiConfiguration>)
                WifiConfiguration.class.getField("CREATOR").get(null);
        for (int mode = 0; mode < 4; mode++) {
            WifiConfiguration wifi = new WifiConfiguration();
            wifi.SSID = "\"Gesher\"";
            wifi.networkId = 17;
            wifi.wepKeys[0] = "0123456789";
            wifi.allowedProtocols.set(1);
            if (mode == 1) wifi.setHttpProxy(null);
            if (mode == 2) wifi.setHttpProxy(ProxyInfo.buildDirectProxy("proxy.example.test", 8080, List.of("*.test")));
            if (mode == 3) wifi.setHttpProxy(ProxyInfo.buildPacProxy(Uri.parse("https://proxy.example.test/a.pac"), 9191));
            WifiConfiguration actual = roundTrip(wifi, creator);
            check(Objects.equals(wifi.getHttpProxy(), actual.getHttpProxy()), "Wi-Fi proxy parcel mode " + mode);
            check(wifi.networkId == actual.networkId && wifi.SSID.equals(actual.SSID)
                    && Arrays.equals(wifi.wepKeys, actual.wepKeys)
                    && wifi.allowedProtocols.equals(actual.allowedProtocols), "Existing Wi-Fi fields retained " + mode);
        }
    }

    public static void main(String[] args) throws Exception {
        boolean host = args.length > 0 && !args[0].equals("windows");
        if (host) System.load(args[0]); else System.loadLibrary("android_runtime");
        String group = args.length > 1 ? args[1] : "all";
        if (group.equals("all") || group.equals("addresses")) addresses();
        if (group.equals("all") || group.equals("routes")) routes();
        if (group.equals("all") || group.equals("values")) values();
        if (group.equals("all") || group.equals("nat64")) nat64();
        if (group.equals("all") || group.equals("proxies")) proxies();
        if (!host && group.equals("all")) parcels();
        System.out.println("NetworkSnapshotContractTest all passed: " + checks + " checks; parcel=" + !host);
    }
}
