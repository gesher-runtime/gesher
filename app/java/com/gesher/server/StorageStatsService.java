/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.server;

import android.app.usage.ExternalStorageStats;
import android.app.usage.IStorageStatsManager;
import android.app.usage.StorageStats;
import android.content.pm.ParceledListSlice;
import android.os.storage.StorageManager;

import java.io.File;
import java.io.IOException;
import java.nio.file.FileVisitResult;
import java.nio.file.Files;
import java.nio.file.LinkOption;
import java.nio.file.NoSuchFileException;
import java.nio.file.Path;
import java.nio.file.SimpleFileVisitor;
import java.nio.file.attribute.BasicFileAttributes;
import java.util.Locale;
import java.util.Objects;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collections;
import java.util.List;

/**
 * {@code storagestats}: StorageStatsManager (the storage screens of file managers and
 * cleaners). Without it getSystemService answered null and those apps showed no sizes
 * or failed ("No service published for: storagestats").
 *
 * <p>There is one volume, the private data volume (its emulated shared view is
 * {@code media/0} on the same Windows drive), and one user. Sizes are measured on the host
 * files: an app's code directory, both private storage areas with their caches, and its directories
 * under {@code media/0/Android}. Overlapping identical requests share a scan; distinct
 * scans have bounded concurrency. Incomplete walks report an I/O error instead of a
 * successful underestimate. Quotas and reserved space
 * are not supported, as on a device without ext4 quotas.
 *
 * <p>As on Android, an app may ask about itself; anything else (other apps, the user's
 * totals, the volume's cache) needs PACKAGE_USAGE_STATS. That is an app-op permission the
 * user grants in Settings' "Usage access"; Gesher has no such screen and its usage stats
 * are open, so an app that declares the permission in its manifest has it. An app that
 * does not cannot list or size other apps.
 */
final class StorageStatsService extends IStorageStatsManager.Default {
    static final int MAX_ENTRIES = 200_000;

    private final PackageManagerService mPm;
    private final File mData;
    private final StorageStatsScans mScans = new StorageStatsScans();

    /** Capture both installed-object identity and paths before a query can wait. */
    private static final class PackageScan {
        final ApkPackage owner;
        final String name;
        final String apk;
        final String ce;
        final String de;
        final int uid;

        PackageScan(ApkPackage p) {
            owner = p;
            name = p.packageName;
            apk = p.apkPath;
            ce = p.credentialProtectedDataDir();
            de = p.deviceProtectedDataDir();
            uid = p.uid();
        }

        @Override public boolean equals(Object value) {
            if (!(value instanceof PackageScan)) return false;
            PackageScan p = (PackageScan) value;
            return owner == p.owner && uid == p.uid && Objects.equals(apk, p.apk)
                    && Objects.equals(ce, p.ce) && Objects.equals(de, p.de);
        }

        @Override public int hashCode() {
            return Objects.hash(System.identityHashCode(owner), uid, apk, ce, de);
        }
    }

    StorageStatsService(PackageManagerService pm) {
        this(pm, android.os.Environment.getDataDirectory());
    }

    StorageStatsService(PackageManagerService pm, File dataDirectory) {
        mPm = Objects.requireNonNull(pm);
        mData = Objects.requireNonNull(dataDirectory);
    }

    private static void volume(String volumeUuid) {
        // StorageStatsManager.convert: UUID_DEFAULT arrives as null (UUID_PRIVATE_INTERNAL),
        // the emulated shared view as "primary_physical". Both are the one data volume.
        if (volumeUuid != null && !volumeUuid.equals(StorageManager.UUID_PRIMARY_PHYSICAL)
                && !volumeUuid.equals(StorageManager.UUID_DEFAULT.toString())) {
            throw new IllegalArgumentException("Unknown volume " + volumeUuid);
        }
    }

    private static void user(int userId) {
        if (userId != 0) throw new IllegalArgumentException("Unknown user " + userId);
    }

