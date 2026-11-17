/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.server;

import static com.gesher.server.ActivityUsageHistory.State.PAUSED;
import static com.gesher.server.ActivityUsageHistory.State.RESUMED;
import static com.gesher.server.ActivityUsageHistory.State.STOPPED;
import static com.gesher.server.ActivityUsageHistory.State.TERMINATED;

import java.nio.file.Files;
import java.nio.file.Path;
import java.util.List;
import java.util.concurrent.atomic.AtomicLong;

public final class ActivityUsageHistoryTest {
    private static final long DAY = ActivityUsageHistory.DAY;

    private static void check(boolean passed, String message) {
        if (!passed) throw new AssertionError(message);
    }

    private static ActivityUsageHistory.Total summary(ActivityUsageHistory history,
            long begin, long end, String pkg) {
        return ActivityUsageHistory.aggregate(history.snapshot(), DAY, begin, end).stream()
                .filter(total -> total.pkg.equals(pkg)).findFirst().orElseThrow();
    }

    private static ActivityUsageHistory store(Path file, AtomicLong clock) {
        return new ActivityUsageHistory(file, clock::get, cause -> { throw new AssertionError(cause); });
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

        clock.set(2 * DAY - 10);
        Object crossing = new Object();
        history.transition(crossing, "crossing.app", "A", RESUMED);
        clock.set(2 * DAY + 20);
        history.transition(crossing, "crossing.app", "A", STOPPED);
        check(summary(history, DAY, 2 * DAY, "crossing.app").foreground == 10,
                "wrong portion in first interval");
        check(summary(history, 2 * DAY, 3 * DAY, "crossing.app").foreground == 20,
                "wrong portion in following interval");
        clock.set(50 * DAY);
        check(ActivityUsageHistory.aggregate(history.snapshot(), DAY, 0, 50 * DAY).isEmpty(),
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
            ActivityUsageHistory corrupt = new ActivityUsageHistory(file, clock::get, failure -> { });
            check(corrupt.snapshot().events().isEmpty(), "partial journal created false events");
        } finally {
            Files.deleteIfExists(file);
            Files.delete(directory);
        }
        System.out.println("activity usage history all passed");
    }
}
