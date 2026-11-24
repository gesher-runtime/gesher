/* Explicit host boundaries: APK parsing and Windows process execution are not simulated evidence. */
package com.gesher.server;

import java.io.File;
import java.io.IOException;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.List;
import java.util.Map;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;

final class Device {
    static File root;
    static File dataDir() { return root; }
    static File logsDir() { return new File(root, "logs"); }
}
final class SystemServer { static void log(String value) { System.out.println(value); } }
final class SharedLibraryGraph {
    boolean contentsUnchanged() { return true; }
    String classLoaderContext() { return "PCL[]"; }
    void writeManifest(File directory) throws IOException { Files.writeString(directory.toPath().resolve("libraries"), "graph"); }
}
final class ApkPackage {
    String packageName;
    SharedLibraryGraph sharedLibraries = new SharedLibraryGraph();
    static ApkPackage parseCodeDirectory(String path) {
        ApkPackage result = new ApkPackage(); result.packageName = new File(path).getName(); return result;
    }
}
/** Explicit notification boundary: real host WatchService, not the Windows implementation. */
final class DexoptProfileNotifications {
    static final AtomicInteger ids = new AtomicInteger();
    static final Map<Long, HostWatch> watches = new ConcurrentHashMap<>();
    static final class HostWatch {
        final Path root;
        final Thread owner = Thread.currentThread();
        final java.nio.file.WatchService service;
        final Map<java.nio.file.WatchKey, Path> paths = new java.util.HashMap<>();
        HostWatch(Path root) throws IOException {
            this.root = root; service = root.getFileSystem().newWatchService(); register(root);
        }
        void register(Path directory) throws IOException {
            try (var children = Files.walk(directory)) {
                for (Path path : children.filter(Files::isDirectory).toList())
                    paths.put(path.register(service, java.nio.file.StandardWatchEventKinds.ENTRY_CREATE,
                            java.nio.file.StandardWatchEventKinds.ENTRY_DELETE,
                            java.nio.file.StandardWatchEventKinds.ENTRY_MODIFY), path);
            }
        }
    }
    static long open(String root) throws IOException {
        HostWatch watch = new HostWatch(Path.of(root));
        long id = ids.incrementAndGet(); watches.put(id, watch); return id;
    }
    static String[] next(long id) throws IOException {
        HostWatch watch = watches.get(id);
        if (watch == null) throw new IOException("closed watch");
        java.nio.file.WatchKey key;
        try { key = watch.service.take(); }
        catch (InterruptedException refresh) { return new String[0]; }
        var names = new java.util.ArrayList<String>();
        for (var event : key.pollEvents()) {
            if (event.kind() == java.nio.file.StandardWatchEventKinds.OVERFLOW) return new String[0];
            Path path = watch.paths.get(key).resolve((Path)event.context());
            names.add(watch.root.relativize(path).toString());
            if (event.kind() == java.nio.file.StandardWatchEventKinds.ENTRY_CREATE && Files.isDirectory(path)) {
                watch.register(path);
                // Native recursive ReadDirectoryChangesW also observes writes made
                // before a user-space per-directory registration could be installed.
                try (var files = Files.walk(path)) {
                    files.filter(Files::isRegularFile).forEach(file -> names.add(watch.root.relativize(file).toString()));
                }
            }
        }
        if (!key.reset()) {
            Path retired = watch.paths.remove(key);
            if (retired.equals(watch.root)) throw new IOException("root invalidated");
        }
        return names.toArray(new String[0]);
    }
    static void wake(long id) { HostWatch watch = watches.get(id); if (watch != null) watch.owner.interrupt(); }
    static void close(long id) {
        HostWatch watch = watches.remove(id);
        if (watch != null) try { watch.service.close(); } catch (IOException failure) { throw new RuntimeException(failure); }
    }
}
final class Zygote {
    static final int STILL_ACTIVE = 259;
    static final AtomicInteger calls = new AtomicInteger(), compiles = new AtomicInteger(), active = new AtomicInteger();
    static final Map<Integer, Tool> tools = new ConcurrentHashMap<>();
    static volatile boolean failCompiler, blockCompiler, cancel, lowMemory;
    static volatile CountDownLatch entered = new CountDownLatch(1), release = new CountDownLatch(1);
    static volatile int maxActive;
    static volatile Runnable duringCompiler;
    static final class Tool {
        final List<String> args;
        Tool(String command) { args = Dexopt.splitCommandLine(command); }
        String arg(String prefix) {
            return args.stream().filter(a -> a.startsWith(prefix)).findFirst().orElseThrow().substring(prefix.length());
        }
        boolean compiler() { return args.get(0).endsWith("dex2oat.exe"); }
        int run() {
            try {
                if (compiler()) {
                    compiles.incrementAndGet();
                    Runnable callback = duringCompiler;
                    if (callback != null) { duringCompiler = null; callback.run(); }
                    entered.countDown();
                    if (blockCompiler && !release.await(10, TimeUnit.SECONDS)) throw new AssertionError("test tool timed out");
                    if (cancel || lowMemory) return -3;
                    if (failCompiler) { failCompiler = false; return 17; }
                    Path oat = Path.of(arg("--oat-file="));
                    Files.createDirectories(oat.getParent());
                    Files.writeString(oat, "compiled:" + Files.readString(Path.of(arg("--profile-file="))));
                    Files.writeString(oat.resolveSibling("base.vdex"), "vdex");
                    return 0;
                }
                Path current = Path.of(arg("--profile-file=")), reference = Path.of(arg("--reference-profile-file="));
                String now = Files.readString(current), before = Files.readString(reference);
                if (before.equals(now)) return 2;
                Files.writeString(reference, now);
                return now.isEmpty() ? 7 : 1;
            } catch (Exception failure) { throw new AssertionError(failure); }
        }
    }
    static String exePath() { return "C:\\runtime\\dalvikvm.exe"; }
    static String nativeCommandLine() { return "dalvikvm -Ximage:/boot/boot.art -Xbootclasspath:boot.jar"; }
    static String quote(String value) { return '"' + value + '"'; }
    static String windowsPath(String value) { return value; }
    static int spawn(String command, List<String> env, String log, boolean breakaway) {
        int pid = calls.incrementAndGet(); tools.put(pid, new Tool(command));
        maxActive = Math.max(maxActive, active.incrementAndGet()); return pid;
    }
    static int nativeSpawnCompiler(String command, String[] set, String[] unset, String log, int memory, int cpu) {
        if (memory != 1536 || cpu != 2000) throw new AssertionError("resource budgets");
        if (command.contains("dex2oat") && (!command.contains("-j1") || !command.contains("-Xmx512m")))
            throw new AssertionError("compiler worker/heap budgets");
        return spawn(command, List.of(), log, false);
    }
    static long childStartTime(int pid) { return pid + 100L; }
    static int nativeWaitChild(int pid, long identity, int timeout, long event, boolean memory) {
        if (identity != pid + 100L) throw new AssertionError("child identity");
        try { return tools.get(pid).run(); }
        finally { tools.remove(pid); active.decrementAndGet(); }
    }
    static int exitCode(int pid) { return nativeWaitChild(pid, pid + 100L, 0, 0, false); }
    static boolean kill(int pid) { return true; }
    static long nativeAvailableMemory() { return lowMemory ? 128L << 20 : 8L << 30; }
    static long createEvent(String name) { return 1; }
    static boolean waitEvent(long event, int timeout) { boolean previous = cancel; cancel = false; return previous; }
    static boolean signalEvent(String name) { cancel = true; release.countDown(); return true; }
}
