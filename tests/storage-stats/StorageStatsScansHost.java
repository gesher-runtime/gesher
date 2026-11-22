/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package com.gesher.server;

import android.app.usage.ExternalStorageStats;
import android.app.usage.StorageStats;
import android.os.Binder;
import android.os.ParcelableException;
import java.io.*;
import java.lang.reflect.Field;
import java.nio.file.*;
import java.util.*;
import java.util.concurrent.*;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.function.BooleanSupplier;

public final class StorageStatsScansHost {
    private static int checks;
    private static void require(boolean result, String message) {
        ++checks;
        if (!result) throw new AssertionError(message);
    }
    private static void await(BooleanSupplier condition, String message) throws Exception {
        long deadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(15);
        while (!condition.getAsBoolean()) {
            if (System.nanoTime() >= deadline) throw new AssertionError(message);
            Thread.sleep(1);
        }
    }
    private static final class Task<T> {
        final FutureTask<T> future;
        final Thread thread;
        Task(Callable<T> work) {
            future = new FutureTask<>(work);
            thread = new Thread(future, "storage-test-caller");
            thread.setDaemon(true);
            thread.start();
        }
        T get() throws Exception { return future.get(20, TimeUnit.SECONDS); }
    }
    private static boolean blocked(Task<?> task) {
        return !task.future.isDone() && (task.thread.getState() == Thread.State.WAITING
                || task.thread.getState() == Thread.State.TIMED_WAITING);
    }
    private static void blocked(List<? extends Task<?>> tasks) throws Exception {
        await(() -> tasks.stream().allMatch(StorageStatsScansHost::blocked), "callers did not block");
    }
    private interface Checked { void run() throws Exception; }
    private static void fails(Class<? extends Throwable> type, Checked work, String message)
            throws Exception {
        try { work.run(); }
        catch (Throwable error) {
            while (error instanceof ExecutionException) error = error.getCause();
            require(type.isInstance(error), message + ": " + error);
            return;
        }
        throw new AssertionError(message + ": unexpectedly succeeded");
    }
    private static void ioFailure(Checked work, String message) throws Exception {
        try { work.run(); }
        catch (Throwable error) {
            while (error instanceof ExecutionException) error = error.getCause();
            require(error instanceof ParcelableException && error.getCause() instanceof IOException,
                    message + ": " + error);
            return;
        }
        throw new AssertionError(message + ": successful partial result");
    }
    private static Path write(Path file, int size) throws IOException {
        Files.createDirectories(file.getParent());
        Files.write(file, new byte[size]);
        return file;
    }
    private static ApkPackage install(PackageManagerService pm, Path root, String name, int uid)
            throws IOException {
        String apk = write(root.resolve("app/" + name + "/base.apk"), 17).toString();
        ApkPackage p = new ApkPackage(name, apk, uid, root.resolve("user/" + name).toString(),
                root.resolve("user_de/" + name).toString());
        pm.put(p);
        return p;
    }
    private static void basic(Path root) throws Exception {
        PackageManagerService pm = new PackageManagerService();
        ApkPackage p = install(pm, root, "app.one", 10001);
        write(Path.of(p.ce, "files/data"), 2000);
        write(Path.of(p.ce, "cache/data"), 1000);
        write(Path.of(p.de, "files/data"), 500);
        write(Path.of(p.de, "cache/data"), 200);
        write(Path.of(p.de, "code_cache/data"), 300);
        write(root.resolve("media/0/Android/data/app.one/cache/data"), 500);
        write(root.resolve("media/0/Pictures/a.jpg"), 4000);
        write(root.resolve("media/0/Music/a.mp3"), 300);
        StorageStatsService service = new StorageStatsService(pm, root.toFile());
        Binder.setCallingUid(p.uid());
        StorageStats stats = service.queryStatsForPackage(null, p.packageName, 0, "app.one");
        require(stats.codeBytes == 17 && stats.apkBytes == 17, "code and APK bytes");
        require(stats.dataBytes == 4500 && stats.cacheBytes == 2000
                && stats.externalCacheBytes == 500, "CE/DE/external and cache inclusion");
        require(service.queryStatsForUid(null, p.uid(), "app.one").dataBytes == 4500, "own UID");
        int walks = CountingFiles.walks.get();
        fails(SecurityException.class, () -> service.queryStatsForUser(null, 0, "forged"), "user authorization");
        fails(SecurityException.class, () -> service.queryStatsForPackage(null, "missing", 0, "forged"), "missing-name authorization");
        require(CountingFiles.walks.get() == walks, "unauthorized caller performed disk I/O");
        Binder.setCallingUid(1000);
        require(service.queryStatsForUid(null, -1, "system").dataBytes == 0, "negative UID matched all packages");
        require(service.getCacheBytes(null, "system") == 1500, "private volume caches");
        ExternalStorageStats external = service.queryExternalStatsForUser(null, 0, "system");
        require(external.totalBytes == 4800 && external.appBytes == 500
                && external.imageBytes == 4000 && external.audioBytes == 300, "external classification");
        fails(IllegalArgumentException.class, () -> service.queryStatsForUser("invalid", 0, "system"), "volume");
        fails(IllegalArgumentException.class, () -> service.queryStatsForUser(null, 1, "system"), "user");
        fails(ParcelableException.class, () -> service.queryStatsForPackage(null, "missing", 0, "system"), "missing package");

        CountingFiles.reset();
        CountingFiles.gate = new CountDownLatch(1);
        List<Task<StorageStats>> callers = new ArrayList<>();
        for (int i = 0; i < 16; ++i) callers.add(new Task<>(() -> {
            Binder.setCallingUid(p.uid());
            return service.queryStatsForPackage(null, p.packageName, 0, p.packageName);
        }));
        blocked(callers);
        require(CountingFiles.walks.get() == 1, "identical package scans did not join");
        CountingFiles.release();
        List<StorageStats> results = new ArrayList<>();
        for (Task<StorageStats> caller : callers) results.add(caller.get());
        require(results.stream().allMatch(r -> r.dataBytes == 4500), "joined totals");
        require(new HashSet<>(results).size() == 16, "shared mutable results");
        results.get(0).dataBytes = -1;
        require(results.get(1).dataBytes == 4500, "mutable output escaped");
        int previous = CountingFiles.walks.get();
        write(Path.of(p.ce, "files/added"), 123);
        require(service.queryStatsForPackage(null, p.packageName, 0, "system").dataBytes == 4623,
                "completed result cached after external file change");
        require(CountingFiles.walks.get() > previous, "fresh request did not rescan");

        CountingFiles.reset();
        CountingFiles.failAt = 4;
        ioFailure(() -> service.queryStatsForPackage(null, p.packageName, 0, "system"), "injected traversal failure");
        CountingFiles.reset();
        require(service.queryStatsForPackage(null, p.packageName, 0, "system").dataBytes == 4623,
                "failed scan poisoned later calls");

        CountingFiles.gate = new CountDownLatch(1);
        Task<StorageStats> old = new Task<>(() -> service.queryStatsForPackage(null, p.packageName, 0, "system"));
        blocked(List.of(old));
        ApkPackage replacement = new ApkPackage(p.packageName, p.apkPath, p.uid, p.ce, p.de);
        pm.put(replacement);
        Task<StorageStats> newer = new Task<>(() -> service.queryStatsForPackage(null, p.packageName, 0, "system"));
        blocked(List.of(old, newer));
        CountingFiles.release();
        ioFailure(old::get, "replaced package generation");
        require(newer.get().dataBytes == 4623, "new generation joined stale scan");

        ApkPackage observer = install(pm, root, "app.observer", 10002);
        observer.parsed.permissions = List.of("usage");
        CountingFiles.gate = new CountDownLatch(1);
        Task<ExternalStorageStats> revoked = new Task<>(() -> {
            Binder.setCallingUid(observer.uid());
            return service.queryExternalStatsForUser(null, 0, observer.packageName);
        });
        blocked(List.of(revoked));
        observer.parsed.permissions = Collections.emptyList();
        CountingFiles.release();
        fails(SecurityException.class, revoked::get, "authorization revoked while waiting");
    }

