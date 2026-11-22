/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.server;

import android.app.ActivityManager;
import android.content.Context;

import java.util.ArrayDeque;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.IdentityHashMap;
import java.util.List;
import java.util.Map;
import java.util.function.BooleanSupplier;
import java.util.function.Predicate;

/**
 * The represented process dependencies relevant to ShortcutService's BOUND_FGS
 * threshold. This does not report an OOM score, capability, or AppOps state.
 * Controllers publish immutable topology; activity state is sampled under ATM.
 */
final class ShortcutProcessState {
    static final int LIMIT = ActivityManager.PROCESS_STATE_BOUND_FOREGROUND_SERVICE;
    static final int BACKGROUND = ActivityManager.PROCESS_STATE_CACHED_EMPTY;

    static final class Owner {
        final ActivityTaskManagerService.ProcessRecord process;
        final ApkPackage code;
        final String component;
        final ApkPackage.ComponentKind kind;

        Owner(ActivityTaskManagerService.ProcessRecord process, ApkPackage code,
                String component, ApkPackage.ComponentKind kind) {
            this.process = process; this.code = code; this.component = component; this.kind = kind;
        }

        boolean current(PackageManagerService pm,
                Predicate<ActivityTaskManagerService.ProcessRecord> live) {
            return process != null && live.test(process) && process.packageCode == code
                    && pm.get(code.packageName) == code && !pm.isRemovalPending(code.packageName)
                    && code.componentEnabled(component, kind);
        }
    }

    static final class Link {
        final ActivityTaskManagerService.ProcessRecord client;
        final ApkPackage clientCode;
        final Owner owner;
        final long flags;
        final boolean system, provider;
        final BooleanSupplier lifetime;

        Link(ActivityTaskManagerService.ProcessRecord client, ApkPackage clientCode,
                Owner owner, long flags, boolean system, boolean provider, BooleanSupplier lifetime) {
            this.client = client; this.clientCode = clientCode; this.owner = owner;
            this.flags = flags; this.system = system; this.provider = provider; this.lifetime = lifetime;
        }

        boolean current(PackageManagerService pm,
                Predicate<ActivityTaskManagerService.ProcessRecord> live) {
            return owner.current(pm, live) && lifetime.getAsBoolean()
                    && (system || client != null && live.test(client) && client.packageCode == clientCode
                        && pm.get(clientCode.packageName) == clientCode
                        && !pm.isRemovalPending(clientCode.packageName));
        }

        int propagated(int state, boolean awake) {
            if (provider) {
                return state <= ActivityManager.PROCESS_STATE_FOREGROUND_SERVICE
                        ? state == ActivityManager.PROCESS_STATE_TOP
                            ? ActivityManager.PROCESS_STATE_BOUND_TOP : LIMIT : state;
            }
            if ((flags & Context.BIND_WAIVE_PRIORITY) != 0) return BACKGROUND;
            // OomAdjusterImpl preserves the host's own state for this case.
            // Gesher's desktop home is not an admitted application process.
            if ((flags & Context.BIND_ALLOW_OOM_MANAGEMENT) != 0 && owner.process.shortcutHasShownUi) {
                return BACKGROUND;
            }
            // The actual system-server client has the strong persistent score
            // required by this exception. A host promoted by that binding has
            // only PERSISTENT_SERVICE_ADJ, so it cannot retransmit this override.
            if (system && state < ActivityManager.PROCESS_STATE_TOP
                    && (flags & (Context.BIND_IMPORTANT | Context.BIND_ABOVE_CLIENT)) != 0) {
                return ActivityManager.PROCESS_STATE_PERSISTENT;
            }
            if ((flags & (Context.BIND_NOT_FOREGROUND | Context.BIND_IMPORTANT_BACKGROUND)) != 0) {
                return BACKGROUND;
            }
            if (state < ActivityManager.PROCESS_STATE_TOP) {
                if ((flags & Context.BIND_TREAT_LIKE_VISIBLE_FOREGROUND_SERVICE) != 0) {
                    return ActivityManager.PROCESS_STATE_FOREGROUND_SERVICE;
                }
                return (flags & Context.BIND_FOREGROUND_SERVICE) != 0
                        || awake && (flags & Context.BIND_FOREGROUND_SERVICE_WHILE_AWAKE) != 0
                        ? LIMIT : BACKGROUND;
            }
            return state == ActivityManager.PROCESS_STATE_TOP
                    ? ActivityManager.PROCESS_STATE_BOUND_TOP : state;
        }
    }

