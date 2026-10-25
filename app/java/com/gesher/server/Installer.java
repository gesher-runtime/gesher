/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.server;

import com.gesher.app.NativeLibs;

import java.io.BufferedInputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.io.OutputStreamWriter;
import java.io.Writer;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;

/**
 * Installed packages, as PackageManagerService installs them: the APK is copied into
 * {@code $ANDROID_DATA/app/<package>-<n>/base.apk}, its {@code lib/x86_64} libraries
 * extracted next to it ({@code lib/x86_64}, the package's nativeLibraryDir), its data
 * directory is {@code $ANDROID_DATA/data/<package>}, and {@code
 * $ANDROID_DATA/system/packages.list} lists them (package, code path, first install and
 * last update times, tab-separated), which the system server reads at start, as
 * PackageManagerService reads packages.xml. An update (a different APK for an installed
 * package) gets a new {@code <n>}, after the package's processes are stopped; the same APK
 * again changes nothing. Uninstalling removes the code, the data and the entry.
 */
final class Installer {
    private static final String REGISTRY = "system/packages.list";

    /** An installed package's entry. */
    private static final class Entry {
        final String packageName;
        final String codePath;
        final long firstInstallTime;
        final long lastUpdateTime;

        Entry(String packageName, String codePath, long firstInstallTime,
                long lastUpdateTime) {
            this.packageName = packageName;
            this.codePath = codePath;
            this.firstInstallTime = firstInstallTime;
            this.lastUpdateTime = lastUpdateTime;
        }
    }

    private final PackageManagerService mPm;
    private final ActivityTaskManagerService mAtm;
    private final Map<String, Entry> mEntries;

    private Installer(PackageManagerService pm, ActivityTaskManagerService atm,
            Map<String, Entry> entries) {
        mPm = pm;
        mAtm = atm;
        mEntries = entries;
    }

    private static File registry() {
        return new File(Device.dataDir(), REGISTRY);
    }

    private static Map<String, Entry> readRegistry() throws IOException {
        Map<String, Entry> entries = new LinkedHashMap<>();
        File f = registry();
        if (!f.isFile()) return entries;
        for (String line : Files.readAllLines(f.toPath(), StandardCharsets.UTF_8)) {
            String[] fields = line.split("\t");
            if (fields.length < 4 || line.startsWith("#")) continue;
            try {
                entries.put(fields[0], new Entry(fields[0], fields[1],
                        Long.parseLong(fields[2]), Long.parseLong(fields[3])));
            } catch (NumberFormatException e) {
                SystemServer.log("packages.list: ignoring the line " + line);
            }
        }
        return entries;
    }

    private void writeRegistry() throws IOException {
        File f = registry();
        f.getParentFile().mkdirs();
        File tmp = new File(f.getPath() + ".tmp");
        try (Writer w = new OutputStreamWriter(new FileOutputStream(tmp),
                StandardCharsets.UTF_8)) {
            w.write("# package\tcode path\tfirst install\tlast update (Gesher, ADR 0026)\n");
            for (Entry e : mEntries.values()) {
                w.write(e.packageName + "\t" + e.codePath + "\t" + e.firstInstallTime + "\t"
                        + e.lastUpdateTime + "\n");
            }
        }
        if (!tmp.renameTo(f)) {
            f.delete();
            if (!tmp.renameTo(f)) throw new IOException("cannot write " + f);
        }
    }

    /**
     * The installed packages, parsed from their code paths, as PackageManagerService scans
     * them at boot. A package that does not parse any more is left out (and logged).
     */
    static List<ApkPackage> loadInstalled() throws IOException {
        List<ApkPackage> result = new ArrayList<>();
        Map<String, Entry> entries = readRegistry();
        // Code directories no entry names: an uninstall or update that could not delete
        // them (a file still open), or one that did not finish.
        java.util.Set<String> kept = new java.util.HashSet<>();
        for (Entry e : entries.values()) kept.add(new File(e.codePath).getName());
        File[] dirs = new File(Device.dataDir(), "app").listFiles();
        if (dirs != null) {
            for (File d : dirs) {
                if (d.isDirectory() && !kept.contains(d.getName())) {
                    SystemServer.log("removing " + d + ": no installed package's");
                    deleteTree(d);
                }
            }
        }
        for (Entry e : entries.values()) {
            try {
                result.add(open(e));
            } catch (Exception ex) {
                SystemServer.log("installed package " + e.packageName + " at " + e.codePath
                        + " does not parse: " + ex);
            }
        }
        return result;
    }

    /** The installer of the system server's package manager (after loadInstalled). */
    static Installer create(PackageManagerService pm, ActivityTaskManagerService atm)
            throws IOException {
        return new Installer(pm, atm, readRegistry());
    }

    private static ApkPackage open(Entry e) throws Exception {
        ApkPackage p = ApkPackage.parse(e.codePath + "/base.apk");
        if (!p.packageName.equals(e.packageName)) {
            throw new IllegalStateException("base.apk is " + p.packageName);
        }
        p.setDataDir(Device.dataDir() + "/data/" + p.packageName);
        // lib/x86_64, or lib/arm64 for an arm64-v8a-only APK (docs/arm64.md).
        String abi = NativeLibs.forApk(e.codePath + "/base.apk");
        p.setNativeLibraryDir(e.codePath + "/lib/" + NativeLibs.instructionSetDir(abi));
        p.setPrimaryCpuAbi(NativeLibs.abiFor(e.codePath + "/base.apk"));
        p.setInstallTimes(e.firstInstallTime, e.lastUpdateTime);
        new File(p.dataDir()).mkdirs();
        return p;
    }

