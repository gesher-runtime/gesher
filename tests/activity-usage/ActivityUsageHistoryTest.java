/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.server;

import static com.gesher.server.ActivityUsageHistory.State.PAUSED;
import static com.gesher.server.ActivityUsageHistory.State.RESUMED;
import static com.gesher.server.ActivityUsageHistory.State.STOPPED;
import static com.gesher.server.ActivityUsageHistory.State.TERMINATED;

import java.io.DataOutputStream;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.List;
import java.util.concurrent.atomic.AtomicLong;

public final class ActivityUsageHistoryTest {
    private static final long DAY = ActivityUsageHistory.DAY;
    private static final int DAILY = 0, YEARLY = 3, BEST = 4;
    private static int checks;

    private static void check(boolean passed, String message) {
        if (!passed) throw new AssertionError(message);
        checks++;
    }

    private static ActivityUsageHistory.Total summary(ActivityUsageHistory history,
            long begin, long end, String pkg) {
        return history.aggregate(DAILY, begin, end).stream()
                .filter(total -> total.pkg.equals(pkg)).findFirst().orElseThrow();
    }

    private static ActivityUsageHistory store(Path file, AtomicLong clock) {
        return new ActivityUsageHistory(file, clock::get, clock::get, cause -> { throw new AssertionError(cause); });
    }

    public static void main(String[] args) throws Exception {
        AtomicLong clock = new AtomicLong(DAY + 100);
        ActivityUsageHistory history = store(null, clock);
        Object a = new Object(), b = new Object(), other = new Object();
        history.transition(a, "app", "A", RESUMED);
        history.transition(a, "app", "A", RESUMED);
        clock.set(DAY + 110);
        history.transition(b, "app", "B", RESUMED);
        history.transition(other, "other.app", "C", RESUMED);
        clock.set(DAY + 120);
        history.transition(a, "app", "A", PAUSED);
        history.transition(a, "app", "A", PAUSED);
        clock.set(DAY + 130);
        history.transition(a, "app", "A", STOPPED);
        clock.set(DAY + 140);
        history.transition(b, "app", "B", PAUSED);
        clock.set(DAY + 150);
        history.transition(b, "app", "B", TERMINATED);
        history.transition(other, "other.app", "C", TERMINATED);
        List<ActivityUsageHistory.Event> events = history.snapshot().events();
        check(events.size() == 8, "duplicate transitions were recorded");
        check(events.get(0).id() == events.get(4).id(), "pause/stop changed instance identity");
        check(events.get(0).id() != events.get(1).id(), "activity instances were conflated");
        ActivityUsageHistory.Total result = summary(history, DAY + 148, DAY + 151, "app");
        check(result.foreground == 40 && result.visible == 50 && result.launches == 2,
                "aggregate double-counted overlapping activities or clipped a selected bucket");
        check(result.lastUsed == DAY + 140 && result.lastVisible == DAY + 150,
                "last-use timestamps do not describe actual durations");

        long next = 2 * DAY + 100;
        clock.set(next - 10);
        Object crossing = new Object();
        history.transition(crossing, "crossing.app", "A", RESUMED);
        clock.set(next + 20);
        history.transition(crossing, "crossing.app", "A", STOPPED);
        check(summary(history, DAY, next, "crossing.app").foreground == 10,
                "wrong portion in first interval");
        check(summary(history, next + 1, 3 * DAY, "crossing.app").foreground == 20,
                "wrong portion in following interval");
        check(result.begin == DAY + 100, "first interval was rounded to the epoch");
        check(summary(history, next + 1, 3 * DAY, "crossing.app").begin == next,
                "active rollover lost its exact persisted origin");
        check(history.aggregate(BEST, DAY + 100, 10 * DAY).stream()
                .filter(total -> total.pkg.equals("crossing.app")).count() == 2,
                "fresh BEST did not use daily intervals");
        clock.set(50 * DAY);
        check(history.aggregate(DAILY, 0, 50 * DAY).isEmpty(),
                "history outside retention was included");

        Path directory = Files.createTempDirectory("activity-usage-test");
        Path file = directory.resolve("activity-usage.bin");
        try {
            clock.set(100);
            history = store(file, clock);
            Object active = new Object();
            history.transition(active, "app", "A", RESUMED);
            int oldId = history.snapshot().events().get(0).id();
            clock.set(200);
            history.flush();

            clock.set(10_000);
            ActivityUsageHistory restarted = store(file, clock);
            check(summary(restarted, 0, DAY, "app").foreground == 100,
                    "server downtime was attributed to the former activity");
            Object replacement = new Object();
            restarted.transition(replacement, "app", "A", RESUMED);
            List<ActivityUsageHistory.Event> afterRestart = restarted.snapshot().events();
            check(afterRestart.get(afterRestart.size() - 1).id() != oldId,
                    "process restart reused a persisted activity instance ID");
            restarted.transition(new Object(), "other.app", "B", RESUMED);
            restarted.forgetPackage("app");
            check(restarted.snapshot().events().stream().noneMatch(event -> "app".equals(event.pkg())),
                    "uninstall failed to clear live history");
            ActivityUsageHistory afterRemoval = store(file, clock);
            check(afterRemoval.snapshot().events().stream()
                    .noneMatch(event -> "app".equals(event.pkg())),
                    "uninstall failed to purge persisted history");
            check(afterRemoval.snapshot().events().stream()
                    .anyMatch(event -> "other.app".equals(event.pkg())),
                    "uninstall removed a different app's history");

            Files.write(file, new byte[] {0, 0, 0, 1, 0});
            ActivityUsageHistory corrupt = new ActivityUsageHistory(file, clock::get, clock::get, failure -> { });
            check(corrupt.snapshot().events().isEmpty(), "partial journal created false events");
        } finally {
            Files.deleteIfExists(file);
            Files.delete(directory);
        }
        testPausedOnly();
        testEventEvictionAndBest();
        testClockCorrection();
        testLegacyJournal();
        System.out.println("activity usage history all passed: " + checks + " checks");
    }

