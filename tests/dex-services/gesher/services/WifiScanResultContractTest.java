/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package gesher.services;

import android.net.MacAddress;
import android.net.wifi.MloLink;
import android.net.wifi.ScanResult;
import android.net.wifi.ScanResult.InformationElement;
import android.net.wifi.WifiInfo;
import android.net.wifi.WifiSsid;
import android.os.Parcel;
import java.lang.reflect.Array;
import java.nio.ReadOnlyBufferException;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collections;
import java.util.List;
import java.util.Objects;

/**
 * Call sites compile against the official public SDK. Reflection supplies isolated
 * producer metadata through Android's hidden data setters/fields, never a scan service.
 * Parcel checks run only with the real android_runtime, not the host initialization adapter.
 */
public final class WifiScanResultContractTest {
    private static int checks;
    private static final int SENTINEL = 0x5ca76036;
    private interface Action { void run() throws Exception; }

    private static void check(boolean value, String message) {
        if (!value) throw new AssertionError(message);
        checks++;
    }

    private static void expect(Class<? extends Throwable> type, Action action, String message)
            throws Exception {
        try { action.run(); }
        catch (Throwable error) {
            if (!type.isInstance(error)) throw new AssertionError(message, error);
            checks++;
            return;
        }
        throw new AssertionError(message + ": did not throw " + type.getName());
    }

    private static void put(Object value, String field, Object content) throws Exception {
        value.getClass().getField(field).set(value, content);
    }

    private static Object get(Object value, String field) throws Exception {
        return value.getClass().getField(field).get(value);
    }

    private static void set(Object value, String method, Class<?> type, Object content)
            throws Exception {
        value.getClass().getMethod(method, type).invoke(value, content);
    }

    private static void channels() throws Exception {
        // Explicit Android 16 channelization tables, including the two special channels.
        int[][] bands = {{1, 1, 14, 2412, 5}, {2, 32, 177, 5160, 5},
                {8, 1, 233, 5955, 5}, {16, 1, 6, 58320, 2160}};
        for (int[] row : bands) {
            for (int channel = row[1]; channel <= row[2]; channel++) {
                int expected = row[3] + (channel - row[1]) * row[4];
                if (row[0] == 1 && channel == 14) expected = 2484;
                if (row[0] == 8 && channel == 2) expected = 5935;
                check(ScanResult.convertChannelToFrequencyMhzIfSupported(channel, row[0])
                        == expected, "Channel frequency " + row[0] + "/" + channel);
                check(ScanResult.convertFrequencyMhzToChannelIfSupported(expected) == channel,
                        "Frequency channel " + expected);
            }
            for (int invalid : new int[]{Integer.MIN_VALUE, -1, 0, row[1] - 1,
                    row[2] + 1, Integer.MAX_VALUE}) {
                check(ScanResult.convertChannelToFrequencyMhzIfSupported(invalid, row[0]) == -1,
                        "Out-of-range channel " + row[0] + "/" + invalid);
            }
        }
        for (int band : new int[]{Integer.MIN_VALUE, -1, 0, 3, 4, 7, 9, 15, 17,
                Integer.MAX_VALUE}) {
            check(ScanResult.convertChannelToFrequencyMhzIfSupported(1, band) == -1,
                    "Unknown or combined band " + band);
        }
        // Upstream floors in-band frequencies: these are not a regulatory channel validator.
        int[][] frequencies = {{Integer.MIN_VALUE, -1}, {-1, -1}, {0, -1}, {2411, -1},
                {2412, 1}, {2413, 1}, {2472, 13}, {2477, 14}, {2483, 15}, {2484, 14},
                {2485, -1}, {5159, -1}, {5160, 32}, {5164, 32}, {5885, 177}, {5886, -1},
                {5934, -1}, {5935, 2}, {5936, -1}, {5954, -1}, {5955, 1}, {5960, 2},
                {7115, 233}, {7116, -1}, {58319, -1}, {58320, 1}, {60479, 1}, {60480, 2},
                {70200, 6}, {70201, -1}, {Integer.MAX_VALUE, -1}};
        for (int[] row : frequencies) {
            check(ScanResult.convertFrequencyMhzToChannelIfSupported(row[0]) == row[1],
                    "Frequency boundary/floor " + row[0]);
        }
        String[] names = {"UNSPECIFIED", "CHANNEL_WIDTH_20MHZ", "CHANNEL_WIDTH_40MHZ",
                "CHANNEL_WIDTH_80MHZ", "CHANNEL_WIDTH_160MHZ", "CHANNEL_WIDTH_80MHZ_PLUS_MHZ",
                "CHANNEL_WIDTH_320MHZ", "PREAMBLE_LEGACY", "PREAMBLE_HT", "PREAMBLE_VHT",
                "PREAMBLE_HE", "PREAMBLE_EHT", "WIFI_STANDARD_UNKNOWN", "WIFI_STANDARD_LEGACY",
                "WIFI_STANDARD_11N", "WIFI_STANDARD_11AC", "WIFI_STANDARD_11AX",
                "WIFI_STANDARD_11AD", "WIFI_STANDARD_11BE", "WIFI_BAND_24_GHZ",
                "WIFI_BAND_5_GHZ", "WIFI_BAND_6_GHZ", "WIFI_BAND_60_GHZ"};
        int[] values = {-1, 0, 1, 2, 3, 4, 5, 0, 1, 2, 3, 4, 0, 1, 4, 5, 6, 7, 8, 1, 2, 8, 16};
        for (int i = 0; i < names.length; i++) {
            check(ScanResult.class.getField(names[i]).getInt(null) == values[i],
                    "Shipped constant, not just SDK inlining: " + names[i]);
        }
    }

