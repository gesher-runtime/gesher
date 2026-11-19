/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.server;

import java.io.BufferedInputStream;
import java.io.BufferedOutputStream;
import java.io.DataInputStream;
import java.io.DataOutputStream;
import java.io.IOException;
import java.io.OutputStream;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.StandardCopyOption;
import java.util.ArrayDeque;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.IdentityHashMap;
import java.util.List;
import java.util.Map;
import java.util.TreeMap;
import java.util.concurrent.Executors;
import java.util.concurrent.ScheduledExecutorService;
import java.util.concurrent.TimeUnit;
import java.util.function.Consumer;
import java.util.function.LongSupplier;

/**
 * Activity usage in the managed server. The bounded public event suffix is independent
 * of interval statistics: evicting an event must never terminate a live activity or
 * discard time already accounted to an interval. See docs/research/activity-usage-contracts.md.
 */
final class ActivityUsageHistory implements AutoCloseable {
    static final long DAY = TimeUnit.DAYS.toMillis(1);
    static final long RETENTION = 30 * DAY;
    static final int MAX_ENTRIES = 12_000;
    private static final long[] WIDTHS = {DAY, 7 * DAY, 30 * DAY, 365 * DAY};
    // UsageStatsDatabase.prune and MAX_FILES_PER_INTERVAL_TYPE, Android 16 r4.
    private static final long[] RETAIN = {10 * DAY, 28 * DAY, 180 * DAY, 730 * DAY};
    private static final int[] MAX_BUCKETS = {100, 50, 12, 10};
    private static final int FILE_VERSION = 3;
    private static final long MAX_FILE_BYTES = 64L * 1024 * 1024;
    private static final long TIME_CHANGE_THRESHOLD = 2_000;

    enum State { RESUMED, PAUSED, STOPPED, TERMINATED, CHECKPOINT_END }

    record Event(long time, String pkg, String cls, int id, State state) { }
    record Interval(long begin, long end, boolean indexed, List<Total> totals) { }
    record Snapshot(long until, List<Event> events, List<List<Interval>> intervals) { }

    private static final class Activity {
        final String pkg, cls;
        final int id;
        State state;

        Activity(String pkg, String cls, int id) {
            this.pkg = pkg;
            this.cls = cls;
            this.id = id;
        }
    }

    /** An aggregate contains the entire selected interval, not a clipped query range. */
    static final class Total {
        long begin;
        final String pkg;
        long end, foreground, visible, lastUsed, lastVisible;
        int launches, appLaunches;
        State lastEvent;
        private Map<Integer, State> activities = Map.of();
        private int resumed;
        private boolean used, seen;

        Total(long begin, String pkg) {
            this.begin = begin;
            this.pkg = pkg;
        }

        Total copy() {
            Total copy = new Total(begin, pkg);
            copy.end = end;
            copy.foreground = foreground;
            copy.visible = visible;
            copy.lastUsed = lastUsed;
            copy.lastVisible = lastVisible;
            copy.launches = launches;
            copy.appLaunches = appLaunches;
            copy.lastEvent = lastEvent;
            copy.used = used;
            copy.seen = seen;
            return copy;
        }

        private void usedUntil(long time) {
            if (time > lastUsed) foreground += time - lastUsed;
            lastUsed = Math.max(lastUsed, time);
        }

        private void visibleUntil(long time) {
            if (time > lastVisible) visible += time - lastVisible;
            lastVisible = Math.max(lastVisible, time);
        }

        // UsageStats.updateActivity: PAUSED may be the first visible state. There
        // need not have been a foreground activity, including after STOPPED.
        void update(Event event) {
            State previous = activities.get(event.id);
            if (previous == State.RESUMED) usedUntil(event.time);
            if (isVisible(previous)) visibleUntil(event.time);
            if (event.state == State.RESUMED) {
                if (activities.isEmpty()) {
                    lastUsed = lastVisible = event.time;
                } else if (resumed == 0) {
                    lastUsed = event.time;
                }
                used = seen = true;
                launches++;
            } else if (event.state == State.PAUSED) {
                if (activities.isEmpty()) lastVisible = event.time;
                seen = true;
            }
            if (previous == State.RESUMED) resumed--;
            if (event.state == State.RESUMED) resumed++;
            if (isVisible(event.state)) {
                if (activities.isEmpty()) activities = new HashMap<>();
                activities.put(event.id, event.state);
            } else if (!activities.isEmpty()) {
                activities.remove(event.id);
                if (activities.isEmpty()) activities = Map.of();
            }
            end = event.time;
            // Android 16 retains the deprecated mLastEvent field at NONE.
        }

