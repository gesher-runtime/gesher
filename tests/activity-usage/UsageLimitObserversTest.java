/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.server;

import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.atomic.AtomicLong;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;

/** Deterministic boundary clock/scheduler; the complete observer engine is production code. */
public final class UsageLimitObserversTest {
    private static int checks;
    static void check(boolean value, String message) {
        if (!value) throw new AssertionError(message);
        checks++;
    }
    static void throwsType(Class<? extends Throwable> type, Runnable action) {
        try { action.run(); }
        catch (Throwable failure) {
            check(type.isInstance(failure), "Wrong exception: " + failure);
            return;
        }
        throw new AssertionError("Missing " + type.getSimpleName());
    }

    static final class ManualScheduler implements UsageLimitObservers.Scheduler {
        final AtomicLong clock;
        final List<Task> tasks = new ArrayList<>();
        int schedules, wakeups, peak;
        final class Task {
            final long at;
            final Runnable action;
            boolean cancelled;
            Task(long at, Runnable action) { this.at = at; this.action = action; }
        }
        ManualScheduler(AtomicLong clock) { this.clock = clock; }
        @Override public UsageLimitObservers.Cancellation after(long delay, Runnable action) {
            check(delay >= 0, "Negative scheduler delay");
            Task task = new Task(clock.get() + delay, action);
            tasks.add(task); schedules++;
            peak = Math.max(peak, pending());
            check(pending() <= 1, "More than one outstanding deadline");
            return () -> task.cancelled = true;
        }
        int pending() { return (int) tasks.stream().filter(t -> !t.cancelled).count(); }
        long next() { return tasks.stream().filter(t -> !t.cancelled).mapToLong(t -> t.at).min().orElse(-1); }
        void drain() {
            for (int rounds = 0; ; rounds++) {
                check(rounds < 100, "Timer spins without time advancing");
                Task task = tasks.stream().filter(t -> !t.cancelled && t.at <= clock.get()).findFirst().orElse(null);
                if (task == null) return;
                task.cancelled = true;
                wakeups++;
                task.action.run();
            }
        }
        void advance(long now) { clock.set(now); drain(); }
    }

    private record Notice(int id, long limit, long used) { }
    private static final class Fixture {
        final AtomicLong clock = new AtomicLong(1_000);
        final ManualScheduler timer = new ManualScheduler(clock);
        final List<Notice> notices = new ArrayList<>();
        final List<RuntimeException> failures = new ArrayList<>();
        final UsageLimitObservers observers = new UsageLimitObservers(clock::get, timer, failures::add);
        final UsageLimitObservers.Callback callback = (id, limit, used) -> notices.add(new Notice(id, limit, used));
        void register(int uid, int id, long limit, String... entities) {
            observers.register(uid, id, "owner." + uid, entities, limit, callback);
        }
    }

    private static void boundaries() {
        Fixture f = new Fixture();
        throwsType(IllegalArgumentException.class, () -> f.register(1, 1, 60_000));
        throwsType(IllegalArgumentException.class,
                () -> f.observers.register(1, 1, "owner.1", null, 60_000, f.callback));
        throwsType(NullPointerException.class,
                () -> f.observers.register(1, 1, "owner.1", new String[]{"app"}, 60_000, null));
        throwsType(IllegalArgumentException.class, () -> f.register(1, 1, 59_999, "app"));
        check(f.timer.schedules == 0, "Rejected registration scheduled work");
        for (int i = 0; i < 1_000; i++) f.register(1, i, 60_000, "app");
        throwsType(IllegalStateException.class, () -> f.register(1, 1_000, 60_000, "app"));
        f.register(1, 0, 60_000, "replacement"); // replacement at quota succeeds
        f.register(2, 1_000, 60_000, "app"); // quota is per UID
        f.observers.unregister(1, 999);
        f.register(1, 1_000, 60_000, "app");
        f.observers.unregister(3, 0);
        check(f.timer.schedules == 0, "Inactive observers allocated timers");
        f.timer.advance(86_400_000);
        check(f.timer.wakeups == 0 && f.notices.isEmpty(), "Inactive observer polled or fired");
    }

