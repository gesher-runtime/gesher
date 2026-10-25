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
import java.util.Arrays;

/**
 * The Connectivity module's IpPrefix (framework.jar compiles against the module's API and
 * does not contain it), with its public API: an address and a prefix length, the bits past
 * the prefix cleared (docs/network.md).
 */
public final class IpPrefix implements Parcelable {
    private final byte[] mAddress;
    private final int mPrefixLength;

    public IpPrefix(InetAddress address, int prefixLength) {
        this(address.getAddress(), prefixLength);
    }

    /** @hide */
    public IpPrefix(byte[] address, int prefixLength) {
        if (address.length != 4 && address.length != 16) {
            throw new IllegalArgumentException("IpPrefix has " + address.length + " bytes");
        }
        if (prefixLength < 0 || prefixLength > address.length * 8) {
            throw new IllegalArgumentException("IpPrefix has length " + prefixLength);
        }
        mAddress = address.clone();
        mPrefixLength = prefixLength;
        for (int i = 0; i < mAddress.length; i++) {
            int keep = Math.max(0, Math.min(8, prefixLength - i * 8));
            mAddress[i] &= (byte) (0xff << (8 - keep));
        }
    }

    /** "address/length", as IpPrefix(String) takes it. */
    public IpPrefix(String prefix) {
        this(parseAddress(prefix), parseLength(prefix));
    }

    private static byte[] parseAddress(String prefix) {
        int slash = prefix.indexOf('/');
        if (slash < 0) throw new IllegalArgumentException("no prefix length in " + prefix);
        return InetAddresses.parseNumericAddress(prefix.substring(0, slash)).getAddress();
    }

    private static int parseLength(String prefix) {
        return Integer.parseInt(prefix.substring(prefix.indexOf('/') + 1).trim());
    }

    public InetAddress getAddress() {
        try {
            return InetAddress.getByAddress(mAddress);
        } catch (UnknownHostException e) {
            throw new IllegalStateException(e);
        }
    }

    public byte[] getRawAddress() {
        return mAddress.clone();
    }

    public int getPrefixLength() {
        return mPrefixLength;
    }

    public boolean contains(InetAddress address) {
        byte[] a = address.getAddress();
        if (a.length != mAddress.length) return false;
        for (int i = 0; i < a.length; i++) {
            int keep = Math.max(0, Math.min(8, mPrefixLength - i * 8));
            if ((a[i] & (0xff << (8 - keep)) & 0xff) != (mAddress[i] & 0xff)) return false;
        }
        return true;
    }

    /** @hide */
    public boolean isIPv4() {
        return getAddress() instanceof Inet4Address;
    }

    /** @hide */
    public boolean isIPv6() {
        return !isIPv4();
    }

    @Override
    public boolean equals(Object o) {
        if (!(o instanceof IpPrefix)) return false;
        IpPrefix p = (IpPrefix) o;
        return Arrays.equals(mAddress, p.mAddress) && mPrefixLength == p.mPrefixLength;
    }

    @Override
    public int hashCode() {
        return Arrays.hashCode(mAddress) + 11 * mPrefixLength;
    }

    @Override
    public String toString() {
        return getAddress().getHostAddress() + "/" + mPrefixLength;
    }

    @Override
    public int describeContents() {
        return 0;
    }

    @Override
    public void writeToParcel(Parcel dest, int flags) {
        dest.writeByteArray(mAddress);
        dest.writeInt(mPrefixLength);
    }

    public static final Creator<IpPrefix> CREATOR = new Creator<IpPrefix>() {
        @Override
        public IpPrefix createFromParcel(Parcel in) {
            byte[] address = in.createByteArray();
            return new IpPrefix(address, in.readInt());
        }

        @Override
        public IpPrefix[] newArray(int size) {
            return new IpPrefix[size];
        }
    };
}
