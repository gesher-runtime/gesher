/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.server;

import static com.gesher.server.ActivityUsageHistory.State.PAUSED;
import static com.gesher.server.ActivityUsageHistory.State.RESUMED;

import java.lang.reflect.Field;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayDeque;
import java.util.ArrayList;
import java.util.List;
import java.util.Random;
import java.util.Set;
import java.util.concurrent.atomic.AtomicLong;
import java.util.concurrent.atomic.AtomicReference;

/** Real history queries against an independent lifecycle-input model; no replacement store. */
public final class ActivityUsageQueryTest {
    private static final int ALL = (1 << ActivityUsageHistory.State.values().length) - 1;
    private static int checks;

    private static void check(boolean value, String message) {
        if (!value) throw new AssertionError(message);
        checks++;
    }

    private static ActivityUsageHistory history(Path path, AtomicLong wall, AtomicLong elapsed) {
        return new ActivityUsageHistory(path, wall::get, elapsed::get,
                failure -> { throw new AssertionError(failure); });
    }

    private static List<ActivityUsageHistory.Event> expected(
            Iterable<ActivityUsageHistory.Event> model, long now, long begin, long end,
            Set<String> packages, int states) {
        List<ActivityUsageHistory.Event> result = new ArrayList<>();
        begin = Math.max(begin, now - ActivityUsageHistory.RETENTION);
        for (var event : model) {
            if (event.time() >= begin && event.time() < end
                    && (packages.isEmpty() || packages.contains(event.pkg()))
                    && (states & (1 << event.state().ordinal())) != 0) result.add(event);
        }
        return result;
    }

    private static void compare(ActivityUsageHistory history,
            Iterable<ActivityUsageHistory.Event> model, long now, long begin, long end,
            Set<String> packages, int states) {
        var snapshot = history.eventSnapshot(begin, end, packages, states);
        check(snapshot.until() == now, "Query time was changed");
        check(snapshot.events().equals(expected(model, now, begin, end, packages, states)),
                "Query lost/reordered/substituted events: " + begin + ".." + end + " " + packages);
        check(snapshot.intervals().isEmpty(), "Event-only query copied unrelated interval metadata");
    }

