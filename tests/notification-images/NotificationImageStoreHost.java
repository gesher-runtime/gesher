/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package com.gesher.server;

import java.io.*;
import java.nio.file.*;
import java.lang.reflect.Field;
import java.util.*;
import java.util.concurrent.*;
import java.util.concurrent.atomic.*;

/** Complete production file/cache/admission owner; explicit synthetic PNG encoder. */
public final class NotificationImageStoreHost {
    private static final AtomicInteger checks = new AtomicInteger();
    private static final AtomicReference<Throwable> failure = new AtomicReference<>();
    private static void check(boolean value, String why) { if (!value) throw new AssertionError(why); checks.incrementAndGet(); }
    private static Object field(String name) throws Exception {
        Field f = NotificationImageStore.class.getDeclaredField(name); f.setAccessible(true); return f.get(null);
    }
    private static void await(CountDownLatch latch) {
        try { check(latch.await(5, TimeUnit.SECONDS), "fixture did not progress"); }
        catch (InterruptedException e) { throw new AssertionError(e); }
    }
    private interface Work { void run() throws Exception; }
    private static Thread thread(Work work) {
        Thread t = new Thread(() -> { try { work.run(); } catch (Throwable e) { failure.compareAndSet(null, e); } });
        t.setDaemon(true); t.start(); return t;
    }
    private static void join(Thread t) throws Exception {
        t.join(5000); check(!t.isAlive(), "worker did not retire");
        if (failure.get() != null) throw new AssertionError("worker failed", failure.get());
    }
    private static final class Source implements NotificationImageStore.Image {
        byte[] bytes;
        boolean stable = true, reusable = true, fail, empty;
        int encodes;
        Runnable encodeHook;
        Source(int value) { bytes = new byte[32 * 1024]; Arrays.fill(bytes, (byte)value); }
        public byte[] signature() { return reusable ? NotificationImageStore.sha256().digest(bytes) : null; }
        public boolean unchanged() { return stable; }
        public boolean encode(OutputStream out) throws IOException {
            encodes++;
            if (encodeHook != null) encodeHook.run();
            out.write(bytes);
            if (fail) throw new IOException("injected encoder/write failure");
            return !empty;
        }
    }
    private static void publish(File file, Object owner, Source source) throws Exception {
        try (var prepared = NotificationImageStore.prepare(file, owner, source)) {
            check(prepared.publish().equals(file), "image not published");
        }
    }
    private static long parts(File dir) throws IOException {
        try (var paths = Files.walk(dir.toPath())) { return paths.filter(p -> p.toString().endsWith(".part")).count(); }
    }

