/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.server;

import android.content.pm.dex.DexMetadataHelper;
import android.content.pm.parsing.result.ParseResult;
import android.content.pm.parsing.result.ParseTypeImpl;

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
 * last update times, uid and install instance, tab-separated), which the system server
 * reads at start, as
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
        final PackageIdentity identity;

        Entry(String packageName, String codePath, long firstInstallTime,
                long lastUpdateTime, PackageIdentity identity) {
            this.packageName = packageName;
            this.codePath = codePath;
            this.firstInstallTime = firstInstallTime;
            this.lastUpdateTime = lastUpdateTime;
            this.identity = identity;
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
        if (Files.notExists(f.toPath(), java.nio.file.LinkOption.NOFOLLOW_LINKS)) return entries;
        if (!Files.isRegularFile(f.toPath(), java.nio.file.LinkOption.NOFOLLOW_LINKS)) {
            throw new IOException("packages.list is not a regular identity registry");
        }
        List<String[]> legacy = new ArrayList<>();
        java.util.Set<Integer> uids = new java.util.HashSet<>();
        for (String line : Files.readAllLines(f.toPath(), StandardCharsets.UTF_8)) {
            if (line.isEmpty() || line.startsWith("#")) continue;
            String[] fields = line.split("\t", -1);
            if (fields.length == 4) {
                legacy.add(fields);
                continue;
            }
            if (fields.length != 6) throw new IOException("invalid packages.list record");
            try {
                PackageIdentity identity = new PackageIdentity(fields[0],
                        Integer.parseInt(fields[4]), fields[5]);
                if (!uids.add(identity.uid) || entries.containsKey(fields[0])) {
                    throw new IllegalArgumentException("duplicate package identity");
                }
                entries.put(fields[0], new Entry(fields[0], fields[1],
                        Long.parseLong(fields[2]), Long.parseLong(fields[3]), identity));
            } catch (IllegalArgumentException e) {
                throw new IOException("invalid packages.list identity", e);
            }
        }
        // One-time migration: reserve all existing six-column identities first.
        // Publish the upgraded registry before exposing any migrated package.
        for (String[] fields : legacy) {
            try {
                if (entries.containsKey(fields[0])) throw new IllegalArgumentException("duplicate package");
                List<PackageIdentity> identities = new ArrayList<>();
                for (Entry e : entries.values()) identities.add(e.identity);
                PackageIdentity identity = PackageIdentity.allocate(fields[0], identities);
                entries.put(fields[0], new Entry(fields[0], fields[1],
                        Long.parseLong(fields[2]), Long.parseLong(fields[3]), identity));
            } catch (IllegalArgumentException | IllegalStateException e) {
                throw new IOException("cannot migrate packages.list", e);
            }
        }
        if (!legacy.isEmpty()) writeRegistry(entries);
        return entries;
    }

    private void writeRegistry() throws IOException {
        writeRegistry(mEntries);
    }

    private static void writeRegistry(Map<String, Entry> entries) throws IOException {
        File f = registry();
        f.getParentFile().mkdirs();
        File tmp = new File(f.getPath() + ".tmp");
        try (Writer w = new OutputStreamWriter(new FileOutputStream(tmp),
                StandardCharsets.UTF_8)) {
            w.write("# package\tcode path\tfirst install\tlast update\tuid\tinstall instance\n");
            for (Entry e : entries.values()) {
                w.write(e.packageName + "\t" + e.codePath + "\t" + e.firstInstallTime + "\t"
                        + e.lastUpdateTime + "\t" + e.identity.uid + "\t"
                        + e.identity.installInstance + "\n");
            }
        }
        // Losing the old identity file on a failed replace would reassign live uids.
        // Fail closed on a provider without atomic replace; never delete the old file.
        Files.move(tmp.toPath(), f.toPath(), java.nio.file.StandardCopyOption.ATOMIC_MOVE,
                java.nio.file.StandardCopyOption.REPLACE_EXISTING);
    }

    /**
     * The installed packages, parsed from their code paths, as PackageManagerService scans
     * them at boot. An unreadable package aborts startup: silently dropping its reserved
     * uid could allocate it to a different package while its install record survives.
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
                throw new IOException("installed package " + e.packageName + " at "
                        + e.codePath + " does not parse", ex);
            }
        }
        return result;
    }

    /** The installer of the system server's package manager (after loadInstalled). */
    static Installer create(PackageManagerService pm, ActivityTaskManagerService atm)
            throws IOException {
        Installer installer = new Installer(pm, atm, readRegistry());
        // Background dexopt with the apps' JIT profiles, while the device is idle (ADR 0054).
        Dexopt.startBackground(installer::codePaths, atm::idleSince);
        return installer;
    }

    /** The installed packages' code paths, by package. */
    synchronized Map<String, String> codePaths() {
        Map<String, String> paths = new LinkedHashMap<>();
        for (Entry e : mEntries.values()) paths.put(e.packageName, e.codePath);
        return paths;
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
        p.setIdentity(e.identity);
        new File(p.dataDir()).mkdirs();
        return p;
    }

    /**
     * Installs or updates the package in {@code apk}. Returns the package name, what was
     * done ({@code installed}, {@code updated}, {@code unchanged}), the code path and the
     * version code.
     */
    synchronized String[] install(String apk) throws Exception {
        synchronized (mPm.launchGate) {
            return installGuarded(apk);
        }
    }

    private String[] installGuarded(String apk) throws Exception {
        File source = new File(apk);
        if (!source.isFile()) throw new IllegalArgumentException("no such file: " + apk);
        ApkPackage parsed = ApkPackage.parse(source.getAbsolutePath());
        String pkg = parsed.packageName;
        File metadata = dexMetadata(source);
        if (metadata.isFile()) {
            ParseResult<?> result = DexMetadataHelper.validateDexMetadataFile(
                    ParseTypeImpl.forDefaultParsing(), metadata.getPath(), pkg, parsed.versionCode());
            if (result.isError()) {
                throw new IllegalArgumentException("INSTALL_FAILED_BAD_DEX_METADATA: "
                        + result.getErrorMessage(), result.getException());
            }
        }
        Entry old = mEntries.get(pkg);
        if (old != null && sameContent(source, new File(old.codePath, "base.apk"))
                && sameMetadata(metadata, new File(old.codePath, "base.dm"))
                && mPm.get(pkg) != null) {
            SystemServer.log("install " + apk + ": " + pkg + " is installed, unchanged ("
                    + old.codePath + ")");
            // A byte-identical APK can depend on changed XML, jar bytes or loader policy.
            boolean libraryChange = !parsed.sharedLibraries.matchesArtifacts(Dexopt.oatDir(old.codePath));
            if (libraryChange || !new File(Dexopt.oatDir(old.codePath), "base.odex").isFile()) {
                if (libraryChange) {
                    mAtm.forceStopPackage(pkg);
                    SystemServer.log("install " + pkg + ": shared-library context/content changed");
                }
                Dexopt.onInstall(pkg, old.codePath, parsed.sharedLibraries);
                mPm.add(open(old));
            }
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
        if (metadata.isFile()) copy(metadata, new File(code, "base.dm"));
        int libs = NativeLibs.extract(base.getPath(), new File(code, "lib/"
                + NativeLibs.instructionSetDir(NativeLibs.forApk(base.getPath()))).getPath());
        // dexopt, as the package manager runs it at install (Dexopt; ADR 0054).
        Dexopt.onInstall(pkg, code.getPath(), parsed.sharedLibraries);
        long now = System.currentTimeMillis();
        Entry entry = new Entry(pkg, code.getPath(), old != null ? old.firstInstallTime : now,
                now, old != null ? old.identity : mPm.installIdentity(pkg));
        ApkPackage installed = open(entry);
        mEntries.put(pkg, entry);
        try {
            writeRegistry();
        } catch (IOException e) {
            if (old == null) mEntries.remove(pkg);
            else mEntries.put(pkg, old);
            throw e;
        }
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
        synchronized (mPm.launchGate) {
            return uninstallGuarded(packageName);
        }
    }

    private boolean uninstallGuarded(String packageName) throws IOException {
        Entry e = mEntries.get(packageName);
        if (e == null) return false;
        mAtm.forceStopPackage(packageName);
        mEntries.remove(packageName);
        try {
            writeRegistry();
        } catch (IOException failure) {
            mEntries.put(packageName, e);
            throw failure;
        }
        mPm.remove(packageName);
        delete(new File(e.codePath));
        delete(new File(Device.dataDir(), "data/" + packageName));
        Dexopt.deleteProfiles(packageName);
        // Its toasts and its AppUserModelID's registration go too (hwsvc2, ADR 0051).
        NotificationManagerService notifications = NotificationManagerService.sLast;
        if (notifications != null) notifications.packageRemoved(packageName);
        SystemServer.log("uninstalled " + packageName + " (" + e.codePath + ")");
        return true;
    }

    private static File dexMetadata(File apk) {
        String path = apk.getPath();
        return new File(path.endsWith(".apk") ? DexMetadataHelper.buildDexMetadataPathForApk(path)
                : path + ".dm");
    }

    private static boolean sameMetadata(File source, File installed) throws IOException {
        return source.isFile() ? sameContent(source, installed) : !installed.exists();
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