    private static void defaults() throws Exception {
        ScanResult empty = new ScanResult();
        check(empty.SSID == null && empty.BSSID == null && empty.capabilities == null,
                "Empty identity remains absent");
        check(empty.frequency == 0 && empty.level == 0 && empty.timestamp == 0,
                "Public empty-constructor numeric defaults");
        check(empty.channelWidth == 0 && empty.centerFreq0 == 0 && empty.centerFreq1 == 0,
                "Empty channel fields");
        check(empty.venueName == null && empty.operatorFriendlyName == null, "Empty display names");
        check(empty.getWifiSsid() == null && empty.getWifiStandard() == ScanResult.WIFI_STANDARD_UNKNOWN,
                "No inferred SSID or Wi-Fi standard");
        check(empty.getApMldMacAddress() == null && empty.getApMloLinkId() == MloLink.INVALID_MLO_LINK_ID
                && empty.getAffiliatedMloLinks().isEmpty(), "Absent MLO facts");
        check(!empty.isPasspointNetwork() && !empty.is80211mcResponder()
                && !empty.is80211azNtbResponder() && !empty.isTwtResponder()
                && !empty.isSecureHeLtfSupported() && !empty.isRangingFrameProtectionRequired(),
                "No invented responder capabilities");
        // AOSP's empty constructor does not initialize producer arrays/capability text.
        expect(NullPointerException.class, () -> empty.getInformationElements(),
                "Unpopulated informationElements retain pinned behavior");
        expect(NullPointerException.class, () -> empty.getSecurityTypes(),
                "Absent capability text is not reported as an open network");
        check(new ScanResult((ScanResult) null).getWifiStandard() == 0,
                "Pinned copy constructor tolerates null despite @NonNull");
        check(empty.describeContents() == 0 && ScanResult.CREATOR.newArray(3).length == 3,
                "Scan Parcelable value shape");
        empty.SSID = "legacy-only";
        check(empty.getWifiSsid() == null, "Legacy text does not invent raw SSID bytes");
    }

    private static MloLink link() throws Exception {
        MloLink value = new MloLink();
        set(value, "setBand", int.class, 8);
        set(value, "setChannel", int.class, 37);
        set(value, "setLinkId", int.class, 3);
        set(value, "setState", int.class, MloLink.MLO_LINK_STATE_ACTIVE);
        set(value, "setRssi", int.class, -63);
        set(value, "setRxLinkSpeedMbps", int.class, 1200);
        set(value, "setTxLinkSpeedMbps", int.class, 600);
        set(value, "setApMacAddress", MacAddress.class, MacAddress.fromString("02:11:22:33:44:55"));
        set(value, "setStaMacAddress", MacAddress.class, MacAddress.fromString("02:66:77:88:99:aa"));
        put(value, "txBad", 11L);
        put(value, "txRetries", 12L);
        put(value, "txSuccess", 13L);
        put(value, "rxSuccess", 14L);
        set(value, "setLostTxPacketsPerSecond", double.class, 0.5);
        set(value, "setRetriedTxPacketsRate", double.class, 1.25);
        set(value, "setSuccessfulTxPacketsPerSecond", double.class, 2.5);
        set(value, "setSuccessfulRxPacketsPerSecond", double.class, 3.75);
        return value;
    }

