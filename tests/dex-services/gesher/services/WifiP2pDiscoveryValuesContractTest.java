/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package gesher.services;

import android.net.MacAddress;
import android.net.wifi.p2p.WifiP2pDiscoveryConfig;
import android.net.wifi.p2p.WifiP2pDirInfo;
import android.net.wifi.p2p.WifiP2pUsdBasedLocalServiceAdvertisementConfig;
import android.net.wifi.p2p.WifiP2pUsdBasedServiceDiscoveryConfig;
import android.net.wifi.p2p.nsd.WifiP2pUsdBasedServiceResponse;
import android.os.Build;
import android.os.Parcel;
import android.os.Parcelable;
import java.security.MessageDigest;
import java.util.Arrays;
import java.util.Locale;
import java.util.Objects;

/** SDK36-only public callers; ordinary value records, no discovery operation. */
public final class WifiP2pDiscoveryValuesContractTest {
    static int checks;
    static int fullReaders;
    interface Action { void run() throws Exception; }

    static void check(boolean result, String label) {
        checks++;
        if (!result) throw new AssertionError(label);
    }
    static void equal(Object expected, Object actual, String label) {
        check(Objects.equals(expected, actual), label + ": " + expected + " != " + actual);
    }
    static void rejects(Class<? extends Throwable> expected, String message, Action action, String label) {
        try { action.run(); }
        catch (Throwable actual) {
            check(actual.getClass() == expected, label + ": wrong exception " + actual);
            if (message != null) equal(message, actual.getMessage(), label + " message");
            return;
        }
        throw new AssertionError(label + ": expected " + expected.getName());
    }
    static void identity(Object first, Object second, String label) throws Exception {
        check(first.equals(first), label + " reflexivity");
        check(!first.equals(second) && !first.equals(null) && !first.equals(new Object()), label + " identity equality");
        equal(Object.class, first.getClass().getMethod("equals", Object.class).getDeclaringClass(), label + " inherited equals");
        equal(Object.class, first.getClass().getMethod("hashCode").getDeclaringClass(), label + " inherited hashCode");
        equal(first.hashCode(), first.hashCode(), label + " stable identity hash");
    }
    static <T> void creatorArrays(Parcelable.Creator<T> creator, Class<T> type) {
        T[] empty = creator.newArray(0), pair = creator.newArray(2);
        equal(0, empty.length, type.getSimpleName() + " empty creator array");
        equal(type, pair.getClass().getComponentType(), type.getSimpleName() + " creator array type");
        check(pair.length == 2 && pair[0] == null && pair[1] == null, type.getSimpleName() + " null initialized array");
        rejects(NegativeArraySizeException.class, null, () -> creator.newArray(-1), type.getSimpleName() + " negative array");
    }
    static String digest(byte[] bytes) throws Exception {
        StringBuilder result = new StringBuilder();
        for (byte value : MessageDigest.getInstance("SHA-256").digest(bytes))
            result.append(String.format(Locale.ROOT, "%02x", value & 255));
        return result.toString();
    }
    static void sameFields(Parcelable expected, Parcelable actual) {
        equal(expected.getClass(), actual.getClass(), "original reader concrete type");
        // Vendor bundles may print array identity addresses; their values are checked separately.
        if (!(expected instanceof WifiP2pDiscoveryConfig))
            equal(expected.toString(), actual.toString(), "reader string fields");
        equal(expected.describeContents(), actual.describeContents(), "reader contents");
        if (expected instanceof WifiP2pDiscoveryConfig) {
            WifiP2pDiscoveryConfig first = (WifiP2pDiscoveryConfig) expected, second = (WifiP2pDiscoveryConfig) actual;
            equal(first.getScanType(), second.getScanType(), "reader scan type");
            equal(first.getFrequencyMhz(), second.getFrequencyMhz(), "reader scan frequency");
        } else if (expected instanceof WifiP2pUsdBasedLocalServiceAdvertisementConfig) {
            equal(((WifiP2pUsdBasedLocalServiceAdvertisementConfig) expected).getFrequencyMhz(),
                    ((WifiP2pUsdBasedLocalServiceAdvertisementConfig) actual).getFrequencyMhz(), "reader advertisement frequency");
        } else if (expected instanceof WifiP2pUsdBasedServiceDiscoveryConfig) {
            WifiP2pUsdBasedServiceDiscoveryConfig first = (WifiP2pUsdBasedServiceDiscoveryConfig) expected;
            WifiP2pUsdBasedServiceDiscoveryConfig second = (WifiP2pUsdBasedServiceDiscoveryConfig) actual;
            equal(first.getBand(), second.getBand(), "reader discovery band");
            check(Arrays.equals(first.getFrequenciesMhz(), second.getFrequenciesMhz()), "reader discovery frequencies");
            check(first.getFrequenciesMhz() != second.getFrequenciesMhz(), "reader frequency array is fresh");
        } else if (expected instanceof WifiP2pDirInfo) {
            WifiP2pDirInfo first = (WifiP2pDirInfo) expected, second = (WifiP2pDirInfo) actual;
            equal(first.getMacAddress(), second.getMacAddress(), "reader DIR MAC");
            check(Arrays.equals(first.getNonce(), second.getNonce()), "reader DIR nonce");
            check(Arrays.equals(first.getDirTag(), second.getDirTag()), "reader DIR tag");
            check(first.getNonce() != second.getNonce() && first.getDirTag() != second.getDirTag(), "reader DIR arrays are fresh");
        } else if (expected instanceof WifiP2pUsdBasedServiceResponse) {
            WifiP2pUsdBasedServiceResponse first = (WifiP2pUsdBasedServiceResponse) expected;
            WifiP2pUsdBasedServiceResponse second = (WifiP2pUsdBasedServiceResponse) actual;
            equal(first.getServiceProtocolType(), second.getServiceProtocolType(), "reader response protocol");
            check(Arrays.equals(first.getServiceSpecificInfo(), second.getServiceSpecificInfo()), "reader response information");
            if (first.getServiceSpecificInfo() != null)
                check(first.getServiceSpecificInfo() != second.getServiceSpecificInfo(), "reader response array is fresh");
        } else throw new AssertionError("Unexpected reader type " + expected.getClass());
    }
    static <T extends Parcelable> T wire(String name, T value, Parcelable.Creator<T> creator) throws Exception {
        Parcel written = Parcel.obtain(), input = Parcel.obtain(), rewritten = Parcel.obtain();
        try {
            value.writeToParcel(written, 0);
            byte[] bytes = written.marshall();
            equal(bytes.length, written.dataPosition(), name + " complete writer extent");
            input.unmarshall(bytes, 0, bytes.length); input.setDataPosition(0);
            T copy = creator.createFromParcel(input);
            equal(0, input.dataAvail(), name + " full original reader consumes record");
            check(copy != value && !copy.equals(value), name + " reader retains identity equality");
            sameFields(value, copy);
            copy.writeToParcel(rewritten, 0);
            byte[] again = rewritten.marshall();
            check(Arrays.equals(bytes, again), name + " exact original reader/writer round trip");
            fullReaders++;
            System.out.println("P2P_DISCOVERY_PARCEL " + name + " bytes=" + bytes.length + " sha256=" + digest(bytes)
                    + " rewrite_sha256=" + digest(again) + " roundtrip=true reader=" + copy.getClass().getName());
            return copy;
        } finally { written.recycle(); input.recycle(); rewritten.recycle(); }
    }
    static void discovery(boolean parcel, boolean host) throws Exception {
        WifiP2pDiscoveryConfig full = new WifiP2pDiscoveryConfig.Builder(0).build();
        WifiP2pDiscoveryConfig social = new WifiP2pDiscoveryConfig.Builder(1).build();
        WifiP2pDiscoveryConfig.Builder singleBuilder = new WifiP2pDiscoveryConfig.Builder(2);
        rejects(IllegalArgumentException.class, "Scan type is single frequency, but no frequency was provided",
                singleBuilder::build, "single frequency required at build");
        check(singleBuilder.setFrequencyMhz(2412) == singleBuilder, "frequency setter chains");
        WifiP2pDiscoveryConfig single = singleBuilder.build();
        WifiP2pDiscoveryConfig fullFrequency = new WifiP2pDiscoveryConfig.Builder(0).setFrequencyMhz(5180).build();
        equal(0, full.getScanType(), "full scan type"); equal(0, full.getFrequencyMhz(), "full default frequency");
        equal(1, social.getScanType(), "social scan type"); equal(0, social.getFrequencyMhz(), "social default frequency");
        equal(2, single.getScanType(), "single scan type"); equal(2412, single.getFrequencyMhz(), "single frequency");
        equal(5180, fullFrequency.getFrequencyMhz(), "full scan retains explicitly supplied frequency");
        equal(2437, new WifiP2pDiscoveryConfig.Builder(1).setFrequencyMhz(2437).build().getFrequencyMhz(),
                "social scan retains explicitly supplied frequency");
        equal("\n scanType: 0\n frequencyMhz: 0\n vendorData: []", full.toString(), "discovery complete string");
        equal(0, full.describeContents(), "discovery contents");
        identity(full, new WifiP2pDiscoveryConfig.Builder(0).build(), "discovery");
        for (int invalid : new int[] {-1, 3})
            rejects(IllegalArgumentException.class, "Invalid scan type " + invalid,
                    () -> new WifiP2pDiscoveryConfig.Builder(invalid).build(), "scan type bound");
        for (int invalid : new int[] {-1, 0})
            rejects(IllegalArgumentException.class, "Frequency must be greater than 0",
                    () -> singleBuilder.setFrequencyMhz(invalid), "scan positive frequency");
        equal(2412, singleBuilder.build().getFrequencyMhz(), "rejected setter preserves previous frequency");
        singleBuilder.setFrequencyMhz(5180);
        equal(2412, single.getFrequencyMhz(), "built integer snapshot");
        if (host) equal(Integer.MAX_VALUE, new WifiP2pDiscoveryConfig.Builder(2)
                .setFrequencyMhz(Integer.MAX_VALUE).build().getFrequencyMhz(), "no invented frequency upper bound");
        creatorArrays(WifiP2pDiscoveryConfig.CREATOR, WifiP2pDiscoveryConfig.class);
        if (parcel) {
            wire("discovery_full", full, WifiP2pDiscoveryConfig.CREATOR);
            wire("discovery_social", social, WifiP2pDiscoveryConfig.CREATOR);
            wire("discovery_single", single, WifiP2pDiscoveryConfig.CREATOR);
            wire("discovery_full_frequency", fullFrequency, WifiP2pDiscoveryConfig.CREATOR);
        }
    }
    static void advertisement(boolean parcel, boolean host) throws Exception {
        WifiP2pUsdBasedLocalServiceAdvertisementConfig value = new WifiP2pUsdBasedLocalServiceAdvertisementConfig.Builder().build();
        WifiP2pUsdBasedLocalServiceAdvertisementConfig.Builder builder = new WifiP2pUsdBasedLocalServiceAdvertisementConfig.Builder();
        check(builder.setFrequencyMhz(5180) == builder, "advertisement setter chains");
        WifiP2pUsdBasedLocalServiceAdvertisementConfig explicit = builder.build();
        equal(2437, value.getFrequencyMhz(), "advertisement default");
        equal(5180, explicit.getFrequencyMhz(), "advertisement requested frequency");
        equal("WifiP2pUsdBasedLocalServiceAdvertisementConfig:\n Frequency: 2437", value.toString(), "advertisement string");
        equal(0, value.describeContents(), "advertisement contents");
        identity(value, new WifiP2pUsdBasedLocalServiceAdvertisementConfig.Builder().build(), "advertisement");
        for (int invalid : new int[] {-1, 0}) rejects(IllegalArgumentException.class, "Frequency must be greater than 0",
                () -> builder.setFrequencyMhz(invalid), "advertisement positive frequency");
        equal(5180, builder.build().getFrequencyMhz(), "advertisement rejection preserves state");
        builder.setFrequencyMhz(2412); equal(5180, explicit.getFrequencyMhz(), "advertisement integer snapshot");
        if (host) equal(Integer.MAX_VALUE, builder.setFrequencyMhz(Integer.MAX_VALUE).build().getFrequencyMhz(),
                "advertisement no invented upper bound");
        creatorArrays(WifiP2pUsdBasedLocalServiceAdvertisementConfig.CREATOR, WifiP2pUsdBasedLocalServiceAdvertisementConfig.class);
        if (parcel) {
            wire("advertisement_default", value, WifiP2pUsdBasedLocalServiceAdvertisementConfig.CREATOR);
            wire("advertisement_explicit", explicit, WifiP2pUsdBasedLocalServiceAdvertisementConfig.CREATOR);
        }
    }
    static void usdDiscovery(boolean parcel, boolean host) throws Exception {
        WifiP2pUsdBasedServiceDiscoveryConfig value = new WifiP2pUsdBasedServiceDiscoveryConfig.Builder().build();
        equal(-1, value.getBand(), "USD discovery unspecified band");
        check(Arrays.equals(new int[] {2437}, value.getFrequenciesMhz()), "USD discovery default frequency");
        equal("WifiP2pUsdBasedServiceDiscoveryConfig:\n Band: -1\n Frequencies: [2437]", value.toString(), "USD discovery string");
        equal(0, value.describeContents(), "USD discovery contents");
        identity(value, new WifiP2pUsdBasedServiceDiscoveryConfig.Builder().build(), "USD discovery");
        int[] frequencies = {2412, 2437, 2462, 5180};
        WifiP2pUsdBasedServiceDiscoveryConfig.Builder builder = new WifiP2pUsdBasedServiceDiscoveryConfig.Builder();
        check(builder.setFrequenciesMhz(frequencies) == builder, "USD frequency setter chains");
        WifiP2pUsdBasedServiceDiscoveryConfig multiple = builder.build();
        check(multiple.getFrequenciesMhz() == frequencies, "USD constructor/getter array alias");
        check(builder.build().getFrequenciesMhz() == frequencies, "USD repeated build retains array alias");
        frequencies[0] = 2422; equal(2422, multiple.getFrequenciesMhz()[0], "USD caller mutation is visible");
        multiple.getFrequenciesMhz()[0] = 2412; equal(2412, frequencies[0], "USD getter mutation is visible");
        rejects(NullPointerException.class, "Frequencies cannot be null", () -> builder.setFrequenciesMhz(null), "USD null array");
        for (int length : new int[] {0, 49}) rejects(IllegalArgumentException.class,
                "Number of frequencies: " + length + " must be between 1 and 48",
                () -> builder.setFrequenciesMhz(new int[length]), "USD frequency count");
        check(builder.build().getFrequenciesMhz() == frequencies, "USD rejected setters preserve array");
        for (int band : new int[] {0, -1, 4, 16}) rejects(IllegalArgumentException.class, "Invalid band: " + band,
                () -> new WifiP2pUsdBasedServiceDiscoveryConfig.Builder().setBand(band), "USD unsupported band bits");
        for (int band : new int[] {1, 2, 3, 8, 9, 10, 11}) {
            WifiP2pUsdBasedServiceDiscoveryConfig.Builder bandBuilder = new WifiP2pUsdBasedServiceDiscoveryConfig.Builder();
            check(bandBuilder.setBand(band) == bandBuilder, "USD band setter accepts supported mask");
            rejects(IllegalStateException.class, "Frequencies and band are mutually exclusive.", bandBuilder::build,
                    "pinned band setter retains default frequency array");
        }
        rejects(IllegalStateException.class, "Frequencies and band are mutually exclusive.",
                () -> new WifiP2pUsdBasedServiceDiscoveryConfig.Builder().setFrequenciesMhz(new int[] {2412}).setBand(1).build(),
                "explicit frequency then band conflict");
        rejects(IllegalStateException.class, "Frequencies and band are mutually exclusive.",
                () -> new WifiP2pUsdBasedServiceDiscoveryConfig.Builder().setBand(1).setFrequenciesMhz(new int[] {2412}).build(),
                "band then explicit frequency conflict");
        int[] maximum = new int[48]; Arrays.fill(maximum, 2437);
        WifiP2pUsdBasedServiceDiscoveryConfig max = new WifiP2pUsdBasedServiceDiscoveryConfig.Builder().setFrequenciesMhz(maximum).build();
        equal(48, max.getFrequenciesMhz().length, "USD maximum frequency count accepted");
        if (host) {
            int[] uninterpreted = {-1, 0, Integer.MAX_VALUE};
            check(new WifiP2pUsdBasedServiceDiscoveryConfig.Builder().setFrequenciesMhz(uninterpreted).build()
                    .getFrequenciesMhz() == uninterpreted, "no invented per-frequency validation");
        }
        creatorArrays(WifiP2pUsdBasedServiceDiscoveryConfig.CREATOR, WifiP2pUsdBasedServiceDiscoveryConfig.class);
        if (parcel) {
            wire("usd_discovery_default", value, WifiP2pUsdBasedServiceDiscoveryConfig.CREATOR);
            wire("usd_discovery_multiple", multiple, WifiP2pUsdBasedServiceDiscoveryConfig.CREATOR);
            wire("usd_discovery_maximum", max, WifiP2pUsdBasedServiceDiscoveryConfig.CREATOR);
        }
    }
    static void dir(boolean parcel, boolean host) throws Exception {
        MacAddress mac = MacAddress.fromString("02:11:22:33:44:55");
        byte[] nonce = new byte[8], tag = new byte[8];
        WifiP2pDirInfo zero = new WifiP2pDirInfo(mac, nonce, tag);
        check(zero.getMacAddress() == mac && zero.getNonce() == nonce && zero.getDirTag() == tag, "DIR constructor aliases");
        equal("WifiP2pDirInfo:\n Mac Address: 02:11:22:33:44:55\n Nonce : [0, 0, 0, 0, 0, 0, 0, 0]"
                + "\n DIR Tag : [0, 0, 0, 0, 0, 0, 0, 0]", zero.toString(), "DIR complete string");
        equal(0, zero.describeContents(), "DIR contents");
        identity(zero, new WifiP2pDirInfo(mac, nonce, tag), "DIR");
        byte[] n = {0, 1, 2, 3, 127, -128, -2, -1}, t = {-1, -2, -128, 127, 3, 2, 1, 0};
        WifiP2pDirInfo bytes = new WifiP2pDirInfo(MacAddress.fromString("02:aa:bb:cc:dd:ee"), n, t);
        byte[] aliasNonce = new byte[8], aliasTag = new byte[8];
        WifiP2pDirInfo alias = new WifiP2pDirInfo(mac, aliasNonce, aliasTag);
        aliasNonce[0] = 17; alias.getDirTag()[7] = 34;
        equal((byte) 17, alias.getNonce()[0], "DIR caller mutation"); equal((byte) 34, aliasTag[7], "DIR getter mutation");
        if (host) {
            WifiP2pDirInfo absent = new WifiP2pDirInfo(null, null, null);
            check(absent.getMacAddress() == null && absent.getNonce() == null && absent.getDirTag() == null,
                    "DIR annotations add no constructor validation");
            equal("WifiP2pDirInfo:\n Mac Address: null\n Nonce : <null>\n DIR Tag : <null>", absent.toString(), "DIR null string");
            byte[] shortValue = {1};
            check(new WifiP2pDirInfo(mac, shortValue, shortValue).getNonce() == shortValue,
                    "DIR constructor does not invent array-length validation");
        }
        creatorArrays(WifiP2pDirInfo.CREATOR, WifiP2pDirInfo.class);
        if (parcel) {
            wire("dir_zero", zero, WifiP2pDirInfo.CREATOR);
            wire("dir_bytes", bytes, WifiP2pDirInfo.CREATOR);
            wire("dir_aliased", alias, WifiP2pDirInfo.CREATOR);
        }
    }
    /** SDK observers accept values constructed by the separately compiled hidden fixture. */
    public static void response(Parcelable supplied, int protocol, byte[] info,
            boolean parcel, String name) throws Exception {
        // Keep modern-only types out of method descriptors so legacy stock can resolve run().
        WifiP2pUsdBasedServiceResponse value = (WifiP2pUsdBasedServiceResponse) supplied;
        equal(protocol, value.getServiceProtocolType(), "response public protocol");
        check(value.getServiceSpecificInfo() == info, "response public array alias");
        equal(0, value.describeContents(), "response contents");
        equal("WifiP2pUsdBasedServiceResponse:\n Protocol type: " + protocol + "\n Service specific info : "
                + (info == null ? "<null>" : Arrays.toString(info)), value.toString(), "response complete string");
        creatorArrays(WifiP2pUsdBasedServiceResponse.CREATOR, WifiP2pUsdBasedServiceResponse.class);
        if (parcel) wire(name, value, WifiP2pUsdBasedServiceResponse.CREATOR);
    }
    public static void baselineControl() { new WifiP2pDiscoveryConfig.Builder(0).build(); }
    public static void run(boolean parcel, boolean modern) throws Exception {
        checks = 0; fullReaders = 0;
        discovery(parcel, !parcel);
        if (modern) { advertisement(parcel, !parcel); usdDiscovery(parcel, !parcel); dir(parcel, !parcel); }
    }
    public static void finish(boolean parcel, boolean modern) {
        equal(parcel ? (modern ? 17 : 6) : 0, fullReaders, "exact complete-reader corpus");
        System.out.println("P2P_DISCOVERY_VALUES: PASS checks=" + checks + " nativeParcel=" + parcel
                + " fullReaders=" + fullReaders + " modern=" + modern);
    }
    static void productionGraph() throws Exception {
        if (Build.VERSION.SDK_INT != 36) throw new AssertionError("Exact SDK36 production profile required");
        ClassLoader boot = Parcel.class.getClassLoader();
        if (boot != null && !boot.getClass().getName().endsWith("BootClassLoader"))
            throw new AssertionError("Production native boot Parcel required");
        for (Class<?> type : new Class<?>[] {WifiP2pDiscoveryConfig.class, WifiP2pDiscoveryConfig.Builder.class,
                WifiP2pUsdBasedLocalServiceAdvertisementConfig.class, WifiP2pUsdBasedLocalServiceAdvertisementConfig.Builder.class,
                WifiP2pUsdBasedServiceDiscoveryConfig.class, WifiP2pUsdBasedServiceDiscoveryConfig.Builder.class,
                WifiP2pDirInfo.class, WifiP2pUsdBasedServiceResponse.class, MacAddress.class, Parcelable.class})
            if (type.getClassLoader() != boot) throw new AssertionError("Non-production value provider: " + type);
        for (Class<?> type : new Class<?>[] {WifiP2pDiscoveryConfig.class, WifiP2pUsdBasedLocalServiceAdvertisementConfig.class,
                WifiP2pUsdBasedServiceDiscoveryConfig.class, WifiP2pDirInfo.class, WifiP2pUsdBasedServiceResponse.class})
            if (type.getField("CREATOR").get(null).getClass().getClassLoader() != boot)
                throw new AssertionError("Non-production original reader: " + type);
        for (String name : new String[] {"android.net.wifi.OuiKeyedData", "android.net.wifi.OuiKeyedData$1",
                "android.net.wifi.OuiKeyedData$Builder", "android.net.wifi.util.PersistableBundleUtils",
                "android.internal.modules.utils.build.SdkLevel", "android.os.PersistableBundle"})
            if (Class.forName(name).getClassLoader() != boot) throw new AssertionError("Non-production dependency: " + name);
        if (WifiP2pDirInfo.class.getConstructor(MacAddress.class, byte[].class, byte[].class).getParameterTypes()[0] != MacAddress.class
                || WifiP2pDirInfo.class.getMethod("getMacAddress").getReturnType() != MacAddress.class
                || MacAddress.CREATOR.getClass().getClassLoader() != boot)
            throw new AssertionError("DIR MacAddress graph identity mismatch");
        System.out.println("P2P_DISCOVERY_PROVIDERS coherent production boot graph; complete original readers");
        System.out.println("P2P_DISCOVERY_PROFILE android16-r4; sdk36; declarations=41");
    }
    public static void main(String[] args) throws Exception {
        boolean parcel = args.length == 0 || !args[0].equals("--no-parcel");
        if (parcel) productionGraph();
        run(parcel, true);
        Class.forName("gesher.services.WifiP2pDiscoveryValuesExtendedTest")
                .getMethod("run", boolean.class, boolean.class).invoke(null, parcel, true);
        finish(parcel, true);
    }
}
