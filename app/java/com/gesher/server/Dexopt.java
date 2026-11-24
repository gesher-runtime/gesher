/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.server;

import android.os.SystemClock;
import android.os.SystemProperties;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.OutputStreamWriter;
import java.io.Writer;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.StandardCopyOption;
import java.util.ArrayList;
import java.util.List;
import java.util.Map;
import java.util.function.Supplier;
import java.util.concurrent.locks.ReentrantLock;

/**
 * Android's compilation model for installed apps, as ART Service (artd, dex2oat, profman)
 * runs it on a device (docs/compiled-code.md, ADR 0054):
 *
 * <ul>
 * <li><b>Install</b> ({@link #onInstall}): dex2oat compiles the package's base.apk with
 *     {@code pm.dexopt.install} (default {@code speed-profile}), which without a profile is
 *     {@code verify}, into {@code <code path>/oat/x86_64/base.{odex,vdex}}, where ART's
 *     OatFileAssistant looks for an app's compiled code.
 * <li><b>Profiles</b>: app processes run with the JIT and its profile saver
 *     ({@link #appVmOptions}); the framework registers the app's code path with ART
 *     (LoadedApk.registerAppInfoToArt), which saves the methods and classes the app used in
 *     {@code $ANDROID_DATA/misc/profiles/cur/0/<package>/primary.prof}, made here at install
 *     as installd makes it ({@link #prepareProfiles}).
 * <li><b>Background dexopt</b> ({@link #startBackground}): when no app process has run for
 *     a while (BackgroundDexOptService runs when the device is idle), each package whose
 *     current profile has new data has it merged into its reference profile ({@code
 *     misc/profiles/ref/<package>/primary.prof}, profman), and, if profman says so, is
 *     compiled again with {@code pm.dexopt.bg-dexopt} (default {@code speed-profile}): the
 *     profile's methods get AOT code, its classes go into an app image ({@code base.art}),
 *     which ART loads with the oat file on the next start.
 * </ul>
 *
 * dex2oat.exe and profman.exe are next to dalvikvm.exe; they run as processes of their own
 * (the system server's job: they end with it), their output appended to {@code
 * dexopt.log} in the device's logs.
 */
final class Dexopt {
    static final String ISA = "x86_64";
    /** dex2oat's runtime: as tools/boot-image/compile.ps1's (ADR 0031). */
    private static final String[] DEX2OAT_RUNTIME = {"-Xms64m", "-Xmx512m",
            "-XX:LargeObjectSpace=map"};
    private static final int TOOL_TIMEOUT_MS = 10 * 60_000;
    private static final ReentrantLock sCompiler = new ReentrantLock(true);
    private static final DexoptWorkQueue sWork = new DexoptWorkQueue();
    private static final ThreadLocal<Attempt> sAttempt = new ThreadLocal<>();
    private static volatile Attempt sActive;
    private static volatile DexoptProfileWatch sProfiles;
    private static String sCancelName;
    private static long sCancelEvent;
    private static Supplier<Long> sIdleSince;
    private static long sIdleMs;
    // Preserve the established compiler heap ceiling; constrain the whole child
    // separately instead of making large applications fail with a smaller heap.
    private static final int BACKGROUND_HEAP_MIB = 512;
    private static final int BACKGROUND_MEMORY_MIB = 1536;
    private static final int BACKGROUND_CPU_RATE = 2000; // 20% of host/parent-job CPU capacity

    private static final class Attempt {
        final DexoptWorkQueue.Work work;
        volatile boolean cancelled;
        boolean completed;
        Attempt(DexoptWorkQueue.Work work) { this.work = work; }
    }

    /** Publication/notification locks never span compilation or a process wait. */
    static void activityChanged() {
        synchronized (sWork) {
            Attempt active = sActive;
            if (active != null) active.cancelled = true;
            sWork.wake();
        }
        String name = sCancelName;
        if (name != null) Zygote.signalEvent(name);
    }

    static void packagesChanged() {
        DexoptProfileWatch profiles = sProfiles;
        if (profiles != null) profiles.refresh();
    }

    /** Withdraw a generation before the installer starts replacing/removing its bytes. */
    static void packageChanging(String name) {
        synchronized (sWork) {
            sWork.removed(name);
            Attempt active = sActive;
            if (active != null && active.work.packageName.equals(name)) active.cancelled = true;
            sWork.wake();
        }
        if (sCancelName != null) Zygote.signalEvent(sCancelName);
    }

