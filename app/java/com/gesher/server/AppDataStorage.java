/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package com.gesher.server;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.nio.channels.FileChannel;
import java.nio.channels.FileLock;
import java.nio.channels.OverlappingFileLockException;
import java.nio.file.AtomicMoveNotSupportedException;
import java.nio.file.DirectoryStream;
import java.nio.file.Files;
import java.nio.file.LinkOption;
import java.nio.file.Path;
import java.nio.file.StandardCopyOption;
import java.nio.file.StandardOpenOption;
import java.util.HashMap;
import java.util.Map;
import java.util.Properties;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.locks.ReentrantLock;

/**
 * Separate Android credential/device storage areas on the always-unlocked desktop user.
 * This is a disk layout, not a claim of separate encryption keys or a pre-unlock state.
 */
final class AppDataStorage {
    final Path credential, device, metadata;
    private final Path boundary;
    private static final String VERSION = "1";
    private static final long LOCK_WAIT_NANOS = TimeUnit.SECONDS.toNanos(10);
    // Traditional F_SETLK locks belong to the process: closing ANY descriptor
    // for their inode can release another thread's lock. Wait locally before
    // opening that inode, and retain the guard until after the channel closes.
    private static final Map<Path, LocalLock> sLocalLocks = new HashMap<>();

    private static final class LocalLock {
        final ReentrantLock lock = new ReentrantLock();
        int references;
    }

    private static final class LocalGuard implements AutoCloseable {
        final Path key;
        final LocalLock entry;
        boolean closed;

        LocalGuard(Path key, LocalLock entry) { this.key = key; this.entry = entry; }

        @Override public void close() {
            if (closed) return;
            closed = true;
            entry.lock.unlock();
            releaseReference(key, entry);
        }
    }

    private static final class StorageLock implements AutoCloseable {
        final LocalGuard local;
        final FileChannel channel;
        final FileLock fileLock;
        boolean closed;

        StorageLock(LocalGuard local, FileChannel channel, FileLock fileLock) {
            this.local = local; this.channel = channel; this.fileLock = fileLock;
        }

        @Override public void close() throws IOException {
            if (closed) return;
            closed = true;
            try {
                try { fileLock.close(); }
                finally { channel.close(); }
            } finally {
                local.close();
            }
        }
    }

    private AppDataStorage(Path boundary, Path credential, Path device, Path metadata) {
        this.boundary = boundary;
        this.credential = credential;
        this.device = device;
        this.metadata = metadata;
    }

    static AppDataStorage installed(File dataRoot, String packageName) {
        if (packageName == null
                || !packageName.matches("[A-Za-z_][A-Za-z0-9_]*(\\.[A-Za-z_][A-Za-z0-9_]*)+")) {
            throw new IllegalArgumentException("Invalid application package name");
        }
        Path root = dataRoot.toPath().toAbsolutePath().normalize();
        return new AppDataStorage(root, root.resolve("data").resolve(packageName),
                root.resolve("user_de/0").resolve(packageName),
                root.resolve("system/app-data-layout").resolve(packageName));
    }

    /** --data-dir keeps its old CE path; DE and the migration journal are siblings. */
    static AppDataStorage custom(String credentialDirectory) {
        if (credentialDirectory == null || credentialDirectory.isEmpty()) {
            throw new IllegalArgumentException("Application data directory is empty");
        }
        Path ce = new File(credentialDirectory).toPath().toAbsolutePath().normalize();
        Path parent = ce.getParent();
        if (parent == null || ce.getFileName() == null) {
            throw new IllegalArgumentException("Application data directory cannot be a filesystem root");
        }
        String name = ce.getFileName().toString();
        return new AppDataStorage(parent, ce, parent.resolve(name + ".device-protected"),
                parent.resolve(name + ".gesher-storage"));
    }

    /**
     * Run before publishing or starting the package. Old Gesher conflated CE and DE.
     * Keep ordinary apps' files in CE. For an app already declaring a DE default,
     * preserve its default data by moving the old area once, never copying secrets
     * into both areas and never merging an existing destination.
     *
     * The journal precedes the rename; ready precedes creating the now-empty CE
     * directory. Thus either interrupted state can be recovered unambiguously.
     * A canonical per-layout guard covers threads before a descriptor is opened;
     * a file lock covers other processes. Use nonblocking F_SETLK, since Gesher's
     * Windows POSIX backend does not yet support blocking F_SETLKW.
     */
    void prepare(boolean defaultToDevice, boolean allowLegacyMigration) throws IOException {
        prepare(defaultToDevice, allowLegacyMigration, false);
    }

