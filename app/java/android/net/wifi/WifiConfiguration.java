/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package android.net.wifi;

import android.net.IpConfiguration;
import android.net.ProxyInfo;
import android.net.Uri;
import android.os.Parcel;
import android.os.Parcelable;
import android.util.Log;
import java.util.Arrays;
import java.util.BitSet;

/** API 36 configuration fields used by clients; installing a network is unsupported. */
public class WifiConfiguration implements Parcelable {
    public static final int INVALID_NETWORK_ID = -1;
    public int networkId = INVALID_NETWORK_ID;
    public String SSID;
    public String BSSID;
    public String preSharedKey;
    public boolean hiddenSSID;
    public String[] wepKeys = new String[4];
    public int wepTxKeyIndex;
    public BitSet allowedKeyManagement = new BitSet();
    public BitSet allowedProtocols = new BitSet();
    public BitSet allowedAuthAlgorithms = new BitSet();
    public BitSet allowedPairwiseCiphers = new BitSet();
    public BitSet allowedGroupCiphers = new BitSet();
    private IpConfiguration mIpConfiguration = new IpConfiguration();
    public WifiConfiguration() { }

    /** @hide */
    public IpConfiguration getIpConfiguration() {
        return new IpConfiguration(mIpConfiguration);
    }

    public void setIpConfiguration(IpConfiguration configuration) {
        mIpConfiguration = configuration == null ? new IpConfiguration() : configuration;
    }

    public ProxyInfo getHttpProxy() {
        if (mIpConfiguration.getProxySettings() == IpConfiguration.ProxySettings.NONE) return null;
        return new ProxyInfo(mIpConfiguration.getHttpProxy());
    }

    public void setHttpProxy(ProxyInfo proxy) {
        if (proxy == null) {
            mIpConfiguration.setProxySettings(IpConfiguration.ProxySettings.NONE);
            mIpConfiguration.setHttpProxy(null);
            return;
        }
        ProxyInfo copy;
        IpConfiguration.ProxySettings settings;
        if (!Uri.EMPTY.equals(proxy.getPacFileUrl())) {
            settings = IpConfiguration.ProxySettings.PAC;
            copy = ProxyInfo.buildPacProxy(proxy.getPacFileUrl(), proxy.getPort());
        } else {
            settings = IpConfiguration.ProxySettings.STATIC;
            String[] exclusions = proxy.getExclusionList();
            copy = ProxyInfo.buildDirectProxy(proxy.getHost(), proxy.getPort(),
                    Arrays.asList(exclusions == null ? new String[0] : exclusions));
        }
        // Android 16 retains an invalid value after warning; it does not reject it here.
        if (!copy.isValid()) Log.w("WifiConfiguration", "ProxyInfo is not valid: " + copy);
        mIpConfiguration.setProxySettings(settings);
        mIpConfiguration.setHttpProxy(copy);
    }

    /** Data-object defaults for the supported open/PSK shapes; no radio is configured. */
    public void setSecurityParams(int securityType) {
        if (securityType != 0 && securityType != 2) throw new IllegalArgumentException("Unsupported Wi-Fi security type");
        allowedKeyManagement = new BitSet(); allowedProtocols = new BitSet();
        allowedAuthAlgorithms = new BitSet(); allowedPairwiseCiphers = new BitSet(); allowedGroupCiphers = new BitSet();
        allowedKeyManagement.set(securityType == 0 ? 0 : 1);
        if (securityType == 2) { allowedProtocols.set(0); allowedProtocols.set(1); }
    }
    @Override public int describeContents() { return 0; }
    @Override public void writeToParcel(Parcel out, int flags) {
        out.writeInt(networkId);
        out.writeString(SSID);
        out.writeString(BSSID);
        out.writeString(preSharedKey);
        out.writeInt(hiddenSSID ? 1 : 0);
        out.writeStringArray(wepKeys); out.writeInt(wepTxKeyIndex);
        writeBits(out, allowedKeyManagement); writeBits(out, allowedProtocols);
        writeBits(out, allowedAuthAlgorithms); writeBits(out, allowedPairwiseCiphers); writeBits(out, allowedGroupCiphers);
        mIpConfiguration.writeToParcel(out, flags);
    }
    private static void writeBits(Parcel out, BitSet bits) { out.writeByteArray(bits == null ? null : bits.toByteArray()); }
    private static BitSet readBits(Parcel in) { byte[] bytes = in.createByteArray(); return bytes == null ? null : BitSet.valueOf(bytes); }
    public static final Creator<WifiConfiguration> CREATOR = new Creator<>() {
        @Override public WifiConfiguration createFromParcel(Parcel in) {
            WifiConfiguration wc = new WifiConfiguration();
            wc.networkId = in.readInt();
            wc.SSID = in.readString();
            wc.BSSID = in.readString();
            wc.preSharedKey = in.readString();
            wc.hiddenSSID = in.readInt() != 0;
            wc.wepKeys = in.createStringArray(); wc.wepTxKeyIndex = in.readInt();
            wc.allowedKeyManagement = readBits(in); wc.allowedProtocols = readBits(in);
            wc.allowedAuthAlgorithms = readBits(in); wc.allowedPairwiseCiphers = readBits(in); wc.allowedGroupCiphers = readBits(in);
            wc.mIpConfiguration = IpConfiguration.CREATOR.createFromParcel(in);
            return wc;
        }
        @Override public WifiConfiguration[] newArray(int size) { return new WifiConfiguration[size]; }
    };
}
