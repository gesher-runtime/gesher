/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package com.gesher.server;

/** Capability policy for a system server's lifetime. */
enum EditionPolicy {
    LEGACY,
    FREE_TRIAL;

    static final String TRIAL_ENV = "GESHER_EDITION_TRIAL";

    enum Capability {
        WINDOWS_NOTIFICATION_DELIVERY,
        HOST_LOCATION
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

    boolean permits(Capability capability) {
        return switch (capability) {
            case WINDOWS_NOTIFICATION_DELIVERY, HOST_LOCATION -> this == LEGACY;
        };
    }
}
