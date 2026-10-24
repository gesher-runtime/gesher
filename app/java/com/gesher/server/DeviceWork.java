/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.server;

import android.os.SystemClock;

import java.util.List;
import java.util.concurrent.CopyOnWriteArrayList;

/**
 * Work the device has scheduled (alarms, jobs) that keeps a persistent Gesher device from
 * stopping when idle (devsvc track, ADR 0038): the device's idle timeout ends it only when
 * no app process ran for the timeout and nothing scheduled is due within it. What is due
 * later is lost when the device stops, as alarms are at a reboot (persisted jobs are kept
 * on disk and scheduled again when it starts).
 */
public final class DeviceWork {
    private DeviceWork() {
    }

    /** A scheduler with work ahead. */
    public interface Source {
        /** When ({@link SystemClock#elapsedRealtime}) its next work is due; Long.MAX_VALUE: none. */
        long nextDueElapsed();
    }

    private static final List<Source> sSources = new CopyOnWriteArrayList<>();

    static void add(Source source) {
        sSources.add(source);
    }

    /** Whether some scheduled work is due within {@code windowMs} from now. */
    public static boolean dueWithin(long windowMs) {
        long limit = SystemClock.elapsedRealtime() + windowMs;
        for (Source s : sSources) {
            if (s.nextDueElapsed() <= limit) return true;
        }
        return false;
    }
}