    /** An incoming APK cannot initialize an existing app's unclassified legacy layout. */
    void prepareUpdate(boolean defaultToDevice) throws IOException {
        prepare(defaultToDevice, false, true);
    }

    private void prepare(boolean defaultToDevice, boolean allowLegacyMigration,
            boolean requireExistingLayout) throws IOException {
        try (StorageLock ignored = lockStorage()) {
            prepareLocked(defaultToDevice, allowLegacyMigration, requireExistingLayout);
        }
    }

    private StorageLock lockStorage() throws IOException {
        long started = System.nanoTime();
        checkPath(metadata);
        Files.createDirectories(metadata);
        Path lockPath = metadata.resolve("lock");
        checkPath(lockPath);
        // Resolve configured-root aliases as well as /mnt drive aliases/casing.
        // Do not open the lock file merely to determine its canonical key.
        Path key = metadata.toRealPath().resolve("lock");
        LocalGuard local = awaitLocalGuard(key, started);
        FileChannel channel = null;
        try {
            channel = FileChannel.open(key, StandardOpenOption.CREATE,
                    StandardOpenOption.WRITE, LinkOption.NOFOLLOW_LINKS);
            return new StorageLock(local, channel, awaitLock(channel, started));
        } catch (IOException | RuntimeException | Error failure) {
            if (channel != null) {
                try { channel.close(); }
                catch (IOException closeFailure) { failure.addSuppressed(closeFailure); }
            }
            local.close();
            throw failure;
        }
    }

    private static LocalGuard awaitLocalGuard(Path key, long started) throws IOException {
        LocalLock entry;
        synchronized (sLocalLocks) {
            entry = sLocalLocks.computeIfAbsent(key, unused -> new LocalLock());
            entry.references++;
        }
        boolean acquired = false;
        try {
            LocalGuard guard = new LocalGuard(key, entry);
            // Recursive preparation must not open a second descriptor either.
            if (entry.lock.isHeldByCurrentThread()) {
                throw new IOException("Recursive application storage preparation");
            }
            long remaining = remainingWait(started);
            if (remaining <= 0 || !entry.lock.tryLock(remaining, TimeUnit.NANOSECONDS)) {
                throw lockTimeout();
            }
            acquired = true;
            return guard;
        } catch (InterruptedException interrupted) {
            Thread.currentThread().interrupt();
            throw new IOException("Interrupted while waiting for application storage", interrupted);
        } finally {
            if (!acquired) releaseReference(key, entry);
        }
    }

    private static void releaseReference(Path key, LocalLock entry) {
        synchronized (sLocalLocks) {
            if (--entry.references == 0) sLocalLocks.remove(key, entry);
        }
    }

    private static long remainingWait(long started) {
        return LOCK_WAIT_NANOS - (System.nanoTime() - started);
    }

    private static IOException lockTimeout() {
        return new IOException("Application storage is busy; timed out waiting for its layout lock");
    }

    private static FileLock awaitLock(FileChannel channel, long started) throws IOException {
        do {
            if (Thread.currentThread().isInterrupted()) {
                throw new IOException("Interrupted while waiting for application storage");
            }
            if (remainingWait(started) <= 0) throw lockTimeout();
            try {
                FileLock lock = channel.tryLock();
                if (lock != null) return lock;
            } catch (OverlappingFileLockException anotherThread) {
                // Only a foreign lock-file user can reach this: production callers
                // share the local guard and never have overlapping descriptors.
            }
            long remaining = remainingWait(started);
            if (remaining <= 0) throw lockTimeout();
            try { TimeUnit.NANOSECONDS.sleep(Math.min(remaining, TimeUnit.MILLISECONDS.toNanos(10))); }
            catch (InterruptedException interrupted) {
                Thread.currentThread().interrupt();
                throw new IOException("Interrupted while waiting for application storage", interrupted);
            }
        } while (remainingWait(started) > 0);
        throw lockTimeout();
    }

