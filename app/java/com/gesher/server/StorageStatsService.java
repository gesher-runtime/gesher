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
import java.nio.file.Path;
import java.nio.file.SimpleFileVisitor;
import java.nio.file.attribute.BasicFileAttributes;
import java.util.Locale;
import java.util.Objects;

/**
 * {@code storagestats}: StorageStatsManager (the storage screens of file managers and
 * cleaners). Without it getSystemService answered null and those apps showed no sizes
 * or failed ("No service published for: storagestats").
 *
 * <p>There is one volume, the private data volume (its emulated shared view is
 * {@code media/0} on the same Windows drive), and one user. Sizes are measured on the host
 * files: an app's code directory, both private storage areas with their caches, and its directories
 * under {@code media/0/Android}. Walks stop after {@link #MAX_ENTRIES} entries, so a huge
 * tree answers an underestimate instead of blocking the caller. Quotas and reserved space
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
        int[] budget = {MAX_ENTRIES};
        long total = 0;
        for (ApkPackage p : mPm.packages()) total += cacheBytes(p, budget);
        return total;
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
        StorageStats stats = new StorageStats();
        add(stats, p, new int[] {MAX_ENTRIES});
        return stats;
    }

    @Override
    public StorageStats queryStatsForUid(String volumeUuid, int uid, String callingPackage) {
        volume(volumeUuid);
        enforceUidOrUsageAccess(uid);
        StorageStats stats = new StorageStats();
        int[] budget = {MAX_ENTRIES};
        for (ApkPackage p : mPm.packages()) {
            if (p.uid() == uid && !mPm.isRemovalPending(p.packageName)) add(stats, p, budget);
        }
        return stats;
    }

    @Override
    public StorageStats queryStatsForUser(String volumeUuid, int userId, String callingPackage) {
        volume(volumeUuid);
        user(userId);
        enforceUsageAccess();
        StorageStats stats = new StorageStats();
        int[] budget = {MAX_ENTRIES};
        for (ApkPackage p : mPm.packages()) {
            if (!mPm.isRemovalPending(p.packageName)) add(stats, p, budget);
        }
        return stats;
    }

    @Override
    public ExternalStorageStats queryExternalStatsForUser(String volumeUuid, int userId,
            String callingPackage) {
        volume(volumeUuid);
        user(userId);
        enforceUsageAccess();
        File shared = new File(mData, "media/0");
        ExternalStorageStats stats = new ExternalStorageStats();
        long[] byKind = new long[5];  // total, audio, video, image, app (Android/data|media|obb)
        walk(shared, new int[] {MAX_ENTRIES}, (path, size) -> {
            byKind[0] += size;
            String rel = shared.toPath().relativize(path).toString().replace('\\', '/');
            if (rel.startsWith("Android/obb/")) {
                stats.obbBytes += size;
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
        stats.totalBytes = byKind[0];
        stats.audioBytes = byKind[1];
        stats.videoBytes = byKind[2];
        stats.imageBytes = byKind[3];
        stats.appBytes = byKind[4];
        return stats;
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

    private void add(StorageStats stats, ApkPackage p, int[] budget) {
        long code = codeBytes(p, budget);
        long cache = cacheBytes(p, budget);
        File external = new File(mData, "media/0/Android/data/" + p.packageName);
        long externalCache = size(new File(external, "cache"), budget);
        // Android's dataBytes includes the cache directories and the app's external dirs.
        long data = size(directory(p.credentialProtectedDataDir()), budget)
                + size(directory(p.deviceProtectedDataDir()), budget)
                + size(external, budget)
                + size(new File(mData, "media/0/Android/media/" + p.packageName), budget);
        stats.codeBytes += code;
        stats.apkBytes += p.apkPath != null ? new File(p.apkPath).length() : 0;
        stats.dataBytes += data;
        stats.cacheBytes += cache + externalCache;
        stats.externalCacheBytes += externalCache;
    }

    /** The installed code directory (data/app/<package>-N: APKs, native libraries, oat). */
    private long codeBytes(ApkPackage p, int[] budget) {
        if (p.apkPath == null) return 0;
        File apk = new File(p.apkPath);
        File dir = apk.getParentFile();
        File apps = new File(mData, "app");
        if (dir != null && apps.equals(dir.getParentFile())) return size(dir, budget);
        return apk.length();
    }

    private long cacheBytes(ApkPackage p, int[] budget) {
        long total = 0;
        for (String path : new String[] {p.credentialProtectedDataDir(), p.deviceProtectedDataDir()}) {
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

    static long size(File root, int[] budget) {
        long[] total = {0};
        walk(root, budget, (path, size) -> total[0] += size);
        return total[0];
    }

    /**
     * Every regular file under root, not following links, while the query's budget of
     * entries lasts: one budget for all the walks of a query, so a huge tree does not hold
     * a binder thread.
     */
    private static void walk(File root, int[] budget, Visitor visitor) {
        if (root == null || budget[0] <= 0 || !root.exists()) return;
        int[] entries = budget;
        try {
            Files.walkFileTree(root.toPath(), java.util.Collections.emptySet(), 64,
                    new SimpleFileVisitor<Path>() {
                        @Override
                        public FileVisitResult preVisitDirectory(Path dir, BasicFileAttributes a) {
                            return --entries[0] <= 0 ? FileVisitResult.TERMINATE
                                    : FileVisitResult.CONTINUE;
                        }

                        @Override
                        public FileVisitResult visitFile(Path file, BasicFileAttributes a) {
                            if (a.isRegularFile()) visitor.file(file, a.size());
                            return --entries[0] <= 0 ? FileVisitResult.TERMINATE
                                    : FileVisitResult.CONTINUE;
                        }

                        @Override
                        public FileVisitResult visitFileFailed(Path file, IOException e) {
                            return --entries[0] <= 0 ? FileVisitResult.TERMINATE
                                    : FileVisitResult.CONTINUE;
                        }
                    });
        } catch (IOException | RuntimeException e) {
            // What was counted so far: a partial size, never a failure of the caller's query.
        }
    }
}
