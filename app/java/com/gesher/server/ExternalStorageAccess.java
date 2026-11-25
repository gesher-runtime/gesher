/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.server;

import android.Manifest;
import android.app.AppOpsManager;
import android.content.pm.PackageManager;
import android.os.UserHandle;

import java.io.File;
import java.io.IOException;
import java.io.UncheckedIOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.AtomicMoveNotSupportedException;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.Paths;
import java.nio.file.StandardCopyOption;
import java.util.Locale;
import java.util.Set;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.atomic.AtomicLong;

/**
 * Android {@code MANAGE_EXTERNAL_STORAGE} and scoped/external storage policy for
 * system_server, persisted per package under {@code system/storage-policy/<pkg>}.
 */
public final class ExternalStorageAccess {
    public static final String PERMISSION = Manifest.permission.MANAGE_EXTERNAL_STORAGE;

    public static final int MEDIA_IMAGES = 1;
    public static final int MEDIA_VIDEO = 2;
    public static final int MEDIA_AUDIO = 4;
    public static final int MEDIA_ALL = MEDIA_IMAGES | MEDIA_VIDEO | MEDIA_AUDIO;

    private static final Set<String> STORAGE_PERMISSIONS = Set.of(
            PERMISSION,
            Manifest.permission.READ_EXTERNAL_STORAGE,
            Manifest.permission.WRITE_EXTERNAL_STORAGE,
            Manifest.permission.READ_MEDIA_IMAGES,
            Manifest.permission.READ_MEDIA_VIDEO,
            Manifest.permission.READ_MEDIA_AUDIO,
            Manifest.permission.READ_MEDIA_VISUAL_USER_SELECTED);

    public enum Mode {
        MANAGER,
        LEGACY_RW,
        LEGACY_RO,
        MEDIA_RO,
        SCOPED
    }

    public record Policy(Mode mode, int mediaMask) { }

    private static final Policy SCOPED = new Policy(Mode.SCOPED, 0);
    private static final Object PUBLICATION_LOCK = new Object();
    private static final AtomicLong PUBLICATION_REVISION = new AtomicLong();
    private record PackageKey(PackageManagerService pm, String name) { }
    private record PublicationFailure(ApkPackage pkg, IOException cause) { }
    private static final ConcurrentHashMap<PackageKey, PublicationFailure> FAILURES =
            new ConcurrentHashMap<>();

    /** Only the filesystem boundary is replaceable by isolated slow/failing-disk tests. */
    interface SnapshotWriter {
        void write(ApkPackage pkg, Policy policy, File target) throws IOException;
    }
    private static volatile SnapshotWriter sWriter = ExternalStorageAccess::writeAtomically;

    private ExternalStorageAccess() { }

    public static boolean isStoragePermission(String permission) {
        return permission != null && STORAGE_PERMISSIONS.contains(permission);
    }

    public static boolean declaresPermission(ApkPackage pkg) {
        return pkg != null && pkg.parsed.getRequestedPermissions().contains(PERMISSION);
    }

    public static boolean isExternalStorageManager(PackageManagerService pm, ApkPackage pkg) {
        if (pm == null || pkg == null) return false;
        return !publicationFailed(pm, pkg.packageName) && requestedExternalStorageManager(pm, pkg);
    }

    private static boolean requestedExternalStorageManager(PackageManagerService pm, ApkPackage pkg) {
        int mode = pm.appOps().checkRawModeForStoragePolicy(
                AppOpsManager.OP_MANAGE_EXTERNAL_STORAGE, pkg.uid(), pkg.packageName);
        if (mode == AppOpsManager.MODE_ALLOWED) return true;
        if (mode == AppOpsManager.MODE_DEFAULT) {
            return UserHandle.isCore(pkg.uid())
                    || pm.permissions().checkUidPermission(pkg.uid(), PERMISSION, 0)
                            == PackageManager.PERMISSION_GRANTED;
        }
        return false;
    }

