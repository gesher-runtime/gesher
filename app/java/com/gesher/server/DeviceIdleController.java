/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.server;

import android.os.IDeviceIdleController;

import java.util.List;

/**
 * The desktop has no Android Doze controller or configured power allowlists.
 * Membership is separate from whether idle mode is enabled: an unrestricted
 * Windows process does not thereby acquire Android DEVICE_POWER privileges.
 *
 * <p>All reads describe that policy, including unknown packages. Administrative
 * changes fail synchronously. Gesher currently shares a uid between apps and the
 * server, so checking that uid (or an app-supplied package name) cannot establish
 * the privileged caller required by Android. These methods must not silently
 * grant exemptions until a trusted caller/host policy exists.
 */
final class DeviceIdleController extends IDeviceIdleController.Default {
    private static SecurityException denied() {
        return new SecurityException("Power allowlist administration requires a trusted "
                + "DEVICE_POWER or CHANGE_DEVICE_IDLE_TEMP_WHITELIST caller; "
                + "Gesher does not expose Android idle administration to applications");
    }

    @Override public String[] getRemovedSystemPowerWhitelistApps() { return new String[0]; }
    @Override public String[] getSystemPowerWhitelistExceptIdle() { return new String[0]; }
    @Override public String[] getSystemPowerWhitelist() { return new String[0]; }
    @Override public String[] getUserPowerWhitelist() { return new String[0]; }
    @Override public String[] getFullPowerWhitelistExceptIdle() { return new String[0]; }
    @Override public String[] getFullPowerWhitelist() { return new String[0]; }
    @Override public int[] getAppIdWhitelistExceptIdle() { return new int[0]; }
    @Override public int[] getAppIdWhitelist() { return new int[0]; }
    @Override public int[] getAppIdUserWhitelist() { return new int[0]; }
    @Override public int[] getAppIdTempWhitelist() { return new int[0]; }
    @Override public boolean isPowerSaveWhitelistExceptIdleApp(String name) { return false; }
    @Override public boolean isPowerSaveWhitelistApp(String name) { return false; }

    @Override public void addPowerSaveWhitelistApp(String name) { throw denied(); }
    @Override public int addPowerSaveWhitelistApps(List<String> names) { throw denied(); }
    @Override public void removePowerSaveWhitelistApp(String name) { throw denied(); }
    @Override public void removeSystemPowerWhitelistApp(String name) { throw denied(); }
    @Override public void restoreSystemPowerWhitelistApp(String name) { throw denied(); }
    @Override public void addPowerSaveTempWhitelistApp(String name, long duration, int userId,
            int reasonCode, String reason) { }
    @Override public long addPowerSaveTempWhitelistAppForMms(String name, int userId,
            int reasonCode, String reason) { return 10000L; }
    @Override public long addPowerSaveTempWhitelistAppForSms(String name, int userId,
            int reasonCode, String reason) { return 10000L; }
    @Override public long whitelistAppTemporarily(String name, int userId,
            int reasonCode, String reason) { return 10000L; }
    @Override public void exitIdle(String reason) { }
}
