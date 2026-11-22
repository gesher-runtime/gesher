/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package com.gesher.server;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.FileDescriptor;
import java.io.IOException;
import java.io.RandomAccessFile;
import java.nio.ByteBuffer;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.CopyOption;
import java.util.Arrays;
import java.util.Comparator;
import java.util.LinkedHashMap;
import java.util.Map;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.atomic.AtomicReference;

/** Production serialization and real filesystem operations; stream adapters count boundary calls. */
public final class SettingsStoreHost {
    private static int checks;
    private static final String[] TABLES = {"system", "secure", "global"};
    private static void check(boolean ok, String reason) {
        if (!ok) throw new AssertionError(reason);
        checks++;
    }
    private static Map<String, Map<String, String>> empty() {
        Map<String, Map<String, String>> tables = new LinkedHashMap<>();
        for (String table : TABLES) tables.put(table, new LinkedHashMap<>());
        return tables;
    }
    private static Map<String, Map<String, String>> defaults() {
        Map<String, Map<String, String>> result = empty();
        result.get("system").put("seed", "original");
        return result;
    }
    private static void string(ByteBuffer out, String value) {
        out.putInt(value.length());
        out.asCharBuffer().put(value);
        out.position(out.position() + value.length() * 2);
    }
    private static void row(ByteBuffer out, String name, long id, String value) {
        string(out, name); out.putLong(id); out.put((byte) (value == null ? 0 : 1));
        if (value != null) string(out, value);
    }
    private static byte[] expected() {
        ByteBuffer out = ByteBuffer.allocate(1024);
        out.putInt(0x47535431); out.putLong(4);
        out.putInt(2); row(out, "seed", 0, "original");
        row(out, "value", 1, "\u0000A\u05d0\ud800\udfff");
        out.putInt(1); row(out, "empty", 2, "");
        out.putInt(1); row(out, "nullable", 3, null);
        return Arrays.copyOf(out.array(), out.position());
    }
    private static void format(Path root) throws Exception {
        File file = root.resolve("format.bin").toFile();
        Map<String, Map<String, String>> tables = defaults();
        SettingsStore store = new SettingsStore(file, tables);
        check(store.put("system", "value", "\u0000A\u05d0\ud800\udfff", false), "Persist binary UTF-16 units");
        check(store.put("secure", "empty", "", false), "Persist empty string");
        check(store.put("global", "nullable", null, false), "Persist null row");
        check(ByteBuffer.wrap(Files.readAllBytes(file.toPath())).getInt() == 0x47535432,
                "New stores publish versioned GST2 checkpoints");
        Map<String, Map<String, String>> loaded = empty();
        SettingsStore restored = new SettingsStore(file, loaded);
        check(loaded.equals(tables), "Reload legacy bytes without changing values");
        check(restored.id("system", "seed") == 0 && restored.id("system", "value") == 1
                && restored.id("secure", "empty") == 2 && restored.id("global", "nullable") == 3,
                "Row IDs survive reload");
        check(!restored.put("system", "value", "\u0000A\u05d0\ud800\udfff", false), "Identical value is not rewritten");
        check(restored.put("system", "value", "changed", true) && restored.id("system", "value") == 4,
                "Generation resumes after reload");
        check(!restored.put("system", "missing", "value", true), "Existing-only mutation remains conditional");

        File legacy = root.resolve("legacy.bin").toFile();
        Files.write(legacy.toPath(), expected());
        loaded = empty();
        restored = new SettingsStore(legacy, loaded);
        check(restored.id("global", "nullable") == 3 && loaded.get("system").get("value")
                .equals("\u0000A\u05d0\ud800\udfff"), "Exact legacy GST1 fixture remains readable");
        check(Arrays.equals(expected(), Files.readAllBytes(legacy.toPath())), "Loading legacy state does not rewrite it");
        restored.put("secure", "empty", "migrated", true);
        check(ByteBuffer.wrap(Files.readAllBytes(legacy.toPath())).getInt() == 0x47535432,
                "First actual legacy change migrates atomically");
        Map<String, Map<String, String>> migrated = empty();
        SettingsStore reread = new SettingsStore(legacy, migrated);
        check(migrated.equals(loaded) && reread.id("secure", "empty") == 4, "Migration preserves rows and generations");
    }
    private static void boundaries(Path root) throws Exception {
        File file = root.resolve("boundaries.bin").toFile();
        Map<String, Map<String, String>> tables = empty();
        SettingsStore store = new SettingsStore(file, tables);
        int[] lengths = {4095, 4096, 4097, 32768};
        for (int length : lengths) {
            char[] value = new char[length];
            for (int i = 0; i < length; i++) value[i] = (char) ((i * 79) & 0xffff);
            check(store.put("secure", "length-" + length, new String(value), false), "Write across stream buffer boundary");
        }
        check(store.put("system", "text-null", "null", false), "Literal null normalization");
        check(store.put("global", "k".repeat(32768), "v".repeat(32768), false),
                "Maximum key and value fit in one bounded journal record");
        check(tables.get("system").containsKey("text-null") && tables.get("system").get("text-null") == null,
                "Literal null remains nullable state");
        Map<String, Map<String, String>> loaded = empty();
        new SettingsStore(file, loaded);
        check(loaded.equals(tables), "Large binary values and unpaired surrogates round-trip exactly");
        byte[] before = Files.readAllBytes(file.toPath());
        check(!store.put("system", "bad\u0000key", "value", false), "Invalid key remains rejected");
        try { store.put("system", "too-long", "x".repeat(32769), false); throw new AssertionError("Missing value limit"); }
        catch (IllegalArgumentException expected) { checks++; }
        check(Arrays.equals(before, Files.readAllBytes(file.toPath())), "Rejected changes do not touch persisted state");
    }
    private static void invalidLoad(Path root) throws Exception {
        byte[] valid = expected();
        Path path = root.resolve("invalid.bin");
        for (int size = 0; size < valid.length; size++) {
            Files.write(path, Arrays.copyOf(valid, size));
            Map<String, Map<String, String>> tables = defaults();
            SettingsStore store = new SettingsStore(path.toFile(), tables);
            check(tables.equals(defaults()) && store.id("system", "seed") == 0,
                    "A truncated file never publishes partially decoded tables");
        }
        byte[][] bad = {valid.clone(), Arrays.copyOf(valid, valid.length + 1), valid.clone(), valid.clone(), valid.clone()};
        bad[0][0] = 0;
        Arrays.fill(bad[2], 4, 12, (byte) 0xff); // negative next ID
        Arrays.fill(bad[3], 12, 16, (byte) 0xff); // negative row count
        Arrays.fill(bad[4], 16, 20, (byte) 0xff); // negative name length
        for (byte[] bytes : bad) {
            Files.write(path, bytes);
            Map<String, Map<String, String>> tables = defaults();
            new SettingsStore(path.toFile(), tables);
            check(tables.equals(defaults()), "Malformed file leaves defaults intact");
        }
    }
    private static void publicationFailure(Path root) throws Exception {
        Path target = Files.createDirectory(root.resolve("publication-target"));
        Files.write(target.resolve("keep"), new byte[] {1});
        Map<String, Map<String, String>> tables = defaults();
        SettingsStore store = new SettingsStore(target.toFile(), tables);
        for (int operation = 0; operation < 3; operation++) {
            try {
                if (operation == 0) store.put("system", "seed", "changed", false);
                if (operation == 1) store.put("system", "fresh", "value", false);
                if (operation == 2) store.remove("system", "seed");
                throw new AssertionError("Expected publication failure");
            } catch (IllegalStateException expected) { checks++; }
            check(tables.equals(defaults()) && store.id("system", "seed") == 0, "Failed publication rolls back state and IDs");
            try (java.util.stream.Stream<Path> files = Files.list(root)) {
                check(files.noneMatch(p -> p.getFileName().toString().startsWith("settings-")), "Failed publication removes temporary file");
            }
        }
        Files.delete(target.resolve("keep")); Files.delete(target);
        check(store.put("system", "fresh", "value", false) && store.id("system", "fresh") == 1,
                "Failed writes do not consume generation IDs");
        Map<String, Map<String, String>> loaded = empty();
        new SettingsStore(target.toFile(), loaded);
        check(loaded.equals(tables), "Persistence recovers after publication failure");
    }
    private static void io(Path root) throws Exception {
        File file = root.resolve("io.bin").toFile();
        Map<String, Map<String, String>> tables = empty();
        tables.get("system").put("wide", "a\u05d0\ud800\u0000".repeat(2048));
        SettingsStore store = new SettingsStore(file, tables);
        SettingsIO.reset();
        store.put("secure", "trigger", "value", false);
        int writes = SettingsIO.writes;
        long written = SettingsIO.written;
        SettingsIO.reset();
        Map<String, Map<String, String>> loaded = empty();
        new SettingsStore(file, loaded);
        int reads = SettingsIO.reads;
        check(loaded.equals(tables), "Measured real I/O preserves full data");
        check(written == file.length(), "Measured output bytes match the complete persisted file");
        System.out.println("SettingsStore I/O: bytes=" + written + " writes=" + writes + " reads=" + reads);
        check(writes <= 4, "Settings snapshot writes are buffered, not one operation per UTF-16 unit");
        check(reads <= 5, "Settings snapshot reads are buffered, not one operation per byte");
    }

