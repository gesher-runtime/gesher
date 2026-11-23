/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package com.gesher.server;

import java.util.LinkedHashMap;
import java.util.LinkedHashSet;
import java.util.Map;

/** Coalesces profile revisions without invalidating the installed code generation. */
final class DexoptWorkQueue {
    static final class Work {
        final String packageName, codePath;
        final long generation, revision;
        Work(String name, String path, long generation, long revision) {
            packageName = name; codePath = path; this.generation = generation;
            this.revision = revision;
        }
    }
    private static final class Entry {
        String codePath;
        final long generation;
        long revision, after;
        int failures;
        Entry(String path, long generation) { codePath = path; this.generation = generation; }
    }
    private final Map<String, Entry> installed = new LinkedHashMap<>();
    private final LinkedHashSet<String> pending = new LinkedHashSet<>();
    private long nextGeneration, epoch, registryEpoch;

    synchronized void reconcile(Map<String, String> paths) {
        reconcile(paths, registryEpoch);
    }

    synchronized long registryEpoch() { return registryEpoch; }

    synchronized boolean reconcile(Map<String, String> paths, long observedRegistryEpoch) {
        // A snapshot captured before installer withdrawal cannot resurrect that
        // code generation after installation/removal has already begun.
        if (registryEpoch != observedRegistryEpoch) return false;
        installed.keySet().removeIf(name -> !paths.containsKey(name));
        pending.retainAll(installed.keySet());
        for (Map.Entry<String, String> path : paths.entrySet()) {
            Entry entry = installed.get(path.getKey());
            if (entry == null || !entry.codePath.equals(path.getValue())) {
                installed.put(path.getKey(), new Entry(path.getValue(), ++nextGeneration));
                changed(path.getKey());
            }
        }
        wake();
        return true;
    }

    synchronized void changed(String name) {
        Entry entry = installed.get(name);
        if (entry == null) return;
        // A captured profile remains valid input when its saver appends more data.
        // Only withdrawal/replacement of the installed APK invalidates that work.
        ++entry.revision;
        // Keep a failed tool's backoff even if it keeps touching the same input.
        pending.add(name);
        wake();
    }

    synchronized void changedAll() {
        for (String name : installed.keySet()) changed(name);
    }

    synchronized void removed(String name) {
        ++registryEpoch;
        installed.remove(name);
        pending.remove(name);
        wake();
    }

    synchronized Work poll(long now) {
        for (String name : pending) {
            Entry entry = installed.get(name);
            if (entry.after <= now)
                return new Work(name, entry.codePath, entry.generation, entry.revision);
        }
        return null;
    }

    synchronized boolean current(Work work) {
        Entry entry = installed.get(work.packageName);
        return entry != null && entry.generation == work.generation
                && entry.codePath.equals(work.codePath);
    }

    synchronized void finished(Work work, boolean success, long now) {
        Entry entry = installed.get(work.packageName);
        if (!current(work)) return;
        if (success) {
            entry.failures = 0;
            entry.after = 0;
            pending.remove(work.packageName);
            // Publish the valid snapshot, then fairly revisit any newer profile.
            if (entry.revision != work.revision) pending.add(work.packageName);
        } else {
            entry.failures = Math.min(6, entry.failures + 1);
            entry.after = now + Math.min(60_000L, 1000L << (entry.failures - 1));
            // A repeatedly broken package must not starve the remaining queue.
            pending.remove(work.packageName);
            pending.add(work.packageName);
        }
        wake();
    }

    /** 0 means no pending work; positive values are real retry deadlines. */
    synchronized long delay(long now) {
        long delay = Long.MAX_VALUE;
        for (String name : pending) delay = Math.min(delay, Math.max(1, installed.get(name).after - now));
        return delay == Long.MAX_VALUE ? 0 : delay;
    }

    synchronized int size() { return pending.size(); }
    synchronized long epoch() { return epoch; }
    synchronized void wake() { ++epoch; notifyAll(); }
    synchronized void await(long observedEpoch, long delay) throws InterruptedException {
        if (epoch == observedEpoch) wait(delay);
    }
}