    /** Rebuilt on owned topology/FGS mutations, never on quota queries or clock ticks. */
    static final class Snapshot {
        static final Snapshot EMPTY = new Snapshot(List.of(), List.of());
        final List<Owner> foreground;
        final List<Link> system;
        final Map<ActivityTaskManagerService.ProcessRecord, List<Link>> outgoing;

        Snapshot(List<Owner> foreground, List<Link> links) {
            this.foreground = List.copyOf(foreground);
            List<Link> local = new ArrayList<>();
            Map<ActivityTaskManagerService.ProcessRecord, List<Link>> byClient = new HashMap<>();
            for (Link link : links) {
                if (link.system) local.add(link);
                else byClient.computeIfAbsent(link.client, ignored -> new ArrayList<>()).add(link);
            }
            byClient.replaceAll((client, values) -> List.copyOf(values));
            system = List.copyOf(local); outgoing = Map.copyOf(byClient);
        }
    }

    /**
     * ATM-owned temporary work storage, not a foreground cache. Every evaluation
     * starts empty and clears all process references in finally. The backing
     * tables may be reused, and shrink when the live registry becomes smaller.
     */
    static final class Evaluation {
        private IdentityHashMap<ActivityTaskManagerService.ProcessRecord, Integer> states = new IdentityHashMap<>();
        private IdentityHashMap<ActivityTaskManagerService.ProcessRecord, Boolean> members = new IdentityHashMap<>();
        private ArrayDeque<ActivityTaskManagerService.ProcessRecord> work = new ArrayDeque<>();
        private List<ActivityTaskManagerService.ProcessRecord> processes;
        private Predicate<ActivityTaskManagerService.ProcessRecord> codeCurrent;
        private final Predicate<ActivityTaskManagerService.ProcessRecord> live = this::current;
        private boolean indexed;
        private int peak;
        boolean inUse;

        private boolean current(ActivityTaskManagerService.ProcessRecord process) {
            if (!indexed) {
                for (var member : processes) members.put(member, Boolean.TRUE);
                indexed = true;
            }
            return process != null && members.containsKey(process) && codeCurrent.test(process);
        }

        Map<ActivityTaskManagerService.ProcessRecord, Integer> evaluate(PackageManagerService pm,
                Map<ActivityTaskManagerService.ProcessRecord, Integer> roots,
                List<ActivityTaskManagerService.ProcessRecord> processes,
                Predicate<ActivityTaskManagerService.ProcessRecord> codeCurrent, boolean awake,
                Snapshot services, Snapshot providers) {
            inUse = true; this.processes = processes; this.codeCurrent = codeCurrent;
            states.putAll(roots); work.addAll(roots.keySet());
            for (Owner owner : services.foreground) {
                if (owner.current(pm, live)) improve(states, work, owner.process,
                        ActivityManager.PROCESS_STATE_FOREGROUND_SERVICE);
            }
            follow(services.system, ActivityManager.PROCESS_STATE_PERSISTENT, pm, live, awake, states, work);
            follow(providers.system, ActivityManager.PROCESS_STATE_PERSISTENT, pm, live, awake, states, work);
            while (!work.isEmpty()) {
                ActivityTaskManagerService.ProcessRecord client = work.removeFirst();
                int state = states.get(client);
                follow(services.outgoing.getOrDefault(client, List.of()), state, pm, live, awake, states, work);
                follow(providers.outgoing.getOrDefault(client, List.of()), state, pm, live, awake, states, work);
            }
            return states;
        }

        void clear(int liveProcesses) {
            peak = Math.max(peak, Math.max(states.size(), members.size()));
            states.clear(); members.clear(); work.clear();
            if (peak > Math.max(64L, 2L * liveProcesses)) {
                states = new IdentityHashMap<>(); members = new IdentityHashMap<>(); work = new ArrayDeque<>(); peak = 0;
            }
            processes = null; codeCurrent = null; indexed = false; inUse = false;
        }
    }

    private static void follow(List<Link> links, int state, PackageManagerService pm,
            Predicate<ActivityTaskManagerService.ProcessRecord> live, boolean awake,
            Map<ActivityTaskManagerService.ProcessRecord, Integer> states,
            ArrayDeque<ActivityTaskManagerService.ProcessRecord> work) {
        for (Link link : links) {
            if (link.current(pm, live)) improve(states, work, link.owner.process, link.propagated(state, awake));
        }
    }

    private static void improve(Map<ActivityTaskManagerService.ProcessRecord, Integer> states,
            ArrayDeque<ActivityTaskManagerService.ProcessRecord> work,
            ActivityTaskManagerService.ProcessRecord process, int state) {
        if (state <= LIMIT && state < states.getOrDefault(process, BACKGROUND)) {
            states.put(process, state); work.addLast(process);
        }
    }
}
