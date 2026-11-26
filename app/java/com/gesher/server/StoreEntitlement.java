/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package com.gesher.server;

/** Server-local Windows Store verification. No launcher or guest input supplies a grant. */
final class StoreEntitlement {
    private StoreEntitlement() {}

    static boolean releaseEnabled() {
        return nativeReleaseEnabled();
    }

    static EditionPolicy.StoreStatus read() {
        return fromNativeStatus(nativeRead());
    }

    static EditionPolicy.StoreStatus fromNativeStatus(int status) {
        return switch (status) {
            case 0 -> EditionPolicy.StoreStatus.UNCONFIGURED;
            case 2 -> EditionPolicy.StoreStatus.INACTIVE;
            case 3 -> EditionPolicy.StoreStatus.ACTIVE;
            default -> EditionPolicy.StoreStatus.UNAVAILABLE;
        };
    }

    private static native boolean nativeReleaseEnabled();
    private static native int nativeRead();
}
