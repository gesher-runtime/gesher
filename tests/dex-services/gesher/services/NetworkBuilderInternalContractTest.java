/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package gesher.services;

import android.net.EthernetNetworkSpecifier;
import android.net.MacAddress;
import android.net.MatchAllNetworkSpecifier;
import android.net.NetworkCapabilities;
import android.net.NetworkRequest;
import android.net.TestNetworkSpecifier;
import android.os.Parcel;
import java.util.Arrays;

/** Hidden module entrypoints used by the runtime; distinct from SDK-only callers. */
public final class NetworkBuilderInternalContractTest {
    private static int checks;
    private static void check(boolean value, String message) {
        if (!value) throw new AssertionError(message);
        checks++;
    }
    private static void rejects(Class<? extends Throwable> type, Runnable action, String message) {
        try { action.run(); }
        catch (Throwable failure) { check(type.isInstance(failure), message + ": " + failure); return; }
        throw new AssertionError(message + ": no exception");
    }
    private static void rawCapabilities() {
        NetworkCapabilities caps = new NetworkCapabilities();
        caps.clearAll();
        caps.addCapability(0).addCapability(37).addTransportType(0).addTransportType(10);
        for (int value : new int[]{64, -1, 38, 63, Integer.MIN_VALUE, Integer.MAX_VALUE}) {
            caps.addCapability(value).removeCapability(value);
            check(Arrays.equals(caps.getCapabilities(), new int[]{0, 37}), "raw invalid cap preserves bits " + value);
            check(!caps.hasCapability(value), "raw invalid cap query " + value);
        }
        for (int value : new int[]{64, -1, 11, 63, Integer.MIN_VALUE, Integer.MAX_VALUE}) {
            rejects(IllegalArgumentException.class, () -> caps.addTransportType(value), "raw invalid transport add");
            rejects(IllegalArgumentException.class, () -> caps.removeTransportType(value), "raw invalid transport remove");
            check(Arrays.equals(caps.getTransportTypes(), new int[]{0, 10}), "raw transports unchanged");
            check(!caps.hasTransport(value), "raw invalid transport query");
        }
        NetworkRequest.Builder request = new NetworkRequest.Builder().addTransportType(3)
                .setNetworkSpecifier(new EthernetNetworkSpecifier("eth0"));
        rejects(IllegalArgumentException.class, () -> request.setNetworkSpecifier(new MatchAllNetworkSpecifier()), "request rejects MatchAll");
        check(request.build().getNetworkSpecifier().equals(new EthernetNetworkSpecifier("eth0")), "rejected MatchAll preserves prior specifier");
        caps.setNetworkSpecifier(new MatchAllNetworkSpecifier());
        check(caps.getNetworkSpecifier() instanceof MatchAllNetworkSpecifier, "capability offer may use MatchAll on supported pair");
    }
    private static void enterprise() {
        NetworkCapabilities caps = new NetworkCapabilities();
        check(caps.getEnterpriseIds().length == 0 && !caps.hasEnterpriseId(1), "ordinary network has no enterprise ID");
        caps.addCapability(NetworkCapabilities.NET_CAPABILITY_ENTERPRISE);
        check(Arrays.equals(caps.getEnterpriseIds(), new int[]{1}) && caps.hasEnterpriseId(1), "implicit enterprise ID one");
        int[] ids = caps.getEnterpriseIds(); ids[0] = 5;
        check(Arrays.equals(caps.getEnterpriseIds(), new int[]{1}), "enterprise getter is detached");
        for (int value : new int[]{-1, 0, 2, 5, 6, 33, 65, Integer.MIN_VALUE, Integer.MAX_VALUE}) {
            check(!caps.hasEnterpriseId(value), "nondefault enterprise ID " + value);
        }
        NetworkCapabilities copy = new NetworkCapabilities(caps);
        caps.removeCapability(NetworkCapabilities.NET_CAPABILITY_ENTERPRISE);
        check(caps.getEnterpriseIds().length == 0 && !caps.hasEnterpriseId(1), "removing enterprise capability clears implied ID");
        check(copy.hasEnterpriseId(1), "copy retains enterprise implication");
        copy.clearAll();
        check(copy.getEnterpriseIds().length == 0, "clear removes enterprise implication");
    }
    private static void values(boolean nativeParcel) {
        MacAddress address = MacAddress.fromString("02:11:22:33:44:55");
        EthernetNetworkSpecifier full = new EthernetNetworkSpecifier("eth0", address);
        EthernetNetworkSpecifier byName = new EthernetNetworkSpecifier("eth0"), byMac = new EthernetNetworkSpecifier(null, address);
        check(byName.canBeSatisfiedBy(full) && byMac.canBeSatisfiedBy(full), "Ethernet optional match keys");
        check(!full.canBeSatisfiedBy(byName) && !full.canBeSatisfiedBy(byMac), "Ethernet matching is asymmetric");
        check(!byMac.canBeSatisfiedBy(new EthernetNetworkSpecifier("eth0", MacAddress.fromString("02:11:22:33:44:56"))), "Ethernet MAC mismatch");
        check(full.getMacAddress().equals(address), "Ethernet MAC getter");
        rejects(IllegalArgumentException.class, () -> new EthernetNetworkSpecifier(null, null), "no Ethernet key");
        rejects(IllegalArgumentException.class, () -> new TestNetworkSpecifier(null), "null test interface");
        rejects(IllegalArgumentException.class, () -> new TestNetworkSpecifier(""), "empty test interface");
        TestNetworkSpecifier test = new TestNetworkSpecifier("test0"), equal = new TestNetworkSpecifier("test0");
        check(test.equals(equal) && test.hashCode() == equal.hashCode(), "test interface equality/hash");
        check(test.canBeSatisfiedBy(equal) && !test.canBeSatisfiedBy(new TestNetworkSpecifier("test1")), "test interface matching");
        check(!test.canBeSatisfiedBy(byName) && !test.canBeSatisfiedBy(null), "test rejects other families");
        check(test.getInterfaceName().equals("test0") && test.describeContents() == 0 && TestNetworkSpecifier.CREATOR.newArray(2).length == 2,
                "test interface value API");
        if (nativeParcel) {
            Parcel parcel = Parcel.obtain();
            try {
                full.writeToParcel(parcel, 0); test.writeToParcel(parcel, 0); parcel.setDataPosition(0);
                check(full.equals(EthernetNetworkSpecifier.CREATOR.createFromParcel(parcel)), "Ethernet MAC native Parcel");
                check(test.equals(TestNetworkSpecifier.CREATOR.createFromParcel(parcel)) && parcel.dataAvail() == 0,
                        "test native Parcel, fully consumed");
            } finally { parcel.recycle(); }
        }
    }
    public static void contracts(String selected, boolean nativeParcel) {
        checks = 0;
        if (selected.equals("all") || selected.equals("raw")) rawCapabilities();
        if (selected.equals("all") || selected.equals("enterprise")) enterprise();
        if (selected.equals("all") || selected.equals("values")) values(nativeParcel);
        System.out.println("NetworkBuilderInternalContractTest all passed: " + checks + ", parcel=" + nativeParcel);
    }
    public static void main(String[] args) {
        System.loadLibrary("android_runtime");
        contracts(args.length == 0 ? "all" : args[0], true);
    }
}