        void checkpoint(long time) {
            if (resumed != 0) usedUntil(time);
            if (!activities.isEmpty()) visibleUntil(time);
            end = time;
        }

        void closeActivities() {
            activities = Map.of();
            resumed = 0;
        }

        void rebase(long delta) {
            begin += delta;
            end += delta;
            if (used) lastUsed += delta;
            if (seen) lastVisible += delta;
        }
    }

    private static final class Bucket {
        long begin, end;
        boolean indexed;
        final Map<String, Total> totals = new TreeMap<>();

        Bucket(long begin) {
            this.begin = begin;
            end = begin + 1;
        }

        void update(Event event, boolean appLaunch) {
            Total total = totals.computeIfAbsent(event.pkg, pkg -> new Total(begin, pkg));
            total.update(event);
            if (appLaunch) total.appLaunches++;
            end = Math.max(end, event.time);
        }

        void checkpoint(long time, boolean activeOnly) {
            for (Total total : totals.values()) {
                if (!activeOnly || !total.activities.isEmpty()) total.checkpoint(time);
            }
            end = Math.max(end, time);
        }

        void closeActivities() {
            for (Total total : totals.values()) total.closeActivities();
        }

        void reloadBoundary() {
            // Android's persisted UsageStatsProtoV2 contains cumulative times
            // and mAppLaunchCount, but no live activities or mLaunchCount. A
            // daily rollover reloads *all* current interval types before their
            // explicit continuations, including a still-current weekly bucket.
            for (Total total : totals.values()) {
                total.closeActivities();
                total.launches = 0;
                total.lastEvent = null;
                total.end = end;
            }
        }

        Interval snapshot() {
            List<Total> result = new ArrayList<>(totals.size());
            for (Total total : totals.values()) result.add(total.copy());
            return new Interval(begin, end, indexed, result);
        }

        void rebase(long delta) {
            begin += delta;
            end += delta;
            for (Total total : totals.values()) total.rebase(delta);
        }
    }

    private final Path mFile;
    private final LongSupplier mWallClock, mElapsedClock;
    private final Consumer<IOException> mOnFailure;
    private final Object mWriterLock = new Object();
    private final ArrayDeque<Event> mHistory = new ArrayDeque<>();
    private final Map<Object, Activity> mActivities = new IdentityHashMap<>();
    private final List<TreeMap<Long, Bucket>> mIntervals = new ArrayList<>(4);
    private final ScheduledExecutorService mWriter;
    private long mWallSnapshot, mElapsedSnapshot, mLastTime, mReadTime;
    private long mRevision, mWrittenRevision, mClockGeneration;
    private long mSavedClockGeneration, mRolloverRevision, mSavedRolloverRevision;
    private int mNextId, mVisible;
    private String mLastPaused;

    /** A single controlled axis is useful to deterministic host and Android fixtures. */
    ActivityUsageHistory(Path file, LongSupplier clock, Consumer<IOException> onFailure) {
        this(file, clock, clock, onFailure);
    }

    ActivityUsageHistory(Path file, LongSupplier wall, LongSupplier elapsed,
            Consumer<IOException> onFailure) {
        mFile = file;
        mWallClock = wall;
        mElapsedClock = elapsed;
        mOnFailure = onFailure;
        mWallSnapshot = wall.getAsLong();
        mElapsedSnapshot = elapsed.getAsLong();
        mLastTime = mReadTime = mWallSnapshot;
        freshIntervals(mLastTime);
        if (file == null) {
            mWriter = null;
            return;
        }
        try { read(); }
        catch (IOException failure) { onFailure.accept(failure); }
        mWriter = Executors.newSingleThreadScheduledExecutor(job -> {
            Thread thread = new Thread(job, "usage-history");
            thread.setDaemon(true);
            return thread;
        });
        mWriter.scheduleWithFixedDelay(() -> {
            try {
                boolean dirty;
                synchronized (this) {
                    now();
                    dirty = mVisible != 0 || mRevision != mWrittenRevision;
                }
                if (dirty) flush();
            } catch (IOException failure) { onFailure.accept(failure); }
        }, 30, 30, TimeUnit.SECONDS);
    }

