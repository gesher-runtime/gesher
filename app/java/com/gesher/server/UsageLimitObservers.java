/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.server;

import java.util.ArrayList;
import java.util.Arrays;
import java.util.Comparator;
import java.util.HashMap;
import java.util.HashSet;
import java.util.IdentityHashMap;
import java.util.Map;
import java.util.Objects;
import java.util.Set;
import java.util.TreeSet;
import java.util.concurrent.ScheduledThreadPoolExecutor;
import java.util.concurrent.TimeUnit;
import java.util.function.Consumer;
import java.util.function.LongSupplier;

/**
 * One-shot AppUsageObservers for the managed profile. The observed group's time is
 * the union of its active entities, not the sum of simultaneous activities/apps.
 * See Android 16 AppTimeLimitController and UsageStatsService.reportEvent: a
 * paused-but-visible activity remains in use until STOPPED/DESTROYED.
 *
 * All state is guarded by this instance. Only the earliest deadline is scheduled;
 * no thread or periodic task is needed while all observed groups are inactive.
 * Application callbacks run outside this lock on the deadline executor.
 */
final class UsageLimitObservers {
    static final long MIN_LIMIT = 60_000L;
    static final int MAX_PER_UID = 1_000;

    interface Cancellation { void cancel(); }
    interface Scheduler { Cancellation after(long delayMillis, Runnable task); }
    interface Callback { void reached(int observerId, long timeLimit, long timeUsed); }

    private static final class Timer {
        static final ScheduledThreadPoolExecutor EXECUTOR = create();
        private static ScheduledThreadPoolExecutor create() {
            ScheduledThreadPoolExecutor executor = new ScheduledThreadPoolExecutor(1, task -> {
                Thread thread = new Thread(task, "gesher-usage-limits");
                thread.setDaemon(true);
                return thread;
            });
            executor.setRemoveOnCancelPolicy(true);
            return executor;
        }
    }

    static Scheduler scheduler() {
        return (delay, task) -> {
            var future = Timer.EXECUTOR.schedule(task, delay, TimeUnit.MILLISECONDS);
            return () -> future.cancel(false);
        };
    }

    private static final class Observer {
        final int uid, id;
        final String owner;
        final Set<String> entities;
        final long limit;
        final Callback callback;
        long used, started, lastEnd, deadline;
        int active;
        boolean queued;

        Observer(int uid, int id, String owner, Set<String> entities, long limit, Callback callback) {
            this.uid = uid; this.id = id; this.owner = owner;
            this.entities = entities; this.limit = limit; this.callback = callback;
        }
    }

    private record Activity(String owner, String source) { }
    private record Reporter(int uid, String owner, String name) {
        String entity() { return owner + "/" + name; }
    }
    private record Notification(Observer observer, long used) { }

    private final LongSupplier mClock;
    private final Scheduler mScheduler;
    private final Consumer<RuntimeException> mOnFailure;
    private final Map<Integer, Map<Integer, Observer>> mOwners = new HashMap<>();
    private final Map<String, Set<Observer>> mByEntity = new HashMap<>();
    private final Map<String, Integer> mActive = new HashMap<>();
    private final IdentityHashMap<Object, Activity> mActivities = new IdentityHashMap<>();
    private final IdentityHashMap<Object, Set<Reporter>> mReporters = new IdentityHashMap<>();
    private final TreeSet<Observer> mDeadlines = new TreeSet<>(
            Comparator.comparingLong((Observer o) -> o.deadline)
                    .thenComparingInt(o -> o.uid).thenComparingInt(o -> o.id));
    private Cancellation mPending;
    private long mPendingAt, mGeneration, mLastNow;

    UsageLimitObservers(LongSupplier clock, Scheduler scheduler, Consumer<RuntimeException> onFailure) {
        mClock = clock; mScheduler = scheduler; mOnFailure = onFailure;
    }

