/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package com.gesher.server;

/** Capability policy for a system server's lifetime. */
enum EditionPolicy {
    LEGACY(StoreStatus.UNCONFIGURED),
    FREE_TRIAL(StoreStatus.UNCONFIGURED),
    FREE_UNCONFIGURED(StoreStatus.UNCONFIGURED),
    FREE_UNAVAILABLE(StoreStatus.UNAVAILABLE),
    FREE_INACTIVE(StoreStatus.INACTIVE),
    PAID(StoreStatus.ACTIVE);

    static final String TRIAL_ENV = "GESHER_EDITION_TRIAL";

    enum Capability {
        WINDOWS_NOTIFICATION_DELIVERY,
        HOST_LOCATION
    }

    enum StoreStatus {
        UNCONFIGURED, UNAVAILABLE, INACTIVE, ACTIVE
    }

    private final StoreStatus mStoreStatus;

    EditionPolicy(StoreStatus storeStatus) {
        mStoreStatus = storeStatus;
    }

    StoreStatus storeStatus() {
        return mStoreStatus;
    }

    boolean upgradeAvailable() {
        return mStoreStatus == StoreStatus.INACTIVE;
    }

    /** Resolve the development trial before creating any services or accepting guest calls. */
    static EditionPolicy forSystemServer(String trial, boolean contained, String networkProfile) {
        if (trial == null) return LEGACY;
        if (!"free".equals(trial)) throw new IllegalArgumentException("Unknown edition trial");
        if (!contained || !"internet-only".equals(networkProfile)) {
            throw new IllegalStateException("Free trial requires the internet-only AppContainer test profile");
        }
        return FREE_TRIAL;
    }

    /** Resolve only a status produced by the server's verified Store authority. */
    static EditionPolicy forStoreLaunch(StoreStatus status, boolean contained, String networkProfile) {
        if (!contained || !"internet-only".equals(networkProfile)) {
            throw new IllegalStateException("Store edition requires the internet-only AppContainer profile");
        }
        return switch (status) {
            case UNCONFIGURED -> FREE_UNCONFIGURED;
            case UNAVAILABLE -> FREE_UNAVAILABLE;
            case INACTIVE -> FREE_INACTIVE;
            case ACTIVE -> PAID;
        };
    }

    boolean permits(Capability capability) {
        return switch (capability) {
            case WINDOWS_NOTIFICATION_DELIVERY, HOST_LOCATION -> this == LEGACY || this == PAID;
        };
    }
}