    private void freshIntervals(long time) {
        mIntervals.clear();
        for (int i = 0; i < WIDTHS.length; i++) {
            TreeMap<Long, Bucket> intervals = new TreeMap<>();
            intervals.put(time, new Bucket(time));
            mIntervals.add(intervals);
        }
    }

    private Bucket current(int interval) {
        return mIntervals.get(interval).lastEntry().getValue();
    }

    private static boolean isVisible(State state) {
        return state == State.RESUMED || state == State.PAUSED;
    }

    void transition(Object token, String pkg, String cls, State state) {
        boolean publish;
        synchronized (this) {
            if (state == State.CHECKPOINT_END) throw new IllegalArgumentException("Internal checkpoint");
            Activity activity = mActivities.get(token);
            if (activity == null) {
                if (!isVisible(state)) return;
                activity = new Activity(pkg, cls, nextId());
                mActivities.put(token, activity);
            }
            if (activity.state == state) return;
            if (state == State.TERMINATED) {
                mActivities.remove(token);
                if (activity.state == State.STOPPED) return;
            }
            long time = now();
            rollover(time);
            boolean wasVisible = isVisible(activity.state), visible = isVisible(state);
            if (wasVisible != visible) mVisible += visible ? 1 : -1;
            activity.state = state;
            Event event = new Event(time, activity.pkg, activity.cls, activity.id, state);
            record(event);
            add(event);
            mRevision++;

            publish = mClockGeneration != mSavedClockGeneration
                    || mRolloverRevision != mSavedRolloverRevision;
        }
        if (publish) {
            try { persist(); }
            catch (IOException failure) { mOnFailure.accept(failure); }
        }
    }

    private int nextId() {
        if (mNextId < Integer.MAX_VALUE) return ++mNextId;
        // Wrap only after exhausting the ID space; never alias a live or queryable instance.
        int candidate = 0;
        outer: while (++candidate > 0) {
            for (Activity activity : mActivities.values()) if (activity.id == candidate) continue outer;
            for (Event event : mHistory) if (event.id == candidate) continue outer;
            mNextId = candidate;
            return candidate;
        }
        throw new IllegalStateException("No activity usage instance IDs available");
    }

    private void record(Event event) {
        boolean appLaunch = event.state == State.RESUMED && !event.pkg.equals(mLastPaused);
        for (int i = 0; i < WIDTHS.length; i++) current(i).update(event, appLaunch);
        if (event.state == State.PAUSED) mLastPaused = event.pkg;
    }

    Snapshot snapshot() {
        return snapshotForRead(-1, Long.MIN_VALUE, Long.MAX_VALUE, true);
    }

    Snapshot eventSnapshot() {
        return snapshotForRead(-2, 0, 0, true);
    }

    Snapshot querySnapshot(int interval, long begin, long end) {
        return snapshotForRead(interval, begin, end, false);
    }

    private Snapshot snapshotForRead(int interval, long begin, long end, boolean events) {
        Snapshot result;
        boolean clockChanged;
        synchronized (this) {
            now();
            result = copySnapshot(mReadTime, interval, begin, end, events);
            clockChanged = mClockGeneration != mSavedClockGeneration;
        }
        if (!clockChanged) return result;
        // checkAndGetTimeLocked may reconcile a real RTC correction. Its
        // persistActiveStats step saves existing accumulators; it does not
        // report FLUSH_TO_DISK, accrue activity time or roll a day for a read.
        try { persist(); }
        catch (IOException failure) { mOnFailure.accept(failure); }
        synchronized (this) { return copySnapshot(mReadTime, interval, begin, end, events); }
    }

    private Snapshot copySnapshot(long time, int selected, long begin, long end, boolean events) {
        if (selected == 4) selected = bestInterval(begin);
        List<List<Interval>> result = new ArrayList<>(WIDTHS.length);
        for (int i = 0; i < WIDTHS.length; i++) {
            var intervals = mIntervals.get(i);
            List<Interval> copies = new ArrayList<>(intervals.size());
            for (Bucket bucket : intervals.values()) {
                boolean include = selected == -1 || selected == i
                        && begin < bucket.end && end > bucket.begin;
                copies.add(include ? bucket.snapshot()
                        : new Interval(bucket.begin, bucket.end, bucket.indexed, List.of()));
            }
            result.add(copies);
        }
        return new Snapshot(time, events ? new ArrayList<>(mHistory) : List.of(), result);
    }

