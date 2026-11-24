/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package gesher.services;

import android.net.ConnectivityManager;

import com.gesher.net.WindowsNetworks;

/**
 * Windows' background-data restriction through the production natives on ART: the
 * network snapshot (compat/net/windows_networks.cc) has a "background" line exactly when
 * Windows could say, and WindowsNetworks maps it to ConnectivityManager's
 * restrict-background status (ENABLED or DISABLED, never WHITELISTED). Read only.
 * Prints "RestrictBackgroundTest all passed".
 */
public final class RestrictBackgroundTest {
    private static void require(boolean condition, String message) {
        if (!condition) throw new AssertionError(message);
    }

    public static void main(String[] args) {
        String text = WindowsNetworks.snapshot();
        require(text != null, "no snapshot");
        String line = null;
        for (String l : text.split("\n")) {
            if (l.startsWith("background\t")) line = l;
        }
        WindowsNetworks.Snapshot s = WindowsNetworks.parse(text);
        require((line == null) == (s.backgroundRestricted == null),
                "background line " + line + " parsed as " + s.backgroundRestricted);
        int status = s.restrictBackgroundStatus();
        require(status == (Boolean.TRUE.equals(s.backgroundRestricted)
                        ? ConnectivityManager.RESTRICT_BACKGROUND_STATUS_ENABLED
                        : ConnectivityManager.RESTRICT_BACKGROUND_STATUS_DISABLED),
                "status " + status + " for " + s.backgroundRestricted);
        System.out.println("RestrictBackgroundTest: Windows background data "
                + (s.backgroundRestricted == null ? "unknown" : s.backgroundRestricted ? "restricted" : "not restricted")
                + ", getRestrictBackgroundStatus " + status + "; default network metered "
                + (s.defaultNetwork() == null ? "(none)" : String.valueOf(s.defaultNetwork().metered)));
        System.out.println("RestrictBackgroundTest all passed");
        System.out.flush();
    }
}