    private static ScanResult populated() throws Exception {
        ScanResult value = new ScanResult();
        set(value, "setWifiSsid", WifiSsid.class, WifiSsid.fromBytes(new byte[]{(byte) 0xff, 0, 1}));
        value.SSID = "legacy text";
        value.BSSID = "02:10:20:30:40:50";
        value.capabilities = "[RSN-PSK+SAE-CCMP][ESS]";
        value.level = -62;
        value.frequency = 6135;
        value.timestamp = 0x1fedcba98765L;
        value.channelWidth = ScanResult.CHANNEL_WIDTH_320MHZ;
        value.centerFreq0 = 6105;
        value.centerFreq1 = 6265;
        value.venueName = "Venue";
        value.operatorFriendlyName = "Operator";
        put(value, "hessid", 0x123456789abcL);
        put(value, "anqpDomainId", 7);
        put(value, "distanceCm", 123);
        put(value, "distanceSdCm", 4);
        put(value, "seen", 0x123456789abL);
        put(value, "untrusted", true);
        put(value, "numUsage", 19);
        put(value, "flags", 63L);
        put(value, "ifaceName", "fixture-wifi");
        put(value, "informationElements", new InformationElement[]{
                new InformationElement(255, 107, new byte[]{0, (byte) 0xff, 3}),
                new InformationElement(48, 0, new byte[]{1, 2})});
        put(value, "anqpLines", new ArrayList<>(Arrays.asList("first", "second")));
        Class<?> anqp = Class.forName("android.net.wifi.AnqpInformationElement");
        Object elements = Array.newInstance(anqp, 1);
        Array.set(elements, 0, anqp.getConstructor(int.class, int.class, byte[].class)
                .newInstance(0x506f9a, 8, new byte[]{9, 0, (byte) 0xff}));
        put(value, "anqpElements", elements);
        Class<?> radio = Class.forName("android.net.wifi.ScanResult$RadioChainInfo");
        Object radios = Array.newInstance(radio, 1);
        Object chain = radio.getConstructor().newInstance();
        put(chain, "id", 2);
        put(chain, "level", -64);
        Array.set(radios, 0, chain);
        put(value, "radioChainInfos", radios);
        set(value, "setWifiStandard", int.class, ScanResult.WIFI_STANDARD_11BE);
        set(value, "setApMldMacAddress", MacAddress.class, MacAddress.fromString("02:ab:cd:ef:12:34"));
        set(value, "setApMloLinkId", int.class, 3);
        set(value, "setAffiliatedMloLinks", List.class, Arrays.asList(link()));
        return value;
    }

    private static void same(ScanResult expected, ScanResult actual) {
        check(Objects.equals(expected.SSID, actual.SSID) && Objects.equals(expected.BSSID, actual.BSSID)
                && Objects.equals(expected.capabilities, actual.capabilities), "Scan identity/capabilities");
        check(expected.frequency == actual.frequency && expected.level == actual.level
                && expected.timestamp == actual.timestamp, "Scan measured values");
        check(expected.channelWidth == actual.channelWidth && expected.centerFreq0 == actual.centerFreq0
                && expected.centerFreq1 == actual.centerFreq1, "Channel geometry survives");
        check(Objects.equals(expected.venueName, actual.venueName)
                && Objects.equals(expected.operatorFriendlyName, actual.operatorFriendlyName),
                "Display names survive");
        check(Objects.equals(expected.getWifiSsid(), actual.getWifiSsid())
                && expected.getWifiStandard() == actual.getWifiStandard(), "SSID bytes and standard");
        check(expected.isPasspointNetwork() == actual.isPasspointNetwork()
                && expected.is80211mcResponder() == actual.is80211mcResponder()
                && expected.is80211azNtbResponder() == actual.is80211azNtbResponder()
                && expected.isTwtResponder() == actual.isTwtResponder()
                && expected.isSecureHeLtfSupported() == actual.isSecureHeLtfSupported()
                && expected.isRangingFrameProtectionRequired() == actual.isRangingFrameProtectionRequired(),
                "All supplied scan capability flags survive");
        check(expected.getInformationElements().equals(actual.getInformationElements()), "Information elements");
        check(Arrays.equals(expected.getSecurityTypes(), actual.getSecurityTypes()), "Security types survive");
        check(Objects.equals(expected.getApMldMacAddress(), actual.getApMldMacAddress())
                && expected.getApMloLinkId() == actual.getApMloLinkId()
                && expected.getAffiliatedMloLinks().equals(actual.getAffiliatedMloLinks()), "MLO metadata");
    }

