/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package android.net;

import android.os.Parcel;
import android.os.Parcelable;

import java.net.Inet4Address;
import java.net.Inet6Address;
import java.net.InetAddress;
import java.net.UnknownHostException;
import java.util.ArrayList;
import java.util.Collection;
import java.util.Collections;
import java.util.List;
import java.util.Objects;

/**
 * The Connectivity module's LinkProperties (framework.jar compiles against the module's
 * API and does not contain it), with its public API: a network's interface, addresses,
 * routes, DNS servers and search domains, MTU and proxy (docs/network.md).
 * {@link ConnectivityManager} fills it from the Windows adapter behind the network.
 */
public final class LinkProperties implements Parcelable {
    private String mIfaceName;
    private final List<LinkAddress> mLinkAddresses = new ArrayList<>();
    private final List<InetAddress> mDnses = new ArrayList<>();
    private final List<InetAddress> mValidatedPrivateDnses = new ArrayList<>();
    private boolean mUsePrivateDns;
    private String mPrivateDnsServerName;
    private String mDomains;
    private final List<RouteInfo> mRoutes = new ArrayList<>();
    private ProxyInfo mHttpProxy;
    private int mMtu;
    private IpPrefix mNat64Prefix;
    private Inet4Address mDhcpServerAddress;
    private boolean mWakeOnLanSupported;

    public LinkProperties() {
    }

    public LinkProperties(LinkProperties source) {
        if (source == null) return;
        mIfaceName = source.mIfaceName;
        mLinkAddresses.addAll(source.mLinkAddresses);
        mDnses.addAll(source.mDnses);
        mValidatedPrivateDnses.addAll(source.mValidatedPrivateDnses);
        mUsePrivateDns = source.mUsePrivateDns;
        mPrivateDnsServerName = source.mPrivateDnsServerName;
        mDomains = source.mDomains;
        mRoutes.addAll(source.mRoutes);
        mHttpProxy = source.mHttpProxy == null ? null : new ProxyInfo(source.mHttpProxy);
        mMtu = source.mMtu;
        mNat64Prefix = source.mNat64Prefix;
        mDhcpServerAddress = source.mDhcpServerAddress;
        mWakeOnLanSupported = source.mWakeOnLanSupported;
    }

    public void setInterfaceName(String iface) {
        mIfaceName = iface;
        for (int i = 0; i < mRoutes.size(); i++) {
            mRoutes.set(i, routeWithInterface(mRoutes.get(i)));
        }
    }

    public String getInterfaceName() {
        return mIfaceName;
    }

    /** @hide */
    public List<String> getAllInterfaceNames() {
        return mIfaceName == null ? Collections.emptyList() : List.of(mIfaceName);
    }

    /** @hide */
    public List<InetAddress> getAddresses() {
        List<InetAddress> addresses = new ArrayList<>();
        for (LinkAddress a : mLinkAddresses) addresses.add(a.getAddress());
        return Collections.unmodifiableList(addresses);
    }

    /** @hide */
    public List<InetAddress> getAllAddresses() {
        return getAddresses();
    }

    /** @hide */
    public boolean addLinkAddress(LinkAddress address) {
        if (address == null || mLinkAddresses.contains(address)) return false;
        mLinkAddresses.removeIf(a -> a.isSameAddressAs(address));
        mLinkAddresses.add(address);
        return true;
    }

    /** @hide */
    public boolean removeLinkAddress(LinkAddress address) {
        return address != null && mLinkAddresses.removeIf(a -> a.isSameAddressAs(address));
    }

    public List<LinkAddress> getLinkAddresses() {
        return Collections.unmodifiableList(new ArrayList<>(mLinkAddresses));
    }

    /** @hide */
    public List<LinkAddress> getAllLinkAddresses() {
        return getLinkAddresses();
    }

    public void setLinkAddresses(Collection<LinkAddress> addresses) {
        mLinkAddresses.clear();
        for (LinkAddress a : addresses) addLinkAddress(a);
    }

    /** @hide */
    public boolean addDnsServer(InetAddress dnsServer) {
        if (dnsServer == null || mDnses.contains(dnsServer)) return false;
        mDnses.add(dnsServer);
        return true;
    }