    private void prepareLocked(boolean defaultToDevice, boolean allowLegacyMigration,
            boolean requireExistingLayout) throws IOException {
        checkDirectory(credential);
        checkDirectory(device);
        Path journal = metadata.resolve("state.properties");
        checkPath(journal);
        String phase = null;
        if (Files.exists(journal, LinkOption.NOFOLLOW_LINKS)) {
            if (!Files.isRegularFile(journal, LinkOption.NOFOLLOW_LINKS)
                    || Files.size(journal) > 4096) {
                throw new IOException("Invalid application storage journal");
            }
            Properties state = new Properties();
            try (InputStream input = Files.newInputStream(journal)) { state.load(input); }
            if (!VERSION.equals(state.getProperty("version"))) {
                throw new IOException("Unsupported application storage layout");
            }
            phase = state.getProperty("phase");
            if (!"ready".equals(phase) && !"moving-default-de".equals(phase)) {
                throw new IOException("Invalid application storage migration phase");
            }
        }
        if (phase == null) {
            if (requireExistingLayout) {
                throw new IOException("Existing application storage must be prepared from its "
                        + "committed manifest before accepting an update");
            }
            if (defaultToDevice && hasEntries(credential)) {
                if (!allowLegacyMigration) {
                    throw new IOException("Legacy default-DE data requires preparation from the "
                            + "previously installed manifest; original credential data was preserved");
                }
                if (Files.exists(device, LinkOption.NOFOLLOW_LINKS)) {
                    throw new IOException("Legacy default-DE data needs recovery: both storage areas exist");
                }
                writePhase(journal, "moving-default-de");
                phase = "moving-default-de";
            } else {
                writePhase(journal, "ready");
                phase = "ready";
            }
        }
        if ("moving-default-de".equals(phase)) {
            boolean source = Files.exists(credential, LinkOption.NOFOLLOW_LINKS);
            boolean target = Files.exists(device, LinkOption.NOFOLLOW_LINKS);
            if (source == target) {
                throw new IOException("Ambiguous interrupted application storage migration");
            }
            if (source) {
                checkPath(device.getParent());
                Files.createDirectories(device.getParent());
                // Both paths are on the same backing volume. Without REPLACE_EXISTING,
                // a concurrently created target fails instead of losing either tree.
                Files.move(credential, device);
            }
            writePhase(journal, "ready");
        }
        Files.createDirectories(credential);
        Files.createDirectories(device);
    }

    private static boolean hasEntries(Path directory) throws IOException {
        if (!Files.exists(directory, LinkOption.NOFOLLOW_LINKS)) return false;
        try (DirectoryStream<Path> children = Files.newDirectoryStream(directory)) {
            return children.iterator().hasNext();
        }
    }

    private void checkDirectory(Path path) throws IOException {
        checkPath(path);
        if (Files.exists(path, LinkOption.NOFOLLOW_LINKS)
                && !Files.isDirectory(path, LinkOption.NOFOLLOW_LINKS)) {
            throw new IOException("Application storage is not a directory: " + path);
        }
    }

    private void checkPath(Path path) throws IOException {
        if (!path.normalize().startsWith(boundary)) {
            throw new IOException("Application storage escaped its configured root");
        }
        // The configured device/override root may itself resolve a host path alias;
        // no app-controlled descendant may redirect a rename, lock or journal write.
        for (Path current = path; current != null && !current.equals(boundary);
                current = current.getParent()) {
            if (Files.isSymbolicLink(current)) {
                throw new IOException("Application storage path is a symbolic link: " + current);
            }
        }
    }

    private static void writePhase(Path journal, String phase) throws IOException {
        Properties state = new Properties();
        state.setProperty("version", VERSION);
        state.setProperty("phase", phase);
        Path temporary = Files.createTempFile(journal.getParent(), "layout-", ".tmp");
        try {
            try (FileOutputStream output = new FileOutputStream(temporary.toFile())) {
                state.store(output, "Gesher application storage layout");
                output.getFD().sync();
            }
            try {
                Files.move(temporary, journal, StandardCopyOption.ATOMIC_MOVE,
                        StandardCopyOption.REPLACE_EXISTING);
            } catch (AtomicMoveNotSupportedException unavailable) {
                // Do not publish a non-atomic migration marker: a retry must still
                // know which directory owns the old application's data.
                throw new IOException("Application storage requires atomic journal replacement", unavailable);
            }
        } finally {
            Files.deleteIfExists(temporary);
        }
    }
}
