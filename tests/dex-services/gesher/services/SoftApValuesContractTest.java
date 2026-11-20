/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package gesher.services;

import android.net.MacAddress;
import android.net.wifi.ScanResult;
import android.net.wifi.SoftApConfiguration;
import android.net.wifi.WifiSsid;
import android.net.wifi.WpsInfo;
import android.os.Parcel;
import android.os.Parcelable;
import android.os.PersistableBundle;
import android.util.SparseIntArray;
import java.lang.reflect.InvocationTargetException;
import java.security.MessageDigest;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collections;
import java.util.HashSet;
import java.util.List;
import java.util.Objects;
import java.util.Set;

/**
 * Public call sites compile solely against SDK36. Reflection seeds system value
 * fields for copy/wire coverage; it does not invoke a manager or radio service.
 */
public final class SoftApValuesContractTest {
    private static int checks;
    private static final int SENTINEL = 0x56a0273;
    private interface Action { void run() throws Exception; }

    private static void check(boolean condition, String label) {
        if (!condition) throw new AssertionError(label);
        checks++;
    }
    private static void expect(Class<? extends Throwable> type, Action action, String label)
            throws Exception {
        try {
            action.run();
        } catch (Throwable failure) {
            while (failure instanceof InvocationTargetException)
                failure = ((InvocationTargetException) failure).getCause();
            if (!type.isInstance(failure)) throw new AssertionError(label, failure);
            checks++;
            return;
        }
        throw new AssertionError(label + ": expected " + type.getSimpleName());
    }
    private static Object call(Object target, String name, Class<?>[] types, Object... args)
            throws Exception {
        return target.getClass().getMethod(name, types).invoke(target, args);
    }
    private static Object get(Object target, String name) throws Exception {
        return call(target, name, new Class<?>[0]);
    }
    private static void set(Object target, String name, Class<?> type, Object value) throws Exception {
        check(call(target, name, new Class<?>[]{type}, value) == target, name + " fluent return");
    }
    private static SparseIntArray channels(int... pairs) {
        SparseIntArray result = new SparseIntArray();
        for (int i = 0; i < pairs.length; i += 2) result.put(pairs[i], pairs[i + 1]);
        return result;
    }
    private static void sameChannels(SparseIntArray expected, SparseIntArray actual, String label) {
        check(expected.size() == actual.size(), label + " count");
        for (int i = 0; i < expected.size(); i++) {
            check(expected.keyAt(i) == actual.keyAt(i) && expected.valueAt(i) == actual.valueAt(i),
                    label + " entry " + i);
        }
    }
    private static void constant(Class<?> owner, String name, int expected) throws Exception {
        check(owner.getField(name).getInt(null) == expected, owner.getSimpleName() + "." + name);
    }
    private static void constants() throws Exception {
        constant(SoftApConfiguration.class, "BAND_2GHZ", SoftApConfiguration.BAND_2GHZ);
        constant(SoftApConfiguration.class, "BAND_5GHZ", SoftApConfiguration.BAND_5GHZ);
        constant(SoftApConfiguration.class, "BAND_6GHZ", SoftApConfiguration.BAND_6GHZ);
        constant(SoftApConfiguration.class, "BAND_60GHZ", SoftApConfiguration.BAND_60GHZ);
        constant(SoftApConfiguration.class, "SECURITY_TYPE_OPEN", SoftApConfiguration.SECURITY_TYPE_OPEN);
        constant(SoftApConfiguration.class, "SECURITY_TYPE_WPA2_PSK", SoftApConfiguration.SECURITY_TYPE_WPA2_PSK);
        constant(SoftApConfiguration.class, "SECURITY_TYPE_WPA3_SAE_TRANSITION", SoftApConfiguration.SECURITY_TYPE_WPA3_SAE_TRANSITION);
        constant(SoftApConfiguration.class, "SECURITY_TYPE_WPA3_SAE", SoftApConfiguration.SECURITY_TYPE_WPA3_SAE);
        constant(SoftApConfiguration.class, "SECURITY_TYPE_WPA3_OWE_TRANSITION", SoftApConfiguration.SECURITY_TYPE_WPA3_OWE_TRANSITION);
        constant(SoftApConfiguration.class, "SECURITY_TYPE_WPA3_OWE", SoftApConfiguration.SECURITY_TYPE_WPA3_OWE);
        constant(WpsInfo.class, "PBC", WpsInfo.PBC);
        constant(WpsInfo.class, "DISPLAY", WpsInfo.DISPLAY);
        constant(WpsInfo.class, "KEYPAD", WpsInfo.KEYPAD);
        constant(WpsInfo.class, "LABEL", WpsInfo.LABEL);
        constant(WpsInfo.class, "INVALID", WpsInfo.INVALID);
        check(SoftApConfiguration.CREATOR != null && WpsInfo.CREATOR != null, "Parcelable creators");
        check(SoftApConfiguration.CREATOR.newArray(3).length == 3
                && SoftApConfiguration.CREATOR.newArray(0).length == 0, "SoftAP creator arrays");
        check(WpsInfo.CREATOR.newArray(3).length == 3 && WpsInfo.CREATOR.newArray(0).length == 0,
                "WPS creator arrays");
        expect(NegativeArraySizeException.class, () -> SoftApConfiguration.CREATOR.newArray(-1),
                "SoftAP negative creator array");
        expect(NegativeArraySizeException.class, () -> WpsInfo.CREATOR.newArray(-1),
                "WPS negative creator array");
    }
    private static SoftApConfiguration.Builder copy(SoftApConfiguration value) throws Exception {
        return SoftApConfiguration.Builder.class.getConstructor(SoftApConfiguration.class).newInstance(value);
    }
    private static void defaultsAndOwnership() throws Exception {
        SoftApConfiguration.Builder builder = new SoftApConfiguration.Builder();
        SoftApConfiguration value = builder.build();
        check(value.getSsid() == null && value.getWifiSsid() == null && value.getBssid() == null
                && value.getPassphrase() == null, "Default nullable identity");
        check(!value.isHiddenSsid() && value.getSecurityType() == SoftApConfiguration.SECURITY_TYPE_OPEN
                && value.describeContents() == 0, "Default value flags");
        sameChannels(channels(1, 0), value.getChannels(), "Default channels");
        SoftApConfiguration same = new SoftApConfiguration.Builder().build();
        check(value != same && value.equals(value) && value.equals(same) && same.equals(value)
                && value.hashCode() == same.hashCode(), "Value equality and hash");
        check(!value.equals(null) && !value.equals("softap"), "Equality type boundary");
        check(value.toString().contains("Passphrase = <empty>")
                && value.toString().contains("Channels = {1=0}"), "Default diagnostics");
        check(copy(value).build().equals(value), "System copy constructor");

        SparseIntArray input = channels(2, 36, 1, 6);
        check(builder.setChannels(input) == builder, "Public Builder fluent return");
        input.put(1, 11);
        SoftApConfiguration first = builder.build();
        sameChannels(channels(1, 6, 2, 36), first.getChannels(), "Builder takes channel ownership");
        SparseIntArray returned = first.getChannels();
        returned.put(1, 1);
        returned.delete(2);
        sameChannels(channels(1, 6, 2, 36), first.getChannels(), "Getter returns owned channels");
        SoftApConfiguration.Builder copied = copy(first);
        copied.setChannels(channels(4, 9));
        sameChannels(channels(1, 6, 2, 36), first.getChannels(), "Copy Builder isolation");
        builder.setChannels(channels(8, 4));
        sameChannels(channels(1, 6, 2, 36), first.getChannels(), "Builder reuse isolation");
        check(!first.equals(builder.build()), "Channel changes affect equality");
        int[] bands = (int[]) get(first, "getBands");
        bands[0] = 8;
        check(((int[]) get(first, "getBands"))[0] == 1, "System bands getter ownership");
    }
    private static void channelValidation() throws Exception {
        // Every channel in the source-defined range, not just conventional Wi-Fi examples.
        int[][] ranges = {{1, 1, 14}, {2, 34, 196}, {4, 1, 253}, {8, 1, 6}};
        for (int[] range : ranges) {
            int band = range[0];
            for (int channel = range[1]; channel <= range[2]; channel++) {
                SoftApConfiguration value = new SoftApConfiguration.Builder()
                        .setChannels(channels(band, channel)).build();
                sameChannels(channels(band, channel), value.getChannels(), "Valid band/channel");
            }
            for (int channel : new int[]{-1, range[1] - 1, range[2] + 1, Integer.MAX_VALUE}) {
                if (channel == 0) continue; // Zero means automatic channel selection.
                expect(IllegalArgumentException.class,
                        () -> new SoftApConfiguration.Builder().setChannels(channels(band, channel)),
                        "Channel range boundary " + band + "/" + channel);
            }
        }
        for (int mask = 1; mask < 16; mask++) {
            SparseIntArray request = channels(mask, 0);
            sameChannels(request, new SoftApConfiguration.Builder().setChannels(request).build().getChannels(),
                    "Automatic channel band mask " + mask);
            if (Integer.bitCount(mask) != 1) {
                final int invalidBand = mask;
                expect(IllegalArgumentException.class, () -> new SoftApConfiguration.Builder()
                        .setChannels(channels(invalidBand, 1)), "Explicit channel with combined band");
            }
        }
        for (int band : new int[]{0, -1, 16, 17, Integer.MAX_VALUE}) {
            expect(IllegalArgumentException.class, () -> new SoftApConfiguration.Builder()
                    .setChannels(channels(band, 0)), "Invalid automatic band " + band);
        }
        SoftApConfiguration.Builder builder = new SoftApConfiguration.Builder()
                .setChannels(channels(2, 44));
        expect(NullPointerException.class, () -> builder.setChannels(null), "Null channels");
        expect(IllegalArgumentException.class, () -> builder.setChannels(channels()), "Empty channels");
        expect(IllegalArgumentException.class, () -> builder.setChannels(channels(1, 1, 2, 36, 4, 1)),
                "Three channels");
        expect(IllegalArgumentException.class, () -> builder.setChannels(channels(1, 6, 2, 7)),
                "Later invalid channel");
        sameChannels(channels(2, 44), builder.build().getChannels(), "Failed setter preserves prior state");
        SparseIntArray overlap = channels(3, 0, 7, 0);
        sameChannels(overlap, new SoftApConfiguration.Builder().setChannels(overlap).build().getChannels(),
                "Overlapping automatic masks retain Android behavior");
    }
    private static SoftApConfiguration populated() throws Exception {
        SoftApConfiguration.Builder builder = new SoftApConfiguration.Builder()
                .setChannels(channels(1, 11, 2, 149));
        set(builder, "setWifiSsid", WifiSsid.class, WifiSsid.fromBytes(new byte[]{'G', 'e', 's', 'h', 'e', 'r'}));
        set(builder, "setMacRandomizationSetting", int.class, 0);
        set(builder, "setBssid", MacAddress.class, MacAddress.fromString("02:23:45:67:89:ab"));
        call(builder, "setPassphrase", new Class<?>[]{String.class, int.class}, "value-secret-123",
                SoftApConfiguration.SECURITY_TYPE_WPA2_PSK);
        set(builder, "setHiddenSsid", boolean.class, true);
        set(builder, "setMaxNumberOfClients", int.class, 7);
        set(builder, "setAutoShutdownEnabled", boolean.class, false);
        set(builder, "setShutdownTimeoutMillis", long.class, 54321L);
        set(builder, "setClientControlByUserEnabled", boolean.class, true);
        set(builder, "setAllowedClientList", List.class,
                Collections.singletonList(MacAddress.fromString("02:01:02:03:04:05")));
        set(builder, "setBlockedClientList", List.class,
                Collections.singletonList(MacAddress.fromString("02:01:02:03:04:06")));
        set(builder, "setBridgedModeOpportunisticShutdownEnabled", boolean.class, false);
        set(builder, "setIeee80211beEnabled", boolean.class, false);
        set(builder, "setUserConfiguration", boolean.class, false);
        set(builder, "setBridgedModeOpportunisticShutdownTimeoutMillis", long.class, 12345L);
        set(builder, "setRandomizedMacAddress", MacAddress.class, MacAddress.fromString("02:aa:bb:cc:dd:ee"));
        call(builder, "setAllowedAcsChannels", new Class<?>[]{int.class, int[].class}, 1, new int[]{1, 6, 11});
        call(builder, "setAllowedAcsChannels", new Class<?>[]{int.class, int[].class}, 2, new int[]{36, 149});
        call(builder, "setAllowedAcsChannels", new Class<?>[]{int.class, int[].class}, 4, new int[]{5, 221});
        set(builder, "setMaxChannelBandwidth", int.class, 4); // SoftApInfo.CHANNEL_WIDTH_80MHZ.
        return builder.build();
    }
    private static void systemValues(boolean pinnedR4) throws Exception {
        SoftApConfiguration value = populated();
        check("Gesher".equals(value.getSsid()) && value.getWifiSsid().equals(WifiSsid.fromBytes("Gesher".getBytes("UTF-8"))),
                "UTF-8 SSID getters");
        check("02:23:45:67:89:ab".equals(value.getBssid().toString()) && value.isHiddenSsid()
                && "value-secret-123".equals(value.getPassphrase()), "Populated public getters");
        check(value.getSecurityType() == SoftApConfiguration.SECURITY_TYPE_WPA2_PSK, "Security getter");
        check(!value.toString().contains(value.getPassphrase()) && value.toString().contains("<non-empty>"),
                "Passphrase diagnostic masking");
        check(copy(value).build().equals(value), "Populated copy equality");
        check((Integer) get(value, "getMaxNumberOfClients") == 7
                && (Long) get(value, "getShutdownTimeoutMillis") == 54321L
                && !(Boolean) get(value, "isAutoShutdownEnabled"), "System scalar state");
        check(Arrays.equals((int[]) call(value, "getAllowedAcsChannels", new Class<?>[]{int.class}, 1),
                new int[]{1, 6, 11}), "ACS channel values");
        int[] acs = (int[]) call(value, "getAllowedAcsChannels", new Class<?>[]{int.class}, 2);
        acs[0] = 0;
        check(!Arrays.equals(acs, (int[]) call(value, "getAllowedAcsChannels", new Class<?>[]{int.class}, 2)),
                "ACS getter owns returned array");
        SoftApConfiguration.Builder builder = new SoftApConfiguration.Builder();
        expect(IllegalArgumentException.class, () -> set(builder, "setSsid", String.class, ""), "Empty legacy SSID");
        expect(IllegalArgumentException.class, () -> set(builder, "setSsid", String.class, "\ud800"),
                "Malformed UTF-16 SSID");
        set(builder, "setSsid", String.class, "A\u05d0");
        check("A\u05d0".equals(builder.build().getSsid()), "Unicode SSID");
        set(builder, "setSsid", String.class, null);
        check(builder.build().getWifiSsid() == null, "SSID null clears");
        set(builder, "setWifiSsid", WifiSsid.class, WifiSsid.fromBytes(new byte[]{(byte) 0xff, 0}));
        check("<unknown ssid>".equals(builder.build().getSsid()), "Non-UTF-8 legacy getter");
        for (String address : new String[]{"00:00:00:00:00:00", "ff:ff:ff:ff:ff:ff", "01:00:00:00:00:01"}) {
            expect(IllegalArgumentException.class, () -> set(builder, "setBssid", MacAddress.class,
                    MacAddress.fromString(address)), "Invalid BSSID " + address);
        }
        for (int security : new int[]{0, 4, 5}) {
            expect(IllegalArgumentException.class, () -> call(builder, "setPassphrase",
                    new Class<?>[]{String.class, int.class}, "not-null", security), "Open security passphrase");
            call(builder, "setPassphrase", new Class<?>[]{String.class, int.class}, null, security);
            check(builder.build().getPassphrase() == null && builder.build().getSecurityType() == security,
                    "Open security values");
        }
        for (int security : new int[]{1, 2, 3, 999}) {
            expect(IllegalArgumentException.class, () -> call(builder, "setPassphrase",
                    new Class<?>[]{String.class, int.class}, "", security), "Empty secured passphrase");
            // API33+ defers PSK length and security support to the service.
            call(builder, "setPassphrase", new Class<?>[]{String.class, int.class}, "x", security);
            check("x".equals(builder.build().getPassphrase()) && builder.build().getSecurityType() == security,
                    "Value layer does not invent service admission");
        }
        expect(IllegalArgumentException.class, () -> set(builder, "setMaxNumberOfClients", int.class, -1),
                "Negative client limit");
        expect(IllegalArgumentException.class, () -> set(builder, "setBridgedModeOpportunisticShutdownTimeoutMillis",
                long.class, 0L), "Zero bridged timeout");
        expect(IllegalArgumentException.class, () -> set(builder, "setBridgedModeOpportunisticShutdownTimeoutMillis",
                long.class, -2L), "Invalid bridged timeout");
        expect(IllegalArgumentException.class, () -> set(builder, "setRandomizedMacAddress", MacAddress.class, null),
                "Null randomized address");
        expect(IllegalArgumentException.class, () -> call(builder, "setAllowedAcsChannels",
                new Class<?>[]{int.class, int[].class}, 8, new int[]{1}), "Unsupported ACS band");
        expect(IllegalArgumentException.class, () -> call(builder, "setAllowedAcsChannels",
                new Class<?>[]{int.class, int[].class}, 1, null), "Null ACS array");
        expect(IllegalArgumentException.class, () -> call(builder, "setAllowedAcsChannels",
                new Class<?>[]{int.class, int[].class}, 2, new int[]{1}), "Invalid ACS channel");
        for (int width : new int[]{0, 1, 5, 7, 10, Integer.MAX_VALUE}) {
            expect(IllegalArgumentException.class, () -> set(builder, "setMaxChannelBandwidth", int.class, width),
                    "Invalid maximum bandwidth " + width);
        }
        List<MacAddress> clients = new ArrayList<>();
        clients.add(MacAddress.fromString("02:11:22:33:44:55"));
        set(builder, "setAllowedClientList", List.class, clients);
        clients.clear();
        check(((List<?>) get(builder.build(), "getAllowedClientList")).size() == 1, "Client-list input copied");
        set(builder, "setBlockedClientList", List.class,
                Collections.singletonList(MacAddress.fromString("02:11:22:33:44:55")));
        expect(IllegalArgumentException.class, builder::build, "Overlapping client lists");
        if (pinnedR4) {
            SoftApConfiguration.Builder modern = new SoftApConfiguration.Builder();
            set(modern, "setClientIsolationEnabled", boolean.class, true);
            set(modern, "setBandOptimizationEnabled", boolean.class, false);
            set(modern, "setIeee80211axEnabled", boolean.class, false);
            SoftApConfiguration built = modern.build();
            check((Boolean) get(built, "isClientIsolationEnabled")
                    && !(Boolean) get(built, "isBandOptimizationEnabled")
                    && !(Boolean) get(built, "isIeee80211beEnabled"), "Pinned r4 flags and AX/BE dependency");
            check(copy(built).build().equals(built), "Pinned r4 copy flags");
        }
    }
    private static SoftApConfiguration vendorValue() throws Exception {
        SoftApConfiguration.Builder builder = copy(populated());
        ScanResult.InformationElement element = new ScanResult.InformationElement(
                221, 0, new byte[]{0, 0x11, 0x22, 0x33});
        List<ScanResult.InformationElement> elements = new ArrayList<>();
        elements.add(element);
        set(builder, "setVendorElements", List.class, elements);
        elements.clear();
        check(((List<?>) get(builder.build(), "getVendorElements")).size() == 1, "Vendor-list input copied");
        List<?> returned = (List<?>) get(builder.build(), "getVendorElements");
        returned.clear();
        check(((List<?>) get(builder.build(), "getVendorElements")).size() == 1, "Vendor-list getter ownership");
        expect(IllegalArgumentException.class, () -> set(builder, "setVendorElements", List.class,
                Arrays.asList(element, element)), "Duplicate vendor elements");
        ScanResult.InformationElement wrong = new ScanResult.InformationElement(1, 0, new byte[]{1});
        expect(IllegalArgumentException.class, () -> set(builder, "setVendorElements", List.class,
                Collections.singletonList(wrong)), "Non-vendor element");
        PersistableBundle bundle = new PersistableBundle();
        bundle.putString("fixture", "softap");
        bundle.putInt("revision", 4);
        Class<?> ouiBuilder = Class.forName("android.net.wifi.OuiKeyedData$Builder", true,
                SoftApConfiguration.class.getClassLoader());
        Object oui = get(ouiBuilder.getConstructor(int.class, PersistableBundle.class)
                .newInstance(0x112233, bundle), "build");
        set(builder, "setVendorData", List.class, Collections.singletonList(oui));
        expect(IllegalArgumentException.class, () -> set(builder, "setVendorData", List.class, null),
                "Null vendor data");
        return builder.build();
    }
    private static void compatibilityBoundaries() throws Exception {
        Class<?> compatibility = Class.forName("android.compat.Compatibility");
        Class<?> config = Class.forName("android.compat.Compatibility$ChangeConfig");
        Set<Long> selected = new HashSet<>(Arrays.asList(215656264L, 213289672L));
        for (int target : new int[]{30, 31, 32, 36}) {
            boolean modern = target > 31; // Both upstream changes are @EnabledAfter(S).
            Object changes = config.getConstructor(Set.class, Set.class).newInstance(
                    modern ? selected : Collections.emptySet(),
                    modern ? Collections.emptySet() : selected);
            compatibility.getMethod("setOverrides", config).invoke(null, changes);
            try {
                SoftApConfiguration.Builder builder = new SoftApConfiguration.Builder();
                check((Long) get(builder.build(), "getShutdownTimeoutMillis") == (modern ? -1L : 0L),
                        "Target " + target + " default timeout");
                expect(IllegalArgumentException.class,
                        () -> set(builder, "setShutdownTimeoutMillis", long.class, -2L),
                        "Target " + target + " negative timeout");
                if (modern) {
                    expect(IllegalArgumentException.class,
                            () -> set(builder, "setShutdownTimeoutMillis", long.class, 0L),
                            "Target " + target + " zero timeout rejected");
                    set(builder, "setShutdownTimeoutMillis", long.class, -1L);
                } else {
                    expect(IllegalArgumentException.class,
                            () -> set(builder, "setShutdownTimeoutMillis", long.class, -1L),
                            "Target " + target + " default sentinel rejected by legacy setter");
                    set(builder, "setShutdownTimeoutMillis", long.class, 0L);
                }
                set(builder, "setShutdownTimeoutMillis", long.class, 1L);
                check((Long) get(builder.build(), "getShutdownTimeoutMillis") == 1L,
                        "Target " + target + " positive timeout");
                MacAddress address = MacAddress.fromString("02:12:34:56:78:90");
                set(builder, "setBssid", MacAddress.class, address);
                if (modern) {
                    expect(IllegalArgumentException.class, builder::build,
                            "Target " + target + " BSSID/randomization exclusion");
                } else {
                    SoftApConfiguration legacy = builder.build();
                    check(address.equals(legacy.getBssid())
                            && (Integer) get(legacy, "getMacRandomizationSetting") == 2,
                            "Target " + target + " legacy BSSID accepted");
                    check((Integer) get(copy(legacy).build(), "getMacRandomizationSetting") == 0,
                            "Legacy value copy reconciles BSSID randomization");
                }
                set(builder, "setMacRandomizationSetting", int.class, 0);
                check(address.equals(builder.build().getBssid()), "Target " + target + " explicit BSSID mode");
            } finally {
                compatibility.getMethod("clearOverrides").invoke(null);
            }
        }
    }
    public static void verifyGeneratedCompatTable() throws Exception {
        Class<?> table = Class.forName("com.gesher.server.CompatChanges");
        for (int target : new int[]{30, 31, 32, 36}) {
            long[] disabled = (long[]) table.getMethod("disabledFor", int.class).invoke(null, target);
            for (long id : new long[]{215656264L, 213289672L}) {
                check((Arrays.binarySearch(disabled, id) >= 0) == (target <= 31),
                        "Generated target " + target + " compatibility entry " + id);
            }
        }
        System.out.println("SoftAP generated target-SDK table matches 4 boundaries");
    }
    private static void wpsValues() {
        WpsInfo value = new WpsInfo();
        check(value.setup == WpsInfo.INVALID && value.BSSID == null && value.pin == null
                && value.describeContents() == 0, "WPS defaults");
        check(" setup: 4\n BSSID: null\n pin: null\n".equals(value.toString()), "WPS diagnostics");
        WpsInfo nullCopy = new WpsInfo(null);
        check(nullCopy.setup == WpsInfo.PBC && nullCopy.BSSID == null && nullCopy.pin == null,
                "WPS null copy retains zero-initialized source contract");
        value.setup = WpsInfo.KEYPAD;
        value.BSSID = "02:33:44:55:66:77";
        value.pin = "12345670";
        WpsInfo copy = new WpsInfo(value);
        check(copy != value && copy.setup == value.setup && Objects.equals(copy.BSSID, value.BSSID)
                && Objects.equals(copy.pin, value.pin), "WPS copy fields");
        value.setup = -19;
        value.BSSID = "unvalidated";
        value.pin = "";
        check(copy.setup == WpsInfo.KEYPAD && "12345670".equals(copy.pin), "WPS copy ownership");
        check(value.toString().contains(" setup: -19") && value.toString().contains("BSSID: unvalidated"),
                "WPS value container does not invent radio validation");
    }
    private static void emitWire(String name, Parcel parcel) throws Exception {
        byte[] wire = parcel.marshall();
        byte[] digest = MessageDigest.getInstance("SHA-256").digest(wire);
        StringBuilder hex = new StringBuilder();
        for (byte b : digest) hex.append(String.format("%02x", b & 255));
        System.out.println("SOFTAP_VALUES_PARCEL " + name + " bytes=" + wire.length + " sha256=" + hex);
    }
    private static void softParcel(String name, SoftApConfiguration value) throws Exception {
        Parcel parcel = Parcel.obtain();
        try {
            value.writeToParcel(parcel, 0);
            emitWire(name, parcel);
            int end = parcel.dataPosition();
            parcel.writeInt(SENTINEL);
            parcel.setDataPosition(0);
            check(Objects.equals(value.getWifiSsid(), parcel.readParcelable(WifiSsid.class.getClassLoader()))
                    && Objects.equals(value.getBssid(), parcel.readParcelable(MacAddress.class.getClassLoader()))
                    && Objects.equals(value.getPassphrase(), parcel.readString())
                    && value.isHiddenSsid() == parcel.readBoolean(), name + " independent identity prefix");
            int size = parcel.readInt();
            SparseIntArray wireChannels = new SparseIntArray();
            for (int i = 0; i < size; i++) wireChannels.put(parcel.readInt(), parcel.readInt());
            sameChannels(value.getChannels(), wireChannels, name + " independent channel prefix");
            check(parcel.readInt() == value.getSecurityType(), name + " independent security prefix");
            parcel.setDataPosition(0);
            SoftApConfiguration restored = SoftApConfiguration.CREATOR.createFromParcel(parcel);
            check(parcel.dataPosition() == end && parcel.readInt() == SENTINEL && parcel.dataAvail() == 0,
                    name + " exact native consumption");
            check(value != restored && value.equals(restored) && restored.equals(value)
                    && value.hashCode() == restored.hashCode(), name + " complete value round trip");
            check(value.toString().equals(restored.toString()), name + " complete diagnostic state");
            SparseIntArray detached = restored.getChannels();
            detached.clear();
            sameChannels(value.getChannels(), restored.getChannels(), name + " decoded ownership");
        } finally { parcel.recycle(); }
    }
    private static void wpsParcel(String name, WpsInfo value) throws Exception {
        Parcel parcel = Parcel.obtain();
        try {
            value.writeToParcel(parcel, 0);
            emitWire(name, parcel);
            int end = parcel.dataPosition();
            parcel.writeInt(SENTINEL);
            parcel.setDataPosition(0);
            check(parcel.readInt() == value.setup && Objects.equals(parcel.readString(), value.BSSID)
                    && Objects.equals(parcel.readString(), value.pin) && parcel.dataPosition() == end,
                    name + " independent WPS reader");
            parcel.setDataPosition(0);
            WpsInfo restored = WpsInfo.CREATOR.createFromParcel(parcel);
            check(parcel.dataPosition() == end && parcel.readInt() == SENTINEL && parcel.dataAvail() == 0,
                    name + " exact native consumption");
            check(restored != value && restored.setup == value.setup && Objects.equals(restored.BSSID, value.BSSID)
                    && Objects.equals(restored.pin, value.pin) && restored.toString().equals(value.toString()),
                    name + " full WPS round trip");
        } finally { parcel.recycle(); }
    }
    public static void parcels() throws Exception {
        softParcel("softap_default", new SoftApConfiguration.Builder().build());
        softParcel("softap_single6", new SoftApConfiguration.Builder().setChannels(channels(4, 221)).build());
        softParcel("softap_pair", new SoftApConfiguration.Builder().setChannels(channels(1, 6, 2, 149)).build());
        softParcel("softap_composite", new SoftApConfiguration.Builder().setChannels(channels(3, 0, 12, 0)).build());
        softParcel("softap_full", populated());
        SoftApConfiguration.Builder builder = new SoftApConfiguration.Builder();
        set(builder, "setWifiSsid", WifiSsid.class, WifiSsid.fromBytes(new byte[]{(byte) 0xff, 0, (byte) 0x80}));
        softParcel("softap_binary_ssid", builder.build());
        call(builder, "setPassphrase", new Class<?>[]{String.class, int.class}, null, SoftApConfiguration.SECURITY_TYPE_WPA3_OWE);
        softParcel("softap_owe", builder.build());
        softParcel("softap_vendor", vendorValue());
        wpsParcel("wps_default", new WpsInfo());
        wpsParcel("wps_null_copy", new WpsInfo(null));
        WpsInfo wps = new WpsInfo();
        wps.setup = WpsInfo.KEYPAD; wps.BSSID = "02:33:44:55:66:77"; wps.pin = "12345670";
        wpsParcel("wps_full", wps);
        wps.setup = -19; wps.BSSID = "unvalidated-\u05d0"; wps.pin = "";
        wpsParcel("wps_unchecked", wps);
    }
    public static void baselineSoftApControl() {
        new SoftApConfiguration.Builder().setChannels(channels(1, 6)).build();
        throw new AssertionError("Absent baseline SoftAP provider unexpectedly executed");
    }
    public static void baselineWpsControl() {
        new WpsInfo();
        throw new AssertionError("Absent baseline WPS provider unexpectedly executed");
    }
    public static void run(boolean nativeParcel, boolean pinnedR4) throws Exception {
        checks = 0;
        constants();
        defaultsAndOwnership();
        channelValidation();
        systemValues(pinnedR4);
        compatibilityBoundaries();
        vendorValue();
        wpsValues();
        if (nativeParcel) parcels();
        System.out.println("SoftApValuesContractTest all passed: " + checks + " checks; parcel=" + nativeParcel
                + "; profile=" + (pinnedR4 ? "android16-r4" : "stock-api36"));
    }
    public static void main(String[] args) throws Exception {
        System.loadLibrary("android_runtime");
        verifyGeneratedCompatTable();
        run(true, true);
    }
}