    void forgetPackage(String pkg) throws IOException {
        synchronized (this) {
            mHistory.removeIf(event -> pkg.equals(event.pkg));
            mActivities.values().removeIf(activity -> {
                if (!pkg.equals(activity.pkg)) return false;
                if (isVisible(activity.state)) mVisible--;
                return true;
            });
            for (var intervals : mIntervals) {
                for (Bucket bucket : intervals.values()) bucket.totals.remove(pkg);
            }
            if (pkg.equals(mLastPaused)) mLastPaused = null;
            mRevision++;
        }
        // A serialized writer prevents an older checkpoint resurrecting purged data.
        persist();
    }

    private long now() {
        long wall = mWallClock.getAsLong(), elapsed = mElapsedClock.getAsLong();
        mReadTime = wall;
        long expected = mWallSnapshot + (elapsed - mElapsedSnapshot);
        long delta = wall - expected;
        if (delta > TIME_CHANGE_THRESHOLD || delta < -TIME_CHANGE_THRESHOLD) {
            rebase(delta, wall);
            mWallSnapshot = wall;
            mElapsedSnapshot = elapsed;
            expected = wall;
        }
        // Event timestamps use the elapsed-to-wall conversion, as reportEvent does.
        // Rebase moves this cursor too, so a backward RTC change never freezes it.
        mLastTime = Math.max(mLastTime, expected);
        return mLastTime;
    }

    private void rebase(long delta, long wall) {
        boolean haveStoredStats = false;
        for (var intervals : mIntervals) {
            for (Bucket bucket : intervals.values()) {
                haveStoredStats |= bucket.indexed || !bucket.totals.isEmpty();
            }
        }
        if (!haveStoredStats && mHistory.isEmpty()) {
            // persistActiveStats has nothing to write in a fresh empty service;
            // loadActiveStats creates fresh origins without publishing an index.
            freshIntervals(wall);
            mLastTime += delta;
            mClockGeneration++;
            mSavedClockGeneration = mClockGeneration;
            return;
        }
        ArrayDeque<Event> shifted = new ArrayDeque<>(mHistory.size());
        for (Event event : mHistory) {
            long time = event.time + delta;
            if (time >= 0) shifted.add(new Event(time, event.pkg, event.cls, event.id, event.state));
        }
        mHistory.clear();
        mHistory.addAll(shifted);
        for (var intervals : mIntervals) {
            List<Bucket> buckets = new ArrayList<>(intervals.values());
            intervals.clear();
            for (Bucket bucket : buckets) {
                bucket.rebase(delta);
                if (bucket.begin >= 0) intervals.put(bucket.begin, bucket);
            }
            // AOSP drops database files whose rebased origin precedes the epoch.
            if (intervals.isEmpty()) intervals.put(wall, new Bucket(wall));
        }
        // onTimeChanged persists then loadActiveStats reloads V2 fields. Live
        // activity maps are not part of that format: a clock check must not
        // silently keep accruing a pre-reload session or synthesize a resume.
        for (int i = 0; i < WIDTHS.length; i++) {
            Bucket latest = current(i);
            latest.reloadBoundary();
            if (wall >= latest.begin + WIDTHS[i]
                    || !latest.indexed && latest.totals.isEmpty()) {
                mIntervals.get(i).put(wall, new Bucket(wall));
            }
        }
        mLastTime += delta;
        mClockGeneration++;
        mRevision++;
    }

    private void add(Event entry) {
        if (mHistory.size() == MAX_ENTRIES) mHistory.removeFirst();
        mHistory.addLast(entry);
    }

    private void checkpoint(long time) {
        for (int i = 0; i < WIDTHS.length; i++) current(i).checkpoint(time, false);
        mRevision++;
    }