    public static Policy computePolicy(PackageManagerService pm, ApkPackage pkg) {
        if (pm == null || pkg == null) return new Policy(Mode.SCOPED, 0);
        if (requestedExternalStorageManager(pm, pkg)) {
            return new Policy(Mode.MANAGER, MEDIA_ALL);
        }
        int targetSdk = pkg.parsed.getTargetSdkVersion();
        boolean isLegacy = targetSdk < 29
                || (targetSdk == 29 && pkg.parsed.isRequestLegacyExternalStorage());

        boolean hasReadExternal = hasUidPermission(pm, pkg, Manifest.permission.READ_EXTERNAL_STORAGE);
        boolean hasWriteExternal = hasUidPermission(pm, pkg, Manifest.permission.WRITE_EXTERNAL_STORAGE);
        boolean hasImages = hasUidPermission(pm, pkg, Manifest.permission.READ_MEDIA_IMAGES);
        boolean hasVideo = hasUidPermission(pm, pkg, Manifest.permission.READ_MEDIA_VIDEO);
        boolean hasAudio = hasUidPermission(pm, pkg, Manifest.permission.READ_MEDIA_AUDIO);
        // Selected visual-media consent applies to individual items, not a whole
        // media category. This path policy cannot represent those item grants.
        int mediaMask = 0;
        if (hasImages) mediaMask |= MEDIA_IMAGES;
        if (hasVideo) mediaMask |= MEDIA_VIDEO;
        if (hasAudio) mediaMask |= MEDIA_AUDIO;
        if (targetSdk <= 32 && hasReadExternal) mediaMask = MEDIA_ALL;

        if (isLegacy && hasWriteExternal) {
            return new Policy(Mode.LEGACY_RW, MEDIA_ALL);
        }
        if (isLegacy && hasReadExternal) {
            return new Policy(Mode.LEGACY_RO, MEDIA_ALL);
        }
        if (mediaMask != 0) {
            return new Policy(Mode.MEDIA_RO, mediaMask);
        }
        return new Policy(Mode.SCOPED, 0);
    }

    private static boolean hasUidPermission(PackageManagerService pm, ApkPackage pkg, String permission) {
        if (pm.permissions().checkUidPermission(pkg.uid(), permission, 0)
                != PackageManager.PERMISSION_GRANTED) return false;
        int op = AppOpsManager.permissionToOpCode(permission);
        return op == AppOpsManager.OP_NONE || pm.appOps().checkRawModeForStoragePolicy(op, pkg.uid(), pkg.packageName)
                == AppOpsManager.MODE_ALLOWED;
    }

    static boolean publicationFailed(PackageManagerService pm, String packageName) {
        PublicationFailure failure = FAILURES.get(new PackageKey(pm, packageName));
        return failure != null && pm.get(packageName) == failure.pkg();
    }

    static boolean isStorageOp(int op) {
        return op == AppOpsManager.OP_MANAGE_EXTERNAL_STORAGE
                || isStoragePermission(AppOpsManager.opToPermission(op));
    }

    public static File policyFile(String packageName) {
        if (packageName == null || !packageName.matches("[A-Za-z0-9_]+(?:\\.[A-Za-z0-9_]+)*")
                || !isDataDirAvailable()) return null;
        return new File(new File(Device.dataDir(), "system/storage-policy"), packageName);
    }

    private static boolean isDataDirAvailable() {
        String env = System.getenv("ANDROID_DATA");
        if (env != null && !env.isEmpty()) return true;
        return new File(Device.dataDir()).isDirectory();
    }

