/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.server;

import android.app.PendingIntent;

import java.util.ArrayList;
import java.util.HashMap;
import java.util.HashSet;
import java.util.IdentityHashMap;
import java.util.LinkedHashSet;
import java.util.List;
import java.util.Map;
import java.util.Objects;
import java.util.Set;
import java.util.concurrent.ScheduledFuture;
import java.util.concurrent.ScheduledThreadPoolExecutor;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicLong;
import java.util.function.IntPredicate;
import java.util.function.LongSupplier;

/**
 * Event-driven foreground app usage observers (AOSP AppTimeLimitController contract).
 *
 * <p>Foreground time accumulates exclusively from {@code transition(...)} boundaries on
 * monotonic elapsed time, armed by at most one scheduled expiry task for the earliest
 * currently-resumed observed group. Idle or backgrounded observers schedule zero tasks
 * and cause zero background wakeups.
 */
final class UsageLimitObservers implements AutoCloseable {
    static final long MIN_TIME_LIMIT_MS = 60_000L;
    static final int MAX_OBSERVERS_PER_UID = 1_000;
    private static final int MAX_CACHED_COUNTERS = 256;

    interface CallbackDispatcher {
        void dispatch(PendingIntent callback, int observerId, long timeLimitMs, long timeUsedMs);
    }

    interface Cancelable {
        void cancel();
    }

    interface ExpiryScheduler extends AutoCloseable {
        Cancelable schedule(long delayMs, Runnable task);
        long wakeupCount();
        int scheduledCount();
        @Override default void close() { }
    }

    private static final class Counter {
        int count;
    }

    private static final class DaemonExpiryScheduler implements ExpiryScheduler {
        private final AtomicLong mWakeups = new AtomicLong();
        private ScheduledThreadPoolExecutor mExecutor;

        private synchronized ScheduledThreadPoolExecutor executor() {
            if (mExecutor == null) {
                ScheduledThreadPoolExecutor exec = new ScheduledThreadPoolExecutor(1, task -> {
                    Thread thread = new Thread(task, "usage-limit-expiry");
                    thread.setDaemon(true);
                    return thread;
                });
                exec.setRemoveOnCancelPolicy(true);
                mExecutor = exec;
            }
            return mExecutor;
        }

        @Override
        public Cancelable schedule(long delayMs, Runnable task) {
            ScheduledFuture<?> future = executor().schedule(() -> {
                mWakeups.incrementAndGet();
                task.run();
            }, Math.max(0L, delayMs), TimeUnit.MILLISECONDS);
            return () -> future.cancel(false);
        }

        @Override
        public long wakeupCount() {
            return mWakeups.get();
        }

        @Override
        public synchronized int scheduledCount() {
            return mExecutor == null ? 0 : mExecutor.getQueue().size();
        }

        @Override
        public synchronized void close() {
            if (mExecutor != null) {
                mExecutor.shutdownNow();
                mExecutor = null;
            }
        }
    }

    private static final class Observer {
        final int callingUid;
        final int observerId;
        final String[] packages;
        final long timeLimitMs;
        final PendingIntent callback;
        long accumulatedMs;
        long sessionStartMs;
        int activePackages;
        boolean removed;

        Observer(int callingUid, int observerId, String[] packages, long timeLimitMs,
                PendingIntent callback) {
            this.callingUid = callingUid;
            this.observerId = observerId;
            this.packages = packages;
            this.timeLimitMs = timeLimitMs;
            this.callback = callback;
        }

        long totalUsedMs(long now) {
            if (activePackages <= 0) return accumulatedMs;
            return safeAdd(accumulatedMs, Math.max(0L, now - sessionStartMs));
        }

        long deadlineMs() {
            long remaining = Math.max(0L, timeLimitMs - accumulatedMs);
            return safeAdd(sessionStartMs, remaining);
        }

        private static long safeAdd(long a, long b) {
            long r = a + b;
            return ((a ^ r) & (b ^ r)) < 0 ? Long.MAX_VALUE : r;
        }
    }

    private record Expired(Observer observer, long timeUsedMs) { }

