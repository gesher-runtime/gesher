// Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0
package android.net.http;

import java.io.IOException;
import java.util.ArrayDeque;
import java.util.ArrayList;
import java.util.IdentityHashMap;
import java.util.List;
import java.util.concurrent.Executor;
import java.util.concurrent.RejectedExecutionException;
import java.util.concurrent.TimeUnit;

/** Admission for unary requests. Duplex and caller-owned URLConnection work
 * have separate progress/ownership requirements and do not use these permits. */
final class HttpRequestScheduler {
    static final int ENGINE_ACTIVE = 32, PROCESS_ACTIVE = 128;
    // Outstanding includes queued, active and canceled-but-not-retired owners.
    static final int ENGINE_OUTSTANDING = 1024, PROCESS_OUTSTANDING = 4096;
    static final long ENGINE_METADATA = 8L << 20, PROCESS_METADATA = 32L << 20;
    private static final Object LOCK = new Object();
    private static final IdentityHashMap<JavaHttpEngine, Group> GROUPS = new IdentityHashMap<>();
    private static final ArrayDeque<Group> READY = new ArrayDeque<>();
    private static int active, outstanding;
    private static long metadata;
    static final Workers IO = new Workers("Gesher-HttpEngine-unary", PROCESS_ACTIVE,
            PROCESS_ACTIVE * 2, TimeUnit.SECONDS.toNanos(60));
    // Socket cancellation/close must make progress when every I/O worker is
    // waiting for a peer. There is at most one queued cleanup per owned ticket.
    static final Workers CLEANUP = new Workers("Gesher-HttpEngine-retire", 4,
            PROCESS_OUTSTANDING, TimeUnit.SECONDS.toNanos(60));
    // Calling an application's Executor can itself block. Keep that handoff
    // away from socket cancellation, with its own bounded workers and queue.
    static final Workers COMPLETION = new Workers("Gesher-HttpEngine-complete", 4,
            PROCESS_OUTSTANDING, TimeUnit.SECONDS.toNanos(60));

    private static final class Group {
        final JavaHttpEngine engine;
        final List<ArrayDeque<Ticket>> pending = new ArrayList<>();
        int active, outstanding;
        int queued;
        long metadata;
        boolean ready;
        Group(JavaHttpEngine engine) {
            this.engine = engine;
            for (int i = 0; i <= UrlRequest.REQUEST_PRIORITY_HIGHEST; i++) pending.add(new ArrayDeque<>());
        }
        void add(Ticket ticket) { pending.get(ticket.priority).add(ticket); queued++; }
        void remove(Ticket ticket) { if (pending.get(ticket.priority).remove(ticket)) queued--; }
        Ticket poll() {
            for (int i = pending.size() - 1; i >= 0; i--) {
                Ticket ticket = pending.get(i).poll();
                if (ticket != null) { queued--; return ticket; }
            }
            return null;
        }
    }

    static final class Ticket {
        final JavaHttpEngine engine;
        final long bytes;
        final int priority;
        private Runnable start;
        private Group group;
        private boolean canceled, retired, released;
        volatile boolean counted, running, pending;
        Ticket(JavaHttpEngine engine, long bytes, int priority, Runnable start) {
            this.engine = engine; this.bytes = bytes; this.priority = priority; this.start = start;
        }
        IOException submit() {
            List<Runnable> starts;
            synchronized (LOCK) {
                if (canceled || released) return null;
                Group owner = GROUPS.get(engine);
                if (outstanding >= PROCESS_OUTSTANDING || bytes > PROCESS_METADATA - metadata
                        || bytes > ENGINE_METADATA
                        || owner != null && (owner.outstanding >= ENGINE_OUTSTANDING
                        || bytes > ENGINE_METADATA - owner.metadata)) {
                    return new IOException("HTTP request admission budget exhausted");
                }
                if (owner == null) { owner = new Group(engine); GROUPS.put(engine, owner); }
                group = owner; counted = true; pending = true;
                owner.outstanding++; outstanding++;
                owner.metadata += bytes; metadata += bytes;
                owner.add(this); ready(owner);
                starts = admit();
            }
            // No request, engine, network or application code runs under LOCK.
            for (Runnable task : starts) task.run();
            return null;
        }
        void cancelQueued() {
            synchronized (LOCK) {
                canceled = true;
                if (pending) { group.remove(this); pending = false; }
                // Keep the outstanding/metadata charge until terminal cleanup
                // has actually retired. Cancellation is not forced termination.
            }
        }
        void retire() {
            List<Runnable> starts;
            synchronized (LOCK) { retireLocked(); starts = admit(); }
            for (Runnable task : starts) task.run();
        }
        private void retireLocked() {
            if (retired) return;
            retired = true;
            if (!counted) return;
            if (pending) group.remove(this);
            if (running) { group.active--; active--; }
            pending = false; running = false;
            ready(group);
        }
        void release() {
            List<Runnable> starts;
            synchronized (LOCK) {
                if (released) return;
                retireLocked();
                released = true; start = null;
                if (!counted) return;
                Group owner = group;
                owner.outstanding--; outstanding--;
                owner.metadata -= bytes; metadata -= bytes;
                counted = false; pending = false; running = false; group = null;
                if (owner.outstanding == 0) {
                    GROUPS.remove(owner.engine);
                    if (owner.ready) { READY.remove(owner); owner.ready = false; }
                } else ready(owner);
                starts = admit();
            }
            for (Runnable task : starts) task.run();
        }
    }