    private void rollover(long time) {
        long expiry = current(0).begin + DAY;
        if (time < expiry) return;
        List<Event> continuing = new ArrayList<>();
        for (Total total : current(0).totals.values()) {
            for (var item : total.activities.entrySet()) {
                continuing.add(new Event(time, total.pkg, null, item.getKey(), item.getValue()));
            }
        }
        boolean haveStats = false;
        for (int i = 0; i < WIDTHS.length; i++) haveStats |= !current(i).totals.isEmpty();
        for (int i = 0; i < WIDTHS.length; i++) {
            Bucket old = current(i);
            // UserUsageStatsService END_OF_DAY is at the expiry minus one millisecond.
            old.checkpoint(expiry - 1, true);
            old.reloadBoundary();
            // loadActiveStats cannot retain an interval which was never stored.
            if (!old.indexed && !haveStats || time >= old.begin + WIDTHS[i]) {
                mIntervals.get(i).put(time, new Bucket(time));
            }
        }
        // Only aggregate state is continued. No fabricated resume enters public events.
        // As in AOSP, a continuing RESUMED instance increments mLaunchCount, but
        // this internal continuation is not an external app launch.
        for (Event event : continuing) {
            for (int i = 0; i < WIDTHS.length; i++) current(i).update(event, false);
        }
        if (haveStats) mRolloverRevision++;
        mRevision++;
        prune(time);
    }

    private void prune(long time) {
        long oldestEvent = Math.max(0, time - RETENTION);
        while (!mHistory.isEmpty() && mHistory.getFirst().time < oldestEvent) mHistory.removeFirst();
        for (int i = 0; i < WIDTHS.length; i++) {
            var intervals = mIntervals.get(i);
            long oldest = time - RETAIN[i];
            while (intervals.size() > 1 && (intervals.firstKey() < oldest
                    || intervals.size() > MAX_BUCKETS[i])) intervals.pollFirstEntry();
        }
    }

    static int bestInterval(Snapshot snapshot, long begin) {
        long distance = Long.MAX_VALUE;
        int best = 0; // No stored bucket: UserUsageStatsService falls back to DAILY.
        for (int i = WIDTHS.length - 1; i >= 0; i--) {
            for (Interval interval : snapshot.intervals.get(i)) {
                if (interval.indexed && interval.begin <= begin && begin - interval.begin < distance) {
                    distance = begin - interval.begin;
                    best = i;
                }
            }
        }
        return best;
    }

    private int bestInterval(long begin) {
        long distance = Long.MAX_VALUE;
        int best = 0;
        for (int i = WIDTHS.length - 1; i >= 0; i--) {
            for (Bucket bucket : mIntervals.get(i).headMap(begin, true).values()) {
                if (bucket.indexed && begin - bucket.begin < distance) {
                    distance = begin - bucket.begin;
                    best = i;
                }
            }
        }
        return best;
    }

    static List<Total> aggregate(Snapshot snapshot, long width, long begin, long end) {
        if (begin >= end || begin > snapshot.until) return List.of();
        int type = -1;
        for (int i = 0; i < WIDTHS.length; i++) if (WIDTHS[i] == width) type = i;
        if (type < 0) return List.of();
        List<Total> result = new ArrayList<>();
        for (Interval interval : snapshot.intervals.get(type)) {
            if (begin < interval.end && end > interval.begin) result.addAll(interval.totals);
        }
        return result;
    }