    synchronized void register(int uid, int id, String owner, String[] entities, long limit,
            Callback callback) {
        if (entities == null || entities.length == 0) {
            throw new IllegalArgumentException("Must specify at least one package or token");
        }
        Objects.requireNonNull(callback, "callbackIntent");
        if (limit < MIN_LIMIT) throw new IllegalArgumentException("Time limit must be >= " + MIN_LIMIT);
        // Copy before publication: a caller changing its array cannot alter an observer.
        Set<String> observed = new HashSet<>(Arrays.asList(entities));
        Map<Integer, Observer> ownerGroups = mOwners.get(uid);
        Observer old = ownerGroups == null ? null : ownerGroups.get(id);
        if (old == null && ownerGroups != null && ownerGroups.size() >= MAX_PER_UID) {
            throw new IllegalStateException("Too many app usage observers added by uid " + uid);
        }
        if (old != null) remove(old);
        Observer observer = new Observer(uid, id, owner, observed, limit, callback);
        mOwners.computeIfAbsent(uid, ignored -> new HashMap<>()).put(id, observer);
        for (String entity : observed) {
            mByEntity.computeIfAbsent(entity, ignored -> new HashSet<>()).add(observer);
            if (mActive.containsKey(entity)) observer.active++;
        }
        long now = now();
        // Registration starts a fresh budget; previously elapsed usage is not retroactive.
        if (observer.active != 0) {
            observer.started = now;
            enqueue(observer, add(now, limit));
        }
        schedule(now);
    }

    synchronized void unregister(int uid, int id) {
        Map<Integer, Observer> groups = mOwners.get(uid);
        Observer observer = groups == null ? null : groups.get(id);
        if (observer != null) remove(observer);
        schedule(now());
    }

    synchronized void transition(Object token, String owner, String usageSource, boolean visible) {
        Objects.requireNonNull(token, "activity");
        long now = now();
        Activity previous = mActivities.get(token);
        if (visible) {
            if (previous == null) {
                mActivities.put(token, new Activity(owner, usageSource));
                start(usageSource, now, now);
            }
            // A later PAUSED/RESUMED event does not restart usage or retarget its
            // task root. AOSP retains the source captured by the first event.
        } else {
            if (previous != null) {
                mActivities.remove(token);
                stop(previous.source, now);
            }
            Set<Reporter> reports = mReporters.remove(token);
            if (reports != null) for (Reporter reporter : reports) stop(reporter.entity(), now);
        }
        schedule(now);
    }

    synchronized void reportStart(int uid, String owner, Object activity, String name, long timeAgo) {
        Objects.requireNonNull(activity, "activity");
        Objects.requireNonNull(name, "token");
        if (timeAgo < 0) throw new IllegalArgumentException("timeAgo must not be negative");
        Reporter reporter = new Reporter(uid, owner, name);
        Set<Reporter> reports = mReporters.computeIfAbsent(activity, ignored -> new HashSet<>());
        if (!reports.add(reporter)) throw new IllegalArgumentException("Usage token is already started");
        long now = now();
        start(reporter.entity(), Math.max(0, now - timeAgo), now);
        schedule(now);
    }

    synchronized void reportStop(int uid, String owner, Object activity, String name) {
        Objects.requireNonNull(activity, "activity");
        Objects.requireNonNull(name, "token");
        Reporter reporter = new Reporter(uid, owner, name);
        Set<Reporter> reports = mReporters.get(activity);
        if (reports == null || !reports.remove(reporter)) {
            throw new IllegalArgumentException("Usage token is not started for this activity");
        }
        if (reports.isEmpty()) mReporters.remove(activity);
        long now = now();
        stop(reporter.entity(), now);
        schedule(now);
    }

    synchronized void forgetPackage(String owner) {
        // First retire this package's callbacks, including shared-UID registrations.
        var retired = new ArrayList<Observer>();
        for (var groups : mOwners.values()) {
            for (Observer observer : groups.values()) if (owner.equals(observer.owner)) retired.add(observer);
        }
        for (Observer observer : retired) remove(observer);
        long now = now();
        var activities = mActivities.entrySet().iterator();
        while (activities.hasNext()) {
            var entry = activities.next();
            if (owner.equals(entry.getValue().owner) || owner.equals(entry.getValue().source)) {
                stop(entry.getValue().source, now);
                activities.remove();
            }
        }
        var reporters = mReporters.values().iterator();
        while (reporters.hasNext()) {
            Set<Reporter> reports = reporters.next();
            reports.removeIf(reporter -> {
                if (!owner.equals(reporter.owner)) return false;
                stop(reporter.entity(), now);
                return true;
            });
            if (reports.isEmpty()) reporters.remove();
        }
        schedule(now);
    }

