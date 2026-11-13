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

/**
 * The Connectivity module's LinkAddress (framework.jar compiles against the module's API
 * and does not contain it), with its public API: an address of an interface and its prefix
 * length (docs/network.md).
 */
public class LinkAddress implements Parcelable {
    /** @hide */
    public static final long LIFETIME_UNKNOWN = -1;
    /** @hide */
    public static final long LIFETIME_PERMANENT = Long.MAX_VALUE;

    private static final int IFA_F_OPTIMISTIC = 0x04;
    private static final int IFA_F_DADFAILED = 0x08;
    private static final int IFA_F_DEPRECATED = 0x20;
    private static final int IFA_F_TENTATIVE = 0x40;
    private static final int RT_SCOPE_UNIVERSE = 0;
    private static final int RT_SCOPE_SITE = 200;
    private static final int RT_SCOPE_LINK = 253;
    private static final int RT_SCOPE_HOST = 254;

    private final InetAddress mAddress;
    private final int mPrefixLength;
    private final int mFlags;
    private final int mScope;

    /** @hide */
    public LinkAddress(InetAddress address, int prefixLength, int flags, int scope) {
        if (address == null || address.isMulticastAddress() || prefixLength < 0
                || prefixLength > (address instanceof Inet4Address ? 32 : 128)) {
            throw new IllegalArgumentException("Bad LinkAddress " + address + "/" + prefixLength);
        }
        mAddress = address;
        mPrefixLength = prefixLength;
        mFlags = flags;
        mScope = scope;
    }

    /** @hide */
    public LinkAddress(InetAddress address, int prefixLength) {
        // Let the common constructor reject null with Android's IllegalArgumentException.
        this(address, prefixLength, 0,
                address == null ? RT_SCOPE_UNIVERSE : scopeForAddress(address));
    }

    /** @hide */
    public LinkAddress(String address) {
        this(InetAddresses.parseNumericAddress(address.substring(0, address.indexOf('/'))),
                Integer.parseInt(address.substring(address.indexOf('/') + 1)));
    }

    private static int scopeForAddress(InetAddress address) {
        if (address.isAnyLocalAddress()) return RT_SCOPE_HOST;
        if (address.isLoopbackAddress() || address.isLinkLocalAddress()) return RT_SCOPE_LINK;
        // Private IPv4 addresses have global scope; IPv6 site-local addresses do not.
        if (!(address instanceof Inet4Address) && address.isSiteLocalAddress()) return RT_SCOPE_SITE;
        return RT_SCOPE_UNIVERSE;
    }

    public InetAddress getAddress() {
        return mAddress;
    }

    public int getPrefixLength() {
        return mPrefixLength;
    }

    /** @hide */
    public int getNetworkPrefixLength() {
        return mPrefixLength;
    }

    public int getFlags() {
        return mFlags;
    }

    public int getScope() {
        return mScope;
    }

    public long getDeprecationTime() {
        return LIFETIME_UNKNOWN;
    }

    public long getExpirationTime() {
        return LIFETIME_UNKNOWN;
    }

    /** @hide */
    public boolean isIpv4() {
        return mAddress instanceof Inet4Address;
    }

    /** @hide */
    public boolean isIpv6() {
        return mAddress instanceof Inet6Address;
    }

    /** @hide */
    public boolean isGlobalPreferred() {
        boolean ipv6Ula = mAddress instanceof Inet6Address
                && (mAddress.getAddress()[0] & 0xfe) == 0xfc;
        return mScope == RT_SCOPE_UNIVERSE && !ipv6Ula && isPreferred();
    }

    /** @hide */
    public boolean isPreferred() {
        int flags = getFlags();
        return (flags & (IFA_F_DADFAILED | IFA_F_DEPRECATED)) == 0
                && ((flags & IFA_F_TENTATIVE) == 0 || (flags & IFA_F_OPTIMISTIC) != 0);
    }

    @Override
    public String toString() {
        return mAddress.getHostAddress() + "/" + mPrefixLength;
    }

    @Override
    public boolean equals(Object o) {
        if (!(o instanceof LinkAddress)) return false;
        LinkAddress a = (LinkAddress) o;
        return mAddress.equals(a.mAddress) && mPrefixLength == a.mPrefixLength
                && mFlags == a.mFlags && mScope == a.mScope;
    }

    @Override
    public int hashCode() {
        return mAddress.hashCode() + 11 * mPrefixLength + 19 * mFlags + 43 * mScope;
    }

    /** @hide */
    public boolean isSameAddressAs(LinkAddress other) {
        return other != null && mAddress.equals(other.mAddress)
                && mPrefixLength == other.mPrefixLength;
    }

    @Override
    public int describeContents() {
        return 0;
    }

    @Override
    public void writeToParcel(Parcel dest, int flags) {
        dest.writeByteArray(mAddress.getAddress());
        dest.writeInt(mPrefixLength);
        dest.writeInt(mFlags);
        dest.writeInt(mScope);
    }

    public static final Creator<LinkAddress> CREATOR = new Creator<LinkAddress>() {
        @Override
        public LinkAddress createFromParcel(Parcel in) {
            try {
                InetAddress address = InetAddress.getByAddress(in.createByteArray());
                return new LinkAddress(address, in.readInt(), in.readInt(), in.readInt());
            } catch (UnknownHostException e) {
                throw new IllegalArgumentException(e);
            }
        }

        @Override
        public LinkAddress[] newArray(int size) {
            return new LinkAddress[size];
        }
    };
}