    private static void journal(Path root) throws Exception {
        File file = root.resolve("journal.bin").toFile();
        Map<String, Map<String, String>> tables = defaults();
        SettingsStore store = new SettingsStore(file, tables);
        store.put("secure", "first", "value", false);
        long checkpoint = file.length();
        SettingsIO.reset();
        check(store.put("secure", "first", "changed", true), "Update appends a row mutation");
        check(file.length() > checkpoint && SettingsIO.replaces == 0 && SettingsIO.syncs == 1,
                "Normal update appends and syncs without replacing the snapshot");
        check(store.remove("system", "seed"), "Deletion appends a tombstone");
        store.put("global", "nullable", null, false);
        long id = store.id("global", "nullable");
        check(store.put("global", "nullable", null, true, true)
                && store.id("global", "nullable") == id + 1, "Same-value forced generation survives journaling");
        long size = file.length();
        check(!store.put("global", "nullable", null, false) && !store.remove("system", "absent")
                && file.length() == size, "No-op changes write nothing");
        Map<String, Map<String, String>> loaded = empty();
        SettingsStore restored = new SettingsStore(file, loaded);
        check(loaded.equals(tables) && restored.id("global", "nullable") == id + 1,
                "Replay preserves removes, nulls and forced generations");
        store.addDefault("secure", "new-default", "introduced");
        store.put("global", "nullable", "changed", false);
        loaded = empty(); new SettingsStore(file, loaded);
        check(loaded.equals(tables), "New defaults force a checkpoint before a later row mutation");

        SettingsIO.reset();
        for (int i = 0; i < 400; i++) {
            store.put("system", "counter", "v" + i, false);
            if (i % 17 == 0) {
                Map<String, Map<String, String>> read = empty();
                store = new SettingsStore(file, read); tables = read;
                check(read.get("system").get("counter").equals("v" + i), "Reload across checkpoint boundaries");
            }
        }
        check(SettingsIO.replaces == 3, "Record-count checkpoints remain bounded across reloads");
        check(file.length() < 9000, "Repeated row updates cannot grow the log indefinitely");
        SettingsIO.reset();
        for (int i = 0; i < 40; i++) {
            store.put("system", "wide", ("v" + i + "-").repeat(12000).substring(0, 32768), false);
        }
        check(SettingsIO.replaces >= 2, "Byte-budget checkpoint applies before 128 large records");
        check(file.length() < 1200000, "Large-value journal is bounded by bytes");
        loaded = empty(); new SettingsStore(file, loaded);
        check(loaded.equals(tables), "Large-value checkpoints and suffix replay match live state");
    }

