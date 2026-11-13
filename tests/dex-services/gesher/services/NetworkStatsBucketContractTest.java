/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package gesher.services;

import android.app.usage.NetworkStats;
import java.lang.reflect.Field;
import java.lang.reflect.Modifier;

/** Public SDK call sites: also compiled separately against SDK 36 on the host. */
public final class NetworkStatsBucketContractTest {
    private static int checks;

    private static void require(boolean value, String reason) {
        if (!value) throw new AssertionError(reason);
        ++checks;
    }

    private static void constant(String name, int expected) throws Exception {
        // Reflection is deliberate: javac inlines SDK constants, which alone
        // would not detect an absent or differently valued runtime field.
        Field field = NetworkStats.Bucket.class.getField(name);
        require(field.getType() == int.class, name + " type");
        require(Modifier.isPublic(field.getModifiers())
                && Modifier.isStatic(field.getModifiers())
                && Modifier.isFinal(field.getModifiers()), name + " modifiers");
        require(field.getInt(null) == expected, name + " value");
    }

    public static void run() throws Exception {
        checks = 0;
        NetworkStats.Bucket bucket = new NetworkStats.Bucket();
        require(bucket.getTag() == 0, "new bucket tag");
        require(bucket.getUid() == 0, "AOSP new bucket UID");
        require(bucket.getState() == 0, "new bucket state");
        require(bucket.getMetered() == 0, "new bucket metered state");
        require(bucket.getRoaming() == 0, "new bucket roaming state");
        require(bucket.getDefaultNetworkStatus() == 0, "new bucket default-network state");
        require(bucket.getStartTimeStamp() == 0L, "new bucket start");
        require(bucket.getEndTimeStamp() == 0L, "new bucket end");
        require(bucket.getRxBytes() == 0L, "new bucket received bytes");
        require(bucket.getTxBytes() == 0L, "new bucket sent bytes");
        require(bucket.getRxPackets() == 0L, "new bucket received packets");
        require(bucket.getTxPackets() == 0L, "new bucket sent packets");
        constant("STATE_ALL", NetworkStats.Bucket.STATE_ALL);
        constant("STATE_DEFAULT", NetworkStats.Bucket.STATE_DEFAULT);
        constant("STATE_FOREGROUND", NetworkStats.Bucket.STATE_FOREGROUND);
        constant("UID_ALL", NetworkStats.Bucket.UID_ALL);
        constant("UID_REMOVED", NetworkStats.Bucket.UID_REMOVED);
        constant("UID_TETHERING", NetworkStats.Bucket.UID_TETHERING);
        constant("METERED_ALL", NetworkStats.Bucket.METERED_ALL);
        constant("METERED_NO", NetworkStats.Bucket.METERED_NO);
        constant("METERED_YES", NetworkStats.Bucket.METERED_YES);
        constant("ROAMING_ALL", NetworkStats.Bucket.ROAMING_ALL);
        constant("ROAMING_NO", NetworkStats.Bucket.ROAMING_NO);
        constant("ROAMING_YES", NetworkStats.Bucket.ROAMING_YES);
        constant("DEFAULT_NETWORK_ALL", NetworkStats.Bucket.DEFAULT_NETWORK_ALL);
        constant("DEFAULT_NETWORK_NO", NetworkStats.Bucket.DEFAULT_NETWORK_NO);
        constant("DEFAULT_NETWORK_YES", NetworkStats.Bucket.DEFAULT_NETWORK_YES);
        constant("TAG_NONE", NetworkStats.Bucket.TAG_NONE);
        // Public API permits subclassing the output type.
        NetworkStats.Bucket derived = new NetworkStats.Bucket() {};
        require(derived.getRxPackets() == 0L && derived.getEndTimeStamp() == 0L,
                "subclass inherits added getters");
        System.out.println("NetworkStatsBucketContractTest all passed: " + checks + " checks");
    }

    public static void main(String[] args) throws Exception {
        run();
    }
}