    /** The caller is the system, or an app that declares PACKAGE_USAGE_STATS. */
    private boolean callerHasUsageAccess() {
        int uid = android.os.Binder.getCallingUid();
        // The shell holds PACKAGE_USAGE_STATS on Android (host commands run as it here).
        if (uid == android.os.Process.SYSTEM_UID || uid == android.os.Process.ROOT_UID
                || uid == android.os.Process.SHELL_UID || uid == android.os.Process.myUid()) {
            return true;
        }
        String[] packages = mPm.getPackagesForUid(uid);
        if (packages == null) return false;
        for (String name : packages) {
            ApkPackage p = mPm.get(name);
            if (p != null && p.parsed.getRequestedPermissions()
                    .contains(android.Manifest.permission.PACKAGE_USAGE_STATS)) {
                return true;
            }
        }
        return false;
    }

    private void enforceUsageAccess() {
        if (!callerHasUsageAccess()) {
            throw new SecurityException("uid " + android.os.Binder.getCallingUid()
                    + " does not have " + android.Manifest.permission.PACKAGE_USAGE_STATS);
        }
    }

    /** Its own uid, or usage access (as StorageStatsService.enforceStatsPermission). */
    private void enforceUidOrUsageAccess(int uid) {
        if (android.os.Binder.getCallingUid() != uid) enforceUsageAccess();
    }

    @Override
    public boolean isQuotaSupported(String volumeUuid, String callingPackage) {
        return false;
    }

    @Override
    public boolean isReservedSupported(String volumeUuid, String callingPackage) {
        return false;
    }

    @Override
    public long getTotalBytes(String volumeUuid, String callingPackage) {
        volume(volumeUuid);
        return mData.getTotalSpace();
    }

    @Override
    public long getFreeBytes(String volumeUuid, String callingPackage) {
        volume(volumeUuid);
        return mData.getUsableSpace();
    }

    @Override
    public long getCacheBytes(String volumeUuid, String callingPackage) {
        volume(volumeUuid);
        enforceUsageAccess();
        List<PackageScan> packages = packages(null);
        long[] result = scan(0, packages, () -> {
            int[] budget = {MAX_ENTRIES};
            long total = 0;
            for (PackageScan p : packages) total += cacheBytes(p, budget);
            return new long[] {total};
        });
        enforceUsageAccess();
        return result[0];
    }

    @Override
    public long getCacheQuotaBytes(String volumeUuid, int uid, String callingPackage) {
        volume(volumeUuid);
        enforceUidOrUsageAccess(uid);
        // No quota daemon: the share of free space Android's default policy allows one app.
        return Math.max(0, mData.getUsableSpace() / 10);
    }

    @Override
    public StorageStats queryStatsForPackage(String volumeUuid, String packageName, int userId,
            String callingPackage) {
        volume(volumeUuid);
        user(userId);
        ApkPackage p = packageName == null ? null : mPm.get(packageName);
        // Checked before the lookup answers, so an app without usage access cannot probe names.
        if (p == null || p.uid() != android.os.Binder.getCallingUid()) enforceUsageAccess();
        if (p == null || mPm.isRemovalPending(packageName)) {
            throw new android.os.ParcelableException(
                    new android.content.pm.PackageManager.NameNotFoundException(packageName));
        }
        List<PackageScan> packages = Collections.singletonList(new PackageScan(p));
        long[] result = scan(1, packages, () -> totals(packages));
        // Permission decisions belong to each caller, never to the shared result.
        enforceUidOrUsageAccess(packages.get(0).uid);
        return stats(result);
    }

    @Override
    public StorageStats queryStatsForUid(String volumeUuid, int uid, String callingPackage) {
        volume(volumeUuid);
        enforceUidOrUsageAccess(uid);
        List<PackageScan> packages = packages(uid);
        long[] result = scan(2, packages, () -> totals(packages));
        enforceUidOrUsageAccess(uid);
        return stats(result);
    }

    @Override
    public StorageStats queryStatsForUser(String volumeUuid, int userId, String callingPackage) {
        volume(volumeUuid);
        user(userId);
        enforceUsageAccess();
        List<PackageScan> packages = packages(null);
        long[] result = scan(3, packages, () -> totals(packages));
        enforceUsageAccess();
        return stats(result);
    }

