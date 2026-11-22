/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package com.gesher.server;

import java.io.BufferedInputStream;
import java.io.BufferedOutputStream;
import java.io.DataInputStream;
import java.io.DataOutputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.StandardCopyOption;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.Objects;
import java.util.function.LongSupplier;
import java.util.function.Supplier;

/** Android 16 ShortcutPackage API accounting; owned state, with no periodic quota worker. */
final class ShortcutQuota {
    static final int MAX_CALLS = 10;
    static final long INTERVAL = 24 * 60 * 60 * 1000L;
    // ShortcutService.isClockValid uses this value (including its units).
    private static final long VALID_CLOCK = 1_420_070_400L;
    private static final int MAGIC = 0x47535131; // GSQ1

    private record Entry(String name, int uid, long installed, long reset, int used) {
        boolean owns(ApkPackage pkg) {
            return pkg != null && uid == pkg.uid() && installed == pkg.firstInstallTime()
                    && name.equals(pkg.packageName);
        }
    }
    private static final class Counter {
        final String name;
        final int uid;
        final long installed;
        long reset;
        int used;
        boolean foregroundVisit;
        Counter(Entry entry) {
            name = entry.name; uid = entry.uid; installed = entry.installed;
            reset = entry.reset; used = entry.used;
        }
        Entry snapshot() { return new Entry(name, uid, installed, reset, used); }
        boolean owns(ApkPackage pkg) {
            return uid == pkg.uid() && installed == pkg.firstInstallTime() && name.equals(pkg.packageName);
        }
    }
    private record State(long reset, String locales, List<Entry> entries) { }

    private final PackageManagerService mPackages;
    private final Path mFile;
    private final LongSupplier mClock;
    private final Supplier<String> mLocales;
    private final PolicyStateWriter<State> mWriter;
    private final Map<String, Counter> mCounters = new LinkedHashMap<>();
    private final Map<Integer, List<Counter>> mByUid = new HashMap<>();
    private long mReset;
    private String mKnownLocales = "";

    ShortcutQuota(PackageManagerService packages, Path file, LongSupplier clock,
            Supplier<String> locales) {
        mPackages = packages; mFile = file; mClock = clock; mLocales = locales;
        read();
        mWriter = file == null ? null : new PolicyStateWriter<>("Shortcut quota", this::write);
    }

    private void add(Counter counter) {
        mCounters.put(counter.name, counter);
        mByUid.computeIfAbsent(counter.uid, ignored -> new ArrayList<>()).add(counter);
    }

    private void removeCounter(String name) {
        Counter old = mCounters.remove(name);
        if (old == null) return;
        List<Counter> members = mByUid.get(old.uid);
        members.remove(old);
        if (members.isEmpty()) mByUid.remove(old.uid);
    }

    /** Lifecycle hooks only mark owned counters; they never acquire a controller or do I/O. */
    synchronized void foreground(int uid) {
        List<Counter> members = mByUid.get(uid);
        if (members != null) for (Counter counter : members) counter.foregroundVisit = true;
    }

    synchronized void removed(String name) {
        if (mCounters.containsKey(name)) { removeCounter(name); changed(); }
    }

    private static long addInterval(long time) {
        return time > Long.MAX_VALUE - INTERVAL ? Long.MAX_VALUE : time + INTERVAL;
    }

    private boolean updateTimes(long now) {
        long old = mReset;
        if (mReset == 0 || now < mReset && now >= VALID_CLOCK) {
            mReset = now;
        } else if (now >= addInterval(mReset)) {
            // Preserve Android 16's phase calculation even when its offset puts
            // this anchor briefly in the future; the next access takes rewind.
            long boundary = now / INTERVAL * INTERVAL;
            long offset = mReset % INTERVAL;
            mReset = offset > 0 && boundary > Long.MAX_VALUE - offset ? Long.MAX_VALUE
                    : offset < 0 && boundary < Long.MIN_VALUE - offset ? Long.MIN_VALUE : boundary + offset;
        }
        return old != mReset;
    }

    private boolean updateLocales() {
        String current = Objects.requireNonNull(mLocales.get());
        if (current.equals(mKnownLocales)) return false;
        mKnownLocales = current;
        for (Counter counter : mCounters.values()) counter.used = 0;
        return true;
    }

