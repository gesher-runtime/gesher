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
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.StandardCopyOption;
import java.util.ArrayDeque;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.IdentityHashMap;
import java.util.List;
import java.util.Map;
import java.util.NavigableMap;
import java.util.NavigableSet;
import java.util.TreeMap;
import java.util.TreeSet;
import java.util.concurrent.Executors;
import java.util.concurrent.ScheduledExecutorService;
import java.util.concurrent.TimeUnit;
import java.util.function.Consumer;
import java.util.function.LongSupplier;

/** Bounded system-server history. Transitions never perform disk I/O on the lifecycle thread. */
final class ActivityUsageHistory {
    static final long DAY = TimeUnit.DAYS.toMillis(1);
    static final long RETENTION = 30 * DAY;
    private static final long[] WIDTHS = {DAY, 7 * DAY, 30 * DAY, 365 * DAY};
    private static final long CLOCK_CHANGE_THRESHOLD = 2_000;
    private static final int MAX_EVENTS = 12_000;
    private static final int FILE_VERSION = 2;

    enum State {
        RESUMED, PAUSED, STOPPED, TERMINATED, CHECKPOINT_END
    }

    record Event(long time, String pkg, String cls, int id, State state) { }
    record Snapshot(long until, List<Event> events) { }

    private static final class Activity {
        final String pkg;
        final String cls;
        final int id;
        State state;

        Activity(String pkg, String cls, int id) {
            this.pkg = pkg;
            this.cls = cls;
            this.id = id;
        }
    }

    /** An aggregate is an entire bucket that overlaps the queried time range. */
    static final class Total {
        final long begin;
        final String pkg;
        long end, foreground, visible, lastUsed, lastVisible;
        int launches, appLaunches;
        State lastEvent;

        Total(long begin, String pkg) {
            this.begin = begin;
            this.pkg = pkg;
        }

        Total shifted(long offset) {
            Total copy = new Total(begin + offset, pkg);
            copy.end = end + offset;
            copy.foreground = foreground;
            copy.visible = visible;
            copy.lastUsed = lastUsed == 0 ? 0 : Math.max(0, lastUsed + offset);
            copy.lastVisible = lastVisible == 0 ? 0 : Math.max(0, lastVisible + offset);
            copy.launches = launches;
            copy.appLaunches = appLaunches;
            copy.lastEvent = lastEvent;
            return copy;
        }
    }

    private static final class PackageState {
        int visible, resumed;

        void advance(State previous, State state) {
            if (visible(previous)) visible--;
            if (visible(state)) visible++;
            if (previous == State.RESUMED) resumed--;
            if (state == State.RESUMED) resumed++;
        }
    }

    private static final class Interval {
        final long width;
        final NavigableMap<Long, Map<String, Total>> buckets = new TreeMap<>();
        final NavigableSet<Long> persisted = new TreeSet<>();
        long origin;

        Interval(long width, long origin) {
            this.width = width;
            this.origin = origin;
        }

        Total total(String pkg) {
            return buckets.computeIfAbsent(origin, ignored -> new TreeMap<>())
                    .computeIfAbsent(pkg, name -> new Total(origin, name));
        }

        void advance(long from, long until, Map<String, PackageState> active, boolean newEvent) {
            if (active.isEmpty()) {
                if (newEvent && until >= origin + width) origin = until;
                return;
            }
            // Skip expired whole buckets after a long suspend without processing one per old day.
            long cutoff = until - RETENTION;
            if (cutoff >= origin + width) origin += ((cutoff - origin) / width) * width;
            long start = Math.max(from, origin);
            while (start < until) {
                long limit = origin + width;
                long end = Math.min(until, limit);
                for (var entry : active.entrySet()) {
                    Total total = total(entry.getKey());
                    total.visible += end - start;
                    total.lastVisible = total.end = end;
                    if (entry.getValue().resumed != 0) {
                        total.foreground += end - start;
                        total.lastUsed = end;
                    }
                }
                start = end;
                if (end == limit) origin = limit;
            }
        }

