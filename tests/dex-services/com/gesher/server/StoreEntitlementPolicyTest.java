/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package com.gesher.server;

public final class StoreEntitlementPolicyTest {
    public static void main(String[] args) {
        check(StoreEntitlement.fromNativeStatus(0) == EditionPolicy.StoreStatus.UNCONFIGURED,
                "native status 0 must be unconfigured");
        check(StoreEntitlement.fromNativeStatus(1) == EditionPolicy.StoreStatus.UNAVAILABLE,
                "native status 1 must be unavailable");
        check(StoreEntitlement.fromNativeStatus(2) == EditionPolicy.StoreStatus.INACTIVE,
                "native status 2 must be inactive");
        check(StoreEntitlement.fromNativeStatus(3) == EditionPolicy.StoreStatus.ACTIVE,
                "native status 3 must be active");
        check(StoreEntitlement.fromNativeStatus(-1) == EditionPolicy.StoreStatus.UNAVAILABLE
                        && StoreEntitlement.fromNativeStatus(4) == EditionPolicy.StoreStatus.UNAVAILABLE,
                "unknown native result must fail closed");
        checkStatus(EditionPolicy.StoreStatus.UNCONFIGURED, EditionPolicy.FREE_UNCONFIGURED);
        checkStatus(EditionPolicy.StoreStatus.UNAVAILABLE, EditionPolicy.FREE_UNAVAILABLE);
        checkStatus(EditionPolicy.StoreStatus.INACTIVE, EditionPolicy.FREE_INACTIVE);
        checkStatus(EditionPolicy.StoreStatus.ACTIVE, EditionPolicy.PAID);
        check(EditionPolicy.forSystemServer(null, false, null) == EditionPolicy.LEGACY,
                "public launch changed with the Store release gate disabled");
        check(EditionPolicy.forSystemServer("free", true, "internet-only") == EditionPolicy.FREE_TRIAL,
                "contained free trial could not start");
        try {
            EditionPolicy.forSystemServer("paid", true, "internet-only");
            throw new AssertionError("trial input accepted a paid value");
        } catch (IllegalArgumentException expected) {
            // The trial input offers only Free.
        }
        System.out.println("Store entitlement policy passed");
    }

    private static void checkStatus(EditionPolicy.StoreStatus status, EditionPolicy expected) {
        EditionPolicy policy = EditionPolicy.forStoreLaunch(status, true, "internet-only");
        check(policy == expected && policy.storeStatus() == status, "unexpected policy for " + status);
        check(policy.upgradeAvailable() == (status == EditionPolicy.StoreStatus.INACTIVE),
                "unexpected upgrade availability for " + status);
        for (EditionPolicy.Capability capability : EditionPolicy.Capability.values()) {
            check(policy.permits(capability) == (status == EditionPolicy.StoreStatus.ACTIVE),
                    "unexpected " + capability + " access for " + status);
        }
        checkRejected(status, false, "internet-only");
        checkRejected(status, true, "unrestricted");
    }

    private static void checkRejected(EditionPolicy.StoreStatus status, boolean contained,
            String networkProfile) {
        try {
            EditionPolicy.forStoreLaunch(status, contained, networkProfile);
            throw new AssertionError("Store policy started without internet-only containment");
        } catch (IllegalStateException expected) {
            // Containment is required even for a verified grant.
        }
    }

    private static void check(boolean condition, String message) {
        if (!condition) throw new AssertionError(message);
    }
}