    private final LongSupplier mElapsed;
    private final CallbackDispatcher mDispatcher;
    private final ExpiryScheduler mScheduler;
    private final long mMinTimeLimitMs;

    private final Map<Object, String> mResumedTokens = new IdentityHashMap<>();
    private final Map<String, Counter> mResumedCounts = new HashMap<>();
    private final Map<Integer, Map<Integer, Observer>> mByUid = new HashMap<>();
    private final Map<String, List<Observer>> mByPackage = new HashMap<>();
    private final Set<Observer> mActiveObservers = new HashSet<>();

    private Cancelable mScheduled;
    private long mScheduledDeadlineMs = Long.MAX_VALUE;
    private long mScheduleGeneration;

    UsageLimitObservers(LongSupplier elapsed, CallbackDispatcher dispatcher) {
        this(elapsed, dispatcher, new DaemonExpiryScheduler(), MIN_TIME_LIMIT_MS);
    }

    UsageLimitObservers(LongSupplier elapsed, CallbackDispatcher dispatcher,
            ExpiryScheduler scheduler, long minTimeLimitMs) {
        mElapsed = Objects.requireNonNull(elapsed, "elapsed");
        mDispatcher = Objects.requireNonNull(dispatcher, "dispatcher");
        mScheduler = Objects.requireNonNull(scheduler, "scheduler");
        if (minTimeLimitMs <= 0) throw new IllegalArgumentException("minTimeLimitMs must be > 0");
        mMinTimeLimitMs = minTimeLimitMs;
    }

    void register(int callingUid, int observerId, String[] packages, long timeLimitMs,
            PendingIntent callback) {
        if (packages == null || packages.length == 0) {
            throw new IllegalArgumentException("Must specify at least one package");
        }
        LinkedHashSet<String> unique = new LinkedHashSet<>(packages.length);
        for (String pkg : packages) {
            if (pkg == null || pkg.isEmpty()) {
                throw new IllegalArgumentException("Package name must not be null or empty");
            }
            unique.add(pkg);
        }
        Objects.requireNonNull(callback, "callbackIntent can't be null");
        if (timeLimitMs < mMinTimeLimitMs) {
            throw new IllegalArgumentException("Time limit must be >= " + mMinTimeLimitMs);
        }
        String[] entities = unique.toArray(new String[0]);

        synchronized (this) {
            Map<Integer, Observer> uidObservers = mByUid.get(callingUid);
            Observer existing = uidObservers != null ? uidObservers.get(observerId) : null;
            if (existing == null && uidObservers != null
                    && uidObservers.size() >= MAX_OBSERVERS_PER_UID) {
                throw new IllegalStateException("Too many observers registered for uid "
                        + callingUid);
            }
            if (existing != null) {
                unlinkLocked(existing);
            }
            long now = mElapsed.getAsLong();
            Observer observer = new Observer(callingUid, observerId, entities, timeLimitMs,
                    callback);
            int activeCount = 0;
            for (String pkg : entities) {
                Counter c = mResumedCounts.get(pkg);
                if (c != null && c.count > 0) activeCount++;
            }
            observer.activePackages = activeCount;
            if (activeCount > 0) {
                observer.sessionStartMs = now;
                mActiveObservers.add(observer);
            }
            mByUid.computeIfAbsent(callingUid, unused -> new HashMap<>()).put(observerId, observer);
            for (String pkg : entities) {
                mByPackage.computeIfAbsent(pkg, unused -> new ArrayList<>(2)).add(observer);
            }
            rescheduleLocked(now);
        }
    }

    void unregister(int callingUid, int observerId) {
        synchronized (this) {
            Map<Integer, Observer> uidObservers = mByUid.get(callingUid);
            if (uidObservers == null) return;
            Observer observer = uidObservers.get(observerId);
            if (observer == null) return;
            boolean wasActive = observer.activePackages > 0;
            unlinkLocked(observer);
            if (wasActive) {
                rescheduleLocked(mElapsed.getAsLong());
            }
        }
    }