    private void read() throws IOException {
        if (!Files.exists(mFile)) return;
        if (Files.size(mFile) > MAX_FILE_BYTES) throw new IOException("Usage checkpoint too large");
        List<Event> events;
        List<TreeMap<Long, Bucket>> intervals = new ArrayList<>(WIDTHS.length);
        long savedAt;
        int next;
        boolean legacy;
        try (DataInputStream in = new DataInputStream(new BufferedInputStream(Files.newInputStream(mFile)))) {
            int version = in.readInt();
            if (version < 1 || version > FILE_VERSION) throw new IOException("Unknown usage history format");
            legacy = version == 1;
            savedAt = in.readLong();
            if (savedAt < 0) throw new IOException("Invalid usage checkpoint time");
            next = legacy ? 0 : in.readInt();
            if (next < 0) throw new IOException("Invalid usage instance counter");
            events = readEvents(in, savedAt);
            for (Event event : events) next = Math.max(next, event.id);
            if (version == 2) {
                // The contributor's published v2 stores interval origins followed
                // by per-package buckets. Version 3 keeps an independent interval
                // end and aggregate state; migrate v2 without replaying its suffix.
                for (int i = 0; i < WIDTHS.length; i++) {
                    intervals.add(readPublishedV2(in, i, savedAt));
                }
            } else if (!legacy) {
                for (int i = 0; i < WIDTHS.length; i++) {
                    int count = in.readInt();
                    if (count < 1 || count > MAX_BUCKETS[i]) throw new IOException("Invalid usage interval count");
                    TreeMap<Long, Bucket> buckets = new TreeMap<>();
                    for (int j = 0; j < count; j++) {
                        long begin = in.readLong(), end = in.readLong();
                        if (begin < 0 || begin > savedAt || end < begin || end > savedAt + 1) {
                            throw new IOException("Invalid usage interval times");
                        }
                        Bucket bucket = new Bucket(begin);
                        bucket.end = end;
                        bucket.indexed = true;
                        int packages = in.readInt();
                        // Each total needs at least 53 bytes even with an empty name.
                        // Bound malformed counts by the same whole-file read/write budget.
                        if (packages < 0 || packages > MAX_FILE_BYTES / 53) {
                            throw new IOException("Invalid usage package count");
                        }
                        for (int k = 0; k < packages; k++) {
                            Total total = readTotal(in, begin, end);
                            if (bucket.totals.put(total.pkg, total) != null) throw new IOException("Duplicate usage package");
                        }
                        if (buckets.put(begin, bucket) != null) throw new IOException("Duplicate usage interval");
                    }
                    intervals.add(buckets);
                }
            }
            if (in.read() != -1) throw new IOException("Trailing usage history bytes");
        }
        // Do not publish a partially decoded checkpoint. No runtime activity tokens
        // are recovered: all saved durations stop at savedAt, excluding broker downtime.
        mNextId = next;
        mHistory.addAll(events);
        if (legacy) {
            migrateLegacy(events, savedAt);
        } else {
            mIntervals.clear();
            mIntervals.addAll(intervals);
        }
        // With no boot identity, an RTC change while down cannot be inferred from
        // elapsed time. Like UsageStatsDatabase.init, reject future-origin intervals.
        long time = mWallSnapshot;
        mHistory.removeIf(event -> event.time > time);
        for (int i = 0; i < WIDTHS.length; i++) {
            var buckets = mIntervals.get(i);
            buckets.values().removeIf(bucket -> bucket.begin > time);
            if (buckets.isEmpty() || time >= buckets.lastKey() + WIDTHS[i]) {
                buckets.put(time, new Bucket(time));
            }
            for (Bucket bucket : buckets.values()) bucket.reloadBoundary();
        }
        mLastPaused = null;
        prune(time);
        mRevision++; // Persist migration/pruning, including an empty recovered suffix.
    }

    private static TreeMap<Long, Bucket> readPublishedV2(DataInputStream in, int type,
            long savedAt) throws IOException {
        long origin = in.readLong();
        int count = in.readInt();
        if (origin < 0 || origin > savedAt || count < 0 || count > RETENTION / WIDTHS[type] + 2) {
            throw new IOException("Invalid v2 usage interval");
        }
        TreeMap<Long, Bucket> buckets = new TreeMap<>();
        for (int i = 0; i < count; i++) {
            long begin = in.readLong();
            int packages = in.readInt();
            if (begin < 0 || begin > origin || packages < 1 || packages > MAX_FILE_BYTES / 51) {
                throw new IOException("Invalid v2 usage bucket");
            }
            Bucket bucket = new Bucket(begin);
            bucket.indexed = true;
            long bound = Math.min(savedAt, begin + WIDTHS[type]);
            for (int j = 0; j < packages; j++) {
                Total total = new Total(begin, in.readUTF());
                total.end = in.readLong();
                total.foreground = in.readLong();
                total.visible = in.readLong();
                total.lastUsed = in.readLong();
                total.lastVisible = in.readLong();
                total.launches = in.readInt();
                total.appLaunches = in.readInt();
                int code = in.readUnsignedByte();
                if (total.end < begin || total.end > bound || total.foreground < 0
                        || total.visible < total.foreground || total.visible > total.end - begin
                        || total.lastUsed < 0 || total.lastUsed > total.end
                        || total.lastVisible < 0 || total.lastVisible > total.end
                        || total.launches < 0 || total.appLaunches < 0
                        || total.appLaunches > total.launches || code > State.CHECKPOINT_END.ordinal()) {
                    throw new IOException("Invalid v2 usage total");
                }
                total.used = total.lastUsed != 0 || total.foreground != 0 || total.launches != 0;
                total.seen = total.lastVisible != 0 || total.visible != 0 || total.used;
                total.lastEvent = code == State.CHECKPOINT_END.ordinal() ? null : State.values()[code];
                bucket.end = Math.max(bucket.end, total.end);
                if (bucket.totals.put(total.pkg, total) != null) throw new IOException("Duplicate v2 usage package");
            }
            if (buckets.put(begin, bucket) != null) throw new IOException("Duplicate v2 usage interval");
        }
        buckets.computeIfAbsent(origin, Bucket::new);
        return buckets;
    }