    private static void unionAndPausedVisibility() {
        Fixture f = new Fixture();
        Object first = new Object(), second = new Object(), third = new Object();
        f.register(1, 7, 60_000, "a", "b", "a");
        f.observers.transition(first, "a", "a", true);
        check(f.timer.next() == 61_000, "First visible activity did not start deadline");
        f.clock.set(11_000);
        f.observers.transition(first, "a", "a", true); // RESUMED -> PAUSED
        f.observers.transition(second, "a", "a", true); // second window of same package
        f.observers.transition(third, "b", "b", true); // second observed entity
        check(f.timer.schedules == 1, "Unchanged union scheduled more timers");
        f.clock.set(21_000);
        f.observers.transition(first, "a", "a", false);
        f.observers.transition(second, "a", "a", false);
        check(f.timer.next() == 61_000, "One group's live peer lost its time budget");
        f.clock.set(31_000);
        f.observers.transition(third, "b", "b", false);
        check(f.timer.pending() == 0, "Inactive group retained a timer");
        f.clock.set(81_000);
        f.observers.transition(first, "a", "a", true);
        check(f.timer.next() == 111_000, "Stopped interval was counted as usage");
        f.timer.advance(110_999);
        check(f.notices.isEmpty(), "Observer fired early");
        f.timer.advance(111_000);
        check(f.notices.equals(List.of(new Notice(7, 60_000, 60_000))), "Incorrect union callback");
        f.timer.advance(211_000);
        f.observers.transition(first, "a", "a", false);
        check(f.notices.size() == 1 && f.timer.pending() == 0, "Observer was not one-shot");
    }

    private static void registrationReplacementAndCancellation() {
        Fixture f = new Fixture();
        Object activity = new Object();
        f.observers.transition(activity, "app", "app", true);
        f.clock.set(31_000);
        String[] input = {"app"};
        f.observers.register(1, 7, "owner.1", input, 60_000, f.callback);
        input[0] = "other";
        check(f.timer.next() == 91_000, "Registration counted earlier usage");
        var stale = f.timer.tasks.getLast();
        f.clock.set(41_000);
        f.register(1, 7, 120_000, "app");
        check(f.timer.next() == 161_000, "Replacement inherited previous time");
        stale.action.run();
        check(f.notices.isEmpty() && f.timer.pending() == 1, "Cancelled timer retired replacement");
        f.register(2, 7, 60_000, "app");
        f.observers.unregister(1, 7);
        f.timer.advance(101_000);
        check(f.notices.equals(List.of(new Notice(7, 60_000, 60_000))), "Unregister crossed UID boundary");
        f.clock.set(201_000);
        f.register(1, 7, 60_000, "app");
        f.clock.set(261_100); // delayed timer: event updates must not lose due callback
        f.observers.transition(activity, "app", "app", false);
        f.observers.transition(activity, "app", "app", true);
        f.timer.drain();
        check(f.notices.getLast().used == 60_100, "Stop/resume lost a queued expiration");
        f.register(1, 8, 60_000, "app");
        var cancelled = f.timer.tasks.getLast();
        f.observers.unregister(1, 8);
        f.clock.addAndGet(80_000);
        cancelled.action.run();
        check(f.notices.size() == 2 && f.timer.pending() == 0, "Unregistered callback ran");
    }

    private static void reportsAndTaskRoots() {
        Fixture f = new Fixture();
        Object child = new Object(), report = new Object();
        f.register(1, 1, 60_000, "root");
        f.register(1, 2, 60_000, "child");
        f.register(1, 3, 60_000, "child/feature");
        f.observers.transition(child, "child", "root", true);
        f.clock.set(11_000);
        f.observers.transition(child, "child", "new-root", true); // keep captured source
        f.observers.reportStart(2, "child", report, "feature", 5_000);
        throwsType(IllegalArgumentException.class,
                () -> f.observers.reportStart(2, "child", report, "feature", 0));
        throwsType(IllegalArgumentException.class,
                () -> f.observers.reportStop(9, "child", report, "feature"));
        f.timer.advance(61_000);
        check(f.notices.equals(List.of(new Notice(1, 60_000, 60_000))), "Task root attribution changed");
        f.timer.advance(66_000);
        check(f.notices.getLast().equals(new Notice(3, 60_000, 60_000)), "Backdated report not counted");
        f.observers.reportStop(2, "child", report, "feature");
        throwsType(IllegalArgumentException.class,
                () -> f.observers.reportStop(2, "child", report, "feature"));
        f.register(1, 4, 60_000, "child/feature");
        f.observers.reportStart(2, "child", child, "feature", 0);
        f.clock.set(76_000);
        f.observers.transition(child, "child", "root", false);
        check(f.timer.pending() == 0, "Activity stop retained its reported token");
        f.timer.advance(200_000);
        check(f.notices.size() == 2, "Stopped report or child package fired");

        f.register(1, 5, 60_000, "x/feature");
        Object a = new Object(), b = new Object();
        f.observers.reportStart(2, "x", a, "feature", 0);
        f.observers.reportStart(2, "x", b, "feature", 0);
        f.clock.addAndGet(20_000);
        f.observers.reportStop(2, "x", a, "feature");
        f.clock.addAndGet(10_000);
        f.observers.reportStop(2, "x", b, "feature");
        f.clock.addAndGet(20_000);
        f.observers.reportStart(2, "x", a, "feature", 40_000); // overlaps completed session
        f.timer.advance(260_000);
        check(f.notices.getLast().equals(new Notice(5, 60_000, 60_000)),
                "Overlapping backdated reports double-counted usage");
    }