    private static void ready(Group owner) {
        if (!owner.ready && owner.active < ENGINE_ACTIVE && owner.queued != 0) {
            owner.ready = true; READY.add(owner);
        }
    }
    private static List<Runnable> admit() {
        List<Runnable> starts = new ArrayList<>();
        while (active < PROCESS_ACTIVE && !READY.isEmpty()) {
            Group owner = READY.remove(); owner.ready = false;
            Ticket ticket = owner.poll();
            if (ticket == null) continue;
            ticket.pending = false; ticket.running = true;
            owner.active++; active++;
            starts.add(ticket.start);
            // FIFO within a priority; round-robin between engines with work.
            ready(owner);
        }
        return starts;
    }

    /** Conservatively charge retained URL/header strings and entry structures.
     * This is an admission charge, not a VM-specific retained-heap measurement. */
    static long metadata(String url, String agent, List<java.util.Map.Entry<String, String>> headers) {
        long bytes = 1024L + 2L * (url.length() + (long) agent.length());
        for (java.util.Map.Entry<String, String> header : headers) {
            bytes += 192L + 2L * (header.getKey().length() + (long) header.getValue().length());
            if (bytes > ENGINE_METADATA) break;
        }
        return bytes;
    }

    /** Demand-grown, bounded workers; idle workers park and retire. No caller
     * runs a blocking network task when the queue is saturated. */
    static final class Workers implements Executor {
        private final String name;
        private final int limit, capacity;
        private final long idleNanos;
        private final ArrayDeque<Runnable> tasks = new ArrayDeque<>();
        private int workers, idle;
        Workers(String name, int limit, int capacity, long idleNanos) {
            this.name = name; this.limit = limit; this.capacity = capacity; this.idleNanos = idleNanos;
        }
        @Override public synchronized void execute(Runnable task) {
            if (tasks.size() >= capacity) throw new RejectedExecutionException("HTTP worker queue exhausted");
            tasks.add(task);
            if (idle != 0) notify();
            // A notified worker still contributes to idle until it reacquires
            // this monitor. Additional queued demand must grow the pool even
            // during that interval, or one blocked task could strand the rest.
            if (tasks.size() > idle && workers < limit) {
                try { startWorker(); }
                catch (RuntimeException | Error failure) { tasks.removeLastOccurrence(task); throw failure; }
            }
        }
        private void startWorker() {
            workers++;
            try {
                Thread thread = new Thread(this::run, name);
                thread.setDaemon(true); thread.start();
            } catch (RuntimeException | Error failure) { workers--; throw failure; }
        }
        private void run() {
            boolean registered = true;
            try {
                for (;;) {
                    Runnable task;
                    synchronized (this) {
                        long deadline = System.nanoTime() + idleNanos;
                        while ((task = tasks.poll()) == null) {
                            long remaining = deadline - System.nanoTime();
                            if (remaining <= 0) {
                                // Retire under the queue lock. An enqueue after
                                // this point must see the free worker capacity.
                                workers--; registered = false; return;
                            }
                            idle++;
                            try { TimeUnit.NANOSECONDS.timedWait(this, remaining); }
                            catch (InterruptedException ignored) { /* A task owns its interruption policy. */ }
                            finally { idle--; }
                        }
                    }
                    // Keep the worker accounting valid even if a client
                    // callback executor rejects or a task unexpectedly throws.
                    try { task.run(); }
                    catch (RuntimeException ignored) { }
                }
            } finally {
                synchronized (this) {
                    if (registered) {
                        workers--;
                        if (!tasks.isEmpty() && workers < limit) startWorker();
                    }
                }
            }
        }
    }
}
