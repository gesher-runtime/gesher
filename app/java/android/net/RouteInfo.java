/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package android.net;

import android.os.Parcel;
import android.os.Parcelable;

import java.net.Inet4Address;
import java.net.InetAddress;
import java.net.UnknownHostException;
import java.util.Objects;

/**
 * The Connectivity module's RouteInfo (framework.jar uses it and does not contain it), with
 * its public API: a destination prefix, a gateway and an interface (docs/network.md).
 */
public final class RouteInfo implements Parcelable {
    public static final int RTN_UNICAST = 1;
    public static final int RTN_UNREACHABLE = 7;
    public static final int RTN_THROW = 9;

    private final IpPrefix mDestination;
    private final InetAddress mGateway;
    private final String mInterface;
    private final int mType;

    /** @hide */
    public RouteInfo(IpPrefix destination, InetAddress gateway, String iface, int type) {
        if (destination == null) {
            boolean v4 = gateway == null || gateway instanceof Inet4Address;
            destination = new IpPrefix(new byte[v4 ? 4 : 16], 0);
        }
        if (gateway == null) {
            try {
                gateway = InetAddress.getByAddress(
                        new byte[destination.getRawAddress().length]);
            } catch (UnknownHostException e) {
                throw new IllegalStateException(e);
            }
        }
        mDestination = destination;
        mGateway = gateway;
        mInterface = iface;
        mType = type;
    }

    /** @hide */
    public RouteInfo(IpPrefix destination, InetAddress gateway, String iface) {
        this(destination, gateway, iface, RTN_UNICAST);
    }

    public IpPrefix getDestination() {
        return mDestination;
    }

    public InetAddress getGateway() {
        return mGateway;
    }

    public String getInterface() {
        return mInterface;
    }

    public int getType() {
        return mType;
    }

    public int getMtu() {
        return 0;
    }

    public boolean isDefaultRoute() {
        return mType == RTN_UNICAST && mDestination.getPrefixLength() == 0;
    }

    /** @hide */
    public boolean isIPv4Default() {
        return isDefaultRoute() && mDestination.isIPv4();
    }

    /** @hide */
    public boolean isIPv6Default() {
        return isDefaultRoute() && mDestination.isIPv6();
    }

    public boolean hasGateway() {
        return !mGateway.isAnyLocalAddress();
    }

    public boolean matches(InetAddress destination) {
        return mDestination.contains(destination);
    }

    @Override
    public String toString() {
        String s = mDestination + " ->";
        if (hasGateway()) s += " " + mGateway.getHostAddress();
        if (mInterface != null) s += " " + mInterface;
        if (mType != RTN_UNICAST) s += mType == RTN_UNREACHABLE ? " unreachable" : " throw";
        return s;
    }

    @Override
    public boolean equals(Object o) {
        if (!(o instanceof RouteInfo)) return false;
        RouteInfo r = (RouteInfo) o;
        return mDestination.equals(r.mDestination) && mGateway.equals(r.mGateway)
                && Objects.equals(mInterface, r.mInterface) && mType == r.mType;
    }

    @Override
    public int hashCode() {
        return Objects.hash(mDestination, mGateway, mInterface, mType);
    }

    @Override
    public int describeContents() {
        return 0;
    }

    @Override
    public void writeToParcel(Parcel dest, int flags) {
        mDestination.writeToParcel(dest, flags);
        dest.writeByteArray(mGateway.getAddress());
        dest.writeString(mInterface);
        dest.writeInt(mType);
    }

    public static final Creator<RouteInfo> CREATOR = new Creator<RouteInfo>() {
        @Override
        public RouteInfo createFromParcel(Parcel in) {
            IpPrefix destination = IpPrefix.CREATOR.createFromParcel(in);
            try {
                InetAddress gateway = InetAddress.getByAddress(in.createByteArray());
                return new RouteInfo(destination, gateway, in.readString(), in.readInt());
            } catch (UnknownHostException e) {
                throw new IllegalArgumentException(e);
            }
        }

        @Override
        public RouteInfo[] newArray(int size) {
            return new RouteInfo[size];
        }
    };
}
