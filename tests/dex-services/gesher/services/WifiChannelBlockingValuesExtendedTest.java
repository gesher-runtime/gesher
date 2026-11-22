/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package gesher.services;

import android.net.wifi.BlockingOption;
import android.net.wifi.WifiAvailableChannel;
import android.os.Parcel;
import android.os.Parcelable;
import java.util.Objects;
import static gesher.services.WifiChannelBlockingValuesContractTest.*;

/** Complete source-level hidden constructors and filters, separate from the SDK caller. */
public final class WifiChannelBlockingValuesExtendedTest {
    public static void run(boolean parcel) throws Exception {
        equal(0, WifiAvailableChannel.FILTER_REGULATORY, "regulatory filter");
        equal(1, WifiAvailableChannel.FILTER_CELLULAR_COEXISTENCE, "cellular filter");
        equal(2, WifiAvailableChannel.FILTER_CONCURRENCY, "concurrency filter");
        equal(4, WifiAvailableChannel.FILTER_NAN_INSTANT_MODE, "instant filter");
        equal(3, WifiAvailableChannel.getUsableFilter(), "usable filter excludes NAN instant bit");
        check(WifiAvailableChannel.OpMode.class.isAnnotation() && WifiAvailableChannel.Filter.class.isAnnotation(),
                "complete nested annotation declarations");
        String[] names = {"channel_width40", "channel_width80", "channel_width160", "channel_width80p80", "channel_width320"};
        int[] frequencies = {2422, 5210, 5250, 5210, 6105};
        for (int i = 0; i < names.length; i++) {
            int width = i + 1;
            WifiAvailableChannel value = new WifiAvailableChannel(frequencies[i], 3, width);
            equal(width, value.getChannelWidth(), "hidden constructor retains width");
            equal(frequencies[i], value.getFrequencyMhz(), "hidden constructor frequency");
            equal(3, value.getOperationalModes(), "hidden constructor modes");
            equal(Objects.hash(frequencies[i], 3, width), value.hashCode(), "width participates in hash");
            check(!value.equals(new WifiAvailableChannel(frequencies[i], 3)), "width participates in equality");
            if (parcel) wire(names[i], value, WifiAvailableChannel.CREATOR);
        }
        if (!parcel) {
            WifiAvailableChannel unusual = new WifiAvailableChannel(-7, -9, Integer.MAX_VALUE);
            equal(-7, unusual.getFrequencyMhz(), "no hidden frequency validation");
            equal(-9, unusual.getOperationalModes(), "no hidden mode validation");
            equal(Integer.MAX_VALUE, unusual.getChannelWidth(), "no hidden width validation");
            equal(0, new BlockingOption(0, false).getBlockingTimeSeconds(), "direct constructor bypasses builder restriction");
            equal(-1, new BlockingOption(-1, true).getBlockingTimeSeconds(), "direct constructor retains signed int");
        }
        BlockingOption network = new BlockingOption(42, false), bssid = new BlockingOption(42, true);
        equal(42, network.getBlockingTimeSeconds(), "direct duration");
        check(!network.isBlockingBssidOnly() && bssid.isBlockingBssidOnly(), "direct boolean");
        if (parcel) {
            wire("blocking_direct_network", network, BlockingOption.CREATOR);
            wire("blocking_direct_bssid", bssid, BlockingOption.CREATOR);
            // Exercise the original public source-level Parcel constructor itself.
            Parcelable.Creator<BlockingOption> direct = new Parcelable.Creator<BlockingOption>() {
                public BlockingOption createFromParcel(Parcel in) { return new BlockingOption(in); }
                public BlockingOption[] newArray(int size) { return new BlockingOption[size]; }
            };
            wire("blocking_parcel_constructor", new BlockingOption(900, true), direct);
        }
    }
}
