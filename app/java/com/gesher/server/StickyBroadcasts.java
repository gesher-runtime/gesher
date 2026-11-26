/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.server;

import android.content.Intent;
import android.content.IntentFilter;

import java.util.LinkedHashMap;
import java.util.Map;
import java.util.Set;
import java.util.HashSet;
import java.util.function.Consumer;
import java.util.function.Supplier;

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
    private static final Map<String, Dynamic> sDynamic = new LinkedHashMap<>();

    /** A current-value sticky whose observation belongs to its registered receivers. */
    private static final class Dynamic {
        final Supplier<Intent> current;
        final Consumer<Boolean> demand;
        final Set<Object> receivers = new HashSet<>();
        Dynamic(Supplier<Intent> current, Consumer<Boolean> demand) {
            this.current = current;
            this.demand = demand;
        }
    }

    /** Installed at service creation, before applications can register receivers. */
    static synchronized void addDynamic(String action, Supplier<Intent> current,
            Consumer<Boolean> demand) {
        sDynamic.put(action, new Dynamic(current, demand));
    }

    /**
     * Called under BroadcastController's receiver lock. Demand transitions are serialized
     * here; their service callbacks only update ownership, never call application code.
     * The token is the ReceiverList generation, not a reusable receiver Binder.
     */
    static synchronized void receiverRegistered(Object token, IntentFilter filter) {
        for (Map.Entry<String, Dynamic> entry : sDynamic.entrySet()) {
            if (!matches(filter, entry.getKey())) continue;
            Dynamic source = entry.getValue();
            if (source.receivers.add(token) && source.receivers.size() == 1) source.demand.accept(true);
        }
    }

    static synchronized void receiverUnregistered(Object token) {
        for (Dynamic source : sDynamic.values()) {
            if (source.receivers.remove(token) && source.receivers.isEmpty()) source.demand.accept(false);
        }
    }

    private static boolean matches(IntentFilter filter, String action) {
        return filter.match(action, null, null, null, null, "gesher") >= 0;
    }

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
        Supplier<Intent> current = null;
        synchronized (StickyBroadcasts.class) {
            Intent stored = matchLocked(filter);
            if (stored != null) return stored;
            for (Map.Entry<String, Dynamic> entry : sDynamic.entrySet()) {
                if (matches(filter, entry.getKey())) { current = entry.getValue().current; break; }
            }
        }
        // A sticky-only query gets current state without acquiring an observer. Native
        // reads stay outside both the sticky registry and receiver registration locks.
        return current != null ? current.get() : null;
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
