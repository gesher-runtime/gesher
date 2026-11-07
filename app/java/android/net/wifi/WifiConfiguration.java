/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package android.net.wifi;

import android.os.Parcel;
import android.os.Parcelable;
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
    public WifiConfiguration() { }
    public void setSecurityParams(int securityType) { }
    @Override public int describeContents() { return 0; }
    @Override public void writeToParcel(Parcel out, int flags) {
        out.writeInt(networkId);
        out.writeString(SSID);
        out.writeString(BSSID);
        out.writeString(preSharedKey);
        out.writeInt(hiddenSSID ? 1 : 0);
    }
    public static final Creator<WifiConfiguration> CREATOR = new Creator<>() {
        @Override public WifiConfiguration createFromParcel(Parcel in) {
            WifiConfiguration wc = new WifiConfiguration();
            wc.networkId = in.readInt();
            wc.SSID = in.readString();
            wc.BSSID = in.readString();
            wc.preSharedKey = in.readString();
            wc.hiddenSSID = in.readInt() != 0;
            return wc;
        }
        @Override public WifiConfiguration[] newArray(int size) { return new WifiConfiguration[size]; }
    };
}
