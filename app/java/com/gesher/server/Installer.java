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
import java.util.Arrays;
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
    /**
     * The files packages were last installed from (path, size, last write time, those of
     * its .dm, package, code path): opening the same file again is "unchanged" without
     * reading it, as the byte comparison of a 200 MB APK took seconds at every launch.
     */
    private static final String SOURCES = "system/install-sources.list";

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
    private boolean mRemovalActive;

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
        try (FileOutputStream out = new FileOutputStream(tmp);
                Writer w = new OutputStreamWriter(out,
                StandardCharsets.UTF_8)) {
            w.write("# package\tcode path\tfirst install\tlast update (Gesher, ADR 0026)\n");
            for (Entry e : mEntries.values()) {
                w.write(e.packageName + "\t" + e.codePath + "\t" + e.firstInstallTime + "\t"
                        + e.lastUpdateTime + "\n");
            }
            w.flush(); out.getFD().sync();
        }
        try {
            Files.move(tmp.toPath(), f.toPath(), java.nio.file.StandardCopyOption.ATOMIC_MOVE,
                    java.nio.file.StandardCopyOption.REPLACE_EXISTING);
        } catch (java.nio.file.AtomicMoveNotSupportedException unavailable) {
            Files.move(tmp.toPath(), f.toPath(), java.nio.file.StandardCopyOption.REPLACE_EXISTING);
        }
    }

    /**
     * The installed packages, parsed from their code paths, as PackageManagerService scans
     * them at boot. A package that does not parse any more is left out (and logged).
     */
    static List<ApkPackage> loadInstalled() throws IOException {
        List<ApkPackage> result = new ArrayList<>();
        Map<String, Entry> entries = readRegistry();
        Map<String, String> installed = new LinkedHashMap<>();
        for (Entry entry : entries.values()) installed.put(entry.packageName, entry.codePath);
        PackageRemovalStorage.recover(installed);
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
        Installer installer = new Installer(pm, atm, readRegistry());
        pm.attachInstaller(atm, installer);
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

    /** The split APKs beside {@code apk} (an extracted .xapk's config.*.apk), or null. */
    /**
     * The ABIs {@code apk} and its companion splits ship native code for, if none of
     * them is one Gesher runs ({@link NativeLibs#SUPPORTED}); null if one is, or if
     * there is no native code.
     */
    private static String[] unsupportedAbis(File apk) throws IOException {
        List<File> files = new ArrayList<>();
        files.add(apk);
        File[] splits = companionSplits(apk);
        if (splits != null) files.addAll(Arrays.asList(splits));
        java.util.Set<String> abis = new java.util.TreeSet<>();
        for (File f : files) {
            try (java.util.zip.ZipFile zip = new java.util.zip.ZipFile(f)) {
                for (java.util.Enumeration<? extends java.util.zip.ZipEntry> e = zip.entries();
                        e.hasMoreElements(); ) {
                    String[] parts = e.nextElement().getName().split("/");
                    if (parts.length == 3 && parts[0].equals("lib") && parts[2].endsWith(".so")) {
                        abis.add(parts[1]);
                    }
                }
            }
        }
        for (String abi : NativeLibs.SUPPORTED) {
            if (abis.contains(abi)) return null;
        }
        return abis.isEmpty() ? null : abis.toArray(new String[0]);
    }

    private static File[] companionSplits(File apk) {
        File dir = apk.getParentFile();
        if (dir == null) return null;
        return dir.listFiles((d, name) -> (name.startsWith("split_") || name.startsWith("config."))
                && name.endsWith(".apk") && !name.equals(apk.getName()));
    }

    /**
     * Whether the installed splits are {@code splits}, by name and content: installing
     * an .xapk over the same base.apk installed alone adds its splits (its ABI split's
     * native libraries), so it is not "unchanged". No splits match any installed ones.
     */
    private static boolean sameSplits(File[] splits, String codePath) throws IOException {
        String[] installed = new File(codePath).list(
                (dir, name) -> name.startsWith("split_") && name.endsWith(".apk"));
        int count = splits != null ? splits.length : 0;
        // base.apk alone over the same base installed with its splits (an .xapk's)
        // keeps them: without its density split the app misses its drawables.
        if (count == 0) return true;
        if ((installed != null ? installed.length : 0) != count) return false;
        for (int i = 0; i < count; i++) {
            String name = splits[i].getName();
            File target = new File(codePath, name.startsWith("split_") ? name : "split_" + name);
            if (!sameContent(splits[i], target)) return false;
        }
        return true;
    }

    /** base.apk and the split APKs installed beside it (split_*.apk). */
    private static String[] apksOf(String codePath) {
        List<String> apks = new ArrayList<>();
        apks.add(codePath + "/base.apk");
        String[] names = new File(codePath).list(
                (dir, name) -> name.startsWith("split_") && name.endsWith(".apk"));
        if (names != null) {
            Arrays.sort(names);
            for (String name : names) apks.add(codePath + "/" + name);
        }
        return apks.toArray(new String[0]);
    }

    private static ApkPackage open(Entry e) throws Exception {
        ApkPackage p = ApkPackage.parse(e.codePath + "/base.apk");
        if (!p.packageName.equals(e.packageName)) {
            throw new IllegalStateException("base.apk is " + p.packageName);
        }
        p.setDataDir(Device.dataDir() + "/data/" + p.packageName);
        // lib/x86_64, or lib/arm64 for an arm64-v8a-only package (docs/arm64.md); its
        // splits count (an .xapk's libraries are in config.arm64_v8a.apk).
        String[] apks = apksOf(e.codePath);
        String abi = NativeLibs.forApk(apks);
        p.setNativeLibraryDir(e.codePath + "/lib/" + NativeLibs.instructionSetDir(abi));
        p.setPrimaryCpuAbi(NativeLibs.abiFor(apks));
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
        return install(apk, null);
    }

    /** Server-owned immutable session metadata, copied before package publication. */
    synchronized String[] install(String apk, InstallSessionMetadata sessionMetadata) throws Exception {
        // A failed rollback is in doubt: no subsequent publication may skip it and
        // thereby turn recoverable old data into a committed deletion.
        recoverRemovals();
        File source = new File(apk);
        if (!source.isFile()) throw new IllegalArgumentException("no such file: " + apk);
        final File original = source;
        Entry recorded = sessionMetadata == null ? recordedSource(original) : null;
        if (recorded != null) {
            ApkPackage parsed = ApkPackage.parse(recorded.codePath + "/base.apk");
            return unchanged(apk, recorded.packageName, parsed, recorded);
        }
        File tempDir = null;
        try {
            if (source.getName().toLowerCase().endsWith(".xapk")
                    || source.getName().toLowerCase().endsWith(".apks")) {
                tempDir = Files.createTempDirectory("gesher-xapk-").toFile();
                extractZip(source, tempDir);
                File[] apks = tempDir.listFiles((dir, name) -> name.endsWith(".apk"));
                if (apks == null || apks.length == 0) {
                    throw new IllegalArgumentException("no apk found in " + apk);
                }
                File mainApk = null;
                for (File a : apks) {
                    if (a.getName().equals("base.apk")
                            || (!a.getName().startsWith("config.") && !a.getName().startsWith("split_"))) {
                        mainApk = a;
                        break;
                    }
                }
                if (mainApk == null) mainApk = apks[0];
                source = mainApk;
            }
            ApkPackage parsed = ApkPackage.parse(source.getAbsolutePath());
            String pkg = parsed.packageName;
            File metadata = dexMetadata(source);
            if (metadata.isFile()) {
                ParseResult<?> result = DexMetadataHelper.validateDexMetadataFile(
                        ParseTypeImpl.forParsingWithoutPlatformCompat(), metadata.getPath(), pkg, parsed.versionCode());
                if (result.isError()) {
                    throw new IllegalArgumentException("INSTALL_FAILED_BAD_DEX_METADATA: "
                            + result.getErrorMessage(), result.getException());
                }
            }
            String[] foreign = unsupportedAbis(source);
            if (foreign != null) {
                // Native code only for ABIs Gesher cannot run (32-bit armeabi-v7a, x86):
                // refused, as a device refuses it, instead of an app whose libraries
                // never load (and an update that replaces a working install).
                throw new IllegalArgumentException("INSTALL_FAILED_NO_MATCHING_ABIS: "
                        + source.getName() + " has native code only for "
                        + String.join(", ", foreign) + "; Gesher runs "
                        + String.join(" and ", NativeLibs.SUPPORTED)
                        + " (use the app's arm64-v8a, x86_64 or universal APK)");
            }
            Entry old = mEntries.get(pkg);
            ApkPackage previous = mPm.get(pkg);
            android.content.pm.Signature[] newSigners = parsed.verifiedSignatures();
            // Unsigned developer APKs remain supported, but a claimed signing block
            // must actually verify. A signed install cannot be downgraded to unsigned
            // or replaced by a different signer while retaining its UID and data.
            if (newSigners == null && hasSigningMaterial(source)) {
                throw new SecurityException("INSTALL_FAILED_BAD_SIGNATURE: " + pkg);
            }
            if (previous != null && previous.hasVerifiedSigningIdentity()
                    && !parsed.updateContinuityFrom(previous)) {
                throw new SecurityException("INSTALL_FAILED_UPDATE_INCOMPATIBLE: " + pkg);
            }
            File[] incomingSplits = companionSplits(source);
            if (incomingSplits != null) {
                for (File split : incomingSplits) {
                    android.content.pm.Signature[] splitSigners = ApkPackage.verifySignatures(split.getPath());
                    if (!Arrays.equals(newSigners, splitSigners)
                            || (splitSigners == null && hasSigningMaterial(split))) {
                        throw new SecurityException("INSTALL_FAILED_BAD_SPLIT_SIGNATURE: " + pkg);
                    }
                }
            }
            if (old != null && sameContent(source, new File(old.codePath, "base.apk"))
                    && sameMetadata(metadata, new File(old.codePath, "base.dm"))
                    && sameSplits(companionSplits(source), old.codePath)
                    && (sessionMetadata == null || sessionMetadata.matches(new File(old.codePath)))
                    && mPm.get(pkg) != null) {
                recordSource(original, pkg, old.codePath);
                return unchanged(apk, pkg, parsed, old);
            }
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
            int occupied = 0;
            while (Files.exists(code.toPath(), java.nio.file.LinkOption.NOFOLLOW_LINKS)
                    || PackageRemovalStorage.reserves(code.toPath())) {
                if (++occupied > 10000 || n == Integer.MAX_VALUE) throw new IOException("No free package code generation");
                code = new File(Device.dataDir(), "app/" + pkg + "-" + (++n));
            }
            code.mkdirs();
            File base = new File(code, "base.apk");
            copy(source, base);
            if (metadata.isFile()) copy(metadata, new File(code, "base.dm"));
            File[] companionSplits = companionSplits(source);
            if (companionSplits != null) {
                for (File split : companionSplits) {
                    String targetName = split.getName().startsWith("split_")
                            ? split.getName() : "split_" + split.getName();
                    copy(split, new File(code, targetName));
                    File splitMetadata = dexMetadata(split);
                    if (splitMetadata.isFile()) copy(splitMetadata,
                            new File(code, InstallSessionMetadata.dexName(targetName)));
                }
            }
            if (sessionMetadata != null) sessionMetadata.copyTo(code);
            // Verification must apply to the bytes being published, not a mutable
            // download that could change after its initial parse/verification.
            ApkPackage staged = ApkPackage.parse(base.getAbsolutePath());
            android.content.pm.Signature[] stagedSigners = staged.verifiedSignatures();
            if (!pkg.equals(staged.packageName) || staged.versionCode() != parsed.versionCode()
                    || !Arrays.equals(newSigners, stagedSigners)
                    || (stagedSigners == null && hasSigningMaterial(base))
                    || (previous != null && previous.hasVerifiedSigningIdentity()
                            && !staged.updateContinuityFrom(previous))) {
                deleteTree(code);
                throw new SecurityException("INSTALL_FAILED_CHANGED_APK: " + pkg);
            }
            for (String copied : apksOf(code.getPath())) {
                if (new File(copied).equals(base)) continue;
                File split = new File(copied);
                android.content.pm.Signature[] copiedSigners = ApkPackage.verifySignatures(copied);
                if (!Arrays.equals(stagedSigners, copiedSigners)
                        || (copiedSigners == null && hasSigningMaterial(split))) {
                    deleteTree(code);
                    throw new SecurityException("INSTALL_FAILED_CHANGED_SPLIT: " + pkg);
                }
            }
            if (old != null || previous != null) mAtm.forceStopPackage(pkg);
            if (previous != null && !previous.hasVerifiedSigningIdentity()) {
                mPm.permissions().forgetPackage(pkg);
                NotificationManagerService notifications = NotificationManagerService.sLast;
                if (notifications != null) notifications.packageRemoved(pkg);
            }
            // The package's ABI over base.apk and its splits: an ABI split's libraries
            // (an .xapk's config.arm64_v8a.apk) go to lib/arm64 and the package runs as
            // arm64-v8a, not as an x86_64 package without its libraries.
            String[] apks = apksOf(code.getPath());
            String abi = NativeLibs.forApk(apks);
            String libDir = new File(code, "lib/" + NativeLibs.instructionSetDir(abi)).getPath();
            int libs = 0;
            for (String file : apks) libs += NativeLibs.extract(file, libDir, abi);
            // dexopt, as the package manager runs it at install (Dexopt; ADR 0054).
            Dexopt.onInstall(pkg, code.getPath(), parsed.sharedLibraries);
            long now = System.currentTimeMillis();
            Entry entry = new Entry(pkg, code.getPath(), old != null ? old.firstInstallTime : now,
                    now);
            ApkPackage installed = open(entry);
            if (previous != null && previous.isSystemAddon()) {
                if (!GmsSignatureSpoofing.verifyAddonApk(pkg, installed.apkPath)) {
                    throw new SecurityException("INSTALL_FAILED_UNTRUSTED_ADDON_UPDATE: " + pkg);
                }
                installed.setSystemAddon(true);
            }
            mEntries.put(pkg, entry);
            writeRegistry();
            mPm.add(installed);
            if (old != null && !old.codePath.equals(entry.codePath)) {
                delete(new File(old.codePath));
            }
            String what = old != null ? "updated" : "installed";
            SystemServer.log(what + " " + installed.describe() + ", in " + code.getPath()
                    + (libs > 0 ? ", " + libs + " native libraries (" + abi
                            + ")" : ""));
            recordSource(original, pkg, code.getPath());
            mPm.packageInstalled(installed, old != null || previous != null, this);
            return new String[] {pkg, what, code.getPath(), String.valueOf(installed.versionCode())};
        } finally {
            if (tempDir != null) deleteTree(tempDir);
        }
    }

    /** The "unchanged" answer for {@code pkg}, installed in {@code old}. */
    private String[] unchanged(String apk, String pkg, ApkPackage parsed, Entry old)
            throws Exception {
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

    /** {@code file}'s line in install-sources.list: path, size, time, .dm size and time. */
    private static String sourceKey(File file) throws IOException {
        File dm = dexMetadata(file);
        return file.getCanonicalPath() + "\t" + file.length() + "\t" + file.lastModified() + "\t"
                + (dm.isFile() ? dm.length() + "\t" + dm.lastModified() : "-1\t-1");
    }

    /**
     * The installed package {@code source} was last installed from, if the file (and its
     * .dm) has the same size and last write time and the package is still installed from
     * that code path; null otherwise.
     */
    private Entry recordedSource(File source) {
        try {
            File f = new File(Device.dataDir(), SOURCES);
            if (!f.isFile()) return null;
            String key = sourceKey(source) + "\t";
            for (String line : Files.readAllLines(f.toPath(), StandardCharsets.UTF_8)) {
                if (!line.startsWith(key)) continue;
                String[] rest = line.substring(key.length()).split("\t");
                if (rest.length < 2) return null;
                Entry e = mEntries.get(rest[0]);
                if (e == null || !e.codePath.equals(rest[1]) || mPm.get(e.packageName) == null
                        || !new File(e.codePath, "base.apk").isFile()) {
                    return null;
                }
                return e;
            }
        } catch (IOException e) {
            SystemServer.log("install-sources.list: " + e);
        }
        return null;
    }

    /** Records that {@code source} is installed as {@code pkg} in {@code codePath}. */
    private static void recordSource(File source, String pkg, String codePath) {
        try {
            File f = new File(Device.dataDir(), SOURCES);
            String path = source.getCanonicalPath() + "\t";
            List<String> lines = new ArrayList<>();
            if (f.isFile()) {
                for (String line : Files.readAllLines(f.toPath(), StandardCharsets.UTF_8)) {
                    if (!line.startsWith(path)) lines.add(line);
                }
            }
            lines.add(sourceKey(source) + "\t" + pkg + "\t" + codePath);
            while (lines.size() > 200) lines.remove(0);
            f.getParentFile().mkdirs();
            Files.write(f.toPath(), lines, StandardCharsets.UTF_8);
        } catch (IOException e) {
            SystemServer.log("install-sources.list: " + e);
        }
    }

    private static void extractZip(File zipFile, File destDir) throws IOException {
        try (java.util.zip.ZipFile zip = new java.util.zip.ZipFile(zipFile)) {
            java.util.Enumeration<? extends java.util.zip.ZipEntry> entries = zip.entries();
            while (entries.hasMoreElements()) {
                java.util.zip.ZipEntry entry = entries.nextElement();
                if (entry.isDirectory()) continue;
                String name = entry.getName();
                if (!name.endsWith(".apk") && !name.endsWith(".dm")) continue;
                File out = new File(destDir, new File(name).getName());
                try (InputStream in = zip.getInputStream(entry);
                        OutputStream os = new FileOutputStream(out)) {
                    byte[] buf = new byte[64 * 1024];
                    for (int n; (n = in.read(buf)) > 0; ) os.write(buf, 0, n);
                }
            }
        }
    }

    synchronized boolean ordinaryInstalled(ApkPackage target) {
        if (target == null || target.isSystemAddon()) return false;
        Entry entry = mEntries.get(target.packageName);
        return entry != null && mPm.get(target.packageName) == target
                && new File(entry.codePath, "base.apk").equals(new File(target.apkPath));
    }

    private void recoverRemovals() throws IOException {
        if (mRemovalActive) throw new IllegalStateException("Package removal is active");
        PackageRemovalStorage.recover(codePaths());
        mPm.recoveredPackageRemovals();
    }

    /** Uninstalls only a persistent ordinary package, retaining its allocated UID tombstone. */
    synchronized boolean uninstall(String packageName) throws IOException {
        recoverRemovals();
        Entry e = mEntries.get(packageName);
        if (e == null) return false;
        ApkPackage target = mPm.get(packageName);
        if (!ordinaryInstalled(target)) throw new SecurityException("System or nonpersistent package");
        PackageRemovalStorage quarantine = new PackageRemovalStorage(packageName, e.codePath);
        mPm.beginPackageRemoval(target);
        mRemovalActive = true;
        boolean committed = false, restored = false;
        try {
            mPm.checkPackageRemoval(packageName);
            mAtm.forceStopPackage(packageName);
            // Fail closed on persistent revocation errors. A failed removal can leave
            // fewer grants, but never restores a capability across a later reinstall.
            mPm.revokePackageCapabilities(packageName);
            mPm.permissions().forgetPackage(packageName);
            NotificationManagerService notifications = NotificationManagerService.sLast;
            if (notifications != null) notifications.packageRemoved(packageName);
            // Retire any background start already queued while capabilities were canceled.
            mAtm.forceStopPackage(packageName);
            quarantine.stage();
            mEntries.remove(packageName);
            writeRegistry();
            committed = true;
            mPm.remove(packageName);
        } catch (IOException | RuntimeException failure) {
            if (!committed) {
                mEntries.put(packageName, e);
                try { quarantine.rollback(); restored = true; }
                catch (IOException restore) { failure.addSuppressed(restore); }
            }
            throw failure;
        } finally {
            mRemovalActive = false;
            if (committed || restored) mPm.endPackageRemoval(target);
        }
        quarantine.discard(); // Locked remnants stay private and are reaped at next boot.
        Dexopt.deleteProfiles(packageName);
        mPm.packageRemoved(target, this);
        SystemServer.log("uninstalled " + packageName + " (" + e.codePath + ")");
        return true;
    }

    private static File dexMetadata(File apk) {
        String path = apk.getPath();
        return new File(path.endsWith(".apk") ? DexMetadataHelper.buildDexMetadataPathForApk(path)
                : path + ".dm");
    }

    private static boolean hasSigningMaterial(File apk) throws IOException {
        if (android.util.apk.ApkSignatureSchemeV2Verifier.hasSignature(apk.getPath())
                || android.util.apk.ApkSignatureSchemeV3Verifier.hasSignature(apk.getPath())) return true;
        try (java.util.zip.ZipFile zip = new java.util.zip.ZipFile(apk)) {
            java.util.Enumeration<? extends java.util.zip.ZipEntry> entries = zip.entries();
            while (entries.hasMoreElements()) {
                String name = entries.nextElement().getName().toUpperCase(java.util.Locale.ROOT);
                if (name.startsWith("META-INF/") && (name.endsWith(".RSA")
                        || name.endsWith(".DSA") || name.endsWith(".EC"))) return true;
            }
        }
        return false;
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
        return PackageRemovalStorage.deleteNoFollow(f.toPath());
    }
}
