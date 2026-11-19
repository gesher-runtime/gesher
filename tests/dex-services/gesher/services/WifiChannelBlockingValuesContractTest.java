/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package gesher.services;

import android.net.wifi.BlockingOption;
import android.net.wifi.WifiAvailableChannel;
import android.os.Build;
import android.os.Parcel;
import android.os.Parcelable;
import java.security.MessageDigest;
import java.util.Arrays;
import java.util.Locale;
import java.util.Objects;

/** Compiled against official SDK36 only; values and normal records, no radio operation. */
public final class WifiChannelBlockingValuesContractTest {
    static int checks;
    static int fullReaders;
    interface Action { void run() throws Exception; }

    static void check(boolean condition, String label) {
        checks++;
        if (!condition) throw new AssertionError(label);
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
    static <T> void creatorArrays(Parcelable.Creator<T> creator, Class<T> type) {
        T[] empty = creator.newArray(0), pair = creator.newArray(2);
        equal(0, empty.length, type.getSimpleName() + " empty array");
        equal(type, pair.getClass().getComponentType(), type.getSimpleName() + " typed array");
        check(pair.length == 2 && pair[0] == null && pair[1] == null, "fresh null array");
        rejects(NegativeArraySizeException.class, null, () -> creator.newArray(-1), "negative array size");
    }
    static String digest(byte[] bytes) throws Exception {
        StringBuilder result = new StringBuilder();
        for (byte value : MessageDigest.getInstance("SHA-256").digest(bytes))
            result.append(String.format(Locale.ROOT, "%02x", value & 255));
        return result.toString();
    }
    static void sameFields(Parcelable expected, Parcelable actual) {
        equal(expected.getClass(), actual.getClass(), "original reader concrete type");
        equal(expected.toString(), actual.toString(), "original reader string");
        equal(expected.describeContents(), actual.describeContents(), "reader contents");
        equal(expected.hashCode(), actual.hashCode(), "reader hash");
        check(expected.equals(actual) && actual.equals(expected), "reader value equality");
        if (expected instanceof WifiAvailableChannel) {
            WifiAvailableChannel first = (WifiAvailableChannel) expected, second = (WifiAvailableChannel) actual;
            equal(first.getFrequencyMhz(), second.getFrequencyMhz(), "reader frequency");
            equal(first.getOperationalModes(), second.getOperationalModes(), "reader modes");
            equal(first.getChannelWidth(), second.getChannelWidth(), "reader width");
        } else if (expected instanceof BlockingOption) {
            BlockingOption first = (BlockingOption) expected, second = (BlockingOption) actual;
            equal(first.getBlockingTimeSeconds(), second.getBlockingTimeSeconds(), "reader duration");
            equal(first.isBlockingBssidOnly(), second.isBlockingBssidOnly(), "reader BSSID flag");
        } else throw new AssertionError("Unlisted normal record type");
    }
    static void writeValue(Parcelable value, Parcel output) {
        if (value instanceof WifiAvailableChannel) ((WifiAvailableChannel) value).writeToParcel(output, 0);
        else if (value instanceof BlockingOption) ((BlockingOption) value).writeToParcel(output, 0);
        else throw new AssertionError("Unlisted value writer");
    }
    static <T extends Parcelable> T wire(String name, T value, Parcelable.Creator<T> creator) throws Exception {
        Parcel written = Parcel.obtain(), input = Parcel.obtain(), rewritten = Parcel.obtain();
        try {
            writeValue(value, written);
            byte[] bytes = written.marshall();
            equal(bytes.length, written.dataPosition(), name + " complete writer extent");
            equal(value instanceof WifiAvailableChannel ? 12 : 8, bytes.length, name + " primitive record extent");
            input.unmarshall(bytes, 0, bytes.length); input.setDataPosition(0);
            T copy = creator.createFromParcel(input);
            equal(0, input.dataAvail(), name + " complete original reader");
            check(copy != value, name + " fresh value");
            sameFields(value, copy);
            writeValue(copy, rewritten);
            byte[] again = rewritten.marshall();
            check(Arrays.equals(bytes, again), name + " original reader/writer rewrite");
            fullReaders++;
            System.out.println("WIFI_CHANNEL_BLOCKING_PARCEL " + name + " bytes=" + bytes.length
                    + " sha256=" + digest(bytes) + " rewrite_sha256=" + digest(again)
                    + " roundtrip=true reader=" + copy.getClass().getName());
            return copy;
        } finally { written.recycle(); input.recycle(); rewritten.recycle(); }
    }
    static void channel(boolean parcel) throws Exception {
        String[] names = {"OP_MODE_STA", "OP_MODE_SAP", "OP_MODE_WIFI_DIRECT_CLI",
                "OP_MODE_WIFI_DIRECT_GO", "OP_MODE_WIFI_AWARE", "OP_MODE_TDLS"};
        int[] modes = {WifiAvailableChannel.OP_MODE_STA, WifiAvailableChannel.OP_MODE_SAP,
                WifiAvailableChannel.OP_MODE_WIFI_DIRECT_CLI, WifiAvailableChannel.OP_MODE_WIFI_DIRECT_GO,
                WifiAvailableChannel.OP_MODE_WIFI_AWARE, WifiAvailableChannel.OP_MODE_TDLS};
        for (int i = 0; i < modes.length; i++) {
            equal(1 << i, modes[i], "SDK constant " + names[i]);
            equal(modes[i], WifiAvailableChannel.class.getField(names[i]).getInt(null), "runtime constant " + names[i]);
        }
        WifiAvailableChannel sta = new WifiAvailableChannel(2412, modes[0]);
        WifiAvailableChannel all = new WifiAvailableChannel(5180, 63);
        WifiAvailableChannel direct = new WifiAvailableChannel(5805, modes[2] | modes[3]);
        equal(2412, sta.getFrequencyMhz(), "frequency"); equal(1, sta.getOperationalModes(), "mode");
        equal(0, sta.getChannelWidth(), "two-argument constructor width");
        equal(0, sta.describeContents(), "channel contents");
        equal("mFrequency = 5180, mChannelWidth = 0, mOpModes = 3f", all.toString(), "lowercase hex modes");
        equal(Objects.hash(5180, 63, 0), all.hashCode(), "frequency/modes/width hash order");
        check(sta.equals(sta) && sta.equals(new WifiAvailableChannel(2412, 1)), "channel equality");
        check(!sta.equals(null) && !sta.equals(new Object()), "channel null and type equality");
        check(!sta.equals(new WifiAvailableChannel(2413, 1)), "frequency participates in equality");
        check(!sta.equals(new WifiAvailableChannel(2412, 2)), "modes participate in equality");
        if (!parcel) {
            WifiAvailableChannel unvalidated = new WifiAvailableChannel(-1, Integer.MIN_VALUE);
            equal(-1, unvalidated.getFrequencyMhz(), "constructor has no frequency validation");
            equal(Integer.MIN_VALUE, unvalidated.getOperationalModes(), "constructor has no modes validation");
        }
        creatorArrays(WifiAvailableChannel.CREATOR, WifiAvailableChannel.class);
        if (parcel) {
            wire("channel_sta20", sta, WifiAvailableChannel.CREATOR);
            wire("channel_all20", all, WifiAvailableChannel.CREATOR);
            wire("channel_direct20", direct, WifiAvailableChannel.CREATOR);
        }
    }
    static void blocking(boolean parcel) throws Exception {
        BlockingOption.Builder builder = new BlockingOption.Builder(3600);
        BlockingOption before = builder.build();
        check(builder.setBlockingBssidOnly(true) == builder, "setter returns original builder");
        BlockingOption snapshot = builder.build(); builder.setBlockingBssidOnly(false);
        equal(3600, snapshot.getBlockingTimeSeconds(), "duration snapshot");
        check(!before.isBlockingBssidOnly() && snapshot.isBlockingBssidOnly()
                && !builder.build().isBlockingBssidOnly(), "builder boolean snapshots");
        check(snapshot != before && snapshot != builder.build(), "fresh build objects");
        equal(0, before.describeContents(), "blocking contents");
        equal("BlockingOption[ DisableTime=3600, BSSIDOnly=true", snapshot.toString(), "original string without closing bracket");
        equal(Objects.hash(true, 3600), snapshot.hashCode(), "boolean/duration hash order");
        check(snapshot.equals(snapshot) && snapshot.equals(new BlockingOption.Builder(3600).setBlockingBssidOnly(true).build()), "blocking equality");
        check(!snapshot.equals(null) && !snapshot.equals(new Object()), "blocking null and type equality");
        check(!snapshot.equals(before), "flag participates in equality");
        check(!snapshot.equals(new BlockingOption.Builder(3599).setBlockingBssidOnly(true).build()), "duration participates in equality");
        for (int invalid : new int[] {-1, 0, 86401, Integer.MIN_VALUE, Integer.MAX_VALUE})
            rejects(IllegalArgumentException.class, "blockingTimeSec should between 1 to 86400",
                    () -> new BlockingOption.Builder(invalid), "eager builder duration validation");
        creatorArrays(BlockingOption.CREATOR, BlockingOption.class);
        BlockingOption minNetwork = new BlockingOption.Builder(1).build();
        BlockingOption minBssid = new BlockingOption.Builder(1).setBlockingBssidOnly(true).build();
        BlockingOption maxNetwork = new BlockingOption.Builder(86400).build();
        BlockingOption maxBssid = new BlockingOption.Builder(86400).setBlockingBssidOnly(true).build();
        equal(1, minNetwork.getBlockingTimeSeconds(), "inclusive minimum");
        equal(86400, maxBssid.getBlockingTimeSeconds(), "inclusive maximum");
        check(!minNetwork.isBlockingBssidOnly() && !maxNetwork.isBlockingBssidOnly(), "default whole-network flag");
        check(minBssid.isBlockingBssidOnly() && maxBssid.isBlockingBssidOnly(), "explicit BSSID flag");
        if (parcel) {
            wire("blocking_min_network", minNetwork, BlockingOption.CREATOR);
            wire("blocking_min_bssid", minBssid, BlockingOption.CREATOR);
            wire("blocking_max_network", maxNetwork, BlockingOption.CREATOR);
            wire("blocking_max_bssid", maxBssid, BlockingOption.CREATOR);
            wire("blocking_snapshot", snapshot, BlockingOption.CREATOR);
        }
    }
    public static void baselineControl() { new WifiAvailableChannel(2412, 1); }
    public static void run(boolean parcel) throws Exception {
        checks = 0; fullReaders = 0;
        channel(parcel); blocking(parcel);
    }
    public static void finish(boolean parcel) {
        equal(parcel ? 16 : 0, fullReaders, "complete normal-record corpus");
        System.out.println("WIFI_CHANNEL_BLOCKING_VALUES: PASS checks=" + checks
                + " nativeParcel=" + parcel + " fullReaders=" + fullReaders);
    }
    static void productionGraph() throws Exception {
        if (Build.VERSION.SDK_INT != 36) throw new AssertionError("Exact SDK36 production profile required");
        ClassLoader boot = Parcel.class.getClassLoader();
        if (boot != null && !boot.getClass().getName().endsWith("BootClassLoader"))
            throw new AssertionError("Genuine production boot Parcel required");
        for (Class<?> type : new Class<?>[] {WifiAvailableChannel.class, BlockingOption.class,
                BlockingOption.Builder.class, Parcelable.class, Build.class})
            if (type.getClassLoader() != boot) throw new AssertionError("Non-production value provider: " + type);
        for (Class<?> type : new Class<?>[] {WifiAvailableChannel.class, BlockingOption.class})
            if (type.getField("CREATOR").get(null).getClass().getClassLoader() != boot)
                throw new AssertionError("Non-production original reader: " + type);
        System.out.println("WIFI_CHANNEL_BLOCKING_PROVIDERS coherent production boot graph; complete original readers");
        System.out.println("WIFI_CHANNEL_BLOCKING_PROFILE android16-r4; sdk36; declarations=27");
    }
    public static void main(String[] args) throws Exception {
        boolean parcel = args.length == 0 || !args[0].equals("--no-parcel");
        if (parcel) productionGraph();
        run(parcel);
        Class.forName("gesher.services.WifiChannelBlockingValuesExtendedTest")
                .getMethod("run", boolean.class).invoke(null, parcel);
        finish(parcel);
    }
}