    @Override
    public ExternalStorageStats queryExternalStatsForUser(String volumeUuid, int userId,
            String callingPackage) {
        volume(volumeUuid);
        user(userId);
        enforceUsageAccess();
        long[] result = scan(4, Collections.emptyList(), () -> externalTotals());
        enforceUsageAccess();
        ExternalStorageStats stats = new ExternalStorageStats();
        stats.totalBytes = result[0];
        stats.audioBytes = result[1];
        stats.videoBytes = result[2];
        stats.imageBytes = result[3];
        stats.appBytes = result[4];
        stats.obbBytes = result[5];
        return stats;
    }

    private long[] externalTotals() throws IOException {
        File shared = new File(mData, "media/0");
        long[] byKind = new long[6];  // total, audio, video, image, app, obb
        walk(shared, new int[] {MAX_ENTRIES}, (path, size) -> {
            byKind[0] += size;
            String rel = shared.toPath().relativize(path).toString().replace('\\', '/');
            if (rel.startsWith("Android/obb/")) {
                byKind[5] += size;
                byKind[4] += size;
                return;
            }
            if (rel.startsWith("Android/data/") || rel.startsWith("Android/media/")) {
                byKind[4] += size;
                return;
            }
            switch (kind(rel)) {
                case 1: byKind[1] += size; break;
                case 2: byKind[2] += size; break;
                case 3: byKind[3] += size; break;
                default: break;
            }
        });
        return byKind;
    }

    @Override
    public ParceledListSlice queryCratesForPackage(String volumeUuid, String packageName,
            int userId, String callingPackage) {
        return ParceledListSlice.emptyList();
    }

    @Override
    public ParceledListSlice queryCratesForUid(String volumeUuid, int uid, String callingPackage) {
        return ParceledListSlice.emptyList();
    }

    @Override
    public ParceledListSlice queryCratesForUser(String volumeUuid, int userId,
            String callingPackage) {
        return ParceledListSlice.emptyList();
    }

    /** 1 audio, 2 video, 3 image, 0 other: by extension, as MediaProvider's MIME map would. */
    static int kind(String name) {
        int dot = name.lastIndexOf('.');
        if (dot < 0 || name.lastIndexOf('/') > dot) return 0;
        switch (name.substring(dot + 1).toLowerCase(Locale.ROOT)) {
            case "mp3": case "m4a": case "aac": case "ogg": case "oga": case "opus": case "flac":
            case "wav": case "amr": case "mid": case "midi": case "wma":
                return 1;
            case "mp4": case "m4v": case "3gp": case "3gpp": case "mkv": case "webm": case "avi":
            case "mov": case "wmv": case "ts":
                return 2;
            case "jpg": case "jpeg": case "png": case "gif": case "webp": case "bmp": case "heic":
            case "heif": case "avif": case "dng": case "svg":
                return 3;
            default:
                return 0;
        }
    }

    private List<PackageScan> packages(Integer uid) {
        List<PackageScan> result = new ArrayList<>();
        for (ApkPackage p : mPm.packages()) {
            if ((uid == null || p.uid() == uid) && !mPm.isRemovalPending(p.packageName)) {
                result.add(new PackageScan(p));
            }
        }
        return result;
    }

    private void validate(List<PackageScan> packages) throws IOException {
        for (PackageScan p : packages) {
            if (mPm.get(p.name) != p.owner || mPm.isRemovalPending(p.name)
                    || !p.equals(new PackageScan(p.owner))) {
                throw new IOException("Package changed during storage query: " + p.name);
            }
        }
    }

    private long[] scan(int kind, List<PackageScan> packages, StorageStatsScans.Work work) {
        try {
            long[] result = mScans.execute(Arrays.asList(kind, packages), () -> {
                validate(packages);
                long[] measured = work.run();
                validate(packages);
                return measured;
            });
            // A joined reader may resume after an install/uninstall completed.
            validate(packages);
            return result;
        } catch (IOException failure) {
            // StorageStatsManager unwraps this checked I/O failure across Binder.
            throw new android.os.ParcelableException(failure);
        }
    }

    private long[] totals(List<PackageScan> packages) throws IOException {
        long[] result = new long[5];
        int[] budget = {MAX_ENTRIES};
        for (PackageScan p : packages) add(result, p, budget);
        return result;
    }