    private static void walker(Path root) throws Exception {
        write(root.resolve("a"), 11);
        write(root.resolve("b"), 13);
        require(StorageStatsService.size(root.toFile(), new int[]{3}) == 24, "exact entry budget");
        fails(IOException.class, () -> StorageStatsService.size(root.toFile(), new int[]{2}), "exhausted entry budget");
        require(StorageStatsService.size(root.resolve("missing").toFile(), new int[]{0}) == 0, "absent optional root");
        try {
            Path outside = write(root.resolveSibling("outside/secret"), 1000);
            Files.createSymbolicLink(root.resolve("link"), outside.getParent());
            require(StorageStatsService.size(root.toFile(), new int[]{10}) == 24, "followed symlink");
        } catch (UnsupportedOperationException | FileSystemException unavailable) {
            System.out.println("Symlink fixture unavailable: " + unavailable.getClass().getSimpleName());
        }
        Path deep = root.resolve("deep");
        for (int i = 0; i < 65; ++i) deep = deep.resolve("d");
        write(deep.resolve("file"), 7);
        fails(IOException.class, () -> StorageStatsService.size(root.toFile(), new int[]{1000}), "depth truncation");
        Thread.currentThread().interrupt();
        try { fails(InterruptedIOException.class, () -> StorageStatsService.size(root.toFile(), new int[]{1000}), "walk interruption"); }
        finally { Thread.interrupted(); }
    }