        void shift(long offset) {
            NavigableMap<Long, Map<String, Total>> shifted = new TreeMap<>();
            for (var bucket : buckets.entrySet()) {
                long begin = bucket.getKey() + offset;
                if (begin < 0) continue;
                Map<String, Total> totals = new TreeMap<>();
                for (Total total : bucket.getValue().values()) {
                    totals.put(total.pkg, total.shifted(offset));
                }
                shifted.put(begin, totals);
            }
            buckets.clear();
            buckets.putAll(shifted);
            replacePersisted(new TreeSet<>(persisted), offset);
            origin = Math.max(0, origin + offset);
        }

        void replacePersisted(Iterable<Long> previous, long offset) {
            persisted.clear();
            for (long begin : previous) {
                long adjusted = begin + offset;
                if (buckets.containsKey(adjusted)) persisted.add(adjusted);
            }
        }
    }

    private record Saved(long time, int nextId, List<Event> events, List<Interval> intervals) { }

    private final Path mFile;
    private final LongSupplier mWall, mElapsed;
    private final Object mWriterLock = new Object();
    private final ArrayDeque<Event> mHistory = new ArrayDeque<>();
    private final Map<Object, Activity> mActivities = new IdentityHashMap<>();
    private final Map<String, PackageState> mActive = new HashMap<>();
    private final List<Interval> mIntervals = new ArrayList<>(WIDTHS.length);
    private long mWallBase, mElapsedBase, mTime, mCorrection;
    private long mRevision, mWrittenRevision;
    private int mNextId;
    private String mLastPaused;

    ActivityUsageHistory(Path file, LongSupplier wall, LongSupplier elapsed,
            Consumer<IOException> onFailure) {
        mFile = file;
        mWall = wall;
        mElapsed = elapsed;
        mElapsedBase = mElapsed.getAsLong();
        mTime = mWallBase = mWall.getAsLong();
        for (long width : WIDTHS) mIntervals.add(new Interval(width, mTime));
        if (mFile == null) return;
        try { read(); }
        catch (IOException failure) { onFailure.accept(failure); }
        ScheduledExecutorService writer = Executors.newSingleThreadScheduledExecutor(job -> {
            Thread thread = new Thread(job, "usage-history");
            thread.setDaemon(true);
            return thread;
        });
        // Also checkpoint currently visible activities, so server failure cannot count offline time.
        writer.scheduleWithFixedDelay(() -> {
            try { flush(); }
            catch (IOException failure) { onFailure.accept(failure); }
        }, 30, 30, TimeUnit.SECONDS);
    }

    private static boolean visible(State state) {
        return state == State.RESUMED || state == State.PAUSED;
    }

    synchronized void transition(Object token, String pkg, String cls, State state) {
        Activity activity = mActivities.get(token);
        if (activity == null) {
            if (!visible(state)) return;
            activity = new Activity(pkg, cls, ++mNextId);
            mActivities.put(token, activity);
        }
        if (activity.state == state) return;
        if (state == State.TERMINATED) {
            mActivities.remove(token);
            if (activity.state == State.STOPPED) return;
        }
        advance(now(), true);
        Event event = new Event(mTime, activity.pkg, activity.cls, activity.id, state);
        note(event, activity.state);
        activity.state = state;
        mRevision++;
    }

    private void note(Event event, State previous) {
        if (mHistory.size() == MAX_EVENTS) mHistory.removeFirst();
        mHistory.addLast(event);
        PackageState active = mActive.computeIfAbsent(event.pkg, ignored -> new PackageState());
        boolean newlyVisible = active.visible == 0;
        active.advance(previous, event.state);
        if (active.visible == 0) mActive.remove(event.pkg);
        for (Interval interval : mIntervals) {
            Total total = interval.total(event.pkg);
            total.end = event.time;
            total.lastEvent = event.state;
            if (event.state == State.RESUMED) {
                total.lastUsed = total.lastVisible = event.time;
                total.launches++;
                if (!event.pkg.equals(mLastPaused)) total.appLaunches++;
            } else if (event.state == State.PAUSED && newlyVisible) {
                total.lastVisible = event.time;
            }
        }
        if (event.state == State.PAUSED) mLastPaused = event.pkg;
    }

    synchronized Snapshot snapshot() {
        advance(now(), false);
        return new Snapshot(mTime, new ArrayList<>(mHistory));
    }

