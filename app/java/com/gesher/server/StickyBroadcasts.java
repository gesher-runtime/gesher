/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.server;

import android.content.Intent;
import android.content.IntentFilter;

import java.util.LinkedHashMap;
import java.util.Map;

/**
 * Sticky broadcasts the system keeps (devsvc track): the last intent of each action, which
 * {@code registerReceiver} returns (and delivers) when its filter matches, as
 * ActivityManagerService's sticky broadcasts. The device's services set them
 * ({@link BatteryService}: ACTION_BATTERY_CHANGED); apps' own sticky broadcasts are not
 * kept.
 */
final class StickyBroadcasts {
    private StickyBroadcasts() {
    }

    // Guarded by StickyBroadcasts.class.
    private static final Map<String, Intent> sSticky = new LinkedHashMap<>();
    private static final java.util.List<Runnable> sStarters = new java.util.ArrayList<>();

    /** Runs {@code starter} before the first query (a service that makes its intents sticky). */
    static synchronized void addStarter(Runnable starter) {
        sStarters.add(starter);
    }

    private static void runStarters() {
        java.util.List<Runnable> run;
        synchronized (StickyBroadcasts.class) {
            if (sStarters.isEmpty()) return;
            run = new java.util.ArrayList<>(sStarters);
            sStarters.clear();
        }
        for (Runnable r : run) r.run();
    }

    static synchronized void put(Intent intent) {
        sSticky.put(intent.getAction(), new Intent(intent));
    }

    /** The sticky intent {@code filter} matches (the first, as AMS returns), or null. */
    static Intent match(IntentFilter filter) {
        if (filter == null) return null;
        runStarters();
        synchronized (StickyBroadcasts.class) {
            return matchLocked(filter);
        }
    }

    private static Intent matchLocked(IntentFilter filter) {
        for (Intent i : sSticky.values()) {
            if (filter.match(i.getAction(), i.getType(), i.getScheme(), i.getData(),
                    i.getCategories(), "gesher") >= 0) {
                return new Intent(i);
            }
        }
        return null;
    }
}