    static void awaitCompiler() {
        sCompiler.lock();
        sCompiler.unlock();
    }

    private static boolean cancelled(Attempt attempt) {
        if (attempt == null) return false;
        if (attempt.cancelled || !sWork.current(attempt.work)) return true;
        long since = sIdleSince.get();
        return attempt.cancelled || since == 0 || SystemClock.uptimeMillis() - since < sIdleMs;
    }

    private Dexopt() {
    }

    // --- files ------------------------------------------------------------------------

    static File oatDir(String codePath) {
        return new File(codePath, "oat/" + ISA);
    }

    static File currentProfile(String packageName) {
        return new File(Device.dataDir(), "misc/profiles/cur/0/" + packageName + "/primary.prof");
    }

    static File referenceProfile(String packageName) {
        return new File(Device.dataDir(), "misc/profiles/ref/" + packageName + "/primary.prof");
    }

    /**
     * installd's prepare_app_profiles: the package's profile directories, and its current
     * profile (empty), which the app's runtime writes.
     */
    static void prepareProfiles(String packageName) {
        for (File f : new File[] {currentProfile(packageName), referenceProfile(packageName)}) {
            f.getParentFile().mkdirs();
            try {
                if (!f.exists()) new FileOutputStream(f).close();
            } catch (IOException e) {
                SystemServer.log("dexopt: cannot make " + f + ": " + e);
            }
        }
    }

    /** The package is uninstalled: its profiles go. */
    static void deleteProfiles(String packageName) {
        new File(referenceProfile(packageName).getPath() + ".cloud").delete();
        for (File f : new File[] {currentProfile(packageName), referenceProfile(packageName)}) {
            f.delete();
            try {
                Files.deleteIfExists(f.getParentFile().toPath());
            } catch (IOException e) {
                // not empty: left
            }
        }
    }

    /** A profile with data in it (an empty file is what prepareProfiles made). */
    private static boolean hasData(File profile) {
        return profile.isFile() && profile.length() > 0;
    }

    // --- app processes ----------------------------------------------------------------

    /**
     * An app process's runtime options, from the system server's ({@code vm}: its
     * executable and options): the JIT and its profile saver, as AndroidRuntime starts an
     * app's runtime ({@code dalvik.vm.usejit}, {@code dalvik.vm.usejitprofiles}, both true
     * by default), instead of the device's options (nterp and the boot image's AOT code,
     * no JIT). {@code dalvik.vm.usejit=false} keeps the device's options; {@code
     * GESHER_APP_EXECUTION=interpret} runs app processes on the switch interpreter only
     * ({@code -Xint}, as every Gesher process ran before ADR 0033).
     */
    static String appVmOptions(String vm) {
        boolean interpret = "interpret".equals(System.getenv("GESHER_APP_EXECUTION"));
        if (!interpret && !SystemProperties.getBoolean("dalvik.vm.usejit", true)) {
            return vm;
        }
        List<String> args = splitCommandLine(vm);
        StringBuilder sb = new StringBuilder();
        for (int i = 0; i < args.size(); i++) {
            String a = args.get(i);
            if (i > 0 && (a.equals("-Xint") || a.startsWith("-Xusejit:"))) continue;
            if (sb.length() > 0) sb.append(' ');
            sb.append(Zygote.quote(a));
        }
        if (interpret) {
            sb.append(" -Xint -Xusejit:false");
            SystemServer.log("app processes run on the switch interpreter only"
                    + " (GESHER_APP_EXECUTION=interpret)");
            return sb.toString();
        }
        sb.append(" -Xusejit:true");
        if (SystemProperties.getBoolean("dalvik.vm.usejitprofiles", true)) {
            sb.append(" -Xjitsaveprofilinginfo");
        }
        SystemServer.log("app processes run with the JIT" + (sb.indexOf(" -Xjitsaveprofilinginfo")
                >= 0 ? " and its profile saver" : "") + " (dexopt: ADR 0054)");
        return sb.toString();
    }

    // --- compiling ----------------------------------------------------------------------

