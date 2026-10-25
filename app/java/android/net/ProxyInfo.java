/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package android.net;

import android.os.Parcel;
import android.os.Parcelable;

import java.net.InetSocketAddress;
import java.util.Arrays;
import java.util.List;
import java.util.Locale;
import java.util.Objects;

/**
 * The Connectivity module's ProxyInfo (framework.jar's android.net.Proxy uses it and
 * framework.jar does not contain it), with its public API: an HTTP proxy, direct or by PAC
 * file. Gesher's networks report none (docs/network.md).
 */
public class ProxyInfo implements Parcelable {
    private final String mHost;
    private final int mPort;
    private final String mExclusionList;
    private final String[] mParsedExclusionList;
    private final Uri mPacFileUrl;

    public static ProxyInfo buildDirectProxy(String host, int port) {
        return new ProxyInfo(host, port, null);
    }

    public static ProxyInfo buildDirectProxy(String host, int port, List<String> exclList) {
        return new ProxyInfo(host, port,
                exclList == null ? null : String.join(",", exclList));
    }

    public static ProxyInfo buildPacProxy(Uri pacUri) {
        return new ProxyInfo(pacUri, -1);
    }

    public static ProxyInfo buildPacProxy(Uri pacUrl, int port) {
        return new ProxyInfo(pacUrl, port);
    }

    /** @hide */
    public ProxyInfo(String host, int port, String exclList) {
        mHost = host;
        mPort = port;
        mExclusionList = exclList == null ? "" : exclList.toLowerCase(Locale.ROOT);
        mParsedExclusionList = mExclusionList.isEmpty() ? new String[0] : mExclusionList.split(",");
        mPacFileUrl = Uri.EMPTY;
    }

    /** @hide */
    public ProxyInfo(Uri pacFileUrl, int localProxyPort) {
        mHost = "localhost";
        mPort = localProxyPort;
        mExclusionList = "";
        mParsedExclusionList = new String[0];
        mPacFileUrl = pacFileUrl == null ? Uri.EMPTY : pacFileUrl;
    }

    public ProxyInfo(ProxyInfo source) {
        mHost = source.mHost;
        mPort = source.mPort;
        mExclusionList = source.mExclusionList;
        mParsedExclusionList = source.mParsedExclusionList.clone();
        mPacFileUrl = source.mPacFileUrl;
    }

    /** @hide */
    public InetSocketAddress getSocketAddress() {
        try {
            return new InetSocketAddress(mHost, mPort);
        } catch (IllegalArgumentException e) {
            return null;
        }
    }

    public Uri getPacFileUrl() {
        return mPacFileUrl;
    }

    public String getHost() {
        return mHost;
    }

    public int getPort() {
        return mPort;
    }

    public String[] getExclusionList() {
        return mParsedExclusionList.clone();
    }

    /** @hide */
    public String getExclusionListAsString() {
        return mExclusionList;
    }

    public boolean isValid() {
        if (!Uri.EMPTY.equals(mPacFileUrl)) return true;
        return mHost != null && !mHost.isEmpty() && mPort > 0 && mPort <= 65535;
    }

    /** @hide */
    public java.net.Proxy makeProxy() {
        if (mHost == null || !isValid()) return java.net.Proxy.NO_PROXY;
        return new java.net.Proxy(java.net.Proxy.Type.HTTP, new InetSocketAddress(mHost, mPort));
    }

    @Override
    public String toString() {
        if (!Uri.EMPTY.equals(mPacFileUrl)) return "PAC Script: " + mPacFileUrl;
        return "[" + mHost + "] " + mPort + (mExclusionList.isEmpty() ? "" : " xl=" + mExclusionList);
    }

    @Override
    public boolean equals(Object o) {
        if (!(o instanceof ProxyInfo)) return false;
        ProxyInfo p = (ProxyInfo) o;
        return Objects.equals(mHost, p.mHost) && mPort == p.mPort
                && mExclusionList.equals(p.mExclusionList) && mPacFileUrl.equals(p.mPacFileUrl);
    }

    @Override
    public int hashCode() {
        return Objects.hash(mHost, mPort, mExclusionList, mPacFileUrl)
                + Arrays.hashCode(mParsedExclusionList);
    }

    @Override
    public int describeContents() {
        return 0;
    }

    @Override
    public void writeToParcel(Parcel dest, int flags) {
        boolean pac = !Uri.EMPTY.equals(mPacFileUrl);
        dest.writeBoolean(pac);
        if (pac) {
            dest.writeString(mPacFileUrl.toString());
            dest.writeInt(mPort);
        } else {
            dest.writeString(mHost);
            dest.writeInt(mPort);
            dest.writeString(mExclusionList);
        }
    }

    public static final Creator<ProxyInfo> CREATOR = new Creator<ProxyInfo>() {
        @Override
        public ProxyInfo createFromParcel(Parcel in) {
            if (in.readBoolean()) {
                return new ProxyInfo(Uri.parse(in.readString()), in.readInt());
            }
            return new ProxyInfo(in.readString(), in.readInt(), in.readString());
        }

        @Override
        public ProxyInfo[] newArray(int size) {
            return new ProxyInfo[size];
        }
    };
}
