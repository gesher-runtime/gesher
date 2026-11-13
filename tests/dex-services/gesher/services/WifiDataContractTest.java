/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package gesher.services;

import android.net.NetworkInfo.DetailedState;
import android.net.wifi.SupplicantState;
import android.net.wifi.WifiInfo;
import android.net.wifi.WifiSsid;
import android.os.Parcel;
import android.os.Parcelable;
import java.nio.charset.StandardCharsets;
import java.util.Arrays;

/** SDK-compiled data-only callers. No Wi-Fi radio or synthetic connection is required. */
public final class WifiDataContractTest {
    private static int checks;
    private static void check(boolean value, String message) {
        if (!value) throw new AssertionError(message);
        checks++;
    }

    private static void ssids() {
        byte[][] inputs = {null, new byte[0], "Gesher".getBytes(StandardCharsets.UTF_8),
                "גשר".getBytes(StandardCharsets.UTF_8), new byte[]{(byte)0xc3, 0x28},
                new byte[]{(byte)0xff, (byte)0x80}, new byte[32]};
        String[] display = {"", "", "\"Gesher\"", "\"גשר\"", "c328", "ff80",
                "\"" + new String(new char[32]) + "\""};
        for (int i = 0; i < inputs.length; ++i) {
            byte[] expected = inputs[i] == null ? new byte[0] : inputs[i];
            WifiSsid value = WifiSsid.fromBytes(inputs[i]);
            check(Arrays.equals(expected, value.getBytes()), "SSID retains raw bytes " + i);
            check(display[i].equals(value.toString()), "SSID UTF-8/hex display " + i);
            WifiSsid equal = WifiSsid.fromBytes(expected.clone());
            check(value.equals(equal) && equal.equals(value), "SSID content equality " + i);
            check(value.hashCode() == equal.hashCode(), "SSID content hash " + i);
            check(!value.equals(null) && !value.equals("other"), "SSID equality types " + i);
            byte[] copy = value.getBytes();
            if (copy.length != 0) copy[0] ^= 1;
            check(Arrays.equals(expected, value.getBytes()), "SSID getter copy " + i);
            check(value.describeContents() == 0, "SSID contains no FDs " + i);
        }
        check(!WifiSsid.fromBytes(new byte[]{1}).equals(WifiSsid.fromBytes(new byte[]{2})),
                "Different SSID bytes remain distinct");
        check(WifiSsid.CREATOR.newArray(4).length == 4, "SSID creator array");
    }

    private static void supplicant() {
        SupplicantState[] states = {SupplicantState.DISCONNECTED, SupplicantState.INTERFACE_DISABLED,
                SupplicantState.INACTIVE, SupplicantState.SCANNING, SupplicantState.AUTHENTICATING,
                SupplicantState.ASSOCIATING, SupplicantState.ASSOCIATED,
                SupplicantState.FOUR_WAY_HANDSHAKE, SupplicantState.GROUP_HANDSHAKE,
                SupplicantState.COMPLETED, SupplicantState.DORMANT, SupplicantState.UNINITIALIZED,
                SupplicantState.INVALID};
        DetailedState[] detailed = {DetailedState.DISCONNECTED, DetailedState.DISCONNECTED,
                DetailedState.IDLE, DetailedState.SCANNING, DetailedState.CONNECTING,
                DetailedState.CONNECTING, DetailedState.CONNECTING, DetailedState.AUTHENTICATING,
                DetailedState.AUTHENTICATING, DetailedState.OBTAINING_IPADDR,
                DetailedState.DISCONNECTED, DetailedState.IDLE, DetailedState.FAILED};
        check(Arrays.equals(states, SupplicantState.values()), "Supplicant enum order");
        for (int i = 0; i < states.length; i++) {
            check(SupplicantState.valueOf(states[i].name()) == states[i], "State enum round trip " + i);
            check(SupplicantState.isValidState(states[i]) == (i < 11), "State validity " + i);
            check(WifiInfo.getDetailedStateOf(states[i]) == detailed[i], "State mapping " + i);
            check(states[i].describeContents() == 0, "State contains no FDs " + i);
        }
        check(WifiInfo.getDetailedStateOf(null) == null, "Null state mapping follows EnumMap");
        check(SupplicantState.isValidState(null), "Pinned validity excludes only two enum values");
    }