    /** The compiler filter for {@code reason}: pm.dexopt.<reason>, as ART Service's ReasonMapping. */
    private static String filterFor(String reason, String fallback) {
        String f = SystemProperties.get("pm.dexopt." + reason, "");
        return f.isEmpty() ? fallback : f;
    }

    /**
     * Install: compiles the package with pm.dexopt.install. A profile-guided filter with no
     * profile (no reference profile yet) is {@code verify}, as ART Service does. Returns what
     * was done, for the install's log line, or null when dexopt is off or failed (the app
     * then runs from its APK).
     */
    static String onInstall(String packageName, String codePath, SharedLibraryGraph libraries) {
        packageChanging(packageName);
        activityChanged(); // foreground install takes precedence over background work
        sCompiler.lock();
        try {
            prepareProfiles(packageName);
            String filter = filterFor("install", "speed-profile");
            File ref = referenceProfile(packageName);
            File metadata = new File(codePath, "base.dm");
            File profile = metadata.isFile() ? importCloudProfile(packageName, codePath, metadata)
                    : hasData(ref) ? ref : null;
            if (filter.endsWith("-profile") && profile == null) filter = "verify";
            return compile(packageName, codePath, filter, "install", profile, libraries);
        } finally {
            sCompiler.unlock();
            packagesChanged();
        }
    }

    /**
     * ART Service's copy-and-update operation validates every profile dex checksum/type/
     * method count and rewrites keys to the installed APK. A valid ZIP with missing, empty,
     * corrupt or mismatched profile data falls back to verify; it never replaces the
     * reference profile with the rejected output. The installer validates the ZIP first.
     */
    private static File importCloudProfile(String packageName, String codePath, File metadata) {
        File ref = referenceProfile(packageName);
        File staged = new File(ref.getPath() + ".cloud");
        staged.delete();
        String apk = new File(codePath, "base.apk").getPath();
        List<String> args = new ArrayList<>();
        args.add("--copy-and-update-profile-key");
        args.add("--profile-file=" + metadata.getPath());
        args.add("--reference-profile-file=" + staged.getPath());
        args.add("--apk=" + apk);
        args.add("--dex-location=" + apk);
        int code = run("profman", args);
        try {
            // ProfmanResult::CopyAndUpdateResult: only 0 matches all profile dex files.
            if (code != 0 || !hasData(staged)) {
                SystemServer.log("dexopt: " + packageName + ": cloud profile rejected (profman "
                        + code + "); falling back to verify");
                return null;
            }
            Files.move(staged.toPath(), ref.toPath(), StandardCopyOption.REPLACE_EXISTING,
                    StandardCopyOption.ATOMIC_MOVE);
            SystemServer.log("dexopt: " + packageName + ": cloud profile accepted from "
                    + metadata + ", " + ref.length() + " bytes");
            return ref;
        } catch (IOException e) {
            SystemServer.log("dexopt: " + packageName + ": cloud profile not committed: " + e
                    + "; falling back to verify");
            return null;
        } finally {
            staged.delete();
        }
    }

    /**
     * dex2oat for {@code codePath/base.apk} into {@code codePath/oat/x86_64}: written into
     * {@code oat.new}, then put in place of the old oat directory, so that a run never sees
     * half of a compile. Returns a description, or null.
     */
    static String compile(String packageName, String codePath, String filter,
            String reason, File profile) {
        sCompiler.lock();
        try {
            ApkPackage parsed = ApkPackage.parseCodeDirectory(codePath);
            if (!parsed.packageName.equals(packageName)) {
                throw new IOException("compiled APK package name does not match " + packageName);
            }
            return compile(packageName, codePath, filter, reason, profile, parsed.sharedLibraries);
        } catch (Exception invalidLibrary) {
            SystemServer.log("dexopt: " + packageName + ": library resolution failed: " + invalidLibrary);
            return null;
        } finally {
            sCompiler.unlock();
        }
    }