    private static void copiesAndFlags() throws Exception {
        ScanResult original = populated();
        ScanResult copy = new ScanResult(original);
        same(original, copy);
        check(copy != original && copy.getInformationElements().get(0) == original.getInformationElements().get(0),
                "Android copy shares IE values");
        original.SSID = "changed";
        check("legacy text".equals(copy.SSID), "Copy snapshots scalar fields");
        List<MloLink> returned = copy.getAffiliatedMloLinks();
        returned.clear();
        check(copy.getAffiliatedMloLinks().size() == 1, "MLO getter list is independent");
        ArrayList<MloLink> input = new ArrayList<>(copy.getAffiliatedMloLinks());
        set(original, "setAffiliatedMloLinks", List.class, input);
        input.clear();
        check(original.getAffiliatedMloLinks().size() == 1, "MLO setter copies list structure");
        set(original, "setAffiliatedMloLinks", List.class, Collections.emptyList());
        check(copy.getAffiliatedMloLinks().size() == 1, "Copy has independent MLO list structure");
        InformationElement replacement = new InformationElement(1, 0, new byte[]{8});
        ((InformationElement[]) get(original, "informationElements"))[0] = replacement;
        check(copy.getInformationElements().get(0) == replacement, "Android copy retains IE array alias");
        expect(UnsupportedOperationException.class, () -> copy.getInformationElements().clear(),
                "IE list is unmodifiable");
        for (int bit = 0; bit < 6; bit++) {
            ScanResult value = new ScanResult();
            put(value, "flags", 1L << bit);
            boolean[] enabled = {value.isPasspointNetwork(), value.is80211mcResponder(),
                    value.is80211azNtbResponder(), value.isTwtResponder(),
                    value.isSecureHeLtfSupported(), value.isRangingFrameProtectionRequired()};
            for (int i = 0; i < enabled.length; i++) check(enabled[i] == (bit == i), "Independent flag " + bit + "/" + i);
        }
    }

    private static void informationElements() throws Exception {
        byte[] bytes = {0, (byte) 0xff, 9};
        InformationElement value = new InformationElement(255, 107, bytes);
        bytes[0] = 44;
        check(value.getId() == 255 && value.getIdExt() == 107 && value.getBytes().get(0) == 0,
                "IE constructor copies input and retains IDs");
        InformationElement copy = new InformationElement(value);
        check(value.equals(copy) && copy.equals(value) && value.hashCode() == copy.hashCode(),
                "IE value equality and hash");
        check(!value.equals(null) && !value.equals("other")
                && !value.equals(new InformationElement(1, 107, new byte[]{0, (byte) 0xff, 9}))
                && !value.equals(new InformationElement(255, 106, new byte[]{0, (byte) 0xff, 9}))
                && !value.equals(new InformationElement(255, 107, new byte[]{0, (byte) 0xff, 8})),
                "IE identity includes IDs and payload");
        expect(ReadOnlyBufferException.class, () -> value.getBytes().put(0, (byte) 8), "Read-only IE buffer");
        value.getBytes().get();
        check(value.getBytes().position() == 0, "Fresh buffer position per getter");
        ((byte[]) get(value, "bytes"))[0] = 7;
        check(copy.getBytes().get(0) == 0, "IE copy constructor owns independent bytes");
        expect(NullPointerException.class, () -> new InformationElement(1, 0, null), "Null IE bytes");
        expect(NullPointerException.class, () -> new InformationElement((InformationElement) null), "Null IE copy");
        check(copy.describeContents() == 0 && InformationElement.CREATOR.newArray(2).length == 2,
                "IE Parcelable value shape");
    }