    private static StorageStats stats(long[] result) {
        // Never expose the shared scan's result as a mutable Binder value object.
        StorageStats stats = new StorageStats();
        stats.codeBytes = result[0];
        stats.apkBytes = result[1];
        stats.dataBytes = result[2];
        stats.cacheBytes = result[3];
        stats.externalCacheBytes = result[4];
        return stats;
    }

    private void add(long[] stats, PackageScan p, int[] budget) throws IOException {
        long code = codeBytes(p, budget);
        long cache = cacheBytes(p, budget);
        File external = new File(mData, "media/0/Android/data/" + p.name);
        long externalCache = size(new File(external, "cache"), budget);
        // Android's dataBytes includes the cache directories and the app's external dirs.
        long data = size(directory(p.ce), budget)
                + size(directory(p.de), budget)
                + size(external, budget)
                + size(new File(mData, "media/0/Android/media/" + p.name), budget);
        stats[0] += code;
        stats[1] += p.apk != null ? fileBytes(new File(p.apk)) : 0;
        stats[2] += data;
        stats[3] += cache + externalCache;
        stats[4] += externalCache;
    }

    /** The installed code directory (data/app/<package>-N: APKs, native libraries, oat). */
    private long codeBytes(PackageScan p, int[] budget) throws IOException {
        if (p.apk == null) return 0;
        File apk = new File(p.apk);
        File dir = apk.getParentFile();
        File apps = new File(mData, "app");
        if (dir != null && apps.equals(dir.getParentFile())) return size(dir, budget);
        return fileBytes(apk);
    }

    private static long fileBytes(File file) throws IOException {
        StorageStatsScans.checkInterrupted();
        BasicFileAttributes a = Files.readAttributes(file.toPath(), BasicFileAttributes.class,
                LinkOption.NOFOLLOW_LINKS);
        return a.isRegularFile() ? a.size() : 0;
    }

    private long cacheBytes(PackageScan p, int[] budget) throws IOException {
        long total = 0;
        for (String path : new String[] {p.ce, p.de}) {
            if (path == null) continue;
            File data = new File(path);
            total += size(new File(data, "cache"), budget) + size(new File(data, "code_cache"), budget);
        }
        return total;
    }

    private static File directory(String path) { return path == null ? null : new File(path); }

    private interface Visitor {
        void file(Path path, long size);
    }

    static long size(File root, int[] budget) throws IOException {
        long[] total = {0};
        walk(root, budget, (path, size) -> total[0] += size);
        return total[0];
    }

    /**
     * Every regular file under root without following links. One entry budget covers
     * the whole query. Missing optional roots are empty; traversal errors or limits
     * invalidate the whole result, never silently turn a partial sum into success.
     */
    private static void walk(File root, int[] budget, Visitor visitor) throws IOException {
        StorageStatsScans.checkInterrupted();
        if (root == null) return;
        int[] entries = budget;
        Files.walkFileTree(root.toPath(), Collections.emptySet(), 64,
                    new SimpleFileVisitor<Path>() {
                        private void visit() throws IOException {
                            StorageStatsScans.checkInterrupted();
                            if (entries[0]-- <= 0) throw new IOException("Storage scan entry limit");
                        }

                        @Override
                        public FileVisitResult preVisitDirectory(Path dir, BasicFileAttributes a)
                                throws IOException {
                            visit();
                            return FileVisitResult.CONTINUE;
                        }

                        @Override
                        public FileVisitResult visitFile(Path file, BasicFileAttributes a)
                                throws IOException {
                            visit();
                            // At maxDepth, walkFileTree visits a directory as a file.
                            if (a.isDirectory()) throw new IOException("Storage scan depth limit");
                            if (a.isRegularFile()) visitor.file(file, a.size());
                            return FileVisitResult.CONTINUE;
                        }

                        @Override
                        public FileVisitResult visitFileFailed(Path file, IOException e)
                                throws IOException {
                            StorageStatsScans.checkInterrupted();
                            if (file.equals(root.toPath()) && e instanceof NoSuchFileException) {
                                return FileVisitResult.CONTINUE;
                            }
                            throw e;
                        }
                    });
    }
}