    private static void tornTail(Path root) throws Exception {
        Path file = root.resolve("torn.bin");
        Map<String, Map<String, String>> tables = defaults();
        SettingsStore store = new SettingsStore(file.toFile(), tables);
        store.put("global", "committed", "first", false);
        store.put("global", "committed", "second", true);
        byte[] before = Files.readAllBytes(file);
        store.put("global", "pending", "last-\u0000-\ud800", false);
        byte[] complete = Files.readAllBytes(file);
        for (int size = before.length; size < complete.length; size++) {
            Files.write(file, Arrays.copyOf(complete, size));
            Map<String, Map<String, String>> loaded = empty();
            SettingsStore recovered = new SettingsStore(file.toFile(), loaded);
            check("second".equals(loaded.get("global").get("committed"))
                    && !loaded.get("global").containsKey("pending"), "Every torn append preserves acknowledged rows");
            recovered.put("secure", "after", "recovered", false);
            Map<String, Map<String, String>> after = empty(); new SettingsStore(file.toFile(), after);
            check(after.equals(loaded), "Torn suffix is truncated before the next durable append");
        }
        for (int at = before.length; at < complete.length; at++) {
            byte[] corrupt = complete.clone(); corrupt[at] ^= 1;
            Files.write(file, corrupt);
            try { new SettingsStore(file.toFile(), empty()); throw new AssertionError("Accepted corrupt complete record"); }
            catch (IllegalStateException expected) { checks++; }
            check(Arrays.equals(corrupt, Files.readAllBytes(file)),
                    "Corrupt header, payload or checksum fails without modifying stored state");
        }
        // Independent review regression: an enlarged but still in-range length
        // must not turn a fully acknowledged record into a recoverable torn tail.
        byte[] enlarged = complete.clone();
        ByteBuffer header = ByteBuffer.wrap(enlarged);
        header.putInt(before.length, header.getInt(before.length) ^ 256);
        Files.write(file, enlarged);
        try { new SettingsStore(file.toFile(), empty()); throw new AssertionError("Accepted corrupted framing as a torn tail"); }
        catch (IllegalStateException expected) { checks++; }
        check(Arrays.equals(enlarged, Files.readAllBytes(file)), "Invalid framing cannot trigger truncation or publication");
        Files.write(file, complete);
        Map<String, Map<String, String>> restored = empty();
        SettingsStore valid = new SettingsStore(file.toFile(), restored);
        check("last-\u0000-\ud800".equals(restored.get("global").get("pending")),
                "Original acknowledged row is intact after failed corruption checks");
        valid.put("secure", "after-corruption-check", "safe", false);
        Map<String, Map<String, String>> verified = empty(); new SettingsStore(file.toFile(), verified);
        check(verified.equals(restored), "Valid framing permits a later mutation without losing the acknowledged row");
        Files.write(file, before);
        byte[] corruptSnapshot = before.clone(); corruptSnapshot[20] ^= 1;
        Files.write(file, corruptSnapshot);
        try { new SettingsStore(file.toFile(), empty()); throw new AssertionError("Accepted corrupt checkpoint"); }
        catch (IllegalStateException expected) { checks++; }
    }