    private static void snapshot() throws Exception {
        // Android hides the default constructor from the SDK; Gesher's manager uses it.
        WifiInfo info = WifiInfo.class.getDeclaredConstructor().newInstance();
        check(info.getSupplicantState() == SupplicantState.UNINITIALIZED, "Uninitialized supplicant");
        check(info.getFrequency() == -1 && info.getLinkSpeed() == -1, "Unknown connection metrics");
        check(info.getRssi() == -127 && info.getNetworkId() == -1, "Unknown RSSI and network");
        check(info.getCurrentSecurityType() == WifiInfo.SECURITY_TYPE_UNKNOWN, "Unknown security");
        check(info.getWifiStandard() == 0 && info.getSubscriptionId() == -1, "Default standard/subscription");
        check(!info.getHiddenSSID() && !info.isRestricted() && info.getIpAddress() == 0,
                "No hidden, restricted or IP connection");
        check(info.getTxLinkSpeedMbps() == 0 && info.getRxLinkSpeedMbps() == 0,
                "New Android 16 snapshot's unset directional rates");
        check(info.getMaxSupportedTxLinkSpeedMbps() == 0 && info.getMaxSupportedRxLinkSpeedMbps() == 0,
                "New Android 16 snapshot's unset maximum rates");
        check(info.getPasspointFqdn() == null && info.getPasspointProviderFriendlyName() == null
                && info.getPasspointUniqueId() == null, "No Passpoint connection");
        check("<unknown ssid>".equals(info.getSSID()) && info.getBSSID() == null,
                "Disconnected identity retained");
        String[] constants = {"SECURITY_TYPE_UNKNOWN", "SECURITY_TYPE_OPEN", "SECURITY_TYPE_WEP",
                "SECURITY_TYPE_PSK", "SECURITY_TYPE_EAP", "SECURITY_TYPE_SAE",
                "SECURITY_TYPE_EAP_WPA3_ENTERPRISE_192_BIT", "SECURITY_TYPE_OWE",
                "SECURITY_TYPE_WAPI_PSK", "SECURITY_TYPE_WAPI_CERT",
                "SECURITY_TYPE_EAP_WPA3_ENTERPRISE", "SECURITY_TYPE_OSEN",
                "SECURITY_TYPE_PASSPOINT_R1_R2", "SECURITY_TYPE_PASSPOINT_R3", "SECURITY_TYPE_DPP"};
        for (int i = 0; i < constants.length; i++) {
            // Reflection verifies shipped fields; javac could inline static final values.
            check(WifiInfo.class.getField(constants[i]).getInt(null) == i - 1,
                    "Runtime security constant " + constants[i]);
        }
        check("Mbps".equals(WifiInfo.class.getField("LINK_SPEED_UNITS").get(null)), "Link speed units");
        check("MHz".equals(WifiInfo.class.getField("FREQUENCY_UNITS").get(null)), "Frequency units");
    }

    @SuppressWarnings("unchecked")
    private static void parcels() throws Exception {
        for (byte[] bytes : new byte[][]{new byte[0], "גשר".getBytes(StandardCharsets.UTF_8),
                new byte[]{(byte)0xff, 0, 1}}) {
            WifiSsid expected = WifiSsid.fromBytes(bytes);
            Parcel parcel = Parcel.obtain();
            try {
                expected.writeToParcel(parcel, 0);
                parcel.writeInt(0x3211);
                parcel.setDataPosition(0);
                WifiSsid actual = WifiSsid.CREATOR.createFromParcel(parcel);
                check(expected.equals(actual) && Arrays.equals(bytes, actual.getBytes()), "SSID parcel bytes");
                check(parcel.readInt() == 0x3211 && parcel.dataPosition() == parcel.dataSize(),
                        "SSID parcel consumes exact data");
            } finally { parcel.recycle(); }
        }
        Parcelable.Creator<SupplicantState> creator = (Parcelable.Creator<SupplicantState>)
                SupplicantState.class.getField("CREATOR").get(null);
        for (SupplicantState expected : SupplicantState.values()) {
            Parcel parcel = Parcel.obtain();
            try {
                expected.writeToParcel(parcel, 0);
                parcel.writeInt(0x3212);
                parcel.setDataPosition(0);
                check(expected == creator.createFromParcel(parcel), "State parcel round trip");
                check(parcel.readInt() == 0x3212 && parcel.dataPosition() == parcel.dataSize(),
                        "State parcel consumes exact data");
            } finally { parcel.recycle(); }
        }
    }

    public static void main(String[] args) throws Exception {
        boolean nativeParcel = args.length == 0;
        if (nativeParcel) System.loadLibrary("android_runtime");
        else System.load(args[0]);
        snapshot();
        ssids();
        supplicant();
        if (nativeParcel) parcels();
        System.out.println("WifiDataContractTest all passed: " + checks + " checks; parcel=" + nativeParcel);
    }
}