    private static void retirementAndReentrancy() {
        Fixture f = new Fixture();
        Object a = new Object(), b = new Object();
        f.register(1, 1, 60_000, "app");
        f.register(2, 2, 60_000, "app");
        f.observers.transition(a, "app", "app", true);
        f.observers.forgetPackage("owner.1");
        f.timer.advance(61_000);
        check(f.notices.equals(List.of(new Notice(2, 60_000, 60_000))), "Removed owner retained callback");
        f.register(1, 3, 60_000, "app");
        f.observers.forgetPackage("app");
        check(f.timer.pending() == 0, "Removed observed package remained active");
        f.clock.set(101_000);
        f.observers.transition(b, "app", "app", true);
        f.timer.advance(161_000);
        check(f.notices.getLast().id == 3, "New installation did not start fresh activity usage");
        f.observers.register(1, 9, "owner.1", new String[]{"app"}, 60_000,
                (id, limit, used) -> {
                    f.callback.reached(id, limit, used);
                    f.register(1, 9, 60_000, "app");
                });
        f.observers.register(2, 8, "owner.2", new String[]{"app"}, 60_000,
                (id, limit, used) -> { throw new IllegalStateException("cancelled boundary"); });
        f.timer.advance(221_000);
        check(f.failures.size() == 1 && f.timer.next() == 281_000,
                "Failed/reentrant callback blocked another observer or removed replacement");
        f.timer.advance(281_000);
        check(f.notices.getLast().id == 9, "Reentrant replacement never fired");
    }

    private static void noPollingAndDeadlineOrder() {
        Fixture f = new Fixture();
        Object activity = new Object();
        for (int i = 0; i < 1_000; i++) f.register(1, i, 60_000 + i, "app");
        f.observers.transition(activity, "app", "app", true);
        int scheduled = f.timer.schedules;
        for (int i = 0; i < 10_000; i++) f.observers.transition(activity, "app", "app", true);
        check(f.timer.schedules == scheduled && f.timer.wakeups == 0,
                "Repeated lifecycle events rearmed or polled the timer");
        f.timer.advance(61_999); // one delayed wake handles every due observer
        check(f.notices.size() == 1_000 && f.timer.wakeups == 1 && f.timer.pending() == 0,
                "Due observers did not share a single deadline wakeup");
        for (int i = 0; i < f.notices.size(); i++) {
            check(f.notices.get(i).id == i && f.notices.get(i).used == 60_999,
                    "Deadline order or elapsed callback amount changed");
        }
        System.out.println("usage-observer scheduling: observers=1000 repeated_events=10000 "
                + "scheduled_tasks=" + scheduled + " deadline_wakeups=" + f.timer.wakeups
                + " peak_pending=" + f.timer.peak + " idle_wakeups=0");
    }

    private static void concurrentRealScheduler() throws Exception {
        AtomicLong clock = new AtomicLong(1_000);
        AtomicInteger delivered = new AtomicInteger(), failures = new AtomicInteger();
        CountDownLatch callbacks = new CountDownLatch(128);
        UsageLimitObservers observers = new UsageLimitObservers(clock::get,
                UsageLimitObservers.scheduler(), failure -> failures.incrementAndGet());
        Object activity = new Object();
        observers.transition(activity, "app", "app", true);
        List<Thread> threads = new ArrayList<>();
        for (int uid = 1; uid <= 4; uid++) {
            final int owner = uid;
            Thread thread = new Thread(() -> {
                for (int i = 0; i < 32; i++) {
                    int id = i;
                    observers.register(owner, id, "owner." + owner, new String[]{"app"}, 60_000,
                            (ignored, limit, used) -> {
                                delivered.incrementAndGet(); callbacks.countDown();
                                // This send may race registration/unregistration in other threads.
                                observers.unregister(owner, id);
                            });
                    observers.register(owner, id + 1_000, "owner." + owner, new String[]{"app"},
                            120_000, (ignored, limit, used) -> failures.incrementAndGet());
                    observers.unregister(owner, id + 1_000);
                }
            });
            threads.add(thread); thread.start();
        }
        for (Thread thread : threads) thread.join(10_000);
        check(threads.stream().noneMatch(Thread::isAlive), "Concurrent registration deadlocked");
        clock.set(61_001);
        observers.transition(activity, "app", "app", false); // wake overdue deadline immediately
        check(callbacks.await(10, TimeUnit.SECONDS), "Real scheduler lost an expiration");
        check(delivered.get() == 128 && failures.get() == 0, "Concurrent callbacks were lost or duplicated");
    }

    public static void main(String[] args) throws Exception {
        boundaries();
        unionAndPausedVisibility();
        registrationReplacementAndCancellation();
        reportsAndTaskRoots();
        retirementAndReentrancy();
        noPollingAndDeadlineOrder();
        concurrentRealScheduler();
        System.out.println("UsageLimitObserversTest passed: " + checks + " checks");
    }
}