    synchronized List<Total> aggregate(int interval, long begin, long end) {
        advance(now(), false);
        long oldest = Math.max(0, mTime - RETENTION);
        long start = Math.max(oldest, begin);
        if (start >= end || start > mTime) return List.of();
        Interval selected = interval == WIDTHS.length ? best(begin) : mIntervals.get(interval);
        long earliestBucket = Math.max(0, start - selected.width);
        List<Total> result = new ArrayList<>();
        for (var bucket : selected.buckets.subMap(earliestBucket, true, end, false).values()) {
            if (bucket.values().stream().noneMatch(total -> total.end >= start)) continue;
            for (Total total : bucket.values()) {
                if (total.end >= oldest) result.add(total.shifted(0));
            }
        }
        return result;
    }

    private Interval best(long begin) {
        Interval result = mIntervals.get(0);
        long closest = -1;
        // The nearest stored origin at or before the beginning wins. Ties prefer the wider interval.
        for (int i = mIntervals.size() - 1; i >= 0; i--) {
            Interval candidate = mIntervals.get(i);
            Long origin = candidate.persisted.floor(begin);
            if (origin != null && origin > closest) {
                closest = origin;
                result = candidate;
            }
        }
        return result;
    }

    void forgetPackage(String pkg) throws IOException {
        synchronized (this) {
            mHistory.removeIf(event -> pkg.equals(event.pkg));
            mActivities.values().removeIf(activity -> pkg.equals(activity.pkg));
            mActive.remove(pkg);
            if (pkg.equals(mLastPaused)) mLastPaused = null;
            for (Interval interval : mIntervals) {
                interval.buckets.values().forEach(bucket -> bucket.remove(pkg));
                interval.buckets.values().removeIf(Map::isEmpty);
                interval.persisted.retainAll(interval.buckets.keySet());
            }
            mRevision++;
        }
        // Do not let a committed uninstall be followed by stale data on reinstall.
        flush();
    }

    private long now() {
        long elapsed = mElapsed.getAsLong();
        long wall = mWall.getAsLong();
        long expected = mWallBase + (elapsed - mElapsedBase);
        long offset = wall - expected;
        if (offset > CLOCK_CHANGE_THRESHOLD || offset < -CLOCK_CHANGE_THRESHOLD) {
            shift(offset);
            mWallBase = wall;
            mElapsedBase = elapsed;
            return wall;
        }
        return expected;
    }

    private void shift(long offset) {
        for (Interval interval : mIntervals) interval.shift(offset);
        List<Event> previous = new ArrayList<>(mHistory);
        mHistory.clear();
        for (Event event : previous) {
            long time = event.time + offset;
            if (time >= 0) mHistory.addLast(new Event(time, event.pkg, event.cls, event.id, event.state));
        }
        mTime = Math.max(0, mTime + offset);
        mCorrection += offset;
        mRevision++;
    }

    private void advance(long until, boolean newEvent) {
        until = Math.max(mTime, until);
        long oldest = until - RETENTION;
        if (until != mTime || newEvent) {
            boolean changed = !mActive.isEmpty() && until != mTime;
            for (Interval interval : mIntervals) {
                interval.advance(mTime, until, mActive, newEvent);
                var expired = interval.buckets.headMap(oldest - interval.width, false);
                changed |= !expired.isEmpty();
                expired.clear();
                interval.persisted.headSet(oldest - interval.width, false).clear();
            }
            while (!mHistory.isEmpty() && mHistory.peekFirst().time < oldest) {
                mHistory.removeFirst();
                changed = true;
            }
            if (changed) mRevision++;
            mTime = until;
        }
    }