    public void setDnsServers(Collection<InetAddress> dnsServers) {
        mDnses.clear();
        for (InetAddress d : dnsServers) addDnsServer(d);
    }

    public List<InetAddress> getDnsServers() {
        return Collections.unmodifiableList(new ArrayList<>(mDnses));
    }

    /** @hide */
    public void setUsePrivateDns(boolean usePrivateDns) {
        mUsePrivateDns = usePrivateDns;
    }

    public boolean isPrivateDnsActive() {
        return mUsePrivateDns;
    }

    /** @hide */
    public void setPrivateDnsServerName(String name) {
        mPrivateDnsServerName = name;
    }

    public String getPrivateDnsServerName() {
        return mPrivateDnsServerName;
    }

    /** @hide */
    public List<InetAddress> getValidatedPrivateDnsServers() {
        return Collections.unmodifiableList(new ArrayList<>(mValidatedPrivateDnses));
    }

    /** @hide */
    public boolean addValidatedPrivateDnsServer(InetAddress dnsServer) {
        if (dnsServer == null || mValidatedPrivateDnses.contains(dnsServer)) return false;
        mValidatedPrivateDnses.add(dnsServer);
        return true;
    }

    /** @hide */
    public boolean removeValidatedPrivateDnsServer(InetAddress dnsServer) {
        return mValidatedPrivateDnses.remove(dnsServer);
    }

    /** @hide */
    public void setValidatedPrivateDnsServers(Collection<InetAddress> dnsServers) {
        mValidatedPrivateDnses.clear();
        for (InetAddress dnsServer : dnsServers) addValidatedPrivateDnsServer(dnsServer);
    }

    public void setDomains(String domains) {
        mDomains = domains;
    }

    public String getDomains() {
        return mDomains;
    }

    public void setMtu(int mtu) {
        mMtu = mtu;
    }

    public int getMtu() {
        return mMtu;
    }

    private RouteInfo routeWithInterface(RouteInfo route) {
        return new RouteInfo(route.getDestination(), route.getGateway(), mIfaceName,
                route.getType(), route.getMtu());
    }

    public boolean addRoute(RouteInfo route) {
        String routeIface = route.getInterface();
        if (routeIface != null && !routeIface.equals(mIfaceName)) {
            throw new IllegalArgumentException("Route interface " + routeIface
                    + " does not match " + mIfaceName);
        }
        route = routeWithInterface(route);
        for (int i = 0; i < mRoutes.size(); i++) {
            RouteInfo current = mRoutes.get(i);
            // Type and MTU are route properties, not part of Android's route key.
            if (current.getDestination().equals(route.getDestination())
                    && current.getGateway().equals(route.getGateway())
                    && Objects.equals(current.getInterface(), route.getInterface())) {
                if (current.equals(route)) return false;
                mRoutes.set(i, route);
                return true;
            }
        }
        mRoutes.add(route);
        return true;
    }

    /** @hide */
    public boolean removeRoute(RouteInfo route) {
        return Objects.equals(mIfaceName, route.getInterface()) && mRoutes.remove(route);
    }

    public List<RouteInfo> getRoutes() {
        return Collections.unmodifiableList(new ArrayList<>(mRoutes));
    }

    /** @hide */
    public List<RouteInfo> getAllRoutes() {
        return getRoutes();
    }

    public void setHttpProxy(ProxyInfo proxy) {
        mHttpProxy = proxy;
    }

    public ProxyInfo getHttpProxy() {
        return mHttpProxy;
    }

    public IpPrefix getNat64Prefix() {
        return mNat64Prefix;
    }

    public void setNat64Prefix(IpPrefix prefix) {
        if (prefix != null && prefix.getPrefixLength() != 96) {
            throw new IllegalArgumentException("Only /96 NAT64 prefixes are supported");
        }
        mNat64Prefix = prefix;
    }

    public Inet4Address getDhcpServerAddress() {
        return mDhcpServerAddress;
    }

    public void setDhcpServerAddress(Inet4Address serverAddress) {
        mDhcpServerAddress = serverAddress;
    }

    public boolean isWakeOnLanSupported() {
        return mWakeOnLanSupported;
    }

    public void setWakeOnLanSupported(boolean supported) {
        mWakeOnLanSupported = supported;
    }