    @SuppressWarnings("unchecked")
    private static int flights(StorageStatsScans scans) throws Exception {
        synchronized (scans) {
            Field field = StorageStatsScans.class.getDeclaredField("mFlights");
            field.setAccessible(true);
            return ((Map<Object, ?>) field.get(scans)).size();
        }
    }

    private static void coordinator() throws Exception {
        StorageStatsScans scans = new StorageStatsScans();
        AtomicInteger active = new AtomicInteger();
        AtomicInteger maximum = new AtomicInteger();
        CountDownLatch release = new CountDownLatch(1);
        List<Task<long[]>> callers = new ArrayList<>();
        for (int i = 0; i < 40; ++i) {
            final int key = i;
            callers.add(new Task<>(() -> scans.execute(key, () -> {
                int count = active.incrementAndGet();
                maximum.accumulateAndGet(count, Math::max);
                try { if (!release.await(15, TimeUnit.SECONDS)) throw new AssertionError("scan gate"); }
                catch (InterruptedException e) { Thread.currentThread().interrupt(); throw new InterruptedIOException(); }
                finally { active.decrementAndGet(); }
                return new long[]{key};
            })));
        }
        blocked(callers);
        require(active.get() == 2 && maximum.get() == 2, "unbounded active scans");
        require(flights(scans) == 16, "unbounded retained flights");
        release.countDown();
        for (int i = 0; i < callers.size(); ++i) require(callers.get(i).get()[0] == i, "distinct result key");
        require(maximum.get() == 2 && flights(scans) == 0, "scan retirement");

        CountDownLatch ownerGate = new CountDownLatch(1);
        Task<long[]> owner = new Task<>(() -> scans.execute("same", () -> {
            try { ownerGate.await(); } catch (InterruptedException e) { throw new InterruptedIOException(); }
            return new long[]{42};
        }));
        blocked(List.of(owner));
        Task<long[]> interrupted = new Task<>(() -> scans.execute("same", () -> { throw new AssertionError("joiner ran"); }));
        Task<long[]> survivor = new Task<>(() -> scans.execute("same", () -> { throw new AssertionError("joiner ran"); }));
        blocked(List.of(owner, interrupted, survivor));
        interrupted.thread.interrupt();
        fails(InterruptedIOException.class, interrupted::get, "joiner cancellation");
        require(flights(scans) == 1, "joiner retired owner");
        ownerGate.countDown();
        require(owner.get()[0] == 42 && survivor.get()[0] == 42 && flights(scans) == 0, "joiner damaged peers");

        CountDownLatch failureGate = new CountDownLatch(1);
        Task<long[]> failed = new Task<>(() -> scans.execute("failure", () -> {
            try { failureGate.await(); } catch (InterruptedException e) { throw new InterruptedIOException(); }
            throw new IOException("owned failure");
        }));
        blocked(List.of(failed));
        Task<long[]> joined = new Task<>(() -> scans.execute("failure", () -> new long[]{-1}));
        blocked(List.of(failed, joined));
        failureGate.countDown();
        fails(IOException.class, failed::get, "owner failure");
        fails(IOException.class, joined::get, "failure fanout");
        require(scans.execute("failure", () -> new long[]{9})[0] == 9 && flights(scans) == 0, "retry after failure");

        CountDownLatch busyGate = new CountDownLatch(1);
        List<Task<long[]>> busy = new ArrayList<>();
        for (int i = 0; i < 2; ++i) {
            final int key = i;
            busy.add(new Task<>(() -> scans.execute(key, () -> {
                try { busyGate.await(); }
                catch (InterruptedException e) { throw new InterruptedIOException(); }
                return new long[]{key};
            })));
        }
        blocked(busy);
        AtomicInteger queuedRuns = new AtomicInteger();
        Task<long[]> queued = new Task<>(() -> scans.execute("queued", () -> {
            queuedRuns.incrementAndGet(); return new long[]{1};
        }));
        blocked(List.of(queued));
        Task<long[]> queuedJoiner = new Task<>(() -> scans.execute("queued", () -> {
            throw new AssertionError("joined queued scan ran twice");
        }));
        blocked(List.of(queuedJoiner));
        queued.thread.interrupt();
        fails(InterruptedIOException.class, queued::get, "queued owner interruption");
        fails(InterruptedIOException.class, queuedJoiner::get, "queued owner releases joined readers");
        require(queuedRuns.get() == 0 && flights(scans) == 2, "cancelled queued work retained admission");
        busyGate.countDown();
        for (Task<long[]> task : busy) task.get();
        require(scans.execute("queued", () -> new long[]{7})[0] == 7 && flights(scans) == 0,
                "queued cancellation prevented subsequent progress");
    }

