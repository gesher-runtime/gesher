/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package gesher.services;

import android.net.ConnectivityManager;

import com.gesher.net.WindowsNetworks;

/**
 * Metering and the restrict-background status through the production natives on ART. Windows
 * reports the connection's cost (metered, roaming; the network snapshot's net lines), which
 * is real. Gesher enforces no background-data restriction, so the snapshot carries no
 * "background" line and ConnectivityManager answers RESTRICT_BACKGROUND_STATUS_DISABLED, as
 * on a device without Data Saver. Read only.
 * Prints "RestrictBackgroundTest all passed".
 */
public final class RestrictBackgroundTest {
    private static void require(boolean condition, String message) {
        if (!condition) throw new AssertionError(message);
    }

    public static void main(String[] args) {
        String text = WindowsNetworks.snapshot();
        require(text != null, "no snapshot");
        for (String line : text.split("\n")) {
            require(!line.startsWith("background\t"), "the snapshot has a background line: " + line);
        }
        WindowsNetworks.Snapshot s = WindowsNetworks.parse(text);
        // A snapshot that carries the line (an older native) is not read either.
        WindowsNetworks.Snapshot legacy = WindowsNetworks.parse(text + "background\t1\n");
        require(legacy.networks.size() == s.networks.size(), "the legacy line changes the networks");
        ConnectivityManager manager = new ConnectivityManager(null);
        int status = manager.getRestrictBackgroundStatus();
        require(status == ConnectivityManager.RESTRICT_BACKGROUND_STATUS_DISABLED,
                "getRestrictBackgroundStatus " + status);
        System.out.println("RestrictBackgroundTest: getRestrictBackgroundStatus " + status
                + "; default network metered "
                + (s.defaultNetwork() == null ? "(none)" : String.valueOf(s.defaultNetwork().metered)));
        System.out.println("RestrictBackgroundTest all passed");
        System.out.flush();
    }
}