    public static void syncPackage(PackageManagerService pm, ApkPackage pkg) {
        if (pm == null || pkg == null || pkg.uid() < 0) return;
        File file = policyFile(pkg.packageName);
        if (file == null) return;
        PackageKey key = new PackageKey(pm, pkg.packageName);
        long revision = PUBLICATION_REVISION.incrementAndGet();
        PublicationFailure failure;
        for (;;) {
            if (pm.get(pkg.packageName) != pkg || pm.isRemovalPending(pkg.packageName)) return;
            // Never acquire PM, PermissionPolicy or AppOps monitors under PUBLICATION_LOCK.
            // Some callers already hold an installer/PM publication monitor. A newer
            // request invalidates a snapshot computed before waiting for disk publication.
            Policy policy = computePolicy(pm, pkg);
            synchronized (PUBLICATION_LOCK) {
                long current = PUBLICATION_REVISION.get();
                if (revision != current) { revision = current; continue; }
                if (pm.get(pkg.packageName) != pkg || pm.isRemovalPending(pkg.packageName)) return;
                try {
                    // Revoke native access before touching ACLs. A failed downgrade must
                    // not leave the old permissive policy active in running processes.
                    sWriter.write(pkg, SCOPED, file);
                    AppSandbox.syncStorageAccess(pkg.packageName, policy);
                    if (!policy.equals(SCOPED)) sWriter.write(pkg, policy, file);
                    FAILURES.remove(key);
                    return;
                } catch (IOException error) {
                    try { Files.deleteIfExists(file.toPath()); }
                    catch (IOException cleanup) { error.addSuppressed(cleanup); }
                    failure = new PublicationFailure(pkg, error);
                    FAILURES.put(key, failure);
                }
            }
            break;
        }
        // Retire only this failed publication, after releasing the file/ACL monitor.
        // A later successful retry removes the marker before a delayed retirement runs.
        Services.Created services = Services.sLast;
        PublicationFailure failed = failure;
        if (services != null && services.pm == pm) {
            pm.storagePublicationFailed(pkg, () -> FAILURES.get(key) == failed);
        }
        throw new UncheckedIOException("Cannot publish storage permissions for " + pkg.packageName,
                failure.cause());
    }

    public static void syncUid(PackageManagerService pm, int uid) {
        if (pm == null || uid < 0) return;
        UncheckedIOException failure = null;
        for (ApkPackage pkg : pm.packages()) {
            if (pkg.uid() == uid && !pm.isRemovalPending(pkg.packageName)) {
                try { syncPackage(pm, pkg); }
                catch (UncheckedIOException error) {
                    if (failure == null) failure = error;
                    else failure.addSuppressed(error);
                }
            }
        }
        if (failure != null) throw failure;
    }

    public static void syncAll(PackageManagerService pm) {
        if (pm == null) return;
        UncheckedIOException failure = null;
        for (ApkPackage pkg : pm.packages()) {
            if (!pm.isRemovalPending(pkg.packageName)) {
                try { syncPackage(pm, pkg); }
                catch (UncheckedIOException error) {
                    if (failure == null) failure = error;
                    else failure.addSuppressed(error);
                }
            }
        }
        if (failure != null) throw failure;
    }

    public static void removePackage(String packageName) {
        removePackage(null, packageName);
    }

    static void removePackage(PackageManagerService pm, String packageName) {
        File file = policyFile(packageName);
        if (file == null) return;
        PUBLICATION_REVISION.incrementAndGet();
        synchronized (PUBLICATION_LOCK) {
            try {
                Files.deleteIfExists(file.toPath());
                AppSandbox.syncStorageAccess(packageName, null);
                if (pm != null) FAILURES.remove(new PackageKey(pm, packageName));
            } catch (IOException e) {
                throw new UncheckedIOException("Cannot retire storage permissions for " + packageName, e);
            }
        }
    }

    private static void writeAtomically(ApkPackage pkg, Policy policy, File targetFile) throws IOException {
        String content = "v=1\n"
                + "pkg=" + pkg.packageName + "\n"
                + "uid=" + pkg.uid() + "\n"
                + "declared=" + (declaresPermission(pkg) ? "1" : "0") + "\n"
                + "mode=" + policy.mode().name() + "\n"
                + "media_mask=" + policy.mediaMask() + "\n";
        Path target = targetFile.toPath();
        Path dir = target.getParent();
        Path temp = null;
        try {
            Files.createDirectories(dir);
            temp = Files.createTempFile(dir, targetFile.getName() + ".", ".tmp");
            Files.writeString(temp, content, StandardCharsets.UTF_8);
            AppSandbox.prepareStoragePolicyFile(pkg.packageName, temp.toFile());
            try {
                Files.move(temp, target,
                        StandardCopyOption.ATOMIC_MOVE, StandardCopyOption.REPLACE_EXISTING);
            } catch (AtomicMoveNotSupportedException e) {
                Files.move(temp, target, StandardCopyOption.REPLACE_EXISTING);
            }
        } finally {
            if (temp != null) {
                try {
                    Files.deleteIfExists(temp);
                } catch (IOException ignored) { }
            }
        }
    }