    private static String compile(String packageName, String codePath, String filter,
            String reason, File profile, SharedLibraryGraph libraries) {
        Attempt attempt = sAttempt.get();
        if (cancelled(attempt)) return null;
        try {
            if (!libraries.contentsUnchanged()) throw new IOException("shared library changed before dex2oat");
        } catch (IOException changedLibrary) {
            SystemServer.log("dexopt: " + packageName + ": " + changedLibrary);
            return null;
        }
        Vm vm = Vm.current();
        if (vm == null) {
            SystemServer.log("dexopt: " + packageName + ": no -Ximage in this runtime's options;"
                    + " not compiled");
            return null;
        }
        String apk = new File(codePath, "base.apk").getPath();
        File newDir = new File(codePath, "oat.new/" + ISA);
        deleteTree(newDir.getParentFile());
        newDir.mkdirs();
        boolean appImage = profile != null && filter.endsWith("-profile");
        List<String> a = new ArrayList<>();
        a.add("--dex-file=" + apk);
        a.add("--dex-location=" + apk);
        a.add("--oat-file=" + new File(newDir, "base.odex").getPath());
        a.add("--instruction-set=" + ISA);
        a.add("--compiler-filter=" + filter);
        a.add("--compilation-reason=" + reason);
        a.add("--class-loader-context=" + libraries.classLoaderContext());
        // The boot image with its extensions (framework.jar, gesher-app.jar): the runtime
        // finds them next to a lone boot.art (ART patch 0012), dex2oat only with `<dir>/*`;
        // without them the app's classes that extend framework classes stay out of its
        // app image.
        String image = vm.image;
        int slash = image.lastIndexOf('/');
        if (image.indexOf(':') < 0 && image.indexOf('*') < 0 && slash >= 0) {
            image = image + ":" + image.substring(0, slash + 1) + "*";
        }
        a.add("--boot-image=" + image);
        a.add("--runtime-arg");
        a.add("-Xbootclasspath:" + vm.bootClassPath);
        for (String r : DEX2OAT_RUNTIME) {
            a.add("--runtime-arg");
            a.add(attempt != null && r.startsWith("-Xmx") ? "-Xmx" + BACKGROUND_HEAP_MIB + "m" : r);
        }
        if (profile != null) a.add("--profile-file=" + profile.getPath());
        if (appImage) {
            a.add("--app-image-file=" + new File(newDir, "base.art").getPath());
            a.add("--image-format=lz4");
        }
        // dex2oat's watchdog thread needs pthread_condattr_setclock, which gesher-base
        // lacks (tools/boot-image/compile.ps1 turns it off too).
        a.add("--no-watch-dog");
        a.add(attempt == null ? "-j4" : "-j1");
        long start = SystemClock.uptimeMillis();
        int code = run("dex2oat", a);
        long ms = SystemClock.uptimeMillis() - start;
        if (code != 0) {
            SystemServer.log("dexopt: " + packageName + ": dex2oat failed (exit code " + code
                    + ", " + filter + ", reason " + reason + "; dexopt.log); the app runs from its"
                    + " APK");
            deleteTree(newDir.getParentFile());
            return null;
        }
        try {
            // Publish code and graph identity together, only while every jar is unchanged.
            if (!libraries.contentsUnchanged()) throw new IOException("shared library changed during dex2oat");
            libraries.writeManifest(newDir);
        } catch (IOException changedLibrary) {
            SystemServer.log("dexopt: " + packageName + ": graph not committed: " + changedLibrary);
            deleteTree(newDir.getParentFile());
            return null;
        }
        if (cancelled(attempt)) {
            deleteTree(newDir.getParentFile());
            return null;
        }
        File oat = new File(codePath, "oat");
        File old = new File(codePath, "oat.old");
        deleteTree(old);
        synchronized (sWork) {
            // A generation withdrawal or foreground notification serializes with this
            // short publication, never with dex2oat, parsing, or an OS process wait.
            if (attempt != null && (attempt.cancelled || !sWork.current(attempt.work))) {
                deleteTree(newDir.getParentFile());
                return null;
            }
            if (oat.exists() && !oat.renameTo(old)) {
                SystemServer.log("dexopt: " + packageName + ": cannot replace " + oat
                        + " (in use); the new code waits in " + newDir.getParentFile());
                return null;
            }
            if (!newDir.getParentFile().renameTo(oat)) {
                SystemServer.log("dexopt: " + packageName + ": cannot move " + newDir.getParentFile()
                        + " to " + oat);
                if (old.exists()) old.renameTo(oat);
                return null;
            }
            if ("bg-dexopt".equals(reason)) {
                try {
                    Files.move(profile.toPath(), referenceProfile(packageName).toPath(),
                            StandardCopyOption.REPLACE_EXISTING, StandardCopyOption.ATOMIC_MOVE);
                } catch (IOException failedProfileCommit) {
                    // Keep the previous code/profile pair on a failed publication.
                    if (oat.renameTo(newDir.getParentFile())) {
                        if (old.exists() && !old.renameTo(oat))
                            SystemServer.log("dexopt: previous artifacts remain in " + old);
                        deleteTree(newDir.getParentFile());
                    }
                    SystemServer.log("dexopt: " + packageName + ": reference profile not committed: "
                            + failedProfileCommit);
                    return null;
                }
                if (attempt != null) attempt.completed = true;
            }
        }
        deleteTree(old);
        File dir = oatDir(codePath);
        StringBuilder sb = new StringBuilder("dexopt: ").append(packageName).append(": ")
                .append(filter).append(" (reason ").append(reason).append(") in ").append(ms)
                .append(" ms:");
        for (String ext : new String[] {"odex", "vdex", "art"}) {
            File f = new File(dir, "base." + ext);
            if (f.isFile()) sb.append(' ').append(f.getName()).append(' ').append(f.length());
        }
        sb.append(" bytes in ").append(dir);
        String what = sb.toString();
        SystemServer.log(what);
        return what;
    }