    private void read() throws IOException {
        if (!Files.exists(mFile)) return;
        Saved saved;
        long fileSize = Files.size(mFile);
        try (DataInputStream in = new DataInputStream(new BufferedInputStream(Files.newInputStream(mFile)))) {
            int version = in.readInt();
            if (version != 1 && version != FILE_VERSION) throw new IOException("Unknown usage history format");
            long time = in.readLong();
            int nextId = version == 1 ? 0 : in.readInt();
            if (time < 0 || nextId < 0) throw new IOException("Invalid usage checkpoint");
            List<Event> events = readEvents(in, time, version == 1);
            List<Interval> intervals = new ArrayList<>();
            if (version == FILE_VERSION) {
                for (long width : WIDTHS) intervals.add(readInterval(in, width, time, fileSize));
                if (events.stream().anyMatch(event -> event.id > nextId)) {
                    throw new IOException("Invalid usage instance sequence");
                }
            }
            if (in.read() != -1) throw new IOException("Trailing usage history bytes");
            saved = new Saved(time, nextId, events, intervals);
        }
        if (saved.intervals.isEmpty()) {
            migrate(saved);
        } else {
            mHistory.addAll(saved.events);
            mIntervals.clear();
            mIntervals.addAll(saved.intervals);
            mTime = saved.time;
            mNextId = saved.nextId;
        }
        // Preserve duration, including the final checkpoint, if the wall clock was moved back
        // while the broker was down. No live activity survives the restart.
        if (mTime > mWallBase) shift(mWallBase - mTime);
        advance(mWallBase, false);
    }

    private static List<Event> readEvents(DataInputStream in, long until, boolean legacy) throws IOException {
        int count = count(in, MAX_EVENTS);
        List<Event> events = new ArrayList<>(count);
        long previous = 0;
        State[] states = State.values();
        for (int i = 0; i < count; i++) {
            long time = in.readLong();
            int id = in.readInt();
            int code = in.readUnsignedByte();
            if (time < previous || time > until || id < 0 || code >= states.length) {
                throw new IOException("Invalid usage transition");
            }
            State state = states[code];
            boolean checkpoint = state == State.CHECKPOINT_END;
            if (checkpoint != (id == 0) || checkpoint && !legacy) {
                throw new IOException("Invalid usage instance");
            }
            events.add(new Event(time, checkpoint ? null : in.readUTF(),
                    checkpoint ? null : in.readUTF(), id, state));
            previous = time;
        }
        return events;
    }

    private void migrate(Saved saved) {
        mTime = saved.events.isEmpty() ? saved.time : saved.events.get(0).time;
        for (Interval interval : mIntervals) interval.origin = mTime;
        Map<Integer, State> states = new HashMap<>();
        for (Event event : saved.events) {
            boolean checkpoint = event.state == State.CHECKPOINT_END;
            advance(event.time, !checkpoint);
            if (checkpoint) {
                mActive.clear();
                states.clear();
                mLastPaused = null;
            } else {
                State previous = states.remove(event.id);
                if (visible(event.state)) states.put(event.id, event.state);
                note(event, previous);
                mNextId = Math.max(mNextId, event.id);
            }
        }
        advance(saved.time, false);
        mActive.clear();
        mLastPaused = null;
        for (Interval interval : mIntervals) interval.persisted.addAll(interval.buckets.keySet());
        mRevision++;
    }

    private static Interval readInterval(DataInputStream in, long width, long until, long fileSize)
            throws IOException {
        long origin = in.readLong();
        if (origin < 0 || origin > until) throw new IOException("Invalid usage interval origin");
        Interval interval = new Interval(width, origin);
        int bucketCount = count(in, RETENTION / width + 2);
        for (int i = 0; i < bucketCount; i++) {
            long begin = in.readLong();
            int totalCount = count(in, fileSize / 51);
            if (begin < 0 || begin > origin || interval.buckets.containsKey(begin) || totalCount == 0) {
                throw new IOException("Invalid usage bucket");
            }
            Map<String, Total> totals = new TreeMap<>();
            for (int j = 0; j < totalCount; j++) {
                Total total = readTotal(in, begin, Math.min(until, begin + width));
                if (totals.putIfAbsent(total.pkg, total) != null) throw new IOException("Duplicate usage package");
            }
            interval.buckets.put(begin, totals);
        }
        interval.persisted.addAll(interval.buckets.keySet());
        return interval;
    }

