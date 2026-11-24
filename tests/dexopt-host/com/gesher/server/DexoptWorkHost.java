/* Production scheduler/file transactions + real host filesystem notifications; Windows edges in Platform.java. */
package com.gesher.server;

import java.io.File;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.attribute.FileTime;
import java.util.LinkedHashMap;
import java.util.Map;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicLong;
import java.util.function.BooleanSupplier;

public final class DexoptWorkHost {
    static int checks;
    static void check(boolean value, String name) {
        if (!value) throw new AssertionError(name);
        checks++;
    }
    static void until(BooleanSupplier condition, String name) throws Exception {
        long deadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(8);
        while (!condition.getAsBoolean() && System.nanoTime() < deadline) Thread.sleep(5);
        check(condition.getAsBoolean(), name);
    }
    static Path code(String name) throws Exception {
        Path code = Device.root.toPath().resolve("app").resolve(name);
        Files.createDirectories(code.resolve("oat/x86_64"));
        Files.writeString(code.resolve("base.apk"), name);
        Files.writeString(code.resolve("oat/x86_64/base.odex"), "old-code");
        Dexopt.prepareProfiles(name);
        Files.writeString(Dexopt.currentProfile(name).toPath(), "new-profile");
        Files.writeString(Dexopt.referenceProfile(name).toPath(), "old-profile");
        return code;
    }
    static void queue() throws Exception {
        DexoptWorkQueue queue = new DexoptWorkQueue();
        Map<String, String> paths = new LinkedHashMap<>();
        for (int i = 0; i < 500; i++) paths.put("pkg" + i, "/code/" + i);
        queue.reconcile(paths);
        for (int i = 0; i < 100_000; i++) queue.changed("pkg1");
        check(queue.size() == 500, "coalesced queue bounded by installed package count");
        DexoptWorkQueue.Work first = queue.poll(100);
        queue.finished(first, false, 100);
        check(queue.poll(100).packageName.equals("pkg1"), "failed package does not starve others");
        DexoptWorkQueue.Work old = queue.poll(100);
        queue.changed("pkg1"); queue.finished(old, true, 100);
        check(queue.current(old) && queue.size() == 500,
                "profile writes preserve valid installed work and remain pending after success");
        check(queue.poll(100).packageName.equals("pkg2"), "continuous profile writes do not starve other packages");
        queue.reconcile(Map.of("pkg1", "/replacement"));
        check(!queue.current(old) && queue.size() == 1, "package replacement invalidates captured work");
        DexoptWorkQueue.Work retry = queue.poll(100);
        queue.finished(retry, false, 100);
        check(queue.poll(1099) == null && queue.poll(1100) != null, "retry deadline without new profile writes");
        queue.finished(queue.poll(1100), true, 1100);
        check(queue.size() == 0 && queue.delay(1100) == 0, "clean queue has no deadline");
        long epoch = queue.epoch(); queue.changed("pkg1");
        long start = System.nanoTime(); queue.await(epoch, 5000);
        check(System.nanoTime() - start < TimeUnit.MILLISECONDS.toNanos(100), "no lost notification before wait");
        long snapshotEpoch = queue.registryEpoch();
        queue.removed("pkg1");
        check(!queue.reconcile(Map.of("pkg1", "/replacement"), snapshotEpoch) && queue.size() == 0,
                "pre-install snapshot cannot resurrect withdrawn generation");
        queue.reconcile(Map.of("pkg1", "/replacement"));
        check(!queue.current(retry), "reinstall at the same code path has a new installed identity");
        queue.finished(retry, true, 1100);
        check(queue.size() == 1, "old success cannot acknowledge reinstalled package work");
        queue.finished(retry, false, 1100);
        check(queue.poll(1100) != null, "old failure cannot back off a reinstalled package");
    }
    static void transactions() throws Exception {
        Path path = code("transaction");
        Zygote.failCompiler = true;
        check(Dexopt.backgroundDexopt("transaction", path.toString()) == null, "failed compiler not successful");
        check(Files.readString(Dexopt.referenceProfile("transaction").toPath()).equals("old-profile"),
                "failed compiler preserved original reference profile");
        check(Files.readString(path.resolve("oat/x86_64/base.odex")).equals("old-code"), "failed compiler preserved artifacts");
        check(Files.readString(Dexopt.currentProfile("transaction").toPath()).equals("new-profile"), "failed compiler retained current profile");
        Zygote.duringCompiler = () -> {
            try { Files.writeString(Dexopt.currentProfile("transaction").toPath(), "newer-profile"); }
            catch (Exception failure) { throw new AssertionError(failure); }
        };
        check(Dexopt.backgroundDexopt("transaction", path.toString()) != null, "unchanged failed input retried successfully");
        check(Files.readString(Dexopt.currentProfile("transaction").toPath()).equals("newer-profile"), "writes during compile retained");
        check(Files.readString(Dexopt.referenceProfile("transaction").toPath()).equals("new-profile"), "reference corresponds to committed code");
        check(Dexopt.backgroundDexopt("transaction", path.toString()) != null, "later profile generation compiled");
        int count = Zygote.compiles.get();
        check(Dexopt.backgroundDexopt("transaction", path.toString()) == null && Zygote.compiles.get() == count,
                "no new data does not compile again");
        try (var files = Files.list(Dexopt.referenceProfile("transaction").toPath().getParent())) {
            check(files.count() == 1, "temporary profile copies removed");
        }
    }
    static void watchers() throws Exception {
        DexoptWorkQueue queue = new DexoptWorkQueue();
        Path root = Device.root.toPath().resolve("watch-recovery");
        Files.writeString(root, "not-a-directory");
        AtomicInteger failures = new AtomicInteger(), snapshots = new AtomicInteger();
        DexoptProfileWatch watcher = new DexoptProfileWatch(root,
                () -> { snapshots.incrementAndGet(); return Map.of("watched", "/code/watched"); },
                queue, message -> failures.incrementAndGet());
        watcher.start();
        try {
            until(() -> failures.get() > 0, "watch registration failure reported");
            check(snapshots.get() == 0, "unwatched startup did not acknowledge an unsafe snapshot");
            Files.delete(root); watcher.refresh();
            until(() -> queue.size() == 1, "watch registration failure recovers on notification");
            var initial = queue.poll(Long.MAX_VALUE);
            queue.finished(initial, true, 0);
            Path profile = root.resolve("watched/primary.prof");
            Files.createDirectories(profile.getParent());
            Files.writeString(profile, "profile");
            until(() -> queue.size() == 1, "real current-profile creation delivered");
            var observed = queue.poll(Long.MAX_VALUE);
            queue.finished(observed, true, 0);
            Files.delete(profile); Files.delete(profile.getParent());
            until(() -> queue.size() == 1, "package-directory deletion causes reconciliation");
            var recovered = queue.poll(Long.MAX_VALUE);
            check(recovered.generation == observed.generation && recovered.revision > observed.revision,
                    "watch rebuild republishes dirty profile without invalidating installed code");
            Files.createDirectory(profile.getParent());
            Files.writeString(profile, "recreated");
            until(() -> queue.poll(Long.MAX_VALUE).revision > recovered.revision,
                    "recursive notifications cover recreated package directory");
            Thread.sleep(150);
            int stable = snapshots.get();
            Thread.sleep(150);
            check(snapshots.get() == stable, "successful watcher recovery leaves no recovery scan timer");
        } finally { watcher.stop(); }
    }
    static void scheduler() throws Exception {
        Path path = code("scheduled");
        AtomicInteger scans = new AtomicInteger();
        AtomicLong idle = new AtomicLong(0);
        Map<String, String> packages = new java.util.concurrent.ConcurrentHashMap<>();
        packages.put("scheduled", path.toString());
        Dexopt.startBackground(() -> { scans.incrementAndGet(); return Map.copyOf(packages); }, idle::get);
        until(() -> scans.get() > 0, "initial watcher snapshot");
        Zygote.failCompiler = true;
        int before = Zygote.compiles.get();
        idle.set(android.os.SystemClock.uptimeMillis());
        invoke("activityChanged");
        until(() -> Zygote.compiles.get() >= before + 2 && Zygote.active.get() == 0,
                "scheduler retries unchanged profile after failure");
        Thread.sleep(150);
        int stableScans = scans.get(), stableTools = Zygote.calls.get();
        Thread.sleep(1200);
        check(scans.get() == stableScans && Zygote.calls.get() == stableTools, "clean idle has no package scans or tools");
        Path profile = Dexopt.currentProfile("scheduled").toPath();
        FileTime originalTime = Files.getLastModifiedTime(profile);
        Files.writeString(profile, "NEW-profile");
        Files.setLastModifiedTime(profile, originalTime);
        until(() -> Zygote.compiles.get() > before + 2, "same-size same-timestamp profile write notified");
        until(() -> Zygote.active.get() == 0, "profile update finished");

        Zygote.entered = new CountDownLatch(1); Zygote.release = new CountDownLatch(1);
        Zygote.blockCompiler = true;
        String oldCode = Files.readString(path.resolve("oat/x86_64/base.odex"));
        String oldRef = Files.readString(Dexopt.referenceProfile("scheduled").toPath());
        Files.writeString(profile, "cancel-me");
        check(Zygote.entered.await(8, TimeUnit.SECONDS), "compiler in flight before foreground");
        idle.set(0); invoke("activityChanged");
        until(() -> Zygote.active.get() == 0, "foreground canceled and reaped compiler");
        check(Files.readString(path.resolve("oat/x86_64/base.odex")).equals(oldCode), "cancellation retained previous artifacts");
        check(Files.readString(Dexopt.referenceProfile("scheduled").toPath()).equals(oldRef), "cancellation retained reference");
        check(Files.readString(profile).equals("cancel-me"), "cancellation retained current input");
        Zygote.blockCompiler = false;
        idle.set(android.os.SystemClock.uptimeMillis()); invoke("activityChanged");
        until(() -> {
            try { return Files.readString(Dexopt.referenceProfile("scheduled").toPath()).equals("cancel-me"); }
            catch (Exception ignored) { return false; }
        }, "canceled work eventually updates AOT");
        check(Zygote.maxActive == 1, "only one compiler process at a time");

        Zygote.lowMemory = true;
        int atPressure = Zygote.calls.get();
        Files.writeString(profile, "after-pressure");
        Thread.sleep(200);
        check(Zygote.calls.get() == atPressure, "low physical memory defers process creation");
        Zygote.lowMemory = false; invoke("activityChanged");
        until(() -> {
            try { return Files.readString(Dexopt.referenceProfile("scheduled").toPath()).equals("after-pressure"); }
            catch (Exception ignored) { return false; }
        }, "memory-deferred unchanged work eventually retries");

        idle.set(0); invoke("activityChanged");
        packages.remove("scheduled"); invoke("packageChanging", "scheduled"); invoke("packagesChanged");
        int atRemoval = Zygote.compiles.get();
        Thread.sleep(150);
        Files.writeString(profile, "uninstalled");
        idle.set(android.os.SystemClock.uptimeMillis()); invoke("activityChanged");
        Thread.sleep(150);
        check(Zygote.compiles.get() == atRemoval, "removed package notification cannot enqueue stale compile");
    }
    static void invoke(String method, Object... args) throws Exception {
        Class<?>[] types = java.util.Arrays.stream(args).map(Object::getClass).toArray(Class<?>[]::new);
        var target = Dexopt.class.getDeclaredMethod(method, types); target.setAccessible(true); target.invoke(null, args);
    }
    public static void main(String[] args) throws Exception {
        Device.root = new File(args[0]); Device.root.mkdirs();
        transactions();
        if (!args[1].equals("negative")) { queue(); watchers(); scheduler(); }
        System.out.println("Dexopt production contracts passed: " + checks);
    }
}