    // --- background dexopt --------------------------------------------------------------

    /** The installed packages' code paths, by package. */
    interface Packages {
        Map<String, String> codePaths();
    }

    private static Thread sBackground;

    /** Profile notifications and real retry/idle deadlines drive one compiler worker. */
    static synchronized void startBackground(Packages packages, Supplier<Long> idleSince) {
        if (sBackground != null) return;
        long idleMs = Math.max(0, SystemProperties.getLong("gesher.dexopt.idle-ms", 2000));
        sIdleSince = idleSince;
        sIdleMs = idleMs;
        sCancelName = "Local\\GesherDexopt-" + android.os.Process.myPid();
        sCancelEvent = Zygote.createEvent(sCancelName);
        if (sCancelEvent == 0) {
            SystemServer.log("dexopt: cancellation event unavailable; pending work will retry");
        }
        sProfiles = new DexoptProfileWatch(currentProfile("_").getParentFile().getParentFile().toPath(),
                packages::codePaths, sWork, SystemServer::log);
        sBackground = new Thread(() -> {
            while (true) {
                long epoch = sWork.epoch();
                try {
                    if (sWork.size() == 0) { sWork.await(epoch, 0); continue; }
                    long since = idleSince.get();
                    if (since == 0) { sWork.await(epoch, 0); continue; }
                    long now = SystemClock.uptimeMillis();
                    long idleDelay = idleMs - (now - since);
                    if (idleDelay > 0) { sWork.await(epoch, idleDelay); continue; }
                    DexoptWorkQueue.Work work = sWork.poll(now);
                    if (work == null) { sWork.await(epoch, sWork.delay(now)); continue; }
                    if (sCancelEvent == 0) sCancelEvent = Zygote.createEvent(sCancelName);
                    if (sCancelEvent == 0) { sWork.finished(work, false, now); continue; }
                    Attempt attempt = new Attempt(work);
                    // Only this worker consumes the auto-reset cancellation event.
                    Zygote.waitEvent(sCancelEvent, 0);
                    synchronized (sWork) { sActive = attempt; }
                    try {
                        if (!cancelled(attempt)) {
                            sAttempt.set(attempt);
                            backgroundDexopt(work.packageName, work.codePath);
                        }
                    } finally {
                        sAttempt.remove();
                        synchronized (sWork) { sActive = null; }
                        sWork.finished(work, attempt.completed, SystemClock.uptimeMillis());
                    }
                } catch (InterruptedException interrupted) {
                    // Only an explicit notification interrupts a wait; re-evaluate state.
                } catch (Throwable failure) {
                    SystemServer.log("dexopt: background dexopt failed: " + failure);
                    try { sWork.await(sWork.epoch(), 60_000); }
                    catch (InterruptedException retryNow) { }
                }
            }
        }, "bg-dexopt");
        sBackground.setDaemon(true);
        sProfiles.start();
        sBackground.start();
        SystemServer.log("dexopt: background dexopt runs once the device is idle for " + idleMs
                + " ms; profile-driven worker, workers=1, heap=" + BACKGROUND_HEAP_MIB + " MiB, process="
                + BACKGROUND_MEMORY_MIB + " MiB, cpu=" + BACKGROUND_CPU_RATE / 100 + "%");
    }