    private static void appendFailures(Path root) throws Exception {
        File file = root.resolve("failures.bin").toFile();
        Map<String, Map<String, String>> tables = defaults();
        SettingsStore store = new SettingsStore(file, tables);
        store.put("global", "seed", "committed", false);
        byte[] before = Files.readAllBytes(file.toPath());
        long id = store.id("global", "seed");
        for (int operation = 0; operation < 3; operation++) {
            SettingsIO.reset(); SettingsIO.failWriteAfter = 11;
            try {
                if (operation == 0) store.put("global", "seed", "failed", true);
                if (operation == 1) store.put("global", "fresh", "failed", false);
                if (operation == 2) store.remove("global", "seed");
                throw new AssertionError("Expected partial append failure");
            } catch (IllegalStateException expected) { checks++; }
            check("committed".equals(tables.get("global").get("seed"))
                    && !tables.get("global").containsKey("fresh") && store.id("global", "seed") == id,
                    "Failed append rolls back rows and IDs");
            check(Arrays.equals(before, Files.readAllBytes(file.toPath())), "Failed append restores durable file exactly");
        }
        SettingsIO.reset(); SettingsIO.failSyncAt = 1;
        try { store.put("global", "seed", "unsynced", true); throw new AssertionError("Expected sync failure"); }
        catch (IllegalStateException expected) { checks++; }
        check(Arrays.equals(before, Files.readAllBytes(file.toPath())) && SettingsIO.syncs == 2,
                "Sync failure truncates and syncs the rollback before rejecting the change");
        SettingsIO.reset(); SettingsIO.failClose = true;
        check(store.put("global", "seed", "durable", true), "Close error after sync cannot report a committed change as failed");
        Map<String, Map<String, String>> loaded = empty(); new SettingsStore(file, loaded);
        check(loaded.equals(tables), "Close failure leaves memory and durable state consistent");

        SettingsIO.reset(); SettingsIO.failWriteAfter = 9; SettingsIO.failTruncate = true;
        try { store.put("global", "seed", "uncertain", true); throw new AssertionError("Expected rollback failure"); }
        catch (IllegalStateException expected) { checks++; }
        SettingsIO.reset();
        try { store.put("global", "seed", "must-not-ack", true); throw new AssertionError("Expected poisoned writer"); }
        catch (IllegalStateException expected) { checks++; }
        check(SettingsIO.writes == 0, "Uncertain rollback refuses further writes before storage is reopened");
        loaded = empty(); store = new SettingsStore(file, loaded);
        check("durable".equals(loaded.get("global").get("seed")), "Restart replays acknowledged prefix after partial failed rollback");
        store.put("global", "seed", "recovered", true);
        tables = empty(); new SettingsStore(file, tables);
        check(tables.equals(loaded), "Restart repairs incomplete suffix and resumes persistence");
    }

