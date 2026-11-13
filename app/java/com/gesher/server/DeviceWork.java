/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.server;

import android.os.SystemClock;

import java.util.List;
import java.util.concurrent.CopyOnWriteArrayList;

/**
 * Scheduled or in-flight work (alarms, jobs, broadcasts) that keeps a persistent Gesher device from
 * stopping when idle (devsvc track, ADR 0038): the device's idle timeout ends it only when
 * no app was in use for the timeout and nothing scheduled is due within it (for at most one
 * more timeout, {@link #keepsIdleDevice}). What is due
 * later is lost when the device stops, as alarms are at a reboot (persisted jobs are kept
 * on disk and scheduled again when it starts).
 */
public final class DeviceWork {
    private DeviceWork() {
    }

    /** A scheduler or bounded delivery owner with work ahead or in flight. */
    public interface Source {
        /** Next due elapsed time; zero: work in flight; Long.MAX_VALUE: none. */
        long nextDueElapsed();
    }

    private static final List<Source> sSources = new CopyOnWriteArrayList<>();

    static void add(Source source) {
        sSources.add(source);
    }

    /**
     * Whether scheduled work keeps a device that has been idle for {@code idleMs} although
     * its idle timeout ({@code timeoutMs}) has passed: work due within the timeout does, but
     * only for one more timeout. After twice the timeout only work due now (a running job,
     * an alarm being delivered) does. An idle app that schedules itself again and again (a
     * telemetry job every 30 s) would otherwise keep the device forever; work due later is
     * lost with the device, as at a reboot (persisted jobs are kept).
     */
    public static boolean keepsIdleDevice(long idleMs, long timeoutMs) {
        return dueWithin(idleMs < 2 * timeoutMs ? timeoutMs : 0);
    }

    /** Whether some scheduled work is due within {@code windowMs} from now. */
    public static boolean dueWithin(long windowMs) {
        for (Source s : sSources) {
            long due = s.nextDueElapsed();
            // A source can wait for its scheduler lock and then report "now" for
            // running work. Compare against a time sampled after that query, especially
            // when the background grace has ended and windowMs is zero.
            long limit = SystemClock.elapsedRealtime() + windowMs;
            if (due != Long.MAX_VALUE && due <= limit) return true;
        }
        return false;
    }
}