    private static void wrappedQueriesAndRecovery(Path file) throws Exception {
        long origin = 10 * ActivityUsageHistory.DAY;
        AtomicLong wall = new AtomicLong(origin), elapsed = new AtomicLong(100);
        ArrayDeque<ActivityUsageHistory.Event> model = new ArrayDeque<>();
        List<ActivityUsageHistory.Event> saved;
        try (ActivityUsageHistory history = history(file, wall, elapsed)) {
            // An unused history does not allocate a 12,000-element array.
            Field suffixField = ActivityUsageHistory.class.getDeclaredField("mHistory");
            suffixField.setAccessible(true);
            Object suffix = suffixField.get(history);
            Field entriesField = suffix.getClass().getDeclaredField("entries");
            entriesField.setAccessible(true);
            check(((Object[]) entriesField.get(suffix)).length == 0, "Empty history allocated its full budget");
            Object[] tokens = new Object[64];
            for (int i = 0; i < tokens.length; i++) tokens[i] = new Object();
            for (int i = 0; i < 32_000; i++) {
                int actor = i % tokens.length;
                long time = origin + i / 5; // Deliberately equal timestamp groups.
                wall.set(time); elapsed.set(100 + i / 5);
                String pkg = "pkg." + actor % 8, cls = "Activity" + actor;
                var state = (i / tokens.length & 1) == 0 ? RESUMED : PAUSED;
                history.transition(tokens[actor], pkg, cls, state);
                if (model.size() == ActivityUsageHistory.MAX_ENTRIES) model.removeFirst();
                model.addLast(new ActivityUsageHistory.Event(time, pkg, cls, actor + 1, state));
            }
            check(history.eventSnapshot().events().equals(new ArrayList<>(model)),
                    "Growth/wraparound changed the retained lifecycle suffix");
            check(((Object[]) entriesField.get(suffix)).length == ActivityUsageHistory.MAX_ENTRIES,
                    "Backing array exceeded the existing event cap");
            long first = model.getFirst().time(), last = model.getLast().time();
            for (long edge : new long[]{Long.MIN_VALUE, first - 1, first, first + 1,
                    (first + last) / 2, last, last + 1, Long.MAX_VALUE}) {
                compare(history, model, wall.get(), edge, edge, Set.of(), ALL);
                compare(history, model, wall.get(), edge, Long.MAX_VALUE, Set.of(), ALL);
                compare(history, model, wall.get(), Long.MIN_VALUE, edge, Set.of(), ALL);
            }
            Random random = new Random(0x56_31_12);
            for (int i = 0; i < 640; i++) {
                long begin = first - 100 + random.nextInt((int) (last - first + 200));
                long end = begin + random.nextInt(350) - 20;
                Set<String> packages = switch (i % 4) {
                    case 0 -> Set.of();
                    case 1 -> Set.of("pkg.2");
                    case 2 -> Set.of("pkg.1", "pkg.6");
                    default -> Set.of("missing");
                };
                int states = switch (i % 5) {
                    case 0 -> ALL;
                    case 1 -> 1 << RESUMED.ordinal();
                    case 2 -> 1 << PAUSED.ordinal();
                    case 3 -> 1 << ActivityUsageHistory.State.STOPPED.ordinal();
                    default -> 0;
                };
                compare(history, model, wall.get(), begin, end, packages, states);
            }

            saved = history.eventSnapshot(0, Long.MAX_VALUE, Set.of(), ALL).events();
            history.forgetPackage("pkg.2");
            model.removeIf(event -> event.pkg().equals("pkg.2"));
            check(saved.size() == ActivityUsageHistory.MAX_ENTRIES
                    && saved.stream().anyMatch(event -> event.pkg().equals("pkg.2")),
                    "Purging a package mutated an already returned snapshot");
            compare(history, model, wall.get(), 0, Long.MAX_VALUE, Set.of(), ALL);
            compare(history, model, wall.get(), first, last, Set.of("pkg.2"), ALL);
            // Vacated circular positions must release removed Event references.
            for (Object value : (Object[]) entriesField.get(suffix)) {
                if (value instanceof ActivityUsageHistory.Event event) {
                    check(!event.pkg().equals("pkg.2"), "Purged event remained retained by the ring");
                }
            }
            for (long delta : new long[]{3_600_000, -7_200_000}) {
                wall.addAndGet(delta);
                ArrayDeque<ActivityUsageHistory.Event> shifted = new ArrayDeque<>();
                for (var event : model) shifted.addLast(new ActivityUsageHistory.Event(
                        event.time() + delta, event.pkg(), event.cls(), event.id(), event.state()));
                model = shifted;
                compare(history, model, wall.get(), 0, Long.MAX_VALUE, Set.of(), ALL);
                compare(history, model, wall.get(), model.getLast().time() - 5,
                        model.getLast().time() + 1, Set.of("pkg.7"), ALL);
            }
            check(saved.get(0).time() == first, "RTC rebasing mutated an older immutable event");
            history.flush();
        }
        try (ActivityUsageHistory recovered = history(file, wall, elapsed)) {
            compare(recovered, model, wall.get(), 0, Long.MAX_VALUE, Set.of(), ALL);
            compare(recovered, model, wall.get(), model.getFirst().time(),
                    model.getFirst().time() + 3, Set.of("pkg.0"), ALL);
            long advanced = ActivityUsageHistory.RETENTION + 1;
            wall.addAndGet(advanced); elapsed.addAndGet(advanced);
            compare(recovered, model, wall.get(), 0, Long.MAX_VALUE, Set.of(), ALL);
            check(recovered.eventSnapshot().events().size() == model.size(),
                    "A query pruned the stored history or advanced interval accounting");
            recovered.flush();
            check(recovered.eventSnapshot().events().isEmpty(), "Real flush did not prune expired events");
        }
    }

    private static void concurrentSnapshots() throws Exception {
        AtomicLong wall = new AtomicLong(100), elapsed = new AtomicLong(100);
        try (ActivityUsageHistory history = history(null, wall, elapsed)) {
            AtomicReference<Throwable> failure = new AtomicReference<>();
            Thread writer = new Thread(() -> {
                try {
                    Object token = new Object();
                    for (int i = 0; i < 24_000; i++) {
                        history.transition(token, "writer", "Activity", (i & 1) == 0 ? RESUMED : PAUSED);
                    }
                } catch (Throwable error) { failure.set(error); }
            });
            writer.start();
            List<List<ActivityUsageHistory.Event>> saved = new ArrayList<>();
            List<List<ActivityUsageHistory.Event>> copies = new ArrayList<>();
            for (int i = 0; i < 64; i++) {
                var events = history.eventSnapshot(100, 101, Set.of("writer"), ALL).events();
                check(events.size() <= ActivityUsageHistory.MAX_ENTRIES, "Concurrent suffix exceeded cap");
                for (int j = 1; j < events.size(); j++) {
                    check(events.get(j - 1).state() != events.get(j).state(),
                            "Snapshot mixed two ring generations");
                }
                saved.add(events); copies.add(new ArrayList<>(events));
            }
            writer.join(10_000);
            check(!writer.isAlive(), "History writer did not finish");
            if (failure.get() != null) throw new AssertionError(failure.get());
            for (int i = 0; i < saved.size(); i++) {
                check(saved.get(i).equals(copies.get(i)), "A returned snapshot retained the mutable ring");
            }
        }
    }

    public static void main(String[] args) throws Exception {
        Path directory = Files.createTempDirectory("gesher-usage-query-");
        Path file = directory.resolve("history.bin");
        try {
            wrappedQueriesAndRecovery(file);
            concurrentSnapshots();
        } finally {
            Files.deleteIfExists(file);
            Files.delete(directory);
        }
        System.out.println("ActivityUsageQueryTest all passed: " + checks + " checks");
    }
}