    private static void checkpointFailure(Path root) throws Exception {
        File file = root.resolve("checkpoint-failure.bin").toFile();
        Map<String, Map<String, String>> tables = defaults();
        SettingsStore store = new SettingsStore(file, tables);
        store.put("secure", "value", "checkpoint", false);
        for (int i = 0; i < 128; i++) store.put("secure", "value", "v" + i, true);
        byte[] before = Files.readAllBytes(file.toPath());
        long id = store.id("secure", "value");
        SettingsIO.reset(); SettingsIO.failReplace = true;
        try { store.put("secure", "value", "failed-checkpoint", true); throw new AssertionError("Expected replace failure"); }
        catch (IllegalStateException expected) { checks++; }
        check(Arrays.equals(before, Files.readAllBytes(file.toPath())) && store.id("secure", "value") == id,
                "Failed checkpoint keeps previous snapshot+journal and generation");
        Map<String, Map<String, String>> loaded = empty(); new SettingsStore(file, loaded);
        check(loaded.equals(tables), "All previously acknowledged records survive failed checkpoint");
        SettingsIO.reset(); SettingsIO.failSyncAt = 1;
        try { store.put("secure", "value", "unsynced-checkpoint", true); throw new AssertionError("Expected checkpoint sync failure"); }
        catch (IllegalStateException expected) { checks++; }
        check(Arrays.equals(before, Files.readAllBytes(file.toPath())) && store.id("secure", "value") == id,
                "Checkpoint sync failure does not publish unsynced replacement or consume an ID");
        SettingsIO.reset(); store.put("secure", "value", "retried", true);
        check(SettingsIO.replaces == 1 && store.id("secure", "value") == id + 1,
                "Checkpoint retries without consuming an extra generation");
    }

    private static void concurrent(Path root) throws Exception {
        File file = root.resolve("concurrent.bin").toFile();
        Map<String, Map<String, String>> tables = defaults();
        SettingsStore store = new SettingsStore(file, tables);
        Object providerLock = new Object();
        CountDownLatch start = new CountDownLatch(1);
        AtomicReference<Throwable> failure = new AtomicReference<>();
        Thread[] threads = new Thread[8];
        for (int t = 0; t < threads.length; t++) {
            final int index = t;
            threads[t] = new Thread(() -> {
                try {
                    start.await();
                    for (int n = 0; n < 24; n++) {
                        synchronized (providerLock) {
                            store.put("system", "thread-" + index, "v" + n, false);
                        }
                    }
                } catch (Throwable error) { failure.compareAndSet(null, error); }
            });
            threads[t].start();
        }
        start.countDown();
        for (Thread thread : threads) thread.join(10000);
        check(failure.get() == null && Arrays.stream(threads).noneMatch(Thread::isAlive),
                "Concurrent clients complete under the provider's existing table lock");
        Map<String, Map<String, String>> loaded = empty(); new SettingsStore(file, loaded);
        check(loaded.equals(tables) && loaded.get("system").size() == 9,
                "All serialized concurrent writes survive checkpoint and replay");
    }