    private static void security() throws Exception {
        String[] caps = {"[ESS]", "[WEP][ESS]", "[RSN-PSK-CCMP]", "[RSN-SAE-CCMP]",
                "[RSN-PSK+SAE-CCMP]", "[OWE_TRANSITION]", "[RSN-OWE-CCMP]", "[WAPI-PSK-SMS4]",
                "[WAPI-CERT-SMS4]", "[RSN-DPP-CCMP]", "[RSN-?-CCMP]", "[RSN-EAP/SHA1-CCMP]",
                "[RSN-EAP/SHA256-CCMP][MFPC][MFPR]", "[RSN-EAP/SHA1+EAP/SHA256-CCMP][MFPC]",
                "[RSN-SUITE_B_192-GCMP-256][MFPR]", "[RSN-EAP-FILS-SHA256-CCMP]",
                "[RSN-EAP/SHA256-TKIP][MFPC][MFPR]"};
        int[][] types = {{0}, {1}, {2}, {4}, {2, 4}, {0, 6}, {6}, {7}, {8}, {13}, {},
                {3}, {9}, {3, 9}, {5}, {3}, {3}};
        for (int i = 0; i < caps.length; i++) {
            ScanResult value = new ScanResult();
            value.capabilities = caps[i];
            check(Arrays.equals(value.getSecurityTypes(), types[i]), "Security classification " + caps[i]);
            int[] result = value.getSecurityTypes();
            if (result.length != 0) result[0] = WifiInfo.SECURITY_TYPE_UNKNOWN;
            check(Arrays.equals(value.getSecurityTypes(), types[i]), "Fresh security array " + i);
        }
        ScanResult passpoint = new ScanResult();
        put(passpoint, "flags", 1L);
        passpoint.capabilities = "[RSN-EAP/SHA1-CCMP]";
        check(Arrays.equals(passpoint.getSecurityTypes(), new int[]{3, 11}), "Enterprise Passpoint R1/R2");
        passpoint.capabilities = "[RSN-EAP/SHA256-CCMP][MFPC][MFPR]";
        check(Arrays.equals(passpoint.getSecurityTypes(), new int[]{9, 11, 12}), "PMF Passpoint R3 ordering");
        passpoint.capabilities = "[RSN-SUITE_B_192-GCMP-256][MFPR]";
        check(Arrays.equals(passpoint.getSecurityTypes(), new int[]{5}), "Suite B excludes Passpoint supplements");
        passpoint.capabilities = "[RSN-PSK-CCMP]";
        check(Arrays.equals(passpoint.getSecurityTypes(), new int[]{2}), "Passpoint flag cannot turn PSK into EAP");
    }

    private static void mlo() throws Exception {
        MloLink empty = new MloLink();
        check(empty.getBand() == 0 && empty.getChannel() == 0 && empty.getLinkId() == -1,
                "MLO empty channel and unknown link ID");
        check(empty.getState() == MloLink.MLO_LINK_STATE_UNASSOCIATED && empty.getRssi() == -127
                && empty.getRxLinkSpeedMbps() == -1 && empty.getTxLinkSpeedMbps() == -1,
                "MLO absent association and measurements");
        check(empty.getApMacAddress() == null && empty.getStaMacAddress() == null, "No invented MLO addresses");
        check(empty.equals(new MloLink()) && empty.hashCode() == new MloLink().hashCode(), "MLO value equality");
        MloLink value = link();
        check(value.getBand() == 8 && value.getChannel() == 37 && value.getLinkId() == 3
                && value.getState() == MloLink.MLO_LINK_STATE_ACTIVE, "MLO supplied link metadata");
        check(value.getRssi() == -63 && value.getRxLinkSpeedMbps() == 1200 && value.getTxLinkSpeedMbps() == 600,
                "MLO supplied measurements");
        check(!empty.equals(value) && !value.equals(null) && !value.equals("other"), "MLO equality types");
        for (int[] row : new int[][]{{Integer.MIN_VALUE, -127}, {-128, -127}, {-127, -127},
                {199, 199}, {200, 200}, {201, 200}, {Integer.MAX_VALUE, 200}}) {
            set(empty, "setRssi", int.class, row[0]);
            check(empty.getRssi() == row[1], "Pinned RSSI clamp " + row[0]);
        }
        // Android 16 hidden NetworkCapabilities masks, supplied by the data producer.
        long mac = 2L;
        long location = 1L;
        for (long mask : new long[]{0, mac, location, mac | location}) {
            MloLink redacted = MloLink.class.getConstructor(MloLink.class, long.class).newInstance(value, mask);
            check(Objects.equals(redacted.getApMacAddress(), (mask & location) != 0 ? null : value.getApMacAddress()),
                    "MLO location redaction " + mask);
            check(Objects.equals(redacted.getStaMacAddress(), (mask & mac) != 0 ? null : value.getStaMacAddress()),
                    "MLO local address redaction " + mask);
            check(redacted.getChannel() == value.getChannel() && redacted.getRssi() == value.getRssi(),
                    "MLO redaction preserves unrelated values " + mask);
            if (mask == 0) check(redacted.equals(value) && redacted.hashCode() == value.hashCode(), "MLO exact copy");
        }
        check(value.describeContents() == 0 && MloLink.CREATOR.newArray(2).length == 2, "MLO Parcelable shape");
    }

