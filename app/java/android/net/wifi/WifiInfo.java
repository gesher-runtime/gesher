/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package android.net.wifi;

import android.net.TransportInfo;
import android.os.Parcel;
import android.os.Parcelable;

/** Disconnected API 36 snapshot. No Windows network identifiers are exposed. */
public class WifiInfo implements TransportInfo, Parcelable {
    public static final int INVALID_RSSI = -127;
    public static final int LINK_SPEED_UNKNOWN = -1;
    public static final String DEFAULT_MAC_ADDRESS = "02:00:00:00:00:00";
    public WifiInfo() { }
    public String getSSID() { return WifiManager.UNKNOWN_SSID; }
    public String getBSSID() { return null; }
    public String getMacAddress() { return DEFAULT_MAC_ADDRESS; }
    public int getRssi() { return INVALID_RSSI; }
    public int getLinkSpeed() { return LINK_SPEED_UNKNOWN; }
    public int getNetworkId() { return -1; }
    @Override public long getApplicableRedactions() { return 0; }
    @Override public WifiInfo makeCopy(long redactions) { return new WifiInfo(); }
    @Override public int describeContents() { return 0; }
    @Override public void writeToParcel(Parcel out, int flags) {
        out.writeString(getSSID());
        out.writeString(getBSSID());
        out.writeString(getMacAddress());
        out.writeInt(getRssi());
        out.writeInt(getLinkSpeed());
        out.writeInt(getNetworkId());
    }
    public static final Creator<WifiInfo> CREATOR = new Creator<>() {
        @Override public WifiInfo createFromParcel(Parcel in) {
            in.readString();
            in.readString();
            in.readString();
            in.readInt();
            in.readInt();
            in.readInt();
            return new WifiInfo();
        }
        @Override public WifiInfo[] newArray(int size) { return new WifiInfo[size]; }
    };
}