    private static void testPausedOnly() {
        AtomicLong clock = new AtomicLong(83);
        ActivityUsageHistory history = store(null, clock);
        Object token = new Object();
        clock.set(100);
        history.transition(token, "app", "A", PAUSED);
        clock.set(200);
        history.transition(token, "app", "A", STOPPED);
        List<ActivityUsageHistory.Event> events = history.snapshot().events();
        check(events.size() == 2 && events.get(0).state() == PAUSED
                && events.get(1).state() == STOPPED && events.get(0).id() == events.get(1).id(),
                "pause-only activity did not expose real lifecycle events");
        ActivityUsageHistory.Total usage = summary(history, 100, 250, "app");
        check(usage.begin == 83 && usage.visible == 100 && usage.foreground == 0
                && usage.launches == 0 && usage.lastUsed == 0 && usage.lastVisible == 200,
                "pause-only activity was ignored or treated as resumed");
    }

    private static void testEventEvictionAndBest() throws Exception {
        Path directory = Files.createTempDirectory("usage-burst");
        Path file = directory.resolve("history");
        try {
            AtomicLong clock = new AtomicLong(DAY + 37);
            ActivityUsageHistory history = store(file, clock);
            Object active = new Object(), noise = new Object();
            history.transition(active, "first", "Activity", RESUMED);
            for (int i = 0; i < 6001; i++) {
                clock.incrementAndGet();
                history.transition(noise, "burst", "Short", RESUMED);
                history.transition(noise, "burst", "Short", STOPPED);
            }
            check(history.snapshot().events().size() == 12_000
                    && history.snapshot().events().stream().noneMatch(event -> "first".equals(event.pkg())),
                    "fixture did not evict the ongoing activity's first event");
            check(summary(history, DAY, DAY + 7000, "first").foreground == 6001,
                    "event eviction erased live totals");
            history.flush();
            clock.addAndGet(DAY + 120);
            ActivityUsageHistory restarted = store(file, clock);
            check(summary(restarted, DAY, 3 * DAY, "first").foreground == 6001
                    && summary(restarted, DAY, 3 * DAY, "first").launches == 1,
                    "restart lost evicted totals or counted offline time");
            check(restarted.aggregate(BEST, DAY + 37, 3 * DAY).stream()
                    .map(total -> total.begin).distinct().count() == 1,
                    "persisted BEST missed its stored origin");
            Object later = new Object();
            long resumed = clock.get();
            restarted.transition(later, "first", "Activity", RESUMED);
            clock.incrementAndGet();
            restarted.transition(later, "first", "Activity", STOPPED);
            check(summary(restarted, resumed, resumed + 20, "first").begin == resumed,
                    "empty downtime produced an invented interval origin");
            ActivityUsageHistory.Total wide = restarted.aggregate(YEARLY, resumed, resumed + 20).stream()
                    .filter(total -> "first".equals(total.pkg)).findFirst().orElseThrow();
            check(wide.begin == DAY + 37 && wide.foreground == 6002,
                    "wider active bucket did not retain its origin and history");
            restarted.flush();
            check(restarted.aggregate(BEST, resumed, resumed + 20).get(0).begin == resumed,
                    "BEST did not choose the closest available daily origin");
        } finally {
            Files.deleteIfExists(file);
            Files.delete(directory);
        }
    }