    private void start(String entity, long started, long now) {
        int count = mActive.getOrDefault(entity, 0);
        mActive.put(entity, count + 1);
        if (count != 0) return;
        Set<Observer> observers = mByEntity.get(entity);
        if (observers == null) return;
        for (Observer observer : observers) {
            if (observer.active++ == 0) {
                // Backdated reports cannot double-count an earlier, completed session.
                observer.started = Math.max(started, observer.lastEnd);
                enqueue(observer, observer.used >= observer.limit ? now
                        : add(observer.started, observer.limit - observer.used));
            }
        }
    }

    private void stop(String entity, long now) {
        Integer count = mActive.get(entity);
        if (count == null) return;
        if (count > 1) { mActive.put(entity, count - 1); return; }
        mActive.remove(entity);
        Set<Observer> observers = mByEntity.get(entity);
        if (observers == null) return;
        for (Observer observer : observers) {
            if (--observer.active == 0) {
                observer.used = add(observer.used, Math.max(0, now - observer.started));
                observer.lastEnd = now;
                dequeue(observer);
                if (observer.used >= observer.limit) enqueue(observer, now);
            }
        }
    }

    private void enqueue(Observer observer, long deadline) {
        dequeue(observer);
        observer.deadline = deadline;
        observer.queued = true;
        mDeadlines.add(observer);
    }

    private void dequeue(Observer observer) {
        if (observer.queued) mDeadlines.remove(observer);
        observer.queued = false;
    }

    private void remove(Observer observer) {
        dequeue(observer);
        Map<Integer, Observer> groups = mOwners.get(observer.uid);
        groups.remove(observer.id);
        if (groups.isEmpty()) mOwners.remove(observer.uid);
        for (String entity : observer.entities) {
            Set<Observer> observers = mByEntity.get(entity);
            observers.remove(observer);
            if (observers.isEmpty()) mByEntity.remove(entity);
        }
    }

    private long now() {
        // elapsedRealtime is monotonic in production; clamp injected backwards
        // samples rather than turning them into negative usage or repeated callbacks.
        return mLastNow = Math.max(mLastNow, mClock.getAsLong());
    }

    private static long add(long a, long b) { return a > Long.MAX_VALUE - b ? Long.MAX_VALUE : a + b; }

    private void schedule(long now) {
        Long next = mDeadlines.isEmpty() ? null : mDeadlines.first().deadline;
        if (mPending != null && next != null && mPendingAt == next) return;
        if (mPending != null) mPending.cancel();
        mPending = null;
        long generation = ++mGeneration;
        if (next != null) {
            mPendingAt = next;
            mPending = mScheduler.after(Math.max(0, next - now), () -> expired(generation));
        }
    }

    private void expired(long generation) {
        var ready = new ArrayList<Notification>();
        synchronized (this) {
            if (generation != mGeneration) return; // cancelled or replaced timer raced us
            mPending = null;
            long now = now();
            while (!mDeadlines.isEmpty() && mDeadlines.first().deadline <= now) {
                Observer observer = mDeadlines.first();
                long used = observer.active == 0 ? observer.used
                        : add(observer.used, Math.max(0, now - observer.started));
                // Still revocable while earlier callbacks run. Unregister,
                // replacement or package removal must cancel a ready callback
                // until its own delivery begins, as AOSP clears the callback.
                dequeue(observer);
                ready.add(new Notification(observer, used));
            }
            schedule(now);
        }
        for (Notification notification : ready) {
            Observer observer = notification.observer;
            synchronized (this) {
                Map<Integer, Observer> groups = mOwners.get(observer.uid);
                if (groups == null || groups.get(observer.id) != observer) continue;
                remove(observer); // claim this one-shot before its reentrant callback
                schedule(now());
            }
            try { observer.callback.reached(observer.id, observer.limit, notification.used); }
            catch (RuntimeException failure) { mOnFailure.accept(failure); }
        }
    }
}
