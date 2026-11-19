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

/** Finite production-history controls; no Android framework or mocked accumulator. */
public final class ActivityUsageHistoryTest {
    private static final long DAY = ActivityUsageHistory.DAY;
    private static int checks;

    private static void check(boolean passed, String message) {
        if (!passed) throw new AssertionError(message);
        checks++;
    }

    private static ActivityUsageHistory.Total summary(ActivityUsageHistory history,
            long begin, long end, String pkg) {
        return ActivityUsageHistory.aggregate(history.snapshot(), DAY, begin, end).stream()
                .filter(total -> total.pkg.equals(pkg)).findFirst().orElseThrow();
    }

    private static ActivityUsageHistory store(Path file, AtomicLong clock) {
        return new ActivityUsageHistory(file, clock::get, cause -> { throw new AssertionError(cause); });
    }

    private static ActivityUsageHistory store(Path file, AtomicLong wall, AtomicLong elapsed) {
        return new ActivityUsageHistory(file, wall::get, elapsed::get,
                cause -> { throw new AssertionError(cause); });
    }

    private static void ordinaryOverlapAndRollover() throws Exception {
        AtomicLong clock = new AtomicLong(DAY + 100);
        try (ActivityUsageHistory history = store(null, clock)) {
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
                    "overlap was double-counted or the selected interval was clipped");
            check(result.begin == DAY + 100 && result.lastUsed == DAY + 140
                    && result.lastVisible == DAY + 150, "origins or last-use times were synthesized");

            long expiry = 2 * DAY + 100;
            clock.set(expiry - 10);
            Object crossing = new Object();
            history.transition(crossing, "crossing.app", "A", RESUMED);
            clock.set(expiry);
            history.flush(); // The real periodic FLUSH_TO_DISK event observes this deadline.
            clock.set(expiry + 20);
            history.transition(crossing, "crossing.app", "A", STOPPED);
            // Pinned UserUsageStatsService closes END_OF_DAY at expiry - 1.
            check(summary(history, DAY + 100, expiry, "crossing.app").foreground == 9,
                    "END_OF_DAY did not use the Android expiry-minus-one boundary");
            ActivityUsageHistory.Total next = summary(history, expiry, expiry + 21, "crossing.app");
            check(next.begin == expiry && next.foreground == 20 && next.launches == 1
                    && next.appLaunches == 0, "ongoing state was lost or counted as a new app launch");
            var week = ActivityUsageHistory.aggregate(history.snapshot(), 7 * DAY,
                    DAY + 100, expiry + 21).stream().filter(t -> t.pkg.equals("crossing.app"))
                    .findFirst().orElseThrow();
            check(week.foreground == 29 && week.launches == 1,
                    "retained weekly state was not closed/reloaded before daily continuation");
            check(history.snapshot().events().stream().filter(e -> "crossing.app".equals(e.pkg()))
                    .count() == 2, "rollover fabricated public lifecycle events");
            clock.set(50 * DAY);
            history.flush();
            check(ActivityUsageHistory.aggregate(history.snapshot(), DAY, 0, 50 * DAY).isEmpty(),
                    "daily intervals outside Android retention were included");
            check(history.snapshot().events().isEmpty(), "expired public events survived retention");
        }
    }

    private static void pausedOnly() throws Exception {
        AtomicLong clock = new AtomicLong(100);
        try (ActivityUsageHistory history = store(null, clock)) {
            Object a = new Object();
            history.transition(a, "paused", "A", PAUSED);
            history.transition(a, "paused", "A", PAUSED);
            clock.set(200);
            history.transition(a, "paused", "A", STOPPED);
            clock.set(300);
            history.transition(a, "paused", "A", PAUSED);
            clock.set(400);
            history.transition(a, "paused", "A", TERMINATED);
            ActivityUsageHistory.Total total = summary(history, 100, 401, "paused");
            check(total.foreground == 0 && total.visible == 200 && total.launches == 0,
                    "paused-only or STOPPED-to-PAUSED visibility invented foreground time");
            List<ActivityUsageHistory.Event> events = history.snapshot().events();
            check(events.size() == 4 && events.stream().noneMatch(e -> e.state() == RESUMED),
                    "paused-only transition was suppressed or replaced by a resume");
            check(events.stream().allMatch(e -> e.id() == events.get(0).id()),
                    "stopped/restarted instance changed identity before destruction");
            history.transition(a, "paused", "A", TERMINATED);
            check(history.snapshot().events().size() == 4, "destruction was duplicated");
        }
    }

    private static void evictionAndRecovery(Path file) throws Exception {
        AtomicLong clock = new AtomicLong(100);
        int firstId;
        try (ActivityUsageHistory history = store(file, clock)) {
            Object continuous = new Object(), completed = new Object(), other = new Object();
            history.transition(continuous, "continuous", "A", RESUMED);
            firstId = history.snapshot().events().get(0).id();
            history.transition(completed, "completed", "C", RESUMED);
            clock.set(110);
            history.transition(completed, "completed", "C", STOPPED);
            for (int i = 0; i < ActivityUsageHistory.MAX_ENTRIES; i++) {
                clock.set(201 + i);
                history.transition(other, "other", "B", (i & 1) == 0 ? RESUMED : PAUSED);
            }
            clock.set(12_201);
            history.flush();
            var events = history.snapshot().events();
            check(events.size() == ActivityUsageHistory.MAX_ENTRIES, "event suffix exceeded its cap");
            check(events.stream().noneMatch(e -> "continuous".equals(e.pkg()) || "completed".equals(e.pkg())),
                    "test did not evict both the active seed and the completed interval's events");
            ActivityUsageHistory.Total live = summary(history, 100, 12_202, "continuous");
            check(live.foreground == 12_101 && live.visible == 12_101,
                    "eviction lost a still-visible package's aggregate state");
            check(summary(history, 100, 12_202, "completed").foreground == 10,
                    "eviction erased completed usage");
            history.flush();
            check(Files.size(file) < 1_500_000, "bounded ordinary checkpoint unexpectedly expanded");
        }
        clock.set(20_000);
        try (ActivityUsageHistory recovered = store(file, clock)) {
            check(summary(recovered, 0, DAY, "continuous").foreground == 12_101,
                    "recovery lost the evicted active seed or counted broker downtime");
            check(summary(recovered, 0, DAY, "completed").foreground == 10,
                    "recovery lost completed aggregates after event eviction");
            Object replacement = new Object();
            recovered.transition(replacement, "continuous", "A", RESUMED);
            var events = recovered.snapshot().events();
            check(events.get(events.size() - 1).id() > firstId,
                    "recovery reused an evicted persisted instance ID");
            clock.set(20_100);
            recovered.transition(replacement, "continuous", "A", STOPPED);
            check(summary(recovered, 0, DAY, "continuous").foreground == 12_201,
                    "new process continuation included the offline gap");
            recovered.forgetPackage("continuous");
            check(recovered.snapshot().events().stream().noneMatch(e -> "continuous".equals(e.pkg())),
                    "uninstall left public events");
        }
        try (ActivityUsageHistory recovered = store(file, clock)) {
            check(ActivityUsageHistory.aggregate(recovered.snapshot(), DAY, 0, DAY).stream()
                    .noneMatch(t -> t.pkg.equals("continuous")), "uninstall left persisted aggregate state");
            check(summary(recovered, 0, DAY, "completed").foreground == 10,
                    "uninstall erased another package's aggregate");
        }
    }

    private static void storedOrigins(Path file) throws Exception {
        AtomicLong clock = new AtomicLong(100);
        long expiry = DAY + 100;
        try (ActivityUsageHistory history = store(file, clock)) {
            Object a = new Object();
            history.transition(a, "app", "A", RESUMED);
            clock.set(200);
            history.transition(a, "app", "A", STOPPED);
            check(summary(history, 100, 201, "app").begin == 100, "fresh interval was epoch-aligned");
            check(ActivityUsageHistory.bestInterval(history.snapshot(), 100) == 0,
                    "fresh BEST did not fall back to daily");
            history.flush();
            check(ActivityUsageHistory.bestInterval(history.snapshot(), 100) == 3,
                    "equidistant persisted origins did not keep AOSP's larger-interval tie");
            check(ActivityUsageHistory.bestInterval(history.snapshot(), 50) == 0,
                    "BEST used a stored interval after the requested beginning");
            clock.set(expiry);
            history.transition(a, "app", "A", PAUSED);
            clock.set(expiry + 50);
            history.transition(a, "app", "A", STOPPED);
            history.flush();
            check(ActivityUsageHistory.bestInterval(history.snapshot(), expiry + 20) == 0,
                    "BEST ignored the nearer stored daily origin");
            var total = summary(history, expiry, expiry + 51, "app");
            check(total.begin == expiry && total.visible == 50 && total.foreground == 0,
                    "rollover did not retain the new actual origin or PAUSED visibility");
        }
        clock.set(expiry + 500);
        try (ActivityUsageHistory history = store(file, clock)) {
            check(summary(history, expiry, expiry + 501, "app").begin == expiry,
                    "restart changed a non-expired persisted origin");
            check(ActivityUsageHistory.bestInterval(history.snapshot(), expiry + 20) == 0,
                    "recovery lost the available-origin index");
            check(summary(history, 100, 201, "app").foreground == 100,
                    "rollover/recovery replaced the completed earlier interval");
        }
    }

    private static void clockCorrections(Path file) throws Exception {
        long origin = 10 * DAY + 100, hour = 3_600_000;
        AtomicLong wall = new AtomicLong(origin), elapsed = new AtomicLong(1_000);
        try (ActivityUsageHistory history = store(file, wall, elapsed)) {
            Object activity = new Object();
            wall.addAndGet(10); elapsed.addAndGet(10);
            history.transition(activity, "clock", "A", RESUMED);
            wall.addAndGet(100); elapsed.addAndGet(100);
            history.flush();
            check(summary(history, 0, wall.get() + 1, "clock").foreground == 100, "explicit elapsed flush failed");
            wall.addAndGet(hour);
            ActivityUsageHistory.Total forward = summary(history, 0, wall.get() + 1, "clock");
            check(forward.foreground == 100 && forward.visible == 100 && forward.begin == origin + hour
                    && forward.launches == 0, "clock reload did not retain V2 fields or clear the live map");
            check(history.snapshot().events().get(0).time() == origin + hour + 10,
                    "forward correction left a public event on the old axis");
            wall.addAndGet(100); elapsed.addAndGet(100);
            history.transition(activity, "clock", "A", PAUSED);
            // onTimeChanged persisted and reloaded V2, which has no live activity
            // maps. A later real pause begins visibility; it cannot accrue a
            // fictitious continuation of the pre-reload foreground session.
            check(summary(history, 0, wall.get() + 1, "clock").foreground == 100,
                    "clock reload fabricated an ongoing foreground session");
            wall.addAndGet(50); elapsed.addAndGet(50);
            history.flush();
            wall.addAndGet(-2 * hour + 50); elapsed.addAndGet(50);
            ActivityUsageHistory.Total backward = summary(history, 0, wall.get() + 1, "clock");
            check(backward.foreground == 100 && backward.visible == 150
                    && backward.begin == origin - hour, "backward reload changed already recorded durations");
            wall.addAndGet(50); elapsed.addAndGet(50);
            history.transition(activity, "clock", "A", STOPPED);
            var events = history.snapshot().events();
            check(events.get(0).time() == origin - hour + 10 && events.get(1).time() == origin - hour + 210
                    && events.get(2).time() == origin - hour + 360,
                    "rebased events and new timestamps are not on the same axis");
            history.flush();
        }
        wall.addAndGet(10_000); elapsed.addAndGet(10_000);
        try (ActivityUsageHistory history = store(file, wall, elapsed)) {
            ActivityUsageHistory.Total total = summary(history, 0, wall.get() + 1, "clock");
            check(total.begin == origin - hour && total.foreground == 100 && total.visible == 150,
                    "persisted correction lost recorded durations or included broker downtime");
            check(history.snapshot().events().get(0).time() == origin - hour + 10,
                    "recovery restored pre-correction event timestamps");
            check(ActivityUsageHistory.bestInterval(history.snapshot(), origin - hour + 1) == 3,
                    "recovery did not rebase the stored-origin index");
            history.flush();
            wall.addAndGet(hour);
            history.flush();
        }
        try (ActivityUsageHistory history = store(file, wall, elapsed)) {
            ActivityUsageHistory.Total total = summary(history, 0, wall.get() + 1, "clock");
            check(total.begin == origin && total.foreground == 100 && total.visible == 150,
                    "idle flush skipped a clock correction or changed already recorded usage");
        }
    }

    private static void smallClockSkewRecovery(Path file) throws Exception {
        AtomicLong wall = new AtomicLong(100), elapsed = new AtomicLong(1_000);
        try (ActivityUsageHistory history = store(file, wall, elapsed)) {
            Object activity = new Object();
            history.transition(activity, "skew", "A", RESUMED);
            // This one-millisecond sampling skew is below the RTC correction
            // threshold. reportEvent still uses the elapsed-to-wall prediction.
            wall.set(199); elapsed.set(1_100);
            history.transition(activity, "skew", "A", STOPPED);
            check(history.snapshot().events().get(1).time() == 200,
                    "small wall-clock skew changed the elapsed event timestamp");
        }
        wall.set(300); elapsed.set(1_200);
        try (ActivityUsageHistory history = store(file, wall, elapsed)) {
            ActivityUsageHistory.Total total = summary(history, 0, 301, "skew");
            check(total.foreground == 100 && total.visible == 100,
                    "small clock skew made a valid checkpoint lose accumulated usage");
            check(history.snapshot().events().size() == 2
                    && history.snapshot().events().get(1).time() == 200,
                    "small clock skew made a valid checkpoint lose lifecycle events");
        }
    }

    private static void legacyAndCorrupt(Path file) throws Exception {
        try (DataOutputStream out = new DataOutputStream(Files.newOutputStream(file))) {
            out.writeInt(1); out.writeLong(200); out.writeInt(3);
            for (int i = 0; i < 3; i++) {
                out.writeLong(new long[]{100, 130, 150}[i]); out.writeInt(7); out.writeByte(i);
                out.writeUTF("legacy"); out.writeUTF("A");
            }
        }
        AtomicLong clock = new AtomicLong(300);
        try (ActivityUsageHistory history = store(file, clock)) {
            ActivityUsageHistory.Total total = summary(history, 0, DAY, "legacy");
            check(total.foreground == 30 && total.visible == 50, "legacy migration changed known durations");
            check(total.begin == 0, "legacy migration fabricated an unavailable service origin");
            history.flush();
        }
        byte[] saved = Files.readAllBytes(file);
        Files.write(file, java.util.Arrays.copyOf(saved, saved.length - 3));
        AtomicLong failures = new AtomicLong();
        try (ActivityUsageHistory history = new ActivityUsageHistory(file, clock::get,
                failure -> failures.incrementAndGet())) {
            check(failures.get() == 1 && history.snapshot().events().isEmpty(),
                    "truncated checkpoint partially published public events");
            check(ActivityUsageHistory.aggregate(history.snapshot(), DAY, 0, DAY).isEmpty(),
                    "truncated checkpoint partially published aggregates");
        }
    }

    private static void offlineClockWithoutAnchor(Path file) throws Exception {
        AtomicLong clock = new AtomicLong(100);
        try (ActivityUsageHistory history = store(file, clock)) {
            Object activity = new Object();
            history.transition(activity, "closed", "A", RESUMED);
            clock.set(200);
            history.transition(activity, "closed", "A", STOPPED);
        }
        // The service did not observe this correction. It cannot infer elapsed
        // duration or shift old totals from a new process's elapsed clock.
        clock.set(150);
        try (ActivityUsageHistory history = store(file, clock)) {
            check(summary(history, 0, 151, "closed").foreground == 100,
                    "offline RTC change rewrote completed usage");
            clock.set(160);
            history.transition(new Object(), "newer", "B", RESUMED);
            history.flush();
        }
        clock.set(170);
        try (ActivityUsageHistory history = store(file, clock)) {
            ActivityUsageHistory.Total old = summary(history, 0, 171, "closed");
            check(old.foreground == 100 && old.visible == 100 && old.end == 200 && old.lastUsed == 200,
                    "new unrelated activity made the checkpoint unreadable or erased prior totals");
            check(summary(history, 0, 171, "newer").foreground == 0,
                    "recovered live activity included the offline gap");
            Object paused = new Object();
            history.transition(paused, "closed", "A", PAUSED);
            clock.set(180);
            history.transition(paused, "closed", "A", STOPPED);
            history.flush();
        }
        clock.set(190);
        try (ActivityUsageHistory history = store(file, clock)) {
            ActivityUsageHistory.Total old = summary(history, 0, 191, "closed");
            check(old.foreground == 100 && old.visible == 110 && old.lastUsed == 200,
                    "a PAUSED-only return after an offline correction lost completed usage");
            check(summary(history, 0, 191, "newer").foreground == 0,
                    "multiple restarts invented continuation of a different package");
        }
    }

    private static void readsDoNotFlush(Path file) throws Exception {
        AtomicLong clock = new AtomicLong(100);
        try (ActivityUsageHistory history = store(file, clock)) {
            history.transition(new Object(), "read", "A", RESUMED);
            clock.set(200);
            ActivityUsageHistory.Total before = summary(history, 0, 201, "read");
            check(before.foreground == 0 && before.visible == 0 && before.end == 100,
                    "ordinary read implicitly reported FLUSH_TO_DISK");
            clock.set(DAY + 101);
            history.eventSnapshot();
            ActivityUsageHistory.Total later = summary(history, 0, clock.get() + 1, "read");
            check(later.begin == 100 && later.foreground == 0 && later.end == 100,
                    "reading past the daily deadline rolled or accrued usage");
            check(!Files.exists(file), "a read unnecessarily wrote a checkpoint");
            history.flush();
            check(summary(history, 0, DAY + 100, "read").foreground == DAY - 1,
                    "a real flush did not execute the Android daily rollover");
        }
    }

    private static void publishedV2Migration(Path file) throws Exception {
        // Bytes emitted by the unchanged published contributor implementation at
        // 30970b372d0b4e36d943139a1bc88bbdb63560fe; SHA256
        // 0c55f28fdc6336bbf8eea5adc8e464c6fc91f240c62c8071ff823be75d0f619f.
        // This is an independent writer fixture, not bytes encoded by this reader.
        String saved = "AAAAAgAAAAAAAADSAAAAAQAAAAQAAAAAAAAAbgAAAAEAAAhtaWdyYXRlZAABQQAAAAAAAACqAAAAAQIACG1pZ3JhdGVkAAFB"
                + "AAAAAAAAALQAAAABAQAIbWlncmF0ZWQAAUEAAAAAAAAAyAAAAAECAAhtaWdyYXRlZAABQQAAAAAAAABkAAAAAQAAAAAAAABk"
                + "AAAAAQAIbWlncmF0ZWQAAAAAAAAAyAAAAAAAAAA8AAAAAAAAAFAAAAAAAAAAqgAAAAAAAADIAAAAAQAAAAECAAAAAAAAAGQA"
                + "AAABAAAAAAAAAGQAAAABAAhtaWdyYXRlZAAAAAAAAADIAAAAAAAAADwAAAAAAAAAUAAAAAAAAACqAAAAAAAAAMgAAAABAAAA"
                + "AQIAAAAAAAAAZAAAAAEAAAAAAAAAZAAAAAEACG1pZ3JhdGVkAAAAAAAAAMgAAAAAAAAAPAAAAAAAAABQAAAAAAAAAKoAAAAA"
                + "AAAAyAAAAAEAAAABAgAAAAAAAABkAAAAAQAAAAAAAABkAAAAAQAIbWlncmF0ZWQAAAAAAAAAyAAAAAAAAAA8AAAAAAAAAFAA"
                + "AAAAAAAAqgAAAAAAAADIAAAAAQAAAAEC";
        Files.write(file, java.util.Base64.getDecoder().decode(saved));
        AtomicLong clock = new AtomicLong(250);
        try (ActivityUsageHistory history = store(file, clock)) {
            ActivityUsageHistory.Total migrated = summary(history, 0, 251, "migrated");
            check(migrated.begin == 100 && migrated.foreground == 60 && migrated.visible == 80
                    && migrated.lastUsed == 170 && migrated.lastVisible == 200 && migrated.appLaunches == 1,
                    "published contributor v2 migration lost the actual origin or known aggregate fields");
            check(history.snapshot().events().size() == 4, "published v2 public events were not recovered");
            clock.set(260);
            Object paused = new Object();
            history.transition(paused, "migrated", "A", PAUSED);
            var events = history.snapshot().events();
            check(events.get(events.size() - 1).id() == 2, "published v2 activity ID counter was discarded");
            clock.set(280);
            history.transition(paused, "migrated", "A", STOPPED);
        }
        try (var in = new java.io.DataInputStream(Files.newInputStream(file))) {
            check(in.readInt() == 3, "migration did not publish the distinct version 3 checkpoint format");
        }
        clock.set(300);
        try (ActivityUsageHistory history = store(file, clock)) {
            ActivityUsageHistory.Total migrated = summary(history, 0, 301, "migrated");
            check(migrated.foreground == 60 && migrated.visible == 100 && migrated.begin == 100,
                    "migration/restart lost totals or counted broker downtime");
        }
    }

    public static void main(String[] args) throws Exception {
        ordinaryOverlapAndRollover();
        pausedOnly();
        Path directory = Files.createTempDirectory("activity-usage-test");
        try {
            evictionAndRecovery(directory.resolve("eviction.bin"));
            storedOrigins(directory.resolve("origins.bin"));
            clockCorrections(directory.resolve("clock.bin"));
            smallClockSkewRecovery(directory.resolve("clock-skew.bin"));
            legacyAndCorrupt(directory.resolve("legacy.bin"));
            offlineClockWithoutAnchor(directory.resolve("offline-clock.bin"));
            readsDoNotFlush(directory.resolve("read.bin"));
            publishedV2Migration(directory.resolve("published-v2.bin"));
        } finally {
            try (var paths = Files.list(directory)) {
                for (Path path : paths.toList()) Files.deleteIfExists(path);
            }
            Files.delete(directory);
        }
        System.out.println("activity usage history all passed: " + checks + " checks");
    }
}