    /** Merge into a private reference, retaining both original profiles until commit. */
    static String backgroundDexopt(String packageName, String codePath) {
        sCompiler.lock();
        File staged = null, snapshot = null;
        try {
            Attempt attempt = sAttempt.get();
            if (cancelled(attempt)) return null;
            File cur = currentProfile(packageName);
            if (!hasData(cur)) {
                if (attempt != null) attempt.completed = true;
                return null;
            }
            File ref = referenceProfile(packageName);
            ref.getParentFile().mkdirs();
            staged = File.createTempFile("primary-", ".prof", ref.getParentFile());
            snapshot = File.createTempFile("current-", ".prof", ref.getParentFile());
            Files.copy(cur.toPath(), snapshot.toPath(), StandardCopyOption.REPLACE_EXISTING);
            if (ref.isFile()) Files.copy(ref.toPath(), staged.toPath(), StandardCopyOption.REPLACE_EXISTING);
            String apk = new File(codePath, "base.apk").getPath();
            List<String> args = new ArrayList<>();
            args.add("--profile-file=" + snapshot.getPath());
            args.add("--reference-profile-file=" + staged.getPath());
            args.add("--apk=" + apk);
            args.add("--dex-location=" + apk);
            args.add("--force-merge-and-analyze");
            int code = run("profman", args);
            SystemServer.log("dexopt: " + packageName + ": profman merged " + cur.length()
                    + " bytes of profile into staged reference (" + staged.length() + " bytes): "
                    + (code == 1 ? "compile" : code == 2 ? "small delta, no compile"
                            : code == 7 ? "empty profiles" : "exit code " + code));
            if (cancelled(attempt)) return null;
            if (code == 2 || code == 7) {
                if (attempt != null) attempt.completed = true;
                return null;
            }
            if (code != 1) return null;
            String what = compile(packageName, codePath, filterFor("bg-dexopt", "speed-profile"),
                    "bg-dexopt", staged);
            if (what == null) return null;
            // The runtime may have appended after profman's read. Never truncate its
            // current profile: later notifications merge the retained cumulative data.
            return what;
        } catch (IOException failure) {
            SystemServer.log("dexopt: " + packageName + ": profile not committed: " + failure);
            return null;
        } finally {
            if (staged != null) staged.delete();
            if (snapshot != null) snapshot.delete();
            sCompiler.unlock();
        }
    }

    // --- tools --------------------------------------------------------------------------

    /** Runs {@code tool}.exe (next to dalvikvm.exe) and returns its exit code. */
    private static int run(String tool, List<String> args) {
        String exe = Zygote.exePath();
        String dir = exe.substring(0, exe.lastIndexOf('\\') + 1);
        StringBuilder cmd = new StringBuilder(Zygote.quote(dir + tool + ".exe"));
        for (String a : args) cmd.append(' ').append(Zygote.quote(a));
        File log = new File(Device.logsDir(), "dexopt.log");
        log.getParentFile().mkdirs();
        try (Writer w = new OutputStreamWriter(new FileOutputStream(log, true),
                StandardCharsets.UTF_8)) {
            w.write("$ " + cmd + "\n");
        } catch (IOException e) {
            // no log line
        }
        // The tool's output starts here in dexopt.log (its stdout and stderr).
        long outputStart = log.length();
        Attempt attempt = sAttempt.get();
        if (cancelled(attempt)) return -3;
        if (attempt != null && Zygote.nativeAvailableMemory() < 2L * BACKGROUND_HEAP_MIB * 1024 * 1024)
            return -3;
        int pid;
        try {
            pid = attempt == null ? Zygote.spawn(cmd.toString(), List.of(), log.getPath(), false)
                    : Zygote.nativeSpawnCompiler(cmd.toString(), new String[0],
                            new String[] {"GESHER_BINDER_UID"}, Zygote.windowsPath(log.getPath()),
                            BACKGROUND_MEMORY_MIB, BACKGROUND_CPU_RATE);
            if (pid <= 0) throw new IllegalStateException("compiler process creation failed: " + pid);
        } catch (RuntimeException failure) {
            SystemServer.log("dexopt: " + failure.getMessage());
            return -1;
        }
        long identity = Zygote.childStartTime(pid);
        if (cancelled(attempt)) Zygote.signalEvent(sCancelName);
        int code = Zygote.nativeWaitChild(pid, identity, TOOL_TIMEOUT_MS,
                attempt == null ? 0 : sCancelEvent, attempt != null);
        if (code < 0) SystemServer.log("dexopt: " + tool + " stopped (" + code
                + ": -2 deadline, -3 foreground/memory cancellation); input retained for retry");
        // profman's exit codes are results (ProfmanResult), not only failures.
        boolean result = tool.equals("profman") && (code == 1 || code == 2 || code == 7);
        if (code == -2 || (code > 0 && !result)) {
            logOutput(code == -2 ? tool + " took more than " + TOOL_TIMEOUT_MS / 1000 + " s; stopped"
                    : tool + " exit code " + code, pid, log, outputStart);
        }
        return code;
    }