    private static void crashWriter(String[] args) {
        File file = new File(args[1]);
        SettingsStore store = new SettingsStore(file, empty());
        switch (args[2]) {
            case "append": SettingsIO.haltAppend = true; break;
            case "checkpoint":
                store.addDefault("secure", "new-default-at-crash", "uncommitted");
                SettingsIO.haltCheckpoint = true;
                break;
            default: break;
        }
        store.put("global", "crash-writer", "acknowledged", false);
        Runtime.getRuntime().halt(73); // No shutdown hooks, finally blocks or normal close.
    }

    private static void crash(Path file, String mode, int exit) throws Exception {
        String executable = new File(System.getProperty("java.home"), "bin/java"
                + (System.getProperty("os.name").startsWith("Windows") ? ".exe" : "")).getPath();
        Process process = new ProcessBuilder(executable, "-XX:-UsePerfData", "-cp",
                System.getProperty("java.class.path"), SettingsStoreHost.class.getName(),
                "--crash-writer", file.toString(), mode).redirectErrorStream(true).start();
        if (!process.waitFor(10, java.util.concurrent.TimeUnit.SECONDS)) {
            process.destroyForcibly(); throw new AssertionError("Crash writer did not exit");
        }
        check(process.exitValue() == exit, "Production writer halted at " + mode + ": "
                + new String(process.getInputStream().readAllBytes(), java.nio.charset.StandardCharsets.UTF_8));
    }

    private static void processCrashes(Path root) throws Exception {
        Path file = root.resolve("process-crashes.bin");
        Map<String, Map<String, String>> tables = defaults();
        SettingsStore store = new SettingsStore(file.toFile(), tables);
        store.put("global", "previous", "checkpoint", false);
        store.put("global", "previous", "committed-append", true);
        crash(file, "append", 71);
        Map<String, Map<String, String>> loaded = empty();
        store = new SettingsStore(file.toFile(), loaded);
        check(loaded.equals(tables), "Abrupt process exit during append preserves previous acknowledged mutations");
        store.put("global", "previous", "recovered", true);
        byte[] before = Files.readAllBytes(file);
        crash(file, "checkpoint", 72);
        check(Arrays.equals(before, Files.readAllBytes(file)), "Abrupt exit during checkpoint leaves the previous file intact");
        Map<String, Map<String, String>> after = empty(); new SettingsStore(file.toFile(), after);
        check(after.equals(loaded), "Unpublished temporary checkpoint is ignored on recovery");
        crash(file, "committed", 73);
        after = empty(); new SettingsStore(file.toFile(), after);
        check("acknowledged".equals(after.get("global").get("crash-writer")),
                "A successful mutation survives immediate process halt without normal shutdown");
    }

    private static void benchmark(Path root) throws Exception {
        File file = root.resolve("benchmark.bin").toFile();
        Map<String, Map<String, String>> tables = empty();
        for (int i = 0; i < 5000; i++) tables.get(TABLES[i % 3]).put("setting-" + i, "v".repeat(96));
        SettingsStore store = new SettingsStore(file, tables);
        store.put("system", "changing", "warmup", false);
        long initial = file.length();
        SettingsIO.reset();
        java.lang.management.ThreadMXBean bean = java.lang.management.ManagementFactory.getThreadMXBean();
        com.sun.management.ThreadMXBean allocations = bean instanceof com.sun.management.ThreadMXBean
                ? (com.sun.management.ThreadMXBean) bean : null;
        long allocated = allocations == null ? -1 : allocations.getCurrentThreadAllocatedBytes();
        long cpu = bean.getCurrentThreadCpuTime(), start = System.nanoTime();
        for (int i = 0; i < 256; i++) store.put("system", "changing", "v" + i, false);
        long elapsed = System.nanoTime() - start;
        cpu = bean.getCurrentThreadCpuTime() - cpu;
        if (allocated >= 0) allocated = allocations.getCurrentThreadAllocatedBytes() - allocated;
        Map<String, Map<String, String>> loaded = empty(); new SettingsStore(file, loaded);
        check(loaded.equals(tables), "Measured workload restores exactly");
        System.out.println("SettingsStore workload: rows=5000 mutations=256 initial_bytes=" + initial
                + " written_bytes=" + SettingsIO.written + " writes=" + SettingsIO.writes
                + " syncs=" + SettingsIO.syncs + " replaces=" + SettingsIO.replaces
                + " elapsed_ns=" + elapsed + " cpu_ns=" + cpu + " allocated_bytes=" + allocated);
    }

