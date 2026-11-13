/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package android.net.wifi;

import android.net.TransportInfo;
import android.net.NetworkInfo.DetailedState;
import android.os.Parcel;
import android.os.Parcelable;
import java.util.EnumMap;

/** Disconnected API 36 snapshot. No Windows network identifiers are exposed. */
public class WifiInfo implements TransportInfo, Parcelable {
    public static final int INVALID_RSSI = -127;
    public static final int LINK_SPEED_UNKNOWN = -1;
    public static final String DEFAULT_MAC_ADDRESS = "02:00:00:00:00:00";
    public static final String LINK_SPEED_UNITS = "Mbps";
    public static final String FREQUENCY_UNITS = "MHz";
    public static final int SECURITY_TYPE_UNKNOWN = -1;
    public static final int SECURITY_TYPE_OPEN = 0;
    public static final int SECURITY_TYPE_WEP = 1;
    public static final int SECURITY_TYPE_PSK = 2;
    public static final int SECURITY_TYPE_EAP = 3;
    public static final int SECURITY_TYPE_SAE = 4;
    public static final int SECURITY_TYPE_EAP_WPA3_ENTERPRISE_192_BIT = 5;
    public static final int SECURITY_TYPE_OWE = 6;
    public static final int SECURITY_TYPE_WAPI_PSK = 7;
    public static final int SECURITY_TYPE_WAPI_CERT = 8;
    public static final int SECURITY_TYPE_EAP_WPA3_ENTERPRISE = 9;
    public static final int SECURITY_TYPE_OSEN = 10;
    public static final int SECURITY_TYPE_PASSPOINT_R1_R2 = 11;
    public static final int SECURITY_TYPE_PASSPOINT_R3 = 12;
    public static final int SECURITY_TYPE_DPP = 13;

    private static final EnumMap<SupplicantState, DetailedState> STATE_MAP =
            new EnumMap<>(SupplicantState.class);
    static {
        STATE_MAP.put(SupplicantState.DISCONNECTED, DetailedState.DISCONNECTED);
        STATE_MAP.put(SupplicantState.INTERFACE_DISABLED, DetailedState.DISCONNECTED);
        STATE_MAP.put(SupplicantState.INACTIVE, DetailedState.IDLE);
        STATE_MAP.put(SupplicantState.SCANNING, DetailedState.SCANNING);
        STATE_MAP.put(SupplicantState.AUTHENTICATING, DetailedState.CONNECTING);
        STATE_MAP.put(SupplicantState.ASSOCIATING, DetailedState.CONNECTING);
        STATE_MAP.put(SupplicantState.ASSOCIATED, DetailedState.CONNECTING);
        STATE_MAP.put(SupplicantState.FOUR_WAY_HANDSHAKE, DetailedState.AUTHENTICATING);
        STATE_MAP.put(SupplicantState.GROUP_HANDSHAKE, DetailedState.AUTHENTICATING);
        STATE_MAP.put(SupplicantState.COMPLETED, DetailedState.OBTAINING_IPADDR);
        STATE_MAP.put(SupplicantState.DORMANT, DetailedState.DISCONNECTED);
        STATE_MAP.put(SupplicantState.UNINITIALIZED, DetailedState.IDLE);
        STATE_MAP.put(SupplicantState.INVALID, DetailedState.FAILED);
    }

    public WifiInfo() { }
    public String getSSID() { return WifiManager.UNKNOWN_SSID; }
    public String getBSSID() { return null; }
    public String getMacAddress() { return DEFAULT_MAC_ADDRESS; }
    public int getRssi() { return INVALID_RSSI; }
    public int getLinkSpeed() { return LINK_SPEED_UNKNOWN; }
    public int getNetworkId() { return -1; }
    public SupplicantState getSupplicantState() { return SupplicantState.UNINITIALIZED; }
    public static DetailedState getDetailedStateOf(SupplicantState state) {
        return STATE_MAP.get(state);
    }
    public int getFrequency() { return -1; }
    public boolean getHiddenSSID() { return false; }
    @Deprecated public int getIpAddress() { return 0; }
    public int getCurrentSecurityType() { return SECURITY_TYPE_UNKNOWN; }
    public int getWifiStandard() { return 0; }
    public int getSubscriptionId() { return -1; }
    // Android 16's new WifiInfo leaves these four fields at their Java default.
    // reset() is a separate upstream operation and is not called by this snapshot.
    public int getTxLinkSpeedMbps() { return 0; }
    public int getRxLinkSpeedMbps() { return 0; }
    public int getMaxSupportedTxLinkSpeedMbps() { return 0; }
    public int getMaxSupportedRxLinkSpeedMbps() { return 0; }
    public String getPasspointFqdn() { return null; }
    public String getPasspointProviderFriendlyName() { return null; }
    public String getPasspointUniqueId() { return null; }
    public boolean isRestricted() { return false; }
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