    /** The most of a failed tool's output read back from dexopt.log. */
    private static final int OUTPUT_READ_LIMIT = 256 * 1024;
    /** The lines of it that go to system_server.log, and their longest length. */
    private static final int OUTPUT_LINES = 8;
    private static final int OUTPUT_LINE_CHARS = 400;

    /**
     * A failed tool's diagnostics in system_server.log as well as in dexopt.log, which a
     * report may leave out: what process {@code pid} wrote from {@code start} on in
     * {@code log}. Other dexopt runs (background profman beside an install's dex2oat) may
     * append to the same file meanwhile; their lines carry their own pids.
     */
    private static void logOutput(String what, int pid, File log, long start) {
        String output;
        try (java.io.RandomAccessFile f = new java.io.RandomAccessFile(log, "r")) {
            long end = f.length();
            long from = Math.max(start, end - OUTPUT_READ_LIMIT);
            byte[] bytes = new byte[(int) Math.max(0, end - from)];
            f.seek(from);
            f.readFully(bytes);
            output = new String(bytes, StandardCharsets.UTF_8);
        } catch (IOException e) {
            SystemServer.log("dexopt: " + what + "; its output in " + log + " is unreadable: " + e);
            return;
        }
        List<String> lines = failureLines(output, pid);
        StringBuilder sb = new StringBuilder("dexopt: ").append(what)
                .append(lines.isEmpty() ? "; no output" : "; its output:");
        for (String line : lines) sb.append("\n  ").append(line);
        SystemServer.log(sb.toString());
    }

    /** Messages that name a failure's cause, preferred over other error lines. */
    private static final java.util.regex.Pattern CAUSE = java.util.regex.Pattern.compile(
            "Failed to|Skipping non-existent|Unable to open|Could not open|No such file"
            + "|Permission denied|Not enough space");
    /** At most this many of the chosen lines are cause lines; the first errors fill the rest. */
    private static final int CAUSE_LINES = 4;

    /**
     * The lines of process {@code pid}'s failed run, in {@code output}, that say why. ART's
     * log lines carry their process's pid: only {@code pid}'s count. Of them, the lines
     * that name a cause (dex2oat's "Skipping non-existent dex file" and "Failed to create
     * oat file", profman's "Unable to open"), then its first errors (ART's E and F lines;
     * the first of a usage error is its cause, the rest the usage text), in their order;
     * with neither, its last lines. Output without ART's format (a crash message, a usage
     * error's last line) cannot be told from another process's, and is used only when
     * {@code pid} wrote no ART lines. At most {@link #OUTPUT_LINES}, each cut to
     * {@link #OUTPUT_LINE_CHARS} characters.
     */
    static List<String> failureLines(String output, int pid) {
        List<String> own = new ArrayList<>();
        List<String> unattributed = new ArrayList<>();
        String self = String.valueOf(pid);
        for (String line : output.split("\r?\n")) {
            String l = line.trim();
            if (l.isEmpty() || l.startsWith("$ ")) continue;  // another run's command line
            java.util.regex.Matcher m = LOG_LINE.matcher(l);
            if (!m.lookingAt()) {
                unattributed.add(l);
            } else if (m.group(1).equals(self)) {
                own.add(l);
            }
        }
        List<Integer> picked = new ArrayList<>();
        for (int i = 0; i < own.size() && picked.size() < CAUSE_LINES; i++) {
            char c = severity(own.get(i));
            if ((c == 'W' || c == 'E' || c == 'F') && CAUSE.matcher(own.get(i)).find()) picked.add(i);
        }
        for (int i = 0; i < own.size() && picked.size() < OUTPUT_LINES; i++) {
            if (isError(own.get(i)) && !picked.contains(i)) picked.add(i);
        }
        java.util.Collections.sort(picked);
        List<String> chosen = new ArrayList<>();
        for (int i : picked) chosen.add(own.get(i));
        if (chosen.isEmpty()) {
            List<String> from = own.isEmpty() ? unattributed : own;
            chosen.addAll(from.subList(Math.max(0, from.size() - OUTPUT_LINES), from.size()));
        }
        List<String> result = new ArrayList<>();
        for (String l : chosen) {
            result.add(l.length() > OUTPUT_LINE_CHARS ? l.substring(0, OUTPUT_LINE_CHARS) + "..." : l);
        }
        return result;
    }

