/* Regression from independent START-03 QA: writes during real scheduler attempts. */
package com.gesher.server;

import java.io.File;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.Map;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicLong;

public final class DexoptProfileProgressHost {
    static Object field(Object value, String name) throws Exception {
        var field = value.getClass().getDeclaredField(name);
        field.setAccessible(true); return field.get(value);
    }
    static Object dexopt(String name) throws Exception {
        var field = Dexopt.class.getDeclaredField(name);
        field.setAccessible(true); return field.get(null);
    }
    public static void main(String[] args) throws Exception {
        Device.root = new File(args[0]); Device.root.mkdirs();
        Path code = DexoptWorkHost.code("progress");
        Path profile = Dexopt.currentProfile("progress").toPath();
        Path reference = Dexopt.referenceProfile("progress").toPath();
        Path artifact = code.resolve("oat/x86_64/base.odex");
        var queue = (DexoptWorkQueue) dexopt("sWork");
        AtomicLong idle = new AtomicLong(android.os.SystemClock.uptimeMillis());
        AtomicInteger writes = new AtomicInteger(), scans = new AtomicInteger();
        Runnable[] saver = new Runnable[1];
        saver[0] = () -> {
            try {
                int write = writes.incrementAndGet();
                if (write > 1) {
                    String previous = write == 2 ? "new-profile" : "saver-" + (write - 2);
                    DexoptWorkHost.check(Files.readString(artifact).equals("compiled:" + previous)
                                    && Files.readString(reference).equals(previous),
                            "previous captured profile published despite later saver write");
                }
                var work = (DexoptWorkQueue.Work) field(dexopt("sActive"), "work");
                long epoch = queue.epoch();
                Files.writeString(profile, "saver-" + write);
                long deadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(3);
                while (queue.epoch() == epoch && System.nanoTime() < deadline) Thread.sleep(2);
                DexoptWorkHost.check(queue.epoch() != epoch, "real saver write notification observed in flight");
                DexoptWorkHost.check(queue.current(work), "saver write keeps captured installed generation valid");
                if (write < 3) Zygote.duringCompiler = saver[0];
            } catch (Exception failure) { throw new AssertionError(failure); }
        };
        Zygote.duringCompiler = saver[0];
        Dexopt.startBackground(() -> { scans.incrementAndGet(); return Map.of("progress", code.toString()); }, idle::get);
        DexoptWorkHost.until(() -> {
            try { return Files.readString(reference).equals("saver-3") && queue.size() == 0; }
            catch (Exception missing) { return false; }
        }, "continuous saver writes eventually publish every captured profile and the latest data");
        DexoptWorkHost.check(writes.get() == 3 && Zygote.compiles.get() == 4,
                "three concurrent writes require four successful compiles without failure backoff");
        DexoptWorkHost.check(Files.readString(profile).equals("saver-3"), "latest live current profile retained");
        int tools = Zygote.calls.get(), snapshots = scans.get();
        // All real compiler staging, reference commits and logging are outside cur/0.
        Files.writeString(reference.getParent().resolve("current-extra.prof"), "temporary");
        Files.writeString(code.resolve("oat/x86_64/extra-output"), "compiler output");
        Files.writeString(Device.logsDir().toPath().resolve("extra-dexopt.log"), "tool log");
        Thread.sleep(1200);
        DexoptWorkHost.check(queue.size() == 0 && Zygote.calls.get() == tools && scans.get() == snapshots,
                "reference, staging, artifact and log output cannot feed the current-profile watch");
        idle.set(0); Dexopt.activityChanged();
        System.out.println("Dexopt profile progress contracts passed: " + DexoptWorkHost.checks);
    }
}
