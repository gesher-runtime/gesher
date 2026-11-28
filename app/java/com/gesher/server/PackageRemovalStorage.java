/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package com.gesher.server;

import java.io.*;
import java.nio.file.*;
import java.nio.file.attribute.BasicFileAttributes;
import java.nio.charset.StandardCharsets;
import java.util.*;

/** Private removal quarantine. A reinstall can never inherit an undeleted old data path. */
final class PackageRemovalStorage {
    private static final String ADJACENT_LOGS = "adjacent-v1";
    // Server-local failure injection for locked-remnant recovery fixtures, never a Binder setting.
    private static java.util.function.Predicate<Path> sReaper = PackageRemovalStorage::deleteNoFollow;
    private final Path root, directory, logs;
    private final String name, code;
    private final LinkedHashMap<Path, Path> paths = new LinkedHashMap<>();
    private final List<Path> adjacentLogSlots = new ArrayList<>(2);

    PackageRemovalStorage(String name, String codePath) throws IOException {
        this(name, codePath, null, logRoot(), true);
    }
    private PackageRemovalStorage(String name, String codePath, Path recovered, Path logs,
            boolean adjacentLogs) throws IOException {
        if (name == null || !name.matches("[A-Za-z_][A-Za-z0-9_]*(\\.[A-Za-z_][A-Za-z0-9_]*)+")) {
            throw new SecurityException("Invalid removal package");
        }
        this.name = name;
        root = deviceRoot();
        this.logs = logs;
        Path apk = codePath(new File(codePath).toPath());
        if (!root.resolve("app").equals(apk.getParent())
                || !apk.getFileName().toString().matches(java.util.regex.Pattern.quote(name) + "-[0-9]+")) {
            throw new SecurityException("Removal code is outside the installed APK directory");
        }
        code = root.relativize(apk).toString();
        Path trash = root.resolve("system/uninstall-trash");
        checkParents(trash.resolve("child"));
        Files.createDirectories(trash);
        directory = recovered == null ? Files.createDirectory(trash.resolve("uninstall-" + UUID.randomUUID()))
                : recovered;
        if (!trash.equals(directory.getParent()) || Files.isSymbolicLink(directory)) {
            throw new SecurityException("Invalid removal quarantine");
        }
        add(apk);
        add(root.resolve("data").resolve(name));
        for (Path external : AppDataStorage.externalDirectories(root, name)) add(external);
        // Keep the original item-0..4 mapping forever. Named additions remain
        // unambiguous when different runtime versions recover the same journal.
        add(root.resolve("user_de/0").resolve(name), "device-protected-data");
        add(root.resolve("system/app-data-layout").resolve(name), "storage-layout");
        add(root.resolve("misc/profiles/cur/0").resolve(name), "current-profile");
        if (logs != null) {
            addLog(name + ".log", "package-log", adjacentLogs);
            addLog(name + ".log.logcat", "package-logcat", adjacentLogs);
        }
        add(root.resolve("system/package-enabled-owners").resolve(name), "package-enabled-owner");
        if (recovered == null) {
            Properties journal = new Properties();
            journal.setProperty("package", name); journal.setProperty("code", code);
            journal.setProperty("logs", logs.toString());
            journal.setProperty("log-layout", ADJACENT_LOGS);
            Path temporary = Files.createTempFile(directory, ".journal-", ".tmp");
            try (FileOutputStream out = new FileOutputStream(temporary.toFile())) {
                journal.store(out, "Gesher package removal transaction"); out.getFD().sync();
            }
            moveUnoccupied(temporary, directory.resolve("journal"));
        }
    }
    private static Path deviceRoot() throws IOException {
        Path data = new File(Device.dataDir()).getCanonicalFile().toPath();
        return Files.exists(data) ? data.toRealPath() : data;
    }
    private static Path logRoot() throws IOException {
        return Device.logsDir().getCanonicalFile().toPath();
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
    private void addLog(String leaf, String slot, boolean adjacent) throws IOException {
        Path original = logs.resolve(leaf);
        if (!adjacent) {
            add(original, slot);
            return;
        }
        String transaction = directory.getFileName().toString();
        if (!transaction.matches("uninstall-[0-9a-f]{8}(?:-[0-9a-f]{4}){3}-[0-9a-f]{12}")) {
            throw new IOException("Invalid adjacent log transaction identifier");
        }
        Path destination = logs.resolve(".gesher-" + transaction + "-" + slot);
        checkParents(original);
        adjacentLogSlots.add(destination);
        paths.put(original, destination);
    }
    private void checkParents(Path original) throws IOException {
        Path parent = original.getParent();
        if (logs != null && logs.equals(parent)) {
            if (present(logs) && (!Files.isDirectory(logs, LinkOption.NOFOLLOW_LINKS)
                    || !logs.toRealPath().equals(logs))) {
                throw new SecurityException("Removal log parent changed");
            }
            return;
        }
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
    private static boolean present(Path path) throws IOException {
        try {
            Files.readAttributes(path, BasicFileAttributes.class, LinkOption.NOFOLLOW_LINKS);
            return true;
        } catch (NoSuchFileException absent) {
            return false;
        }
    }

    private static void moveUnoccupied(Path source, Path destination) throws IOException {
        if (present(destination)) throw new FileAlreadyExistsException(destination.toString());
        // Files.move without ATOMIC_MOVE can fall back to a byte copy. A
        // quarantine must keep the original file object, ACL and contents.
        Files.move(source, destination, StandardCopyOption.ATOMIC_MOVE);
    }

    private void checkAdjacentLog(Path path) throws IOException {
        checkParents(path);
        if (!Files.readAttributes(path, BasicFileAttributes.class, LinkOption.NOFOLLOW_LINKS).isRegularFile()) {
            throw new IOException("Invalid package log leaf: " + path.getFileName());
        }
    }

    private static String readOwner(Path path) throws IOException {
        BasicFileAttributes attrs = Files.readAttributes(path, BasicFileAttributes.class, LinkOption.NOFOLLOW_LINKS);
        if (!attrs.isRegularFile() || attrs.size() != 36) throw new IOException("Invalid removal owner marker");
        try (InputStream in = Files.newInputStream(path, LinkOption.NOFOLLOW_LINKS)) {
            byte[] bytes = in.readNBytes(37);
            if (bytes.length != 36) throw new IOException("Invalid removal owner marker length");
            String owner = new String(bytes, StandardCharsets.US_ASCII);
            try {
                if (UUID.fromString(owner).toString().equals(owner)) return owner;
            } catch (IllegalArgumentException ignored) { }
            throw new IOException("Invalid removal owner identity");
        }
    }

    private void writeFence(String leaf, String owner) throws IOException {
        Path marker = directory.resolve(leaf);
        Path temporary = Files.createTempFile(directory, ".fence-", ".tmp");
        try {
            try (FileOutputStream out = new FileOutputStream(temporary.toFile())) {
                out.write(owner.getBytes(StandardCharsets.US_ASCII));
                out.getFD().sync();
            }
            moveUnoccupied(temporary, marker);
        } finally {
            Files.deleteIfExists(temporary);
        }
    }

    void beginNativeFence(String owner) throws IOException {
        verifyRestoredOwner(owner);
        writeFence("native-fence", owner);
    }

    void acknowledgeNativeStop(String owner) throws IOException {
        writeFence("native-stopped", owner);
    }

    void verifyRollbackOwner(String owner) throws IOException {
        Path active = root.resolve("system/package-enabled-owners").resolve(name);
        Path quarantined = paths.get(active);
        checkParents(active);
        Path marker = present(quarantined) ? quarantined : active;
        if (!readOwner(marker).equals(owner)) throw new IOException("Removal owner changed: " + name);
    }

    void verifyRestoredOwner(String owner) throws IOException {
        Path marker = root.resolve("system/package-enabled-owners").resolve(name);
        Path apk = root.resolve(code).resolve("base.apk");
        checkParents(marker);
        checkParents(apk);
        if (!readOwner(marker).equals(owner) || !Files.isRegularFile(apk, LinkOption.NOFOLLOW_LINKS)) {
            throw new IOException("Original package generation could not be confirmed: " + name);
        }
    }

    void releaseNativeFence() throws IOException {
        Files.delete(directory.resolve("native-fence"));
    }
    void stage() throws IOException {
        for (Map.Entry<Path, Path> path : paths.entrySet()) {
            checkParents(path.getKey());
            if (!present(path.getKey())) continue;
            if (adjacentLogSlots.contains(path.getValue())) checkAdjacentLog(path.getKey());
            IOException last = null;
            for (int attempt = 0; attempt < 5; attempt++) {
                try { moveUnoccupied(path.getKey(), path.getValue()); last = null; break; }
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
            boolean adjacent = adjacentLogSlots.contains(path.getValue());
            if (adjacent) checkParents(path.getValue());
            if (!present(path.getValue())) continue;
            if (adjacent) checkAdjacentLog(path.getValue());
            checkParents(path.getKey());
            moveUnoccupied(path.getValue(), path.getKey());
        }
    }
    void discard() {
        try {
            for (Path slot : adjacentLogSlots) {
                checkParents(slot);
                if (!present(slot)) continue;
                checkAdjacentLog(slot);
                Files.delete(slot);
            }
        } catch (IOException | SecurityException failure) {
            // Retain the main journal until every external slot is reaped.
            return;
        }
        sReaper.test(directory);
    }

    /** Registry is the commit point: restore precommit moves, reap committed quarantine. */
    static void recover(Map<String, String> installed) throws IOException {
        recover(installed, false);
    }

    static void recover(Map<String, String> installed, boolean freshProtectedAuthority) throws IOException {
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
                String layout = p.getProperty("log-layout");
                if (layout != null && !ADJACENT_LOGS.equals(layout)) throw new IOException("Invalid removal log layout");
                boolean adjacentLogs = ADJACENT_LOGS.equals(layout);
                PackageRemovalStorage old = new PackageRemovalStorage(name, root.resolve(code).toString(), entry, null, false);
                String retained = installed.get(name);
                boolean rollback = retained != null && codePath(new File(retained).toPath())
                        .equals(root.resolve(old.code));
                if (rollback || adjacentLogs) {
                    Path logRoot = journalLogs(p, rollback);
                    if (adjacentLogs && logRoot == null) continue;
                    if (logRoot != null) {
                        old = new PackageRemovalStorage(name, root.resolve(code).toString(), entry, logRoot, adjacentLogs);
                    }
                }
                if (rollback) {
                    Path fence = entry.resolve("native-fence");
                    String owner = null;
                    if (present(fence)) {
                        owner = readOwner(fence);
                        // Only a fresh authenticated device may recover a previously
                        // acknowledged exact process stop without the old broker.
                        if (!freshProtectedAuthority || !owner.equals(readOwner(entry.resolve("native-stopped")))) {
                            throw new IOException("Native package retirement is pending: " + name);
                        }
                        old.verifyRollbackOwner(owner);
                    }
                    old.rollback();
                    if (owner != null) {
                        old.verifyRestoredOwner(owner);
                        old.releaseNativeFence();
                    }
                }
                old.discard();
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
                        root.resolve(old).toString(), entry, null, false);
                if (root.resolve(transaction.code).equals(codePath(codePath))) return true;
            }
        }
        return false;
    }

    private static Path journalLogs(Properties journal, boolean rollback) throws IOException {
        String saved = journal.getProperty("logs");
        if (saved == null) {
            if (journal.containsKey("log-layout")) throw new IOException("Missing removal log root");
            return null; // journals before the log slots existed
        }
        Path current = logRoot();
        if (!current.toString().equals(saved)) {
            if (rollback) throw new IOException("Removal log root changed");
            return null; // committed: preserve the journal, never open the unverified old path
        }
        return current;
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
