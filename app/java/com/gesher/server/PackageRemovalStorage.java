/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package com.gesher.server;

import java.io.*;
import java.nio.file.*;
import java.nio.file.attribute.BasicFileAttributes;
import java.util.*;

/** Private removal quarantine. A reinstall can never inherit an undeleted old data path. */
final class PackageRemovalStorage {
    // Server-local failure injection for locked-remnant recovery fixtures, never a Binder setting.
    private static java.util.function.Predicate<Path> sReaper = PackageRemovalStorage::deleteNoFollow;
    private final Path root, directory;
    private final String name, code;
    private final LinkedHashMap<Path, Path> paths = new LinkedHashMap<>();

    // Fixed server-owned targets only. Shared stores cannot be moved with app data:
    // prepare their complete replacements first and publish after packages.list.
    enum SharedStore {
        ACCOUNTS("accounts.sealed"), SYNC("sync.properties");
        final String filename;
        SharedStore(String filename) { this.filename = filename; }
    }

    Path replacementFile(SharedStore store) throws IOException {
        Path staged = directory.resolve("replacement-" + store.filename);
        if (Files.exists(staged, LinkOption.NOFOLLOW_LINKS)) {
            throw new IOException("Shared removal snapshot already staged");
        }
        return staged;
    }

    /** Idempotent roll-forward; a moved snapshot must never be replayed later. */
    synchronized void publishReplacements() throws IOException {
        for (SharedStore store : SharedStore.values()) {
            Path staged = directory.resolve("replacement-" + store.filename);
            if (!Files.exists(staged, LinkOption.NOFOLLOW_LINKS)) continue;
            if (!Files.isRegularFile(staged, LinkOption.NOFOLLOW_LINKS)) {
                throw new IOException("Invalid shared removal snapshot");
            }
            Path target = root.resolve("system").resolve(store.filename);
            checkParents(target);
            if (Files.exists(target, LinkOption.NOFOLLOW_LINKS)
                    && !Files.isRegularFile(target, LinkOption.NOFOLLOW_LINKS)) {
                throw new IOException("Invalid shared removal target");
            }
            // Both paths are on the device volume. Retain the journal on failure,
            // so boot/install recovery finishes before opening the shared stores.
            try {
                Files.move(staged, target, StandardCopyOption.ATOMIC_MOVE,
                        StandardCopyOption.REPLACE_EXISTING);
            } catch (AtomicMoveNotSupportedException unavailable) {
                Files.move(staged, target, StandardCopyOption.REPLACE_EXISTING);
            }
        }
    }

    PackageRemovalStorage(String name, String codePath) throws IOException {
        this(name, codePath, null);
    }
    private PackageRemovalStorage(String name, String codePath, Path recovered) throws IOException {
        if (name == null || !name.matches("[A-Za-z_][A-Za-z0-9_]*(\\.[A-Za-z_][A-Za-z0-9_]*)+")) {
            throw new SecurityException("Invalid removal package");
        }
        this.name = name;
        root = deviceRoot();
        Path apk = codePath(new File(codePath).toPath());
        if (!root.resolve("app").equals(apk.getParent())
                || !apk.getFileName().toString().matches(java.util.regex.Pattern.quote(name) + "-[0-9]+")) {
            throw new SecurityException("Removal code is outside the installed APK directory");
        }
        code = root.relativize(apk).toString();
        Path trash = root.resolve("system/uninstall-trash");
        checkParents(trash.resolve("child"));
        Files.createDirectories(trash);
        directory = recovered == null ? Files.createTempDirectory(trash, "uninstall-") : recovered;
        if (!trash.equals(directory.getParent()) || Files.isSymbolicLink(directory)) {
            throw new SecurityException("Invalid removal quarantine");
        }
        add(apk);
        add(root.resolve("data").resolve(name));
        for (String kind : new String[]{"data", "obb", "media"}) {
            add(root.resolve("media/0/Android").resolve(kind).resolve(name));
        }
        // Keep the original item-0..4 mapping forever. Named additions remain
        // unambiguous when different runtime versions recover the same journal.
        add(root.resolve("user_de/0").resolve(name), "device-protected-data");
        add(root.resolve("system/app-data-layout").resolve(name), "storage-layout");
        if (recovered == null) {
            Properties journal = new Properties();
            journal.setProperty("package", name); journal.setProperty("code", code);
            try (FileOutputStream out = new FileOutputStream(directory.resolve("journal").toFile())) {
                journal.store(out, "Gesher package removal transaction"); out.getFD().sync();
            }
        }
        add(root.resolve("system/package-enabled-owners").resolve(name), "package-enabled-owner");
    }
    private static Path deviceRoot() throws IOException {
        Path data = new File(Device.dataDir()).getCanonicalFile().toPath();
        return Files.exists(data) ? data.toRealPath() : data;
    }
    private static Path codePath(Path path) throws IOException {
        Path apk = path.toAbsolutePath().normalize();
        // Windows guest aliases (for example /mnt/d) and the canonical device
        // root must share a namespace. Resolve ancestors only: the APK leaf may
        // itself be a link, which must be quarantined rather than followed.
        if (apk.getParent() != null) {
            Path parent = apk.getParent().toFile().getCanonicalFile().toPath();
            if (Files.exists(parent)) parent = parent.toRealPath();
            apk = parent.resolve(apk.getFileName());
        }
        return apk;
    }
    private void add(Path original) throws IOException {
        add(original, "item-" + paths.size());
    }
    private void add(Path original, String slot) throws IOException {
        checkParents(original);
        paths.put(original, directory.resolve(slot));
    }
    private void checkParents(Path original) throws IOException {
        Path parent = original.getParent();
        if (!original.normalize().startsWith(root)) throw new SecurityException("Removal path escaped device");
        while (parent != null && !parent.equals(root)) {
            if (Files.isSymbolicLink(parent)
                    || (Files.exists(parent, LinkOption.NOFOLLOW_LINKS)
                        && !parent.toRealPath().startsWith(root))) {
                throw new SecurityException("Removal parent is a link outside the device");
            }
            parent = parent.getParent();
        }
        if (parent == null) throw new SecurityException("Removal path escaped device");
    }
    void stage() throws IOException {
        for (Map.Entry<Path, Path> path : paths.entrySet()) {
            checkParents(path.getKey());
            if (!Files.exists(path.getKey(), LinkOption.NOFOLLOW_LINKS)) continue;
            IOException last = null;
            for (int attempt = 0; attempt < 5; attempt++) {
                try { Files.move(path.getKey(), path.getValue()); last = null; break; }
                catch (IOException busy) {
                    last = busy; System.gc(); System.runFinalization();
                    android.os.SystemClock.sleep(100);
                }
            }
            if (last != null) throw last;
        }
    }
    void rollback() throws IOException {
        List<Map.Entry<Path, Path>> reverse = new ArrayList<>(paths.entrySet());
        Collections.reverse(reverse);
        for (Map.Entry<Path, Path> path : reverse) {
            if (!Files.exists(path.getValue(), LinkOption.NOFOLLOW_LINKS)) continue;
            checkParents(path.getKey());
            if (Files.exists(path.getKey(), LinkOption.NOFOLLOW_LINKS)) {
                throw new IOException("Cannot restore occupied application path");
            }
            Files.move(path.getValue(), path.getKey());
        }
        discard();
    }
    void discard() { sReaper.test(directory); }

