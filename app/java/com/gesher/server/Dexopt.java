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
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.util.function.Supplier;

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
    private static final long TOOL_TIMEOUT_MS = 10 * 60_000;

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
     * by default), instead of the device's interpreter-only options. {@code
     * GESHER_APP_EXECUTION=interpret} keeps the device's options.
     */
    static String appVmOptions(String vm) {
        if ("interpret".equals(System.getenv("GESHER_APP_EXECUTION"))
                || !SystemProperties.getBoolean("dalvik.vm.usejit", true)) {
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
        prepareProfiles(packageName);
        String filter = filterFor("install", "speed-profile");
        File ref = referenceProfile(packageName);
        File metadata = new File(codePath, "base.dm");
        File profile = metadata.isFile() ? importCloudProfile(packageName, codePath, metadata)
                : hasData(ref) ? ref : null;
        if (filter.endsWith("-profile") && profile == null) filter = "verify";
        return compile(packageName, codePath, filter, "install", profile, libraries);
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
    static synchronized String compile(String packageName, String codePath, String filter,
            String reason, File profile) {
        try {
            ApkPackage parsed = ApkPackage.parse(new File(codePath, "base.apk").getPath());
            if (!parsed.packageName.equals(packageName)) {
                throw new IOException("compiled APK package name does not match " + packageName);
            }
            return compile(packageName, codePath, filter, reason, profile, parsed.sharedLibraries);
        } catch (Exception invalidLibrary) {
            SystemServer.log("dexopt: " + packageName + ": library resolution failed: " + invalidLibrary);
            return null;
        }
    }

    private static synchronized String compile(String packageName, String codePath, String filter,
            String reason, File profile, SharedLibraryGraph libraries) {
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
            a.add(r);
        }
        if (profile != null) a.add("--profile-file=" + profile.getPath());
        if (appImage) {
            a.add("--app-image-file=" + new File(newDir, "base.art").getPath());
            a.add("--image-format=lz4");
        }
        // dex2oat's watchdog thread needs pthread_condattr_setclock, which gesher-base
        // lacks (tools/boot-image/compile.ps1 turns it off too).
        a.add("--no-watch-dog");
        a.add("-j4");
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
        File oat = new File(codePath, "oat");
        File old = new File(codePath, "oat.old");
        deleteTree(old);
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

    /**
     * BackgroundDexOptService: every few seconds, once no app process has run or started for
     * {@code gesher.dexopt.idle-ms} (default 2000), the packages whose current profile has
     * new data are compiled with their merged profile (pm.dexopt.bg-dexopt).
     */
    static synchronized void startBackground(Packages packages, Supplier<Long> idleSince) {
        if (sBackground != null) return;
        long idleMs = SystemProperties.getLong("gesher.dexopt.idle-ms", 2000);
        Map<String, Long> done = new HashMap<>();  // package -> the profile's time compiled
        sBackground = new Thread(() -> {
            while (true) {
                SystemClock.sleep(1000);
                try {
                    long since = idleSince.get();
                    if (since == 0 || SystemClock.uptimeMillis() - since < idleMs) continue;
                    // The packages used last first.
                    List<Map.Entry<String, String>> order =
                            new ArrayList<>(packages.codePaths().entrySet());
                    order.sort((x, y) -> Long.compare(currentProfile(y.getKey()).lastModified(),
                            currentProfile(x.getKey()).lastModified()));
                    for (Map.Entry<String, String> e : order) {
                        String pkg = e.getKey();
                        File cur = currentProfile(pkg);
                        if (!hasData(cur)) continue;
                        Long t = done.get(pkg);
                        if (t != null && t == cur.lastModified()) continue;
                        if (idleSince.get() == 0) break;  // an app starts: later
                        done.put(pkg, cur.lastModified());
                        backgroundDexopt(pkg, e.getValue());
                    }
                } catch (Throwable t) {
                    SystemServer.log("dexopt: background dexopt failed: " + t);
                }
            }
        }, "bg-dexopt");
        sBackground.setDaemon(true);
        sBackground.start();
        SystemServer.log("dexopt: background dexopt runs once the device is idle for " + idleMs
                + " ms (" + filterFor("bg-dexopt", "speed-profile") + ")");
    }

    /**
     * One package: profman merges its current profile into the reference profile (and says
     * whether that is worth a compile), then dex2oat compiles it with the reference profile.
     */
    static String backgroundDexopt(String packageName, String codePath) {
        File cur = currentProfile(packageName);
        File ref = referenceProfile(packageName);
        ref.getParentFile().mkdirs();
        String apk = new File(codePath, "base.apk").getPath();
        List<String> a = new ArrayList<>();
        a.add("--profile-file=" + cur.getPath());
        a.add("--reference-profile-file=" + ref.getPath());
        a.add("--apk=" + apk);
        a.add("--dex-location=" + apk);
        // As artd merges since Android 14: compile unless the merge added nothing.
        a.add("--force-merge-and-analyze");
        int code = run("profman", a);
        // ProfmanResult: 1 compile, 2 small delta, 7 empty profiles.
        SystemServer.log("dexopt: " + packageName + ": profman merged " + cur.length()
                + " bytes of profile into " + ref + " (" + ref.length() + " bytes): "
                + (code == 1 ? "compile" : code == 2 ? "small delta, no compile"
                        : code == 7 ? "empty profiles" : "exit code " + code));
        if (code != 1) return null;
        String filter = filterFor("bg-dexopt", "speed-profile");
        String what = compile(packageName, codePath, filter, "bg-dexopt", ref);
        if (what != null) {
            // Merged: the app's runtime starts its current profile anew.
            try {
                new FileOutputStream(cur).close();
            } catch (IOException e) {
                // left as it is
            }
        }
        return what;
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
        int pid;
        try {
            pid = Zygote.spawn(cmd.toString(), List.of(), log.getPath(), false);
        } catch (RuntimeException e) {
            SystemServer.log("dexopt: " + e.getMessage());
            return -1;
        }
        long deadline = SystemClock.uptimeMillis() + TOOL_TIMEOUT_MS;
        while (true) {
            int code = Zygote.exitCode(pid);
            if (code != Zygote.STILL_ACTIVE) return code;
            if (SystemClock.uptimeMillis() > deadline) {
                Zygote.kill(pid);
                SystemServer.log("dexopt: " + tool + " took more than " + TOOL_TIMEOUT_MS / 1000
                        + " s; stopped");
                return -2;
            }
            SystemClock.sleep(50);
        }
    }

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