    private void migrateLegacy(List<Event> events, long savedAt) {
        long first = events.isEmpty() ? savedAt : events.get(0).time;
        // Version 1 recorded no origin or prefix aggregate. Preserve its known
        // event suffix with its legacy epoch origins; missing prefix data cannot
        // be recovered. All subsequently created intervals use real load times.
        mIntervals.clear();
        for (long width : WIDTHS) {
            long begin = first / width * width;
            TreeMap<Long, Bucket> buckets = new TreeMap<>();
            buckets.put(begin, new Bucket(begin));
            mIntervals.add(buckets);
        }
        for (Event event : events) {
            rollover(event.time);
            if (event.state == State.CHECKPOINT_END) {
                for (int i = 0; i < WIDTHS.length; i++) {
                    current(i).checkpoint(event.time, false);
                    current(i).closeActivities();
                }
                mLastPaused = null;
            } else {
                record(event);
            }
        }
        rollover(savedAt);
        for (int i = 0; i < WIDTHS.length; i++) {
            current(i).checkpoint(savedAt, false);
            current(i).closeActivities();
        }
    }

    private static List<Event> readEvents(DataInputStream in, long savedAt) throws IOException {
        int count = in.readInt();
        if (count < 0 || count > MAX_ENTRIES) throw new IOException("Invalid usage history count");
        List<Event> result = new ArrayList<>(count);
        State[] states = State.values();
        long previous = 0;
        for (int i = 0; i < count; i++) {
            long time = in.readLong();
            int id = in.readInt(), code = in.readUnsignedByte();
            if (time < previous || time > savedAt || id < 0 || code >= states.length) {
                throw new IOException("Invalid usage transition");
            }
            State state = states[code];
            boolean checkpoint = state == State.CHECKPOINT_END;
            if (checkpoint != (id == 0)) throw new IOException("Invalid usage instance");
            String pkg = checkpoint ? null : in.readUTF(), cls = checkpoint ? null : in.readUTF();
            result.add(new Event(time, pkg, cls, id, state));
            previous = time;
        }
        return result;
    }

    private static Total readTotal(DataInputStream in, long begin, long end) throws IOException {
        Total total = new Total(begin, in.readUTF());
        total.end = in.readLong();
        total.foreground = in.readLong();
        total.visible = in.readLong();
        total.lastUsed = in.readLong();
        total.lastVisible = in.readLong();
        total.launches = in.readInt();
        total.appLaunches = in.readInt();
        int code = in.readUnsignedByte();
        total.used = in.readBoolean();
        total.seen = in.readBoolean();
        if (total.end < begin || total.end > end || total.foreground < 0 || total.visible < total.foreground
                || total.lastUsed < 0 || total.lastVisible < 0 || total.launches < 0 || total.appLaunches < 0
                || total.lastUsed > total.end || total.lastVisible > total.end
                || !total.used && total.lastUsed != 0 || !total.seen && total.lastVisible != 0
                || code > State.values().length) throw new IOException("Invalid usage aggregate");
        total.lastEvent = code == State.values().length ? null : State.values()[code];
        return total;
    }

    /** The real FLUSH_TO_DISK processing path; ordinary snapshots never call it. */
    void flush() throws IOException {
        synchronized (mWriterLock) {
            synchronized (this) {
                long time = now();
                rollover(time);
                checkpoint(time);
                prune(time);
            }
            persist();
        }
    }