    private static boolean isError(String line) {
        char c = severity(line);
        return c == 'E' || c == 'F';
    }

    /** An ART log line's severity ('W', 'E', 'F'...): "MM-DD HH:MM:SS.mmm pid tid E tag: ...". */
    private static char severity(String line) {
        java.util.regex.Matcher m = LOG_LINE.matcher(line);
        return m.lookingAt() ? m.group(2).charAt(0) : ' ';
    }

    /** An ART log line's start: "MM-DD HH:MM:SS.mmm pid tid S ", pid group 1, severity 2. */
    private static final java.util.regex.Pattern LOG_LINE =
            java.util.regex.Pattern.compile("\\d\\d-\\d\\d \\S+\\s+(\\d+)\\s+\\d+ ([VDIWEF]) ");

    /** What dex2oat needs of this runtime's options: its boot image and boot class path. */
    private static final class Vm {
        final String image;
        final String bootClassPath;

        private Vm(String image, String bootClassPath) {
            this.image = image;
            this.bootClassPath = bootClassPath;
        }

        static Vm current() {
            String image = null;
            String bcp = null;
            for (String a : splitCommandLine(Zygote.nativeCommandLine())) {
                if (a.startsWith("-Ximage:")) image = a.substring("-Ximage:".length());
                if (a.startsWith("-Xbootclasspath:")) bcp = a.substring("-Xbootclasspath:".length());
            }
            if (bcp == null) bcp = dalvik.system.VMRuntime.getRuntime().bootClassPath();
            return image == null ? null : new Vm(image, bcp);
        }
    }

    /** A Windows command line's arguments, as CommandLineToArgvW splits them. */
    static List<String> splitCommandLine(String s) {
        List<String> args = new ArrayList<>();
        StringBuilder cur = new StringBuilder();
        boolean inArg = false;
        boolean quoted = false;
        for (int i = 0; i < s.length(); ) {
            char c = s.charAt(i);
            if (!quoted && (c == ' ' || c == '\t')) {
                if (inArg) {
                    args.add(cur.toString());
                    cur.setLength(0);
                    inArg = false;
                }
                i++;
                continue;
            }
            inArg = true;
            if (c == '\\') {
                int n = 0;
                while (i < s.length() && s.charAt(i) == '\\') {
                    n++;
                    i++;
                }
                if (i < s.length() && s.charAt(i) == '"') {
                    for (int k = 0; k < n / 2; k++) cur.append('\\');
                    if (n % 2 == 1) {
                        cur.append('"');
                        i++;
                    }
                } else {
                    for (int k = 0; k < n; k++) cur.append('\\');
                }
                continue;
            }
            if (c == '"') {
                if (quoted && i + 1 < s.length() && s.charAt(i + 1) == '"') {
                    cur.append('"');
                    i += 2;
                    continue;
                }
                quoted = !quoted;
                i++;
                continue;
            }
            cur.append(c);
            i++;
        }
        if (inArg) args.add(cur.toString());
        return args;
    }

    private static boolean deleteTree(File f) {
        File[] children = f.listFiles();
        if (children != null) {
            for (File c : children) deleteTree(c);
        }
        try {
            Files.deleteIfExists(f.toPath());
            return true;
        } catch (IOException e) {
            return false;
        }
    }
}