    public android.net.Uri getCaptivePortalApiUrl() {
        return null;
    }

    /** @hide */
    public boolean hasIpv4Address() {
        for (LinkAddress a : mLinkAddresses) {
            if (a.getAddress() instanceof Inet4Address) return true;
        }
        return false;
    }

    /** @hide */
    public boolean hasGlobalIpv6Address() {
        for (LinkAddress a : mLinkAddresses) {
            if (a.getAddress() instanceof Inet6Address && a.isGlobalPreferred()) return true;
        }
        return false;
    }

    /** @hide */
    public boolean hasIpv4DefaultRoute() {
        for (RouteInfo r : mRoutes) {
            if (r.isIPv4Default()) return true;
        }
        return false;
    }

    /** @hide */
    public boolean hasIpv6DefaultRoute() {
        for (RouteInfo r : mRoutes) {
            if (r.isIPv6Default()) return true;
        }
        return false;
    }

    /** @hide */
    public boolean hasIpv4DnsServer() {
        for (InetAddress d : mDnses) {
            if (d instanceof Inet4Address) return true;
        }
        return false;
    }

    /** @hide */
    public boolean hasIpv6DnsServer() {
        for (InetAddress d : mDnses) {
            if (d instanceof Inet6Address) return true;
        }
        return false;
    }

    /** @hide */
    public boolean isIpv4Provisioned() {
        return hasIpv4Address() && hasIpv4DefaultRoute() && hasIpv4DnsServer();
    }

    /** @hide */
    public boolean isIpv6Provisioned() {
        return hasGlobalIpv6Address() && hasIpv6DefaultRoute() && hasIpv6DnsServer();
    }

    /** @hide */
    public boolean isProvisioned() {
        return isIpv4Provisioned() || isIpv6Provisioned();
    }

    /** @hide */
    public boolean isReachable(InetAddress ip) {
        for (RouteInfo r : mRoutes) {
            if (r.matches(ip)) return true;
        }
        return false;
    }

    public void clear() {
        mIfaceName = null;
        mLinkAddresses.clear();
        mDnses.clear();
        mValidatedPrivateDnses.clear();
        mUsePrivateDns = false;
        mPrivateDnsServerName = null;
        mDomains = null;
        mRoutes.clear();
        mHttpProxy = null;
        mMtu = 0;
        mNat64Prefix = null;
        mDhcpServerAddress = null;
        mWakeOnLanSupported = false;
    }

    @Override
    public String toString() {
        StringBuilder sb = new StringBuilder("{");
        if (mIfaceName != null) sb.append("InterfaceName: ").append(mIfaceName).append(" ");
        sb.append("LinkAddresses: ").append(mLinkAddresses).append(" ");
        sb.append("DnsAddresses: ").append(mDnses).append(" ");
        if (mUsePrivateDns) sb.append("UsePrivateDns: true ");
        if (mDomains != null) sb.append("Domains: ").append(mDomains).append(" ");
        if (mMtu != 0) sb.append("MTU: ").append(mMtu).append(" ");
        sb.append("Routes: ").append(mRoutes).append(" ");
        if (mHttpProxy != null) sb.append("HttpProxy: ").append(mHttpProxy).append(" ");
        return sb.append("}").toString();
    }

    @Override
    public boolean equals(Object o) {
        if (!(o instanceof LinkProperties)) return false;
        LinkProperties lp = (LinkProperties) o;
        return Objects.equals(mIfaceName, lp.mIfaceName)
                // Android's LinkProperties equality compares IP addresses here;
                // LinkAddress's own equality additionally includes prefix/flags/scope.
                // Its helper receives (target, this). Preserve that direction even
                // for duplicate IPs with different prefixes, where AOSP is asymmetric.
                && sameElements(lp.getAddresses(), getAddresses()) && sameElements(mDnses, lp.mDnses)
                && sameElements(mValidatedPrivateDnses, lp.mValidatedPrivateDnses)
                && mUsePrivateDns == lp.mUsePrivateDns
                && Objects.equals(mPrivateDnsServerName, lp.mPrivateDnsServerName)
                && Objects.equals(mDomains, lp.mDomains) && sameElements(mRoutes, lp.mRoutes)
                && Objects.equals(mHttpProxy, lp.mHttpProxy) && mMtu == lp.mMtu
                && Objects.equals(mNat64Prefix, lp.mNat64Prefix)
                && Objects.equals(mDhcpServerAddress, lp.mDhcpServerAddress)
                && mWakeOnLanSupported == lp.mWakeOnLanSupported;
    }