    private static final String[] GUEST_EXTERNAL_PREFIXES = {
            "/sdcard",
            "/storage/emulated/0",
            "/storage/self/primary",
    };

    private static final Set<String> IMAGE_EXTENSIONS = Set.of(
            ".jpg", ".jpeg", ".png", ".gif", ".webp", ".bmp", ".heic", ".heif", ".dng", ".avif");
    private static final Set<String> VIDEO_EXTENSIONS = Set.of(
            ".mp4", ".m4v", ".mkv", ".webm", ".3gp", ".avi", ".mov", ".ts");
    private static final Set<String> AUDIO_EXTENSIONS = Set.of(
            ".mp3", ".m4a", ".aac", ".ogg", ".oga", ".opus", ".wav", ".flac", ".mid", ".midi");

    private static String mapGuestExternalPath(String path, Path sharedRoot) {
        String normalized = path.replace('\\', '/');
        for (String prefix : GUEST_EXTERNAL_PREFIXES) {
            if (normalized.equals(prefix)) {
                return sharedRoot.toString();
            }
            if (normalized.startsWith(prefix + "/")) {
                String rest = normalized.substring(prefix.length() + 1);
                // Reject '..' traversal that escapes the external storage root.
                int depth = 0;
                for (String part : rest.split("/")) {
                    if (part.isEmpty() || ".".equals(part)) continue;
                    if ("..".equals(part)) {
                        if (--depth < 0) return null;
                    } else {
                        depth++;
                    }
                }
                return sharedRoot.resolve(rest).toString();
            }
        }
        return path;
    }

    private static Path resolveRealOrAncestor(Path candidate) {
        Path normalized = candidate.toAbsolutePath().normalize();
        if (Files.exists(normalized)) {
            try {
                return normalized.toRealPath();
            } catch (IOException e) {
                return normalized;
            }
        }
        java.util.Deque<Path> tail = new java.util.ArrayDeque<>();
        Path cur = normalized;
        while (cur != null && !Files.exists(cur)) {
            if (cur.getFileName() != null) tail.addFirst(cur.getFileName());
            cur = cur.getParent();
        }
        if (cur == null) return normalized;
        try {
            cur = cur.toRealPath();
        } catch (IOException ignored) { }
        for (Path part : tail) {
            cur = cur.resolve(part);
        }
        return cur.normalize();
    }

    private static boolean isAllowedMediaRead(Path target, int mediaMask) {
        if (Files.isDirectory(target)) return true;
        String name = target.getFileName() == null ? "" : target.getFileName().toString();
        int dot = name.lastIndexOf('.');
        if (dot <= 0) return false;
        String ext = name.substring(dot).toLowerCase(Locale.ROOT);
        if ((mediaMask & MEDIA_IMAGES) != 0 && IMAGE_EXTENSIONS.contains(ext)) return true;
        if ((mediaMask & MEDIA_VIDEO) != 0 && VIDEO_EXTENSIONS.contains(ext)) return true;
        if ((mediaMask & MEDIA_AUDIO) != 0 && AUDIO_EXTENSIONS.contains(ext)) return true;
        return false;
    }