    void transition(Object token, String pkg, ActivityUsageHistory.State state) {
        if (token == null || pkg == null || state == null) return;
        List<Expired> expired = null;
        synchronized (this) {
            if (state == ActivityUsageHistory.State.RESUMED) {
                String previous = mResumedTokens.put(token, pkg);
                if (pkg.equals(previous)) return;
                if (previous != null) {
                    expired = updatePackageResumedLocked(previous, -1, null);
                }
                expired = updatePackageResumedLocked(pkg, 1, expired);
            } else {
                if (mResumedTokens.isEmpty()) return;
                String previous = mResumedTokens.remove(token);
                if (previous == null) return;
                expired = updatePackageResumedLocked(previous, -1, null);
            }
        }
        dispatchAll(expired);
    }

    void packageRemoved(String pkg, IntPredicate uidStillInstalled) {
        if (pkg == null) return;
        List<Expired> expired = null;
        synchronized (this) {
            if (uidStillInstalled != null && !mByUid.isEmpty()) {
                List<Observer> orphaned = null;
                for (var entry : mByUid.entrySet()) {
                    if (!uidStillInstalled.test(entry.getKey())) {
                        if (orphaned == null) orphaned = new ArrayList<>();
                        orphaned.addAll(entry.getValue().values());
                    }
                }
                if (orphaned != null) {
                    for (Observer obs : orphaned) unlinkLocked(obs);
                }
            }
            if (!mResumedTokens.isEmpty()) {
                int removedTokens = 0;
                var it = mResumedTokens.entrySet().iterator();
                while (it.hasNext()) {
                    if (pkg.equals(it.next().getValue())) {
                        it.remove();
                        removedTokens++;
                    }
                }
                if (removedTokens > 0) {
                    expired = updatePackageResumedLocked(pkg, -removedTokens, null);
                }
            }
            mResumedCounts.remove(pkg);
            rescheduleLocked(mElapsed.getAsLong());
        }
        dispatchAll(expired);
    }

    private List<Expired> updatePackageResumedLocked(String pkg, int delta, List<Expired> expired) {
        Counter counter = mResumedCounts.get(pkg);
        int before = counter == null ? 0 : counter.count;
        int after = Math.max(0, before + delta);
        if (after == 0) {
            if (counter != null) {
                counter.count = 0;
                if (mResumedCounts.size() > MAX_CACHED_COUNTERS) {
                    mResumedCounts.remove(pkg);
                }
            }
        } else {
            if (counter == null) {
                if (mResumedCounts.size() >= MAX_CACHED_COUNTERS) {
                    mResumedCounts.values().removeIf(c -> c.count == 0);
                }
                counter = new Counter();
                mResumedCounts.put(pkg, counter);
            }
            counter.count = after;
        }
        if ((before > 0) == (after > 0)) return expired;

        if (mByPackage.isEmpty()) return expired;
        List<Observer> watchers = mByPackage.get(pkg);
        if (watchers == null || watchers.isEmpty()) return expired;

        long now = mElapsed.getAsLong();
        boolean isForeground = after > 0;
        int size = watchers.size();
        Observer[] snapshot = watchers.toArray(new Observer[size]);
        if (isForeground) {
            for (Observer obs : snapshot) {
                if (obs.removed) continue;
                if (obs.activePackages == 0) {
                    obs.activePackages = 1;
                    obs.sessionStartMs = now;
                    mActiveObservers.add(obs);
                } else {
                    obs.activePackages++;
                    long used = obs.totalUsedMs(now);
                    if (used >= obs.timeLimitMs) {
                        unlinkLocked(obs);
                        expired = appendExpired(expired, obs, used);
                    }
                }
            }
        } else {
            for (Observer obs : snapshot) {
                if (obs.removed || obs.activePackages <= 0) continue;
                obs.activePackages--;
                if (obs.activePackages == 0) {
                    long used = obs.accumulatedMs + Math.max(0L, now - obs.sessionStartMs);
                    obs.accumulatedMs = used;
                    mActiveObservers.remove(obs);
                    if (used >= obs.timeLimitMs) {
                        unlinkLocked(obs);
                        expired = appendExpired(expired, obs, used);
                    }
                } else {
                    long used = obs.totalUsedMs(now);
                    if (used >= obs.timeLimitMs) {
                        unlinkLocked(obs);
                        expired = appendExpired(expired, obs, used);
                    }
                }
            }
        }
        expired = collectDueLocked(now, expired);
        rescheduleLocked(now);
        return expired;
    }