    private static boolean sameElements(Collection<?> first, Collection<?> second) {
        return first.size() == second.size() && first.containsAll(second);
    }

    @Override
    public int hashCode() {
        // These collections are sets for equality. Counts, as in AOSP, keep hashes
        // independent of their insertion order while allowing unequal values to collide.
        return Objects.hash(mIfaceName, mLinkAddresses.size(), mDnses.size(),
                mValidatedPrivateDnses.size(), mUsePrivateDns, mPrivateDnsServerName,
                mDomains, mRoutes.size(), mHttpProxy, mMtu, mNat64Prefix,
                mDhcpServerAddress, mWakeOnLanSupported);
    }

    @Override
    public int describeContents() {
        return 0;
    }

    private static void writeAddresses(Parcel dest, List<InetAddress> addresses) {
        dest.writeInt(addresses.size());
        for (InetAddress a : addresses) dest.writeByteArray(a.getAddress());
    }

    private static void readAddresses(Parcel in, List<InetAddress> out) {
        for (int i = in.readInt(); i > 0; i--) {
            try {
                out.add(InetAddress.getByAddress(in.createByteArray()));
            } catch (UnknownHostException e) {
                throw new IllegalArgumentException(e);
            }
        }
    }

    @Override
    public void writeToParcel(Parcel dest, int flags) {
        dest.writeString(mIfaceName);
        dest.writeTypedList(mLinkAddresses);
        writeAddresses(dest, mDnses);
        writeAddresses(dest, mValidatedPrivateDnses);
        dest.writeBoolean(mUsePrivateDns);
        dest.writeString(mPrivateDnsServerName);
        dest.writeString(mDomains);
        dest.writeTypedList(mRoutes);
        dest.writeInt(mMtu);
        dest.writeBoolean(mHttpProxy != null);
        if (mHttpProxy != null) mHttpProxy.writeToParcel(dest, flags);
        dest.writeBoolean(mNat64Prefix != null);
        if (mNat64Prefix != null) mNat64Prefix.writeToParcel(dest, flags);
        dest.writeByteArray(mDhcpServerAddress == null ? null : mDhcpServerAddress.getAddress());
        dest.writeBoolean(mWakeOnLanSupported);
    }

    public static final Creator<LinkProperties> CREATOR = new Creator<LinkProperties>() {
        @Override
        public LinkProperties createFromParcel(Parcel in) {
            LinkProperties lp = new LinkProperties();
            lp.mIfaceName = in.readString();
            List<LinkAddress> addresses = in.createTypedArrayList(LinkAddress.CREATOR);
            if (addresses != null) lp.mLinkAddresses.addAll(addresses);
            readAddresses(in, lp.mDnses);
            readAddresses(in, lp.mValidatedPrivateDnses);
            lp.mUsePrivateDns = in.readBoolean();
            lp.mPrivateDnsServerName = in.readString();
            lp.mDomains = in.readString();
            List<RouteInfo> routes = in.createTypedArrayList(RouteInfo.CREATOR);
            if (routes != null) lp.mRoutes.addAll(routes);
            lp.mMtu = in.readInt();
            if (in.readBoolean()) lp.mHttpProxy = ProxyInfo.CREATOR.createFromParcel(in);
            if (in.readBoolean()) lp.mNat64Prefix = IpPrefix.CREATOR.createFromParcel(in);
            byte[] dhcpAddress = in.createByteArray();
            if (dhcpAddress != null) {
                try {
                    lp.mDhcpServerAddress = (Inet4Address) InetAddress.getByAddress(dhcpAddress);
                } catch (UnknownHostException e) {
                    throw new IllegalArgumentException(e);
                }
            }
            lp.mWakeOnLanSupported = in.readBoolean();
            return lp;
        }

        @Override
        public LinkProperties[] newArray(int size) {
            return new LinkProperties[size];
        }
    };
}
