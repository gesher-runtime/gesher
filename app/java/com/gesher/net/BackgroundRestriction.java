/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.net;

import android.net.ConnectivityManager;

/**
 * Windows' background-data restriction as Android's restrict-background status
 * (ConnectivityManager.getRestrictBackgroundStatus). The network snapshot
 * (compat/net/windows_networks.cc) has a "background" line with
 * ConnectionCost.BackgroundDataUsageRestricted of the internet connection: Windows' data
 * limit setting "Background data", which Windows' guidance describes as "Restrict
 * background-only telemetry and sync; allow user foreground actions", the advice Android
 * gives with RESTRICT_BACKGROUND_STATUS_ENABLED.
 */
public final class BackgroundRestriction {
    private BackgroundRestriction() {
    }

    /** The snapshot line's value: TRUE ("1"), FALSE ("0"), null (anything else: unknown). */
    public static Boolean parse(String value) {
        if ("1".equals(value)) return Boolean.TRUE;
        if ("0".equals(value)) return Boolean.FALSE;
        return null;
    }

    /**
     * ENABLED while Windows restricts background data (Gesher has no per-app "unrestricted
     * data" allowance, so never WHITELISTED); DISABLED when it does not, or could not say.
     */
    public static int status(Boolean restricted) {
        return Boolean.TRUE.equals(restricted)
                ? ConnectivityManager.RESTRICT_BACKGROUND_STATUS_ENABLED
                : ConnectivityManager.RESTRICT_BACKGROUND_STATUS_DISABLED;
    }
}
