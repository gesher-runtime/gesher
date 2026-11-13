/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package gesher.services;

import android.net.EthernetNetworkSpecifier;
import android.net.NetworkCapabilities;
import android.net.NetworkRequest;
import android.net.NetworkSpecifier;
import android.net.TelephonyNetworkSpecifier;
import android.os.Parcel;
import android.os.Parcelable;
import java.util.Arrays;

/** SDK-only callers. Native Parcel checks run only on the actual Android runtime. */
public final class NetworkBuilderContractTest {
    private static int checks;
    private static final int TEST = 7; // Android 16's module-only TRANSPORT_TEST.
    private static void check(boolean value, String message) {
        if (!value) throw new AssertionError(message);
        checks++;
    }
    private static void rejects(Class<? extends Throwable> type, Runnable action, String message) {
        try { action.run(); }
        catch (Throwable failure) { check(type.isInstance(failure), message + ": " + failure); return; }
        throw new AssertionError(message + ": no exception");
    }
    private static NetworkRequest.Builder ethernet() {
        return new NetworkRequest.Builder().addTransportType(NetworkCapabilities.TRANSPORT_ETHERNET);
    }
    private static void bounds() {
        NetworkRequest.Builder builder = new NetworkRequest.Builder().addCapability(0).addCapability(37);
        NetworkRequest before = builder.build();
        for (int value : new int[]{64, -1, 38, 63, Integer.MIN_VALUE, Integer.MAX_VALUE}) {
            check(builder.addCapability(value) == builder, "invalid capability add is chainable " + value);
            check(Arrays.equals(before.getCapabilities(), builder.build().getCapabilities()),
                    "invalid capability add preserves bits " + value);
            check(builder.removeCapability(value) == builder, "invalid capability remove is chainable " + value);
            check(Arrays.equals(before.getCapabilities(), builder.build().getCapabilities()),
                    "invalid capability remove preserves bits " + value);
            check(!builder.build().hasCapability(value), "invalid capability query " + value);
        }
        check(builder.build().hasCapability(0) && builder.build().hasCapability(37), "both capability bounds remain present");
        builder.addTransportType(0).addTransportType(10);
        for (int value : new int[]{64, -1, 11, 63, Integer.MIN_VALUE, Integer.MAX_VALUE}) {
            rejects(IllegalArgumentException.class, () -> builder.addTransportType(value), "invalid transport add " + value);
            rejects(IllegalArgumentException.class, () -> builder.removeTransportType(value), "invalid transport remove " + value);
            check(Arrays.equals(builder.build().getTransportTypes(), new int[]{0, 10}), "invalid transport preserves valid bits " + value);
            check(!builder.build().hasTransport(value), "invalid transport query " + value);
        }
    }
    private static void specifiers() {
        EthernetNetworkSpecifier specifier = new EthernetNetworkSpecifier("eth0");
        rejects(IllegalStateException.class, () -> new NetworkRequest.Builder().setNetworkSpecifier(specifier), "specifier needs a transport");
        for (int transport = 0; transport <= 10; transport++) {
            NetworkRequest request = new NetworkRequest.Builder().addTransportType(transport).setNetworkSpecifier(specifier).build();
            check(request.getNetworkSpecifier() == specifier, "single transport " + transport);
            request = new NetworkRequest.Builder().addTransportType(TEST).addTransportType(transport).setNetworkSpecifier(specifier).build();
            check(request.getNetworkSpecifier() == specifier, "test and transport " + transport);
        }
        for (boolean test : new boolean[]{false, true}) {
            NetworkRequest.Builder builder = new NetworkRequest.Builder().addTransportType(0).addTransportType(10);
            if (test) builder.addTransportType(TEST);
            check(builder.setNetworkSpecifier(specifier).build().getNetworkSpecifier() == specifier,
                    "cellular and satellite, with test=" + test);
        }
        rejects(IllegalStateException.class, () -> ethernet().addTransportType(1).setNetworkSpecifier(specifier), "incompatible two transports");
        rejects(IllegalStateException.class, () -> ethernet().addTransportType(1).addTransportType(TEST).setNetworkSpecifier(specifier),
                "test does not permit two unrelated transports");
        NetworkRequest.Builder changed = ethernet().setNetworkSpecifier(specifier);
        NetworkRequest snapshot = changed.build();
        rejects(IllegalStateException.class, () -> changed.addTransportType(1), "adding transport revalidates specifier");
        check(changed.build().hasTransport(1), "Android validates after mutating the transport");
        check(changed.build().getNetworkSpecifier() == specifier, "failed transport change retains specifier");
        check(!snapshot.hasTransport(1), "previous build is detached");
        changed.setNetworkSpecifier((NetworkSpecifier)null);
        check(changed.build().getNetworkSpecifier() == null, "null repairs multi-transport request");
        NetworkRequest.Builder removed = ethernet().setNetworkSpecifier(specifier);
        rejects(IllegalStateException.class, () -> removed.removeTransportType(3), "removing final transport revalidates specifier");
        check(removed.build().getTransportTypes().length == 0, "failed remove preserves Android mutation order");
        check(removed.clearCapabilities().build().getNetworkSpecifier() == null, "clear removes specifier");
    }
    private static void legacySpecifiers() {
        for (String value : new String[]{"0", "-1", "+17", "2147483647"}) {
            NetworkSpecifier converted = new NetworkRequest.Builder().addTransportType(0).setNetworkSpecifier(value).build().getNetworkSpecifier();
            check(converted instanceof TelephonyNetworkSpecifier, "numeric legacy specifier type " + value);
            check(((TelephonyNetworkSpecifier)converted).getSubscriptionId() == Integer.parseInt(value), "numeric legacy subscription " + value);
        }
        rejects(IllegalArgumentException.class,
                () -> new NetworkRequest.Builder().addTransportType(0).setNetworkSpecifier("-2147483648"),
                "legacy subscription cannot use the uninitialized Telephony builder sentinel");
        for (String value : new String[]{"eth0", "2147483648", " 17", "ממשק-רשת"}) {
            NetworkSpecifier converted = ethernet().setNetworkSpecifier(value).build().getNetworkSpecifier();
            check(converted instanceof EthernetNetworkSpecifier, "interface legacy specifier type " + value);
            check(((EthernetNetworkSpecifier)converted).getInterfaceName().equals(value), "interface legacy name " + value);
        }
        NetworkSpecifier test = new NetworkRequest.Builder().addTransportType(TEST).setNetworkSpecifier("test0").build().getNetworkSpecifier();
        check(test.getClass().getName().equals("android.net.TestNetworkSpecifier"), "test legacy specifier type");
        check(new NetworkRequest.Builder().setNetworkSpecifier((String)null).build().getNetworkSpecifier() == null, "null legacy specifier without transport");
        check(new NetworkRequest.Builder().setNetworkSpecifier("").build().getNetworkSpecifier() == null, "empty legacy specifier without transport");
        check(ethernet().setNetworkSpecifier("eth0").setNetworkSpecifier("").build().getNetworkSpecifier() == null, "empty clears existing specifier");
    }
    private static void ethernetValues() {
        rejects(IllegalArgumentException.class, () -> new EthernetNetworkSpecifier(null), "null Ethernet name");
        rejects(IllegalArgumentException.class, () -> new EthernetNetworkSpecifier(""), "empty Ethernet name");
        EthernetNetworkSpecifier first = new EthernetNetworkSpecifier("eth0"), equal = new EthernetNetworkSpecifier("eth0"), other = new EthernetNetworkSpecifier("eth1");
        check(first.getInterfaceName().equals("eth0"), "Ethernet getter");
        check(first.equals(equal) && equal.equals(first) && first.hashCode() == equal.hashCode(), "Ethernet equality and hash");
        check(!first.equals(other) && !first.equals(null) && !first.equals("eth0"), "Ethernet unequal values");
        check(first.canBeSatisfiedBy(equal) && !first.canBeSatisfiedBy(other) && !first.canBeSatisfiedBy(null), "Ethernet request matching");
        check(!first.canBeSatisfiedBy(new TelephonyNetworkSpecifier.Builder().setSubscriptionId(1).build()), "Ethernet rejects other family");
        check(first.describeContents() == 0 && EthernetNetworkSpecifier.CREATOR.newArray(3).length == 3, "Ethernet Parcelable metadata");
        check(first.toString().contains("eth0"), "Ethernet diagnostic interface");
    }
    private static <T extends Parcelable> T roundTrip(T value, Parcelable.Creator<T> creator) {
        Parcel parcel = Parcel.obtain();
        try {
            value.writeToParcel(parcel, 0); parcel.setDataPosition(0);
            T copy = creator.createFromParcel(parcel);
            check(parcel.dataAvail() == 0, "complete Parcel consumed");
            return copy;
        } finally { parcel.recycle(); }
    }
    private static void parcels() {
        EthernetNetworkSpecifier ethernet = new EthernetNetworkSpecifier("eth-שלום");
        EthernetNetworkSpecifier copy = roundTrip(ethernet, EthernetNetworkSpecifier.CREATOR);
        check(ethernet.equals(copy) && ethernet.hashCode() == copy.hashCode(), "native Ethernet Parcel");
        for (NetworkRequest request : new NetworkRequest[]{ethernet().setNetworkSpecifier(ethernet).build(),
                new NetworkRequest.Builder().addTransportType(0).setNetworkSpecifier("123").build(),
                new NetworkRequest.Builder().addTransportType(TEST).setNetworkSpecifier("test0").build()}) {
            NetworkRequest copied = roundTrip(request, NetworkRequest.CREATOR);
            check(request.equals(copied) && request.hashCode() == copied.hashCode(),
                    "native request Parcel retains " + request.getNetworkSpecifier().getClass().getSimpleName());
        }
    }
    public static void contracts(String selected, boolean nativeParcel) {
        checks = 0;
        if (selected.equals("all") || selected.equals("bounds")) bounds();
        if (selected.equals("all") || selected.equals("specifiers")) specifiers();
        if (selected.equals("all") || selected.equals("legacy")) legacySpecifiers();
        if (selected.equals("all") || selected.equals("ethernet")) ethernetValues();
        if (nativeParcel) parcels();
        System.out.println("NetworkBuilderContractTest all passed: " + checks + ", parcel=" + nativeParcel);
    }
    public static void main(String[] args) {
        System.loadLibrary("android_runtime");
        contracts(args.length == 0 ? "all" : args[0], true);
    }
}