    private static void benchmark(Path root, boolean reportOnly) throws Exception {
        for (int files : new int[]{1, 100, 10000}) {
            Path data = root.resolve("files-" + files);
            Path shared = data.resolve("media/0");
            for (int i = 0; i < files; ++i) write(shared.resolve("f" + i + ".jpg"), 3);
            StorageStatsService service = new StorageStatsService(new PackageManagerService(), data.toFile());
            CountingFiles.reset();
            CountingFiles.gate = new CountDownLatch(1);
            List<Task<ExternalStorageStats>> callers = new ArrayList<>();
            for (int i = 0; i < 32; ++i) callers.add(new Task<>(() -> service.queryExternalStatsForUser(null, 0, "system")));
            blocked(callers);
            int admitted = CountingFiles.walks.get();
            CountingFiles.release();
            for (Task<ExternalStorageStats> caller : callers) {
                ExternalStorageStats result = caller.get();
                require(result.totalBytes == 3L * files && result.imageBytes == result.totalBytes, "benchmark totals");
            }
            System.out.printf(Locale.ROOT, "BENCH files=%d callers=32 walks=%d visits=%d admitted=%d%n",
                    files, CountingFiles.walks.get(), CountingFiles.entries.get(), admitted);
            if (!reportOnly) require(CountingFiles.walks.get() == 1 && CountingFiles.entries.get() == files + 1,
                    "identical requests repeated the tree walk");
        }
    }

    public static void main(String[] args) throws Exception {
        Path root = Files.createTempDirectory("storage-scans-fixture-");
        try {
            boolean onlyBenchmark = Arrays.asList(args).contains("--benchmark");
            if (!onlyBenchmark) {
                basic(root.resolve("basic"));
                walker(root.resolve("walker"));
                coordinator();
            }
            benchmark(root.resolve("bench"), Arrays.asList(args).contains("--report-only"));
            System.out.println("StorageStatsScansHost: " + checks + " checks passed (real host trees, Android boundary stubs)");
        } finally {
            CountingFiles.release();
            try (java.util.stream.Stream<Path> paths = Files.walk(root)) {
                for (Path path : (Iterable<Path>) paths.sorted(Comparator.reverseOrder())::iterator) Files.deleteIfExists(path);
            }
        }
    }
}