    private static Total readTotal(DataInputStream in, long begin, long until) throws IOException {
        Total total = new Total(begin, in.readUTF());
        total.end = in.readLong();
        total.foreground = in.readLong();
        total.visible = in.readLong();
        total.lastUsed = in.readLong();
        total.lastVisible = in.readLong();
        total.launches = in.readInt();
        total.appLaunches = in.readInt();
        int state = in.readUnsignedByte();
        if (total.end < begin || total.end > until || total.foreground < 0
                || total.visible < total.foreground || total.visible > total.end - begin
                || total.lastUsed < 0 || total.lastUsed > total.end
                || total.lastVisible < 0 || total.lastVisible > total.end
                || total.launches < 0 || total.appLaunches < 0 || total.appLaunches > total.launches
                || state > State.CHECKPOINT_END.ordinal()) {
            throw new IOException("Invalid usage total");
        }
        total.lastEvent = state == State.CHECKPOINT_END.ordinal() ? null : State.values()[state];
        return total;
    }

    private static int count(DataInputStream in, long maximum) throws IOException {
        int count = in.readInt();
        if (count < 0 || count > maximum) throw new IOException("Invalid usage record count");
        return count;
    }

    void flush() throws IOException {
        if (mFile == null) return;
        synchronized (mWriterLock) {
            Saved saved;
            long revision, correction;
            synchronized (this) {
                advance(now(), false);
                if (mRevision == mWrittenRevision) return;
                List<Interval> intervals = new ArrayList<>(WIDTHS.length);
                for (Interval interval : mIntervals) {
                    Interval copy = new Interval(interval.width, interval.origin);
                    for (var bucket : interval.buckets.entrySet()) {
                        Map<String, Total> totals = new TreeMap<>();
                        for (Total total : bucket.getValue().values()) totals.put(total.pkg, total.shifted(0));
                        copy.buckets.put(bucket.getKey(), totals);
                    }
                    intervals.add(copy);
                }
                saved = new Saved(mTime, mNextId, new ArrayList<>(mHistory), intervals);
                revision = mRevision;
                correction = mCorrection;
            }
            Files.createDirectories(mFile.getParent());
            Path temporary = Files.createTempFile(mFile.getParent(), "usage-", ".tmp");
            try {
                write(temporary, saved);
                Files.move(temporary, mFile, StandardCopyOption.ATOMIC_MOVE,
                        StandardCopyOption.REPLACE_EXISTING);
                synchronized (this) {
                    for (int i = 0; i < WIDTHS.length; i++) {
                        mIntervals.get(i).replacePersisted(saved.intervals.get(i).buckets.keySet(),
                                mCorrection - correction);
                    }
                    mWrittenRevision = revision;
                }
            } finally {
                Files.deleteIfExists(temporary);
            }
        }
    }

    private static void write(Path temporary, Saved saved) throws IOException {
        try (java.io.FileOutputStream file = new java.io.FileOutputStream(temporary.toFile());
                DataOutputStream out = new DataOutputStream(new BufferedOutputStream(file))) {
            out.writeInt(FILE_VERSION);
            out.writeLong(saved.time);
            out.writeInt(saved.nextId);
            out.writeInt(saved.events.size());
            for (Event event : saved.events) {
                out.writeLong(event.time);
                out.writeInt(event.id);
                out.writeByte(event.state.ordinal());
                out.writeUTF(event.pkg);
                out.writeUTF(event.cls);
            }
            for (Interval interval : saved.intervals) {
                out.writeLong(interval.origin);
                out.writeInt(interval.buckets.size());
                for (var bucket : interval.buckets.entrySet()) {
                    out.writeLong(bucket.getKey());
                    out.writeInt(bucket.getValue().size());
                    for (Total total : bucket.getValue().values()) writeTotal(out, total);
                }
            }
            out.flush();
            file.getFD().sync();
        }
    }

    private static void writeTotal(DataOutputStream out, Total total) throws IOException {
        out.writeUTF(total.pkg);
        out.writeLong(total.end);
        out.writeLong(total.foreground);
        out.writeLong(total.visible);
        out.writeLong(total.lastUsed);
        out.writeLong(total.lastVisible);
        out.writeInt(total.launches);
        out.writeInt(total.appLaunches);
        out.writeByte(total.lastEvent == null ? State.CHECKPOINT_END.ordinal() : total.lastEvent.ordinal());
    }
}