    private static void exhausted(Parcel parcel, String message) {
        check(parcel.readInt() == SENTINEL && parcel.dataPosition() == parcel.dataSize(), message);
    }

    private static void parcels() throws Exception {
        ScanResult expected = populated();
        Parcel parcel = Parcel.obtain();
        try {
            expected.writeToParcel(parcel, 0);
            parcel.writeInt(SENTINEL);
            parcel.setDataPosition(0);
            // Decode the pinned wire order independently of ScanResult.CREATOR.
            check(parcel.readInt() == 1 && WifiSsid.CREATOR.createFromParcel(parcel).equals(expected.getWifiSsid()),
                    "Wire SSID presence and raw bytes");
            check("legacy text".equals(parcel.readString()) && expected.BSSID.equals(parcel.readString()), "Wire identity order");
            check(parcel.readLong() == 0x123456789abcL && parcel.readInt() == 7, "Wire ANQP identity");
            check(expected.capabilities.equals(parcel.readString()) && parcel.readInt() == -62
                    && parcel.readInt() == 6135 && parcel.readLong() == expected.timestamp, "Wire observations");
            check(parcel.readInt() == 123 && parcel.readInt() == 4 && parcel.readInt() == 5
                    && parcel.readInt() == 6105 && parcel.readInt() == 6265 && parcel.readInt() == 8,
                    "Wire ranging, channel and standard");
            check(parcel.readLong() == 0x123456789abL && parcel.readInt() == 1 && parcel.readInt() == 19,
                    "Wire usage metadata");
            check("Venue".equals(parcel.readString()) && "Operator".equals(parcel.readString())
                    && parcel.readLong() == 63L, "Wire names and flags");
            check(Arrays.asList(parcel.createTypedArray(InformationElement.CREATOR))
                    .equals(expected.getInformationElements()), "Wire information elements");
            check(parcel.readInt() == 2 && "first".equals(parcel.readString())
                    && "second".equals(parcel.readString()), "Wire ANQP text");
            check(parcel.readInt() == 1 && parcel.readInt() == 0x506f9a && parcel.readInt() == 8
                    && parcel.readInt() == 3 && Arrays.equals(parcel.createByteArray(), new byte[]{9, 0, (byte) 0xff}),
                    "Wire ANQP payload and duplicated length");
            check(parcel.readInt() == 1 && parcel.readInt() == 2 && parcel.readInt() == -64,
                    "Wire radio chains");
            check("fixture-wifi".equals(parcel.readString()), "Wire interface");
            check(expected.getApMldMacAddress().equals(parcel.readParcelable(MacAddress.class.getClassLoader()))
                    && parcel.readInt() == 3 && parcel.createTypedArrayList(MloLink.CREATOR)
                    .equals(expected.getAffiliatedMloLinks()), "Wire MLO tail");
            exhausted(parcel, "Independent ScanResult wire reader consumes exactly one object");
            parcel.setDataPosition(0);
            ScanResult actual = ScanResult.CREATOR.createFromParcel(parcel);
            same(expected, actual);
            exhausted(parcel, "ScanResult native round trip consumes exactly one object");
            check(actual.getInformationElements().get(0) != expected.getInformationElements().get(0)
                    && actual.getAffiliatedMloLinks().get(0) != expected.getAffiliatedMloLinks().get(0),
                    "Parcel reconstructs independent data objects");
        } finally { parcel.recycle(); }

        parcel = Parcel.obtain();
        try {
            // Independent minimal Android 16 producer: no wifiSsid, IE, ANQP or MLO records.
            parcel.writeInt(0);
            parcel.writeString("external"); parcel.writeString(null);
            parcel.writeLong(0); parcel.writeInt(0); parcel.writeString("[ESS]");
            parcel.writeInt(-70); parcel.writeInt(2412); parcel.writeLong(99);
            for (int field : new int[]{-1, -1, 0, 0, 0, 0}) parcel.writeInt(field);
            parcel.writeLong(0); parcel.writeInt(0); parcel.writeInt(0);
            parcel.writeString(""); parcel.writeString(""); parcel.writeLong(0);
            parcel.writeTypedArray((InformationElement[]) null, 0);
            parcel.writeInt(0); parcel.writeInt(0); parcel.writeInt(0); parcel.writeString("");
            parcel.writeParcelable(null, 0); parcel.writeInt(-1); parcel.writeTypedList(Collections.<MloLink>emptyList());
            parcel.writeInt(SENTINEL);
            parcel.setDataPosition(0);
            ScanResult actual = ScanResult.CREATOR.createFromParcel(parcel);
            check("external".equals(actual.SSID) && actual.getWifiSsid() == null && actual.frequency == 2412
                    && actual.level == -70 && actual.timestamp == 99, "Independent native wire producer");
            check(actual.getApMldMacAddress() == null && actual.getApMloLinkId() == -1
                    && actual.getAffiliatedMloLinks().isEmpty(), "Wire absence stays absent");
            exhausted(parcel, "Independent producer consumed exactly");
        } finally { parcel.recycle(); }

        for (byte[] bytes : new byte[][]{new byte[0], new byte[]{0, (byte) 0xff, 7}}) {
            InformationElement value = new InformationElement(255, 107, bytes);
            parcel = Parcel.obtain();
            try {
                value.writeToParcel(parcel, 0); parcel.writeInt(SENTINEL); parcel.setDataPosition(0);
                check(parcel.readInt() == 255 && parcel.readInt() == 107
                        && Arrays.equals(parcel.createByteArray(), bytes), "Independent IE wire order");
                exhausted(parcel, "IE wire consumes exactly");
                parcel.setDataPosition(0);
                check(value.equals(InformationElement.CREATOR.createFromParcel(parcel)), "IE native round trip");
                exhausted(parcel, "IE round trip consumes exactly");
            } finally { parcel.recycle(); }
        }
        for (MloLink value : new MloLink[]{new MloLink(), link()}) {
            parcel = Parcel.obtain();
            try {
                value.writeToParcel(parcel, 0); parcel.writeInt(SENTINEL); parcel.setDataPosition(0);
                MloLink actual = MloLink.CREATOR.createFromParcel(parcel);
                check(value.equals(actual) && value.hashCode() == actual.hashCode(), "MLO native round trip");
                exhausted(parcel, "MLO round trip consumes exactly");
            } finally { parcel.recycle(); }
        }
        ScanResult empty = new ScanResult();
        parcel = Parcel.obtain();
        try {
            empty.writeToParcel(parcel, 0); parcel.writeInt(SENTINEL); parcel.setDataPosition(0);
            ScanResult actual = ScanResult.CREATOR.createFromParcel(parcel);
            check(actual.SSID == null && actual.capabilities == null && actual.getWifiSsid() == null,
                    "Empty parcel preserves absence");
            check("".contentEquals(actual.venueName) && "".contentEquals(actual.operatorFriendlyName),
                    "Pinned empty parcel name normalization");
            exhausted(parcel, "Empty scan native round trip consumes exactly");
        } finally { parcel.recycle(); }
    }

    public static void run(boolean nativeParcel) throws Exception {
        checks = 0;
        channels();
        defaults();
        copiesAndFlags();
        informationElements();
        security();
        mlo();
        if (nativeParcel) parcels();
        System.out.println("WifiScanResultContractTest all passed: " + checks + " checks; parcel=" + nativeParcel);
    }

    /** Reference Android entry point: app_process already initializes the real native runtime. */
    public static void main(String[] args) throws Exception {
        run(true);
    }
}
