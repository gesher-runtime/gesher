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
import java.util.TreeMap;
import java.util.concurrent.Executors;
import java.util.concurrent.ScheduledExecutorService;
import java.util.concurrent.TimeUnit;
import java.util.function.Consumer;
import java.util.function.LongSupplier;

/** Bounded system-server history. Transitions never perform disk I/O on the lifecycle thread. */
final class ActivityUsageHistory {
    static final long DAY = TimeUnit.DAYS.toMillis(1);
    static final long RETENTION = 30 * DAY;
    private static final int MAX_ENTRIES = 12_000;
    private static final int FILE_VERSION = 1;

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
    }

    private static final class PackageState {
        final Map<Integer, State> activities = new HashMap<>();
        int resumed;
        long time;

        void advance(Event event) {
            State previous = activities.remove(event.id);
            if (previous == State.RESUMED) resumed--;
            if (event.state == State.RESUMED) resumed++;
            if (event.state == State.RESUMED || event.state == State.PAUSED) {
                activities.put(event.id, event.state);
            }
            time = event.time;
        }
    }

    private final Path mFile;
    private final LongSupplier mClock;
    private final Object mWriterLock = new Object();
    private final ArrayDeque<Event> mHistory = new ArrayDeque<>();
    private final Map<Object, Activity> mActivities = new IdentityHashMap<>();
    private long mRevision, mWrittenRevision;
    private int mNextId, mVisible;

    ActivityUsageHistory(Path file, LongSupplier clock, Consumer<IOException> onFailure) {
        mFile = file;
        mClock = clock;
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

    synchronized void transition(Object token, String pkg, String cls, State state) {
        Activity activity = mActivities.get(token);
        if (activity == null) {
            if (state != State.RESUMED) return;
            activity = new Activity(pkg, cls, ++mNextId);
            mActivities.put(token, activity);
        }
        if (activity.state == state || state == State.PAUSED && activity.state != State.RESUMED) {
            return;
        }
        if (state == State.TERMINATED) {
            mActivities.remove(token);
            if (activity.state == State.STOPPED) return;
        }
        boolean wasVisible = activity.state == State.RESUMED || activity.state == State.PAUSED;
        boolean visible = state == State.RESUMED || state == State.PAUSED;
        if (wasVisible != visible) mVisible += visible ? 1 : -1;
        activity.state = state;
        add(new Event(now(), activity.pkg, activity.cls, activity.id, state));
        mRevision++;
    }

    synchronized Snapshot snapshot() {
        return new Snapshot(now(), new ArrayList<>(mHistory));
    }

    void forgetPackage(String pkg) throws IOException {
        synchronized (this) {
            mHistory.removeIf(event -> pkg.equals(event.pkg));
            mActivities.values().removeIf(activity -> {
                if (!pkg.equals(activity.pkg)) return false;
                if (activity.state == State.RESUMED || activity.state == State.PAUSED) mVisible--;
                return true;
            });
            mRevision++;
        }
        // Do not let a committed uninstall be followed by stale data on reinstall.
        flush();
    }

    private long now() {
        long wall = mClock.getAsLong();
        return mHistory.isEmpty() ? wall : Math.max(wall, mHistory.getLast().time);
    }

    private void add(Event entry) {
        if (mHistory.size() == MAX_ENTRIES) mHistory.removeFirst();
        mHistory.addLast(entry);
    }

    private void read() throws IOException {
        if (!Files.exists(mFile)) return;
        List<Event> loaded = new ArrayList<>();
        long savedAt;
        int nextId = 0;
        try (DataInputStream in = new DataInputStream(new BufferedInputStream(
                Files.newInputStream(mFile)))) {
            if (in.readInt() != FILE_VERSION) throw new IOException("Unknown usage history format");
            savedAt = in.readLong();
            int count = in.readInt();
            if (count < 0 || count > MAX_ENTRIES) throw new IOException("Invalid usage history count");
            State[] states = State.values();
            long previous = 0;
            for (int i = 0; i < count; i++) {
                long time = in.readLong();
                int id = in.readInt();
                int code = in.readUnsignedByte();
                if (time < previous || time > savedAt || id < 0 || code >= states.length) {
                    throw new IOException("Invalid usage transition");
                }
                State state = states[code];
                boolean checkpoint = state == State.CHECKPOINT_END;
                if (checkpoint != (id == 0)) throw new IOException("Invalid usage instance");
                String pkg = checkpoint ? null : in.readUTF();
                String cls = checkpoint ? null : in.readUTF();
                loaded.add(new Event(time, pkg, cls, id, state));
                previous = time;
                nextId = Math.max(nextId, id);
            }
            if (in.read() != -1) throw new IOException("Trailing usage history bytes");
        }
        // An RTC correction while the broker was down can put the snapshot in the future.
        // Ignore only that suffix. A future cutoff cannot be a valid duration endpoint.
        long now = mClock.getAsLong();
        for (Event event : loaded) {
            if (event.time > now) break;
            add(event);
        }
        mNextId = nextId;
        if (!mHistory.isEmpty()) {
            add(new Event(Math.min(savedAt, now), null, null, 0, State.CHECKPOINT_END));
        }
    }

    void flush() throws IOException {
        if (mFile == null) return;
        synchronized (mWriterLock) {
            Snapshot snapshot;
            long revision;
            synchronized (this) {
                if (mRevision == mWrittenRevision && mVisible == 0) return;
                snapshot = snapshot();
                revision = mRevision;
            }
            Files.createDirectories(mFile.getParent());
            Path temporary = Files.createTempFile(mFile.getParent(), "usage-", ".tmp");
            try {
                try (java.io.FileOutputStream file = new java.io.FileOutputStream(temporary.toFile());
                        DataOutputStream out = new DataOutputStream(new BufferedOutputStream(file))) {
                    out.writeInt(FILE_VERSION);
                    out.writeLong(snapshot.until);
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
                    out.flush();
                    file.getFD().sync();
                }
                Files.move(temporary, mFile, StandardCopyOption.ATOMIC_MOVE,
                        StandardCopyOption.REPLACE_EXISTING);
                synchronized (this) { mWrittenRevision = revision; }
            } finally {
                Files.deleteIfExists(temporary);
            }
        }
    }

    static List<Total> aggregate(Snapshot snapshot, long width, long begin, long end) {
        long oldest = Math.max(0, snapshot.until - RETENTION);
        begin = Math.max(oldest, begin);
        if (begin >= end || begin > snapshot.until) return List.of();
        // A query selects whole buckets, but never aggregates before retention or into the future.
        long first = begin / width * width;
        long last = (Math.min(end - 1, snapshot.until) / width) * width;
        Map<String, PackageState> active = new HashMap<>();
        Map<Long, Map<String, Total>> buckets = new TreeMap<>();
        String lastPaused = null;
        for (Event event : snapshot.events) {
            if (event.state == State.CHECKPOINT_END) {
                for (var item : active.entrySet()) {
                    addTime(buckets, item.getKey(), item.getValue(), event.time,
                            width, first, last, oldest);
                }
                active.clear();
                lastPaused = null;
                continue;
            }
            PackageState state = active.computeIfAbsent(event.pkg, ignored -> new PackageState());
            addTime(buckets, event.pkg, state, event.time, width, first, last, oldest);
            state.advance(event);
            if (state.activities.isEmpty()) active.remove(event.pkg);
            long bucket = event.time / width * width;
            if (event.time >= oldest && bucket >= first && bucket <= last) {
                Total total = total(buckets, bucket, event.pkg);
                total.end = event.time;
                total.lastEvent = event.state;
                if (event.state == State.RESUMED) {
                    total.lastUsed = event.time;
                    total.lastVisible = event.time;
                    total.launches++;
                    if (!event.pkg.equals(lastPaused)) total.appLaunches++;
                }
            }
            if (event.state == State.PAUSED) lastPaused = event.pkg;
        }
        for (var item : active.entrySet()) {
            addTime(buckets, item.getKey(), item.getValue(), snapshot.until,
                    width, first, last, oldest);
        }
        List<Total> result = new ArrayList<>();
        for (Map<String, Total> bucket : buckets.values()) result.addAll(bucket.values());
        return result;
    }

    private static void addTime(Map<Long, Map<String, Total>> buckets, String pkg,
            PackageState state, long end, long width, long first, long last, long oldest) {
        if (state.activities.isEmpty()) return;
        long start = Math.max(state.time, Math.max(oldest, first));
        while (start < end) {
            long bucket = start / width * width;
            if (bucket > last) break;
            long stop = Math.min(end, bucket + width);
            Total total = total(buckets, bucket, pkg);
            total.visible += stop - start;
            total.lastVisible = stop;
            total.end = stop;
            if (state.resumed != 0) {
                total.foreground += stop - start;
                total.lastUsed = stop;
            }
            start = stop;
        }
    }

    private static Total total(Map<Long, Map<String, Total>> buckets, long bucket, String pkg) {
        return buckets.computeIfAbsent(bucket, ignored -> new TreeMap<>())
                .computeIfAbsent(pkg, ignored -> new Total(bucket, pkg));
    }
}