    private static void testClockCorrection() throws Exception {
        Path directory = Files.createTempDirectory("usage-rtc");
        Path file = directory.resolve("history");
        try {
            AtomicLong wall = new AtomicLong(DAY + 20_000), elapsed = new AtomicLong(100);
            ActivityUsageHistory history = new ActivityUsageHistory(file, wall::get, elapsed::get,
                    failure -> { throw new AssertionError(failure); });
            Object active = new Object();
            history.transition(active, "app", "A", RESUMED);
            wall.addAndGet(10_100);
            elapsed.addAndGet(100);
            check(summary(history, DAY, 2 * DAY, "app").foreground == 100,
                    "forward RTC correction counted artificial activity");
            check(history.snapshot().events().get(0).time() == DAY + 30_000,
                    "forward RTC correction did not rebase the event");
            wall.addAndGet(-14_950);
            elapsed.addAndGet(50);
            history.transition(active, "app", "A", PAUSED);
            ActivityUsageHistory.Total live = summary(history, DAY, 2 * DAY, "app");
            check(live.begin == DAY + 15_000 && live.foreground == 150 && live.visible == 150,
                    "backward RTC correction froze time or corrupted aggregate origins");
            wall.addAndGet(25);
            elapsed.addAndGet(25);
            history.flush();
            wall.addAndGet(5_000);
            ActivityUsageHistory restarted = store(file, wall);
            ActivityUsageHistory.Total persisted = summary(restarted, DAY, 2 * DAY, "app");
            check(persisted.foreground == 150 && persisted.visible == 175
                    && persisted.lastUsed == DAY + 15_150 && persisted.lastVisible == DAY + 15_175,
                    "RTC rebase did not persist durations and checkpoint endpoints");
            check(restarted.snapshot().events().get(1).time() == DAY + 15_150,
                    "backward correction did not shift event chronology");
        } finally {
            Files.deleteIfExists(file);
            Files.delete(directory);
        }
    }

    private static void testLegacyJournal() throws Exception {
        Path directory = Files.createTempDirectory("usage-v1");
        Path file = directory.resolve("history");
        try {
            try (DataOutputStream out = new DataOutputStream(Files.newOutputStream(file))) {
                out.writeInt(1);
                out.writeLong(240);
                out.writeInt(1);
                out.writeLong(100);
                out.writeInt(24);
                out.writeByte(RESUMED.ordinal());
                out.writeUTF("app");
                out.writeUTF("Activity");
            }
            AtomicLong clock = new AtomicLong(400);
            ActivityUsageHistory migrated = store(file, clock);
            check(summary(migrated, 0, 500, "app").foreground == 140,
                    "legacy active journal failed to migrate to its saved checkpoint");
            migrated.flush();
            ActivityUsageHistory reopened = store(file, clock);
            check(summary(reopened, 0, 500, "app").foreground == 140
                    && reopened.snapshot().events().get(0).id() == 24,
                    "migration failed to persist totals and instance IDs");
        } finally {
            Files.deleteIfExists(file);
            Files.delete(directory);
        }
    }
}