    private static void files(File dir) throws Exception {
        Object owner = new Object(); File file = new File(dir, "image.png"); Source source = new Source(1);
        try (var prepared = NotificationImageStore.prepare(file, owner, source)) {
            check(!file.exists() && parts(dir) == 1, "preparation became visible before publication");
            prepared.publish();
        }
        check(Arrays.equals(Files.readAllBytes(file.toPath()), source.bytes), "published bytes differ");
        for (int i = 0; i < 100; ++i) publish(file, owner, source);
        check(source.encodes == 1, "unchanged updates repeated encoding");
        Source duplicate = new Source(1); publish(file, owner, duplicate);
        check(duplicate.encodes == 0, "content-equal distinct source missed cache");
        Source changed = new Source(2); publish(file, owner, changed);
        check(changed.encodes == 1, "changed mutable pixels reused stale data");
        publish(file, new Object(), changed); check(changed.encodes == 2, "new owner reused previous generation");
        publish(file, owner, changed);
        long stamp = file.lastModified();
        byte[] corrupt = changed.bytes.clone(); corrupt[17] ^= 1;
        Files.write(file.toPath(), corrupt); file.setLastModified(stamp);
        int before = changed.encodes; publish(file, owner, changed);
        check(changed.encodes == before + 1 && Arrays.equals(Files.readAllBytes(file.toPath()), changed.bytes),
                "same-size/same-stamp file corruption was treated as a cache hit");
        byte[] previous = Files.readAllBytes(file.toPath());
        try (var ignored = NotificationImageStore.prepare(file, owner, new Source(3))) { }
        check(Arrays.equals(previous, Files.readAllBytes(file.toPath())) && parts(dir) == 0,
                "discarded preparation replaced the active image or leaked staging");
        Source broken = new Source(4); broken.fail = true;
        try { NotificationImageStore.prepare(file, owner, broken); throw new AssertionError("failed encode accepted"); }
        catch (IOException expected) { checks.incrementAndGet(); }
        check(Arrays.equals(previous, Files.readAllBytes(file.toPath())) && parts(dir) == 0, "failed encode harmed old image");
        broken.fail = false; broken.empty = true;
        try (var prepared = NotificationImageStore.prepare(file, owner, broken)) { check(prepared.publish() == null, "false encode accepted"); }
        check(Arrays.equals(previous, Files.readAllBytes(file.toPath())) && parts(dir) == 0, "false encode harmed old image");
        Source uncacheable = new Source(5); uncacheable.reusable = false;
        publish(file, owner, uncacheable); publish(file, owner, uncacheable);
        check(uncacheable.encodes == 2, "unknown content identity was cached");
        Source mutating = new Source(6); mutating.stable = false;
        publish(file, owner, mutating); publish(file, owner, mutating);
        check(mutating.encodes == 2, "concurrently mutating source was cached");
        File obstruction = new File(dir, "not-a-directory"); Files.writeString(obstruction.toPath(), "occupied");
        try { NotificationImageStore.prepare(new File(obstruction, "x.png"), owner, source); throw new AssertionError("bad directory accepted"); }
        catch (IOException expected) { checks.incrementAndGet(); }
        File targetDir = new File(dir, "directory-target"); targetDir.mkdir();
        Files.writeString(new File(targetDir, "keep").toPath(), "old");
        try (var prepared = NotificationImageStore.prepare(targetDir, owner, source)) {
            try { prepared.publish(); throw new AssertionError("failed atomic replace accepted"); }
            catch (IOException expected) { checks.incrementAndGet(); }
        }
        check(Files.readString(new File(targetDir, "keep").toPath()).equals("old") && parts(dir) == 0,
                "failed atomic move removed prior target or leaked staging");
    }

    private static void bounds(File dir) throws Exception {
        Object owner = new Object(); Source source = new Source(7); File first = new File(dir, "cache-0.png");
        for (int i = 0; i < NotificationImageStore.MAX_CACHE_ENTRIES + 12; ++i) {
            publish(new File(dir, "cache-" + i + ".png"), owner, source);
        }
        check(((Map<?, ?>)field("CACHE")).size() == NotificationImageStore.MAX_CACHE_ENTRIES, "metadata cache exceeded bound");
        check(first.isFile(), "cache eviction removed a live persisted toast image");
        int before = source.encodes; publish(first, owner, source);
        check(source.encodes == before + 1, "old fingerprint was not evicted");
        CountDownLatch entered = new CountDownLatch(4), release = new CountDownLatch(1), fifth = new CountDownLatch(1);
        List<Thread> workers = new ArrayList<>();
        for (int i = 0; i < 4; ++i) {
            File target = new File(dir, "blocked-" + i + ".png"); Source blocked = new Source(8);
            blocked.encodeHook = () -> { entered.countDown(); await(release); };
            workers.add(thread(() -> publish(target, owner, blocked)));
        }
        await(entered);
        Source extra = new Source(9); extra.encodeHook = fifth::countDown;
        Thread last = thread(() -> publish(new File(dir, "fifth.png"), owner, extra));
        check(!fifth.await(150, TimeUnit.MILLISECONDS), "fifth encoder exceeded admission bound");
        check(((Semaphore)field("ENCODERS")).availablePermits() == 0, "blocked calls lost ownership");
        release.countDown(); for (Thread t : workers) join(t); join(last); await(fifth);
    }

