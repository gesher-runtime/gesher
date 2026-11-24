/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package com.gesher.server;

/**
 * When JobScheduler's charging, battery-not-low and device-idle constraints hold, from
 * Windows' own state: {@link WindowsHost#powerStatus()} (GetSystemPowerStatus) and
 * {@link WindowsHost#jobIdleMillis()} (the session display has been off, without input,
 * this long). {@link JobSchedulerService} and {@link DownloadQueue} (whose requests AOSP
 * runs as jobs, Helpers.scheduleJob) read both through here, so they agree.
 *
 * <p>{@code power} is {ACLineStatus, BatteryFlag, BatteryLifePercent, SystemStatusFlag};
 * null when Windows did not answer, which satisfies no constraint.
 */
final class DeviceConstraints {
    /** config_jobSchedulerInactivityIdleThreshold (core/res/res/values/config.xml). */
    static final long IDLE_AFTER_MS = 31 * 60_000;

    private DeviceConstraints() {}

    /** BatteryStateTracker.isPowerConnected: AC power is online. */
    static boolean powerConnected(int[] power) {
        return power != null && power.length >= 3 && power[0] == 1;
    }

    /** BatteryStateTracker.isBatteryNotLow: no low (2) or critical (4) battery flag. */
    static boolean batteryNotLow(int[] power) {
        return power != null && power.length >= 3 && power[1] >= 0 && power[1] != 255
                && (power[1] & (2 | 4)) == 0;
    }

    /**
     * BatteryController's charging constraint without the top-app exemption:
     * {@code isBatteryCharging() && isBatteryNotLow()}. On AC power the battery is charging
     * (8), full (100%), or absent (128): a battery-less device on mains, which AOSP's
     * BatteryService reports as plugged in.
     */
    static boolean stablePower(int[] power) {
        return powerConnected(power) && batteryNotLow(power)
                && ((power[1] & (128 | 8)) != 0 || power[2] == 100);
    }

    /** DeviceIdlenessTracker: the screen has been off for the inactivity threshold. */
    static boolean idle(long idleMillis) {
        return idleMillis >= IDLE_AFTER_MS;
    }

    /**
     * How long until {@link #idle} can hold without another display change, or -1 when the
     * display is on (a display change is reported first) or the device is already idle.
     */
    static long untilIdle(long idleMillis) {
        return idleMillis > 0 && idleMillis < IDLE_AFTER_MS ? IDLE_AFTER_MS - idleMillis : -1;
    }
}