    /** Saves existing accumulators without introducing a lifecycle/accounting event. */
    private void persist() throws IOException {
        if (mFile == null) return;
        synchronized (mWriterLock) {
            for (int attempt = 0; attempt < 3; attempt++) {
                Snapshot snapshot;
                long revision, generation, rollover;
                int next;
                synchronized (this) {
                    if (mRevision == mWrittenRevision) return;
                    snapshot = copySnapshot(mReadTime, -1, Long.MIN_VALUE, Long.MAX_VALUE, true);
                    revision = mRevision;
                    generation = mClockGeneration;
                    rollover = mRolloverRevision;
                    next = mNextId;
                }
                Path parent = mFile.toAbsolutePath().getParent();
                Files.createDirectories(parent);
                Path temporary = Files.createTempFile(parent, "usage-", ".tmp");
                try {
                    try (java.io.FileOutputStream file = new java.io.FileOutputStream(temporary.toFile());
                            DataOutputStream out = new DataOutputStream(new BufferedOutputStream(new BoundedOutput(file)))) {
                        out.writeInt(FILE_VERSION);
                        // A backward RTC change while the broker was down has
                        // no trustworthy elapsed anchor. Retained past-origin
                        // intervals may still end after the new wall time, as
                        // in AOSP. Keep a valid timestamp bound when rewriting
                        // them; never project a recovered live session to it.
                        long bound = snapshot.until;
                        for (List<Interval> intervals : snapshot.intervals) {
                            for (Interval interval : intervals) {
                                // Even a one-millisecond wall/elapsed sampling
                                // skew can put an event after the wall snapshot.
                                // The decoder requires every event <= savedAt.
                                bound = Math.max(bound, interval.end);
                            }
                        }
                        out.writeLong(bound);
                        out.writeInt(next);
                        out.writeInt(snapshot.events.size());
                        for (Event event : snapshot.events) {
                            out.writeLong(event.time);
                            out.writeInt(event.id);
                            out.writeByte(event.state.ordinal());
                            if (event.state != State.CHECKPOINT_END) {
                                out.writeUTF(event.pkg);
                                out.writeUTF(event.cls);
                            }
                        }
                        for (List<Interval> intervals : snapshot.intervals) {
                            out.writeInt(intervals.size());
                            for (Interval interval : intervals) {
                                out.writeLong(interval.begin);
                                out.writeLong(interval.end);
                                out.writeInt(interval.totals.size());
                                for (Total total : interval.totals) writeTotal(out, total, interval.end);
                            }
                        }
                        out.flush();
                        file.getFD().sync();
                    }
                    synchronized (this) {
                        // Discard a pre-correction image rather than publishing it
                        // after the correction became visible to queries. Only the
                        // atomic replacement, not encoding or fsync, holds this lock.
                        if (generation != mClockGeneration) continue;
                        Files.move(temporary, mFile, StandardCopyOption.ATOMIC_MOVE, StandardCopyOption.REPLACE_EXISTING);
                        mWrittenRevision = revision;
                        mSavedClockGeneration = generation;
                        mSavedRolloverRevision = rollover;
                        for (int i = 0; i < WIDTHS.length; i++) {
                            for (Interval interval : snapshot.intervals.get(i)) {
                                Bucket bucket = mIntervals.get(i).get(interval.begin);
                                if (bucket != null) bucket.indexed = true;
                            }
                        }
                    }
                    return;
                } finally {
                    Files.deleteIfExists(temporary);
                }
            }
            throw new IOException("Clock repeatedly changed during usage checkpoint");
        }
    }

    private static void writeTotal(DataOutputStream out, Total total, long intervalEnd) throws IOException {
        out.writeUTF(total.pkg);
        // AOSP V2 restores each package's end from the interval. An offline
        // backward RTC correction can leave a completed last-use time above a
        // later FLUSH_TO_DISK timestamp; persist the containing interval bound,
        // not that lower in-memory package end. Never erase its known duration.
        out.writeLong(intervalEnd);
        out.writeLong(total.foreground);
        out.writeLong(total.visible);
        out.writeLong(total.lastUsed);
        out.writeLong(total.lastVisible);
        out.writeInt(total.launches);
        out.writeInt(total.appLaunches);
        out.writeByte(total.lastEvent == null ? State.values().length : total.lastEvent.ordinal());
        out.writeBoolean(total.used);
        out.writeBoolean(total.seen);
    }

    private static final class BoundedOutput extends OutputStream {
        private final OutputStream out;
        private long count;
        BoundedOutput(OutputStream out) { this.out = out; }
        @Override public void write(int value) throws IOException {
            check(1);
            out.write(value);
        }
        @Override public void write(byte[] bytes, int offset, int length) throws IOException {
            check(length);
            out.write(bytes, offset, length);
        }
        private void check(int length) throws IOException {
            if (count + length > MAX_FILE_BYTES) throw new IOException("Usage checkpoint too large");
            count += length;
        }
    }

    @Override public void close() throws IOException {
        if (mWriter != null) mWriter.shutdown();
        flush();
    }
}