    public static void main(String[] args) throws Exception {
        if (args.length > 0 && args[0].equals("--crash-writer")) { crashWriter(args); return; }
        Path root = Files.createTempDirectory("gesher-settings-performance-");
        try {
            if (args.length > 0 && args[0].equals("--benchmark-only")) { benchmark(root); return; }
            format(root); boundaries(root); invalidLoad(root); publicationFailure(root); io(root);
            journal(root); tornTail(root); appendFailures(root); checkpointFailure(root); concurrent(root);
            processCrashes(root);
            System.out.println("SettingsStoreHost all passed: " + checks + " checks");
        } finally {
            try (java.util.stream.Stream<Path> paths = Files.walk(root)) {
                for (Path path : (Iterable<Path>) paths.sorted(Comparator.reverseOrder())::iterator) Files.deleteIfExists(path);
            }
        }
    }
}

// The production stream construction sites are instrumented by the runner. These
// subclasses always delegate to real File*Stream operations and real FileDescriptor.sync.
final class SettingsIO {
    static int writes, reads, syncs, replaces;
    static long written;
    static int failWriteAfter = -1, failSyncAt = -1;
    static boolean failTruncate, failClose, failReplace, haltAppend, haltCheckpoint;
    static void reset() {
        writes = reads = syncs = replaces = 0; written = 0;
        failWriteAfter = failSyncAt = -1; failTruncate = failClose = failReplace = false;
        haltAppend = haltCheckpoint = false;
    }
    static void sync(FileDescriptor descriptor) throws IOException {
        if (++syncs == failSyncAt) throw new IOException("injected sync failure");
        descriptor.sync();
    }
    static Path move(Path source, Path target, CopyOption... options) throws IOException {
        if (failReplace) throw new IOException("injected replace failure");
        replaces++;
        return Files.move(source, target, options);
    }
}
final class CountingRandomAccessFile extends RandomAccessFile {
    CountingRandomAccessFile(File file, String mode) throws IOException { super(file, mode); }
    @Override public void write(byte[] bytes) throws IOException {
        if (SettingsIO.haltAppend) {
            super.write(bytes, 0, Math.min(11, bytes.length)); getFD().sync();
            Runtime.getRuntime().halt(71);
        }
        if (SettingsIO.failWriteAfter >= 0) {
            int count = Math.min(bytes.length, SettingsIO.failWriteAfter);
            super.write(bytes, 0, count); SettingsIO.writes++; SettingsIO.written += count;
            throw new IOException("injected partial append failure");
        }
        super.write(bytes); SettingsIO.writes++; SettingsIO.written += bytes.length;
    }
    @Override public void setLength(long length) throws IOException {
        if (SettingsIO.failTruncate) throw new IOException("injected rollback failure");
        super.setLength(length);
    }
    @Override public void close() throws IOException {
        super.close();
        if (SettingsIO.failClose) throw new IOException("injected close failure");
    }
}
final class CountingFileOutputStream extends FileOutputStream {
    CountingFileOutputStream(File file) throws IOException { super(file); }
    public void write(int value) throws IOException { SettingsIO.writes++; SettingsIO.written++; super.write(value); }
    public void write(byte[] bytes) throws IOException { write(bytes, 0, bytes.length); }
    public void write(byte[] bytes, int offset, int length) throws IOException {
        if (SettingsIO.haltCheckpoint) {
            super.write(bytes, offset, Math.min(11, length)); getFD().sync();
            Runtime.getRuntime().halt(72);
        }
        SettingsIO.writes++; SettingsIO.written += length; super.write(bytes, offset, length);
    }
}
final class CountingFileInputStream extends FileInputStream {
    CountingFileInputStream(File file) throws IOException { super(file); }
    public int read() throws IOException { SettingsIO.reads++; return super.read(); }
    public int read(byte[] bytes) throws IOException { return read(bytes, 0, bytes.length); }
    public int read(byte[] bytes, int offset, int length) throws IOException {
        SettingsIO.reads++; return super.read(bytes, offset, length);
    }
}
final class SystemServer {
    static void log(String ignored) {} // Host test boundary: no system server is started.
}
