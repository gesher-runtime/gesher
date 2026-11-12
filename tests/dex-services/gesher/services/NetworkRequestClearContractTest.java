/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package gesher.services;

import android.net.NetworkCapabilities;
import android.net.NetworkRequest;
import android.net.StringNetworkSpecifier;
import java.util.Set;

/** Request reset semantics without a radio, network connection or Parcel adapter. */
public final class NetworkRequestClearContractTest {
    private static int checks;
    private static void check(boolean value, String message) {
        if (!value) throw new AssertionError(message);
        checks++;
    }
    public static int contracts() {
        checks = 0;
        NetworkRequest.Builder builder = new NetworkRequest.Builder()
                .addCapability(NetworkCapabilities.NET_CAPABILITY_INTERNET)
                .addTransportType(NetworkCapabilities.TRANSPORT_WIFI)
                .setNetworkSpecifier(new StringNetworkSpecifier("original"))
                .setSignalStrength(-20).setSubscriptionIds(Set.of(7));
        NetworkRequest before = builder.build();
        check(builder.clearCapabilities() == builder, "clear is chainable");
        NetworkRequest cleared = builder.build();
        check(cleared.getCapabilities().length == 0, "clear includes default capabilities");
        check(cleared.getTransportTypes().length == 0, "clear removes transports");
        check(cleared.getNetworkSpecifier() == null, "clear removes network specifier");
        check(cleared.networkCapabilities.getSignalStrength()
                == NetworkCapabilities.SIGNAL_STRENGTH_UNSPECIFIED, "clear removes signal threshold");
        check(cleared.getSubscriptionIds().isEmpty(), "clear removes subscriptions");
        NetworkCapabilities any = new NetworkCapabilities();
        any.clearAll();
        check(cleared.canBeSatisfiedBy(any), "cleared request accepts an unconstrained offer");
        check(before.hasCapability(NetworkCapabilities.NET_CAPABILITY_INTERNET)
                && before.hasTransport(NetworkCapabilities.TRANSPORT_WIFI), "earlier request retains filters");
        check(before.getNetworkSpecifier() != null && before.getSubscriptionIds().equals(Set.of(7))
                && before.networkCapabilities.getSignalStrength() == -20, "earlier request remains detached");
        check(builder.clearCapabilities().build().equals(cleared), "repeated clear is stable");
        NetworkRequest reused = builder.addTransportType(NetworkCapabilities.TRANSPORT_ETHERNET)
                .addCapability(NetworkCapabilities.NET_CAPABILITY_INTERNET).build();
        check(reused.getTransportTypes().length == 1
                && reused.hasTransport(NetworkCapabilities.TRANSPORT_ETHERNET), "reuse adds only new transport");
        check(reused.getNetworkSpecifier() == null && reused.getSubscriptionIds().isEmpty(), "reuse retains no old filters");
        check(!reused.canBeSatisfiedBy(any), "new requirement remains effective");
        any.addTransportType(NetworkCapabilities.TRANSPORT_ETHERNET)
                .addCapability(NetworkCapabilities.NET_CAPABILITY_INTERNET);
        check(reused.canBeSatisfiedBy(any), "new requirement matches without old signal/subscription");
        check(cleared.getTransportTypes().length == 0 && cleared.getCapabilities().length == 0,
                "reusing builder does not mutate cleared snapshot");
        return checks;
    }
    public static void main(String[] args) {
        System.out.println("NetworkRequestClearContractTest all passed: " + contracts());
    }
}