    private static void cancellation(File dir) throws Exception {
        Object owner = new Object(); File file = new File(dir, "cancel.png"); publish(file, owner, new Source(1));
        CountDownLatch entered = new CountDownLatch(1), release = new CountDownLatch(1);
        Source blocked = new Source(2); blocked.encodeHook = () -> { entered.countDown(); await(release); };
        Thread worker = thread(() -> { try (var ignored = NotificationImageStore.prepare(file, owner, blocked)) { } });
        await(entered);
        long start = System.nanoTime(); NotificationImageStore.remove(file);
        check(System.nanoTime() - start < TimeUnit.MILLISECONDS.toNanos(300), "cancellation joined active encoder");
        check(file.exists(), "cancellation freed an image while a preparation could still own it");
        release.countDown(); join(worker);
        check(!file.exists() && parts(dir) == 0, "cancelled encoder left old or staged data");
        publish(file, owner, new Source(3));
        try (var prepared = NotificationImageStore.prepare(file, owner, new Source(3))) {
            NotificationImageStore.remove(file);
            check(prepared.publish().equals(file), "later accepted cache hit lost its file");
        }
        check(file.isFile(), "retired cancellation removed a newer publication");
        CountDownLatch entered2 = new CountDownLatch(1), release2 = new CountDownLatch(1);
        Source slow = new Source(4); slow.encodeHook = () -> { entered2.countDown(); await(release2); };
        Thread first = thread(() -> publish(file, owner, slow)); await(entered2);
        AtomicBoolean cancelled = new AtomicBoolean();
        Thread waiting = new Thread(() -> {
            try (var ignored = NotificationImageStore.prepare(file, owner, new Source(5))) { throw new AssertionError("interrupted lane entered"); }
            catch (InterruptedException expected) { cancelled.set(true); }
            catch (Throwable e) { failure.compareAndSet(null, e); }
        });
        waiting.start(); waiting.interrupt(); join(waiting); check(cancelled.get(), "lane interruption was swallowed");
        release2.countDown(); join(first);
        check(((Map<?, ?>)field("LANES")).isEmpty() && ((Semaphore)field("ENCODERS")).availablePermits() == 4,
                "lane/permit ownership did not drain");
        NotificationImageStore.remove(file); check(!file.exists(), "idle cancellation retained image");
    }

    private static void notifiedWaiter(File dir) throws Exception {
        Object owner = new Object(); File file = new File(dir, "notified-waiter.png");
        var first = NotificationImageStore.prepare(file, owner, new Source(1));
        CountDownLatch secondEntered = new CountDownLatch(1), releaseSecond = new CountDownLatch(1);
        Source second = new Source(2); second.encodeHook = () -> { secondEntered.countDown(); await(releaseSecond); };
        Thread waiting = thread(() -> publish(file, owner, second));
        long deadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(5);
        while (waiting.getState() != Thread.State.WAITING && System.nanoTime() < deadline) Thread.yield();
        check(waiting.getState() == Thread.State.WAITING, "second caller did not wait for pathname ownership");
        // Interpose deletion after notify but before the waiter can reacquire
        // the monitor. This used to replace its still-owned lane with a new one.
        synchronized (field("LOCK")) { first.close(); NotificationImageStore.remove(file); }
        await(secondEntered);
        CountDownLatch thirdEntered = new CountDownLatch(1);
        Source third = new Source(3); third.encodeHook = thirdEntered::countDown;
        Thread newcomer = thread(() -> publish(file, owner, third));
        try { check(!thirdEntered.await(150, TimeUnit.MILLISECONDS), "deletion detached a notified same-path waiter"); }
        finally { releaseSecond.countDown(); }
        join(waiting); join(newcomer); await(thirdEntered);
        check(Arrays.equals(Files.readAllBytes(file.toPath()), third.bytes), "notified waiter overwrote its successor");
        NotificationImageStore.remove(file);
        check(((Map<?, ?>)field("LANES")).isEmpty(), "notified-waiter ownership did not drain");
    }

    public static void main(String[] args) throws Exception {
        File dir = new File(args[0]); dir.mkdirs(); files(dir); bounds(dir); cancellation(dir); notifiedWaiter(dir);
        check(parts(dir) == 0, "staging leak at completion");
        System.out.println("NotificationImageStoreHost passed: " + checks.get() + " checks");
        System.out.println("100 unchanged updates: 1 encode total; 256 metadata entries; 4 encoders; no cached pixel/PNG arrays.");
        System.out.println("Complete Java owner and real filesystem; synthetic encoded bytes, not Bitmap/PNG/Windows performance.");
    }
}