    /**
     * Installs or updates the package in {@code apk}. Returns the package name, what was
     * done ({@code installed}, {@code updated}, {@code unchanged}), the code path and the
     * version code.
     */
    synchronized String[] install(String apk) throws Exception {
        File source = new File(apk);
        if (!source.isFile()) throw new IllegalArgumentException("no such file: " + apk);
        ApkPackage parsed = ApkPackage.parse(source.getAbsolutePath());
        String pkg = parsed.packageName;
        Entry old = mEntries.get(pkg);
        if (old != null && sameContent(source, new File(old.codePath, "base.apk"))
                && mPm.get(pkg) != null) {
            SystemServer.log("install " + apk + ": " + pkg + " is installed, unchanged ("
                    + old.codePath + ")");
            return new String[] {pkg, "unchanged", old.codePath,
                    String.valueOf(parsed.versionCode())};
        }
        if (old != null || mPm.get(pkg) != null) mAtm.forceStopPackage(pkg);
        int n = 1;
        if (old != null) {
            String name = new File(old.codePath).getName();
            try {
                n = Integer.parseInt(name.substring(name.lastIndexOf('-') + 1)) + 1;
            } catch (NumberFormatException e) {
                n = 2;
            }
        }
        File code = new File(Device.dataDir(), "app/" + pkg + "-" + n);
        deleteTree(code);
        code.mkdirs();
        File base = new File(code, "base.apk");
        copy(source, base);
        int libs = NativeLibs.extract(base.getPath(), new File(code, "lib/"
                + NativeLibs.instructionSetDir(NativeLibs.forApk(base.getPath()))).getPath());
        long now = System.currentTimeMillis();
        Entry entry = new Entry(pkg, code.getPath(), old != null ? old.firstInstallTime : now,
                now);
        ApkPackage installed = open(entry);
        mEntries.put(pkg, entry);
        writeRegistry();
        mPm.add(installed);
        if (old != null && !old.codePath.equals(entry.codePath)) {
            delete(new File(old.codePath));
        }
        String what = old != null ? "updated" : "installed";
        SystemServer.log(what + " " + installed.describe() + ", in " + code.getPath()
                + (libs > 0 ? ", " + libs + " native libraries (" + NativeLibs.forApk(base.getPath())
                        + ")" : ""));
        return new String[] {pkg, what, code.getPath(), String.valueOf(installed.versionCode())};
    }

    /** Uninstalls the package: its processes, code, data and entry. */
    synchronized boolean uninstall(String packageName) throws IOException {
        Entry e = mEntries.get(packageName);
        if (e == null) return false;
        mAtm.forceStopPackage(packageName);
        mPm.remove(packageName);
        mEntries.remove(packageName);
        writeRegistry();
        delete(new File(e.codePath));
        delete(new File(Device.dataDir(), "data/" + packageName));
        SystemServer.log("uninstalled " + packageName + " (" + e.codePath + ")");
        return true;
    }

    private static boolean sameContent(File a, File b) throws IOException {
        if (!b.isFile() || a.length() != b.length()) return false;
        try (InputStream x = new BufferedInputStream(new FileInputStream(a));
                InputStream y = new BufferedInputStream(new FileInputStream(b))) {
            byte[] bx = new byte[64 * 1024];
            byte[] by = new byte[64 * 1024];
            while (true) {
                int n = readFully(x, bx);
                int m = readFully(y, by);
                if (n != m) return false;
                if (n == 0) return true;
                for (int i = 0; i < n; i++) {
                    if (bx[i] != by[i]) return false;
                }
            }
        }
    }

    private static int readFully(InputStream in, byte[] buffer) throws IOException {
        int total = 0;
        for (int n; total < buffer.length
                && (n = in.read(buffer, total, buffer.length - total)) > 0; ) {
            total += n;
        }
        return total;
    }

    private static void copy(File from, File to) throws IOException {
        byte[] buffer = new byte[64 * 1024];
        try (InputStream in = new FileInputStream(from);
                OutputStream out = new FileOutputStream(to)) {
            for (int n; (n = in.read(buffer)) > 0; ) {
                out.write(buffer, 0, n);
            }
        }
    }

    /**
     * Deletes a tree. The system server may still have the package's APK open (the parser's
     * assets, closed when collected): it tries again after a collection, and a code
     * directory left behind is removed at the next start.
     */
    private static void delete(File f) {
        for (int i = 0; i < 5 && !deleteTree(f); i++) {
            System.gc();
            System.runFinalization();
            android.os.SystemClock.sleep(200);
        }
        if (f.exists()) SystemServer.log("cannot delete " + f + " now");
    }

    private static boolean deleteTree(File f) {
        File[] children = f.listFiles();
        if (children != null) {
            for (File c : children) deleteTree(c);
        }
        // Not File.delete: that is remove(3), which removes no directory on Windows' C
        // runtime; java.nio's delete calls rmdir for one.
        try {
            Files.deleteIfExists(f.toPath());
            return true;
        } catch (IOException e) {
            return false;
        }
    }
}
