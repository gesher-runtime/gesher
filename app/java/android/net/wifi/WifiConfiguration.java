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
    public void setSecurityParams(int securityType) {
        throw new UnsupportedOperationException("Android Wi-Fi network configuration is unavailable on Gesher");
    }
    @Override public int describeContents() { return 0; }
    @Override public void writeToParcel(Parcel out, int flags) { throw unsupportedParcel(); }
    public static final Creator<WifiConfiguration> CREATOR = new Creator<>() {
        @Override public WifiConfiguration createFromParcel(Parcel in) { throw unsupportedParcel(); }
        @Override public WifiConfiguration[] newArray(int size) { return new WifiConfiguration[size]; }
    };
    private static UnsupportedOperationException unsupportedParcel() {
        return new UnsupportedOperationException("Wi-Fi parcel transport is unavailable on Gesher");
    }
}