    /** Registry is the commit point: restore precommit moves, reap committed quarantine. */
    static void recover(Map<String, String> installed) throws IOException {
        Path root = deviceRoot();
        Path trash = root.resolve("system/uninstall-trash");
        if (!Files.isDirectory(trash, LinkOption.NOFOLLOW_LINKS)) return;
        try (DirectoryStream<Path> entries = Files.newDirectoryStream(trash, "uninstall-*")) {
            for (Path entry : entries) {
                if (!Files.isDirectory(entry, LinkOption.NOFOLLOW_LINKS)) continue;
                Path journal = entry.resolve("journal");
                if (!Files.isRegularFile(journal, LinkOption.NOFOLLOW_LINKS)) {
                    // A crash before journal publication moved no app paths.
                    deleteNoFollow(entry); continue;
                }
                if (Files.size(journal) > 4096) throw new IOException("Oversized removal journal");
                Properties p = new Properties();
                try (InputStream in = Files.newInputStream(journal)) { p.load(in); }
                String name = p.getProperty("package"), code = p.getProperty("code");
                if (code == null) throw new IOException("Invalid removal journal");
                PackageRemovalStorage old = new PackageRemovalStorage(name, root.resolve(code).toString(), entry);
                String retained = installed.get(name);
                if (retained != null && codePath(new File(retained).toPath())
                        .equals(root.resolve(old.code))) old.rollback();
                else {
                    old.publishReplacements();
                    old.discard();
                }
            }
        }
    }

    /** A surviving journal reserves its original generation until recovery/reaping. */
    static boolean reserves(Path codePath) throws IOException {
        Path root = deviceRoot();
        Path trash = root.resolve("system/uninstall-trash");
        if (!Files.isDirectory(trash, LinkOption.NOFOLLOW_LINKS)) return false;
        try (DirectoryStream<Path> entries = Files.newDirectoryStream(trash, "uninstall-*")) {
            for (Path entry : entries) {
                Path journal = entry.resolve("journal");
                if (!Files.isDirectory(entry, LinkOption.NOFOLLOW_LINKS)
                        || !Files.isRegularFile(journal, LinkOption.NOFOLLOW_LINKS)) continue;
                if (Files.size(journal) > 4096) throw new IOException("Oversized removal journal");
                Properties p = new Properties();
                try (InputStream in = Files.newInputStream(journal)) { p.load(in); }
                String old = p.getProperty("code");
                if (old == null) throw new IOException("Invalid removal journal");
                // Validate the complete record before using its path as authority.
                PackageRemovalStorage transaction = new PackageRemovalStorage(p.getProperty("package"),
                        root.resolve(old).toString(), entry);
                if (root.resolve(transaction.code).equals(codePath(codePath))) return true;
            }
        }
        return false;
    }

    /** No FOLLOW_LINKS: links inside app data are removed, never traversed. */
    static boolean deleteNoFollow(Path path) {
        try {
            Files.walkFileTree(path, new SimpleFileVisitor<Path>() {
                @Override public FileVisitResult visitFile(Path file, BasicFileAttributes attrs) throws IOException {
                    Files.delete(file); return FileVisitResult.CONTINUE;
                }
                @Override public FileVisitResult postVisitDirectory(Path dir, IOException failure) throws IOException {
                    if (failure != null) throw failure;
                    Files.delete(dir); return FileVisitResult.CONTINUE;
                }
            });
            return true;
        } catch (NoSuchFileException absent) { return true; }
        catch (IOException failure) { return false; }
    }
}
