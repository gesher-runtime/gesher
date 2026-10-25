/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package android.net;

import java.net.InetAddress;

/**
 * The Connectivity module's InetAddresses (framework.jar uses it and does not contain it):
 * numeric addresses through libcore, which never looks a name up for them.
 */
public final class InetAddresses {
    private InetAddresses() {
    }

    public static boolean isNumericAddress(String address) {
        return libcore.net.InetAddressUtils.isNumericAddress(address);
    }

    public static InetAddress parseNumericAddress(String address) {
        return libcore.net.InetAddressUtils.parseNumericAddress(address);
    }
}