    private void onScheduledExpiry(long generation) {
        List<Expired> expired;
        synchronized (this) {
            if (generation != mScheduleGeneration) return;
            mScheduled = null;
            mScheduledDeadlineMs = Long.MAX_VALUE;
            long now = mElapsed.getAsLong();
            expired = collectDueLocked(now, null);
            rescheduleLocked(now);
        }
        dispatchAll(expired);
    }

    private List<Expired> collectDueLocked(long now, List<Expired> expired) {
        if (mActiveObservers.isEmpty()) return expired;
        List<Observer> due = null;
        for (Observer obs : mActiveObservers) {
            if (obs.totalUsedMs(now) >= obs.timeLimitMs) {
                if (due == null) due = new ArrayList<>(2);
                due.add(obs);
            }
        }
        if (due != null) {
            for (Observer obs : due) {
                long used = obs.totalUsedMs(now);
                unlinkLocked(obs);
                expired = appendExpired(expired, obs, used);
            }
        }
        return expired;
    }

    private void unlinkLocked(Observer obs) {
        if (obs.removed) return;
        obs.removed = true;
        mActiveObservers.remove(obs);
        Map<Integer, Observer> uidMap = mByUid.get(obs.callingUid);
        if (uidMap != null) {
            uidMap.remove(obs.observerId, obs);
            if (uidMap.isEmpty()) mByUid.remove(obs.callingUid);
        }
        for (String pkg : obs.packages) {
            List<Observer> list = mByPackage.get(pkg);
            if (list != null) {
                list.remove(obs);
                if (list.isEmpty()) mByPackage.remove(pkg);
            }
        }
    }

    private void rescheduleLocked(long now) {
        if (mActiveObservers.isEmpty()) {
            if (mScheduled != null) {
                mScheduled.cancel();
                mScheduled = null;
                mScheduledDeadlineMs = Long.MAX_VALUE;
                mScheduleGeneration++;
            }
            return;
        }
        long earliest = Long.MAX_VALUE;
        for (Observer obs : mActiveObservers) {
            long deadline = obs.deadlineMs();
            if (deadline < earliest) earliest = deadline;
        }
        if (mScheduled != null && mScheduledDeadlineMs == earliest) return;
        if (mScheduled != null) {
            mScheduled.cancel();
            mScheduled = null;
        }
        long gen = ++mScheduleGeneration;
        mScheduledDeadlineMs = earliest;
        long delayMs = Math.max(0L, earliest - now);
        mScheduled = mScheduler.schedule(delayMs, () -> onScheduledExpiry(gen));
    }

    private static List<Expired> appendExpired(List<Expired> list, Observer obs, long used) {
        if (list == null) list = new ArrayList<>(2);
        list.add(new Expired(obs, used));
        return list;
    }

    private void dispatchAll(List<Expired> expired) {
        if (expired == null) return;
        for (Expired item : expired) {
            mDispatcher.dispatch(item.observer.callback, item.observer.observerId,
                    item.observer.timeLimitMs, item.timeUsedMs);
        }
    }

    synchronized int observerCountForUid(int uid) {
        Map<Integer, Observer> map = mByUid.get(uid);
        return map == null ? 0 : map.size();
    }

    synchronized int activeObserverCount() {
        return mActiveObservers.size();
    }

    long wakeupCount() {
        return mScheduler.wakeupCount();
    }

    int scheduledTaskCount() {
        return mScheduler.scheduledCount();
    }

    @Override
    public void close() {
        synchronized (this) {
            mScheduleGeneration++;
            if (mScheduled != null) {
                mScheduled.cancel();
                mScheduled = null;
            }
            mScheduledDeadlineMs = Long.MAX_VALUE;
            mActiveObservers.clear();
            mByUid.clear();
            mByPackage.clear();
            mResumedTokens.clear();
            mResumedCounts.clear();
        }
        mScheduler.close();
    }
}