    public static boolean canAccessPath(PackageManagerService pm, int uid, String callingPkg,
            String path, boolean write) {
        if (UserHandle.isCore(uid)) return true;
        if (pm == null || path == null || path.isEmpty()) return false;

        Path sharedRoot = resolveRealOrAncestor(Paths.get(Device.dataDir(), "media", "0"));
        String mapped = mapGuestExternalPath(path, sharedRoot);
        if (mapped == null) return false;
        Path target = resolveRealOrAncestor(Paths.get(mapped));
        if (!target.startsWith(sharedRoot)) {
            return false;
        }

        if (!target.equals(sharedRoot)) {
            Path rel = sharedRoot.relativize(target);
            int count = rel.getNameCount();
            if (count >= 2 && "android".equalsIgnoreCase(rel.getName(0).toString())) {
                String sub = rel.getName(1).toString().toLowerCase(Locale.ROOT);
                if ("data".equals(sub) || "obb".equals(sub)) {
                    if (count >= 3 && uidOwnsPackage(pm, uid, rel.getName(2).toString())) {
                        return true;
                    }
                    return false;
                }
                if ("media".equals(sub) && count >= 3
                        && uidOwnsPackage(pm, uid, rel.getName(2).toString())) {
                    return true;
                }
            }
        }

        ApkPackage pkg = resolveUidPackage(pm, uid, callingPkg);
        if (pkg == null) return false;
        if (publicationFailed(pm, pkg.packageName)) return false;
        Policy policy = computePolicy(pm, pkg);
        if (write) {
            return policy.mode() == Mode.MANAGER || policy.mode() == Mode.LEGACY_RW;
        }
        if (policy.mode() == Mode.SCOPED) return false;
        if (policy.mode() == Mode.MEDIA_RO) {
            return isAllowedMediaRead(target, policy.mediaMask());
        }
        return true;
    }

    /**
     * Resolve the mount-service helper's app directory before creating it. A null
     * owner is reserved for an already authenticated system caller; ordinary apps
     * may prepare only their own Android/{data,obb,media} directory through Binder.
     */
    static File resolveMkdirsPath(PackageManagerService pm, ApkPackage owner, String path) {
        if (path == null || path.isEmpty()) throw new IllegalArgumentException("Missing directory path");
        try {
            Path sharedRoot = new File(Device.dataDir(), "media/0").getCanonicalFile().toPath();
            String mapped = mapGuestExternalPath(path, sharedRoot);
            if (mapped == null) throw new SecurityException("Invalid mkdirs path: " + path);
            File target = new File(mapped).getCanonicalFile();
            if (owner != null) {
                if (pm == null || pm.get(owner.packageName) != owner
                        || pm.isRemovalPending(owner.packageName)) {
                    throw new SecurityException("Storage caller belongs to retired package code");
                }
                Path resolved = target.toPath();
                if (!resolved.startsWith(sharedRoot)) {
                    throw new SecurityException("Invalid mkdirs path: " + target);
                }
                Path relative = sharedRoot.relativize(resolved);
                if (relative.getNameCount() < 3
                        || !"Android".equalsIgnoreCase(relative.getName(0).toString())
                        || !Set.of("data", "obb", "media").contains(
                                relative.getName(1).toString().toLowerCase(Locale.ROOT))
                        || !owner.packageName.equals(relative.getName(2).toString())) {
                    throw new SecurityException("Invalid mkdirs path for " + owner.packageName + ": " + target);
                }
            }
            return target;
        } catch (IOException failure) {
            throw new IllegalStateException("Failed to resolve " + path, failure);
        }
    }

    private static boolean uidOwnsPackage(PackageManagerService pm, int uid, String packageName) {
        if (packageName == null || packageName.isEmpty()) return false;
        for (ApkPackage pkg : pm.packages()) {
            if (pkg.uid() == uid && !pm.isRemovalPending(pkg.packageName)
                    && pkg.packageName.equalsIgnoreCase(packageName)) {
                return true;
            }
        }
        return false;
    }

    private static ApkPackage resolveUidPackage(PackageManagerService pm, int uid, String callingPkg) {
        if (callingPkg != null) {
            ApkPackage pkg = pm.get(callingPkg);
            if (pkg != null && pkg.uid() == uid && !pm.isRemovalPending(callingPkg)) {
                return pkg;
            }
        }
        for (ApkPackage pkg : pm.packages()) {
            if (pkg.uid() == uid && !pm.isRemovalPending(pkg.packageName)) {
                return pkg;
            }
        }
        return null;
    }
}