    /** Foreground state and permission checks must be sampled before taking this monitor. */
    synchronized int access(ApkPackage owner, boolean foreground, boolean unlimited, boolean take) {
        long now = mClock.getAsLong();
        boolean dirty = updateLocales() | updateTimes(now);
        Counter counter = mCounters.get(owner.packageName);
        if (counter == null || !counter.owns(owner)) {
            removeCounter(owner.packageName);
            counter = new Counter(new Entry(owner.packageName, owner.uid(), owner.firstInstallTime(), 0, 0));
            add(counter);
            dirty = true;
        }
        int used = counter.used;
        long reset = counter.reset;
        if (foreground || unlimited || counter.foregroundVisit) counter.used = 0;
        counter.foregroundVisit = false;
        if (now >= VALID_CLOCK && counter.reset > now) {
            counter.reset = now;
            counter.used = 0;
        } else if (counter.reset < mReset) {
            counter.reset = mReset;
            counter.used = 0;
        }
        int remaining = MAX_CALLS - counter.used;
        if (take && remaining > 0) counter.used++;
        if (dirty || used != counter.used || reset != counter.reset) changed();
        return remaining;
    }

    synchronized long nextReset() {
        if (updateTimes(mClock.getAsLong())) changed();
        return addInterval(mReset);
    }

    synchronized void reset(String name) {
        boolean dirty = false;
        for (Counter counter : mCounters.values()) {
            if (name == null || name.equals(counter.name)) {
                dirty |= counter.used != 0 || name != null && counter.reset != 0;
                counter.used = 0;
                if (name != null) counter.reset = 0;
            }
        }
        if (dirty) changed();
    }

    private void changed() {
        if (mWriter == null) return;
        List<Entry> entries = new ArrayList<>(mCounters.size());
        for (Counter counter : mCounters.values()) entries.add(counter.snapshot());
        mWriter.schedule(new State(mReset, mKnownLocales, List.copyOf(entries)));
    }

    boolean flush() { return mWriter == null || mWriter.flush(); }

    private void read() {
        if (mFile == null || !Files.exists(mFile)) return;
        // Parse into detached state: a truncated file cannot publish a prefix of counters.
        try (DataInputStream input = new DataInputStream(new BufferedInputStream(Files.newInputStream(mFile)))) {
            if (input.readInt() != MAGIC) throw new IOException("Unknown shortcut quota format");
            long reset = input.readLong();
            String locales = input.readUTF();
            int count = input.readInt();
            if (count < 0 || count > Files.size(mFile) / 26) {
                throw new IOException("Invalid shortcut quota header");
            }
            Map<String, Counter> decoded = new LinkedHashMap<>();
            for (int i = 0; i < count; i++) {
                Entry entry = new Entry(input.readUTF(), input.readInt(), input.readLong(),
                        input.readLong(), input.readInt());
                if (entry.uid < 0 || entry.used < 0 || entry.used > MAX_CALLS) {
                    throw new IOException("Invalid shortcut quota counter");
                }
                if (entry.owns(mPackages.get(entry.name))) {
                    if (decoded.put(entry.name, new Counter(entry)) != null) {
                        throw new IOException("Duplicate shortcut quota counter");
                    }
                }
            }
            if (input.read() != -1) throw new IOException("Trailing shortcut quota data");
            mReset = reset; mKnownLocales = locales;
            for (Counter counter : decoded.values()) add(counter);
        } catch (IOException failure) {
            SystemServer.log("Shortcut quota: ignoring unreadable state: " + failure.getMessage());
        }
    }

    private void write(State state) throws IOException {
        Path temporary = mFile.resolveSibling(mFile.getFileName() + ".new");
        Files.createDirectories(mFile.toAbsolutePath().getParent());
        try {
            try (FileOutputStream file = new FileOutputStream(temporary.toFile());
                    DataOutputStream output = new DataOutputStream(new BufferedOutputStream(file))) {
                output.writeInt(MAGIC); output.writeLong(state.reset); output.writeUTF(state.locales);
                output.writeInt(state.entries.size());
                for (Entry entry : state.entries) {
                    output.writeUTF(entry.name); output.writeInt(entry.uid); output.writeLong(entry.installed);
                    output.writeLong(entry.reset); output.writeInt(entry.used);
                }
                output.flush(); file.getFD().sync();
            }
            Files.move(temporary, mFile, StandardCopyOption.ATOMIC_MOVE, StandardCopyOption.REPLACE_EXISTING);
        } finally { Files.deleteIfExists(temporary); }
    }
}
