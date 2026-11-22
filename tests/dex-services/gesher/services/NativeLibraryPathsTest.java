/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package gesher.services;

import com.gesher.app.NativeLibs;
import java.io.IOException;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.attribute.FileTime;
import java.util.Arrays;
import java.util.Comparator;
import java.util.zip.ZipEntry;
import java.util.zip.ZipOutputStream;

/** Real ZIP extraction through the production native-library installer. */
public final class NativeLibraryPathsTest {
    private static int checks;
    private static final byte[] PAYLOAD = {1, 2, 3, 4, 5};

    private static void check(boolean value, String message) {
        if (!value) throw new AssertionError(message);
        checks++;
    }

    private static void archive(Path target, String... names) throws IOException {
        try (ZipOutputStream out = new ZipOutputStream(Files.newOutputStream(target))) {
            for (String name : names) {
                out.putNextEntry(new ZipEntry(name));
                out.write(PAYLOAD);
                out.closeEntry();
            }
        }
    }

    private static void discovery(Path root) throws IOException {
        Path plain = root.resolve("plain.apk");
        Path arm = root.resolve("arm.apk");
        Path x64 = root.resolve("x64.apk");
        Path unsupported = root.resolve("unsupported.apk");
        Path missing = root.resolve("missing.apk");
        Path corrupt = root.resolve("corrupt.apk");
        archive(plain, "classes.dex", "assets/elsewhere.so", "lib/x86_64/nested/ignored.so",
                "lib/arm64-v8a/nested/ignored.so", "lib/x86_64/not.so.txt");
        archive(arm, "lib/arm64-v8a/libarm.so");
        archive(x64, "lib/x86_64/libx64.so");
        archive(unsupported, "lib/armeabi-v7a/libold.so", "lib/x86/libold.so");
        Files.write(corrupt, PAYLOAD);
        check(NativeLibs.abiFor() == null, "No archives have no native ABI");
        check(NativeLibs.abiFor(plain.toString(), unsupported.toString()) == null,
                "Java-only and unsupported libraries have no supported ABI");
        check(NativeLibs.ABI.equals(NativeLibs.forApk(plain.toString())), "Java-only directory fallback");
        check("arm64-v8a".equals(NativeLibs.abiFor(plain.toString(), arm.toString())),
                "Native libraries may live in an ABI split");
        check(NativeLibs.ABI.equals(NativeLibs.abiFor(arm.toString(), x64.toString())),
                "Preferred ABI in a later split beats an earlier fallback");
        check(NativeLibs.ABI.equals(NativeLibs.abiFor(x64.toString(), arm.toString())),
                "Split order does not change ABI preference");
        check(NativeLibs.ABI.equals(NativeLibs.abiFor(x64.toString(), missing.toString())),
                "Preferred ABI stops before an unnecessary unreadable split");
        for (Path bad : new Path[] {missing, corrupt}) {
            boolean rejected = false;
            try { NativeLibs.abiFor(arm.toString(), bad.toString()); }
            catch (IOException expected) { rejected = true; }
            check(rejected, "Fallback ABI does not hide an unreadable required split");
            check(NativeLibs.ABI.equals(NativeLibs.forApk(bad.toString())),
                    "forApk preserves unreadable-archive fallback");
        }
        // Discovery is not a persistent cache: replacing an archive is immediately visible.
        archive(arm, "lib/x86_64/replaced.so");
        check(NativeLibs.ABI.equals(NativeLibs.abiFor(arm.toString())), "Replaced APK is rescanned");
    }

    /** Re-extraction keeps identical files; a system code directory is synced exactly. */
    private static void sync(Path root) throws IOException {
        Path apk = root.resolve("sync.apk");
        archive(apk, "lib/x86_64/libkeep.so", "lib/x86_64/libother.so");
        Path dir = root.resolve("sync/lib/x86_64");
        check(NativeLibs.syncDirectory(apk.toString(), dir.toString(), "x86_64") == 2,
                "Sync extracts every selected library");
        Path keep = dir.resolve("libkeep.so");
        FileTime old = FileTime.fromMillis(1_600_000_000_000L);
        Files.setLastModifiedTime(keep, old);
        Object identity = Files.readAttributes(keep,
                java.nio.file.attribute.BasicFileAttributes.class).fileKey();
        check(NativeLibs.extract(apk.toString(), dir.toString(), "x86_64") == 2,
                "Identical libraries still count as extracted");
        check(old.equals(Files.getLastModifiedTime(keep)), "Identical library is not rewritten");
        check(identity == null || identity.equals(Files.readAttributes(keep,
                java.nio.file.attribute.BasicFileAttributes.class).fileKey()),
                "Identical library keeps its file");
        Files.write(keep, new byte[] {9, 9, 9, 9, 9});
        Files.setLastModifiedTime(keep, old);
        NativeLibs.extract(apk.toString(), dir.toString(), "x86_64");
        check(Arrays.equals(PAYLOAD, Files.readAllBytes(keep)), "Same-size modified library is replaced");
        Files.write(keep, new byte[] {1, 2, 3, 4, 5, 6});
        NativeLibs.extract(apk.toString(), dir.toString(), "x86_64");
        check(Arrays.equals(PAYLOAD, Files.readAllBytes(keep)), "Longer modified library is replaced");
        Files.delete(keep);
        NativeLibs.extract(apk.toString(), dir.toString(), "x86_64");
        check(Arrays.equals(PAYLOAD, Files.readAllBytes(keep)), "Missing library is restored");

        Files.write(dir.resolve("libstale.so"), PAYLOAD);
        Files.write(dir.resolve("notes.txt"), PAYLOAD);
        Files.write(dir.resolve("libupper.SO"), PAYLOAD);
        Files.write(dir.resolve("libkeep.so123.part"), PAYLOAD);
        Files.createDirectory(dir.resolve("nested.so"));
        archive(apk, "lib/x86_64/libkeep.so");
        check(NativeLibs.syncDirectory(apk.toString(), dir.toString(), "x86_64") == 1,
                "Sync counts the APK's libraries");
        check(Files.exists(keep), "Listed library kept");
        check(!Files.exists(dir.resolve("libother.so")) && !Files.exists(dir.resolve("libstale.so")),
                "Libraries the APK no longer has are removed");
        check(!Files.exists(dir.resolve("notes.txt"), java.nio.file.LinkOption.NOFOLLOW_LINKS)
                && !Files.exists(dir.resolve("libupper.SO"), java.nio.file.LinkOption.NOFOLLOW_LINKS),
                "Unlisted files are removed, whatever their extension's case");
        check(Files.isDirectory(dir.resolve("nested.so")) && Files.exists(dir.resolve("libkeep.so123.part")),
                "Directories and in-progress extractions are left alone");
        Path outsideTarget = root.resolve("link-target.so");
        Files.write(outsideTarget, PAYLOAD);
        boolean staleLink;
        try {
            Files.createSymbolicLink(dir.resolve("liblinked.so"), outsideTarget);
            staleLink = true;
        } catch (IOException | UnsupportedOperationException noPrivilege) {
            staleLink = false;
        }
        if (staleLink) {
            NativeLibs.syncDirectory(apk.toString(), dir.toString(), "x86_64");
            check(!Files.exists(dir.resolve("liblinked.so"), java.nio.file.LinkOption.NOFOLLOW_LINKS)
                    && Files.exists(outsideTarget), "A stale link is removed, not its target");
        }

        archive(apk, "classes.dex");
        check(NativeLibs.syncDirectory(apk.toString(), dir.toString(), "x86_64") == 0,
                "APK without native code has no libraries");
        check(!Files.exists(keep), "Libraries of an earlier native release are removed");
        Path fresh = root.resolve("fresh/lib/x86_64");
        check(NativeLibs.syncDirectory(apk.toString(), fresh.toString(), "x86_64") == 0
                && !Files.exists(fresh.getParent()), "No directory is created without libraries");

        archive(apk, "lib/x86_64/libkeep.so");
        Path fileTarget = root.resolve("file-target/lib/x86_64");
        Files.createDirectories(fileTarget.getParent());
        Files.write(fileTarget, PAYLOAD);
        Path fileParent = root.resolve("file-parent/lib");
        Files.createDirectories(fileParent.getParent());
        Files.write(fileParent, PAYLOAD);
        for (Path refused : new Path[] {fileTarget, fileParent.resolve("x86_64")}) {
            boolean rejected = false;
            try { NativeLibs.syncDirectory(apk.toString(), refused.toString(), "x86_64"); }
            catch (IOException expected) { rejected = true; }
            check(rejected, "Non-directory library path refused: " + refused);
        }
        check(Arrays.equals(PAYLOAD, Files.readAllBytes(fileTarget)), "Refused file unchanged");

        Path elsewhere = Files.createDirectories(root.resolve("elsewhere"));
        Path link = root.resolve("linked/lib/x86_64");
        Files.createDirectories(link.getParent());
        boolean linked;
        try {
            Files.createSymbolicLink(link, elsewhere);
            linked = true;
        } catch (IOException | UnsupportedOperationException noPrivilege) {
            linked = false;  // Windows without the symbolic-link privilege
        }
        if (linked) {
            boolean rejected = false;
            try { NativeLibs.syncDirectory(apk.toString(), link.toString(), "x86_64"); }
            catch (IOException expected) { rejected = true; }
            check(rejected, "Linked library directory refused");
            check(!Files.exists(elsewhere.resolve("libkeep.so")), "Nothing written through a link");
            Files.delete(link);
        }

        // A Windows junction (no privilege needed), as the lib directory or its parent.
        for (String layout : new String[] {"junction-target/lib/x86_64", "junction-parent/lib"}) {
            Path outside = Files.createDirectories(root.resolve("outside-" + layout.replace('/', '-')));
            Files.write(outside.resolve("victim.so"), PAYLOAD);
            Path junction = root.resolve(layout);
            Files.createDirectories(junction.getParent());
            if (!junction(junction, outside)) break;
            Path target = layout.endsWith("x86_64") ? junction : junction.resolve("x86_64");
            if (!layout.endsWith("x86_64")) Files.createDirectories(outside.resolve("x86_64"));
            Path victim = layout.endsWith("x86_64") ? outside.resolve("victim.so")
                    : outside.resolve("x86_64/victim.so");
            if (!layout.endsWith("x86_64")) Files.write(victim, PAYLOAD);
            boolean rejected = false;
            try { NativeLibs.syncDirectory(apk.toString(), target.toString(), "x86_64"); }
            catch (IOException expected) { rejected = true; }
            check(rejected, "Junction refused: " + layout);
            check(Files.exists(victim), "Nothing deleted through a junction: " + layout);
            check(!Files.exists(outside.resolve("libkeep.so"))
                    && !Files.exists(outside.resolve("x86_64/libkeep.so")),
                    "Nothing written through a junction: " + layout);
            Files.delete(junction);
        }

        // A stale library that cannot be removed fails the sync (not a silent success).
        Path held = Files.createDirectories(root.resolve("held/lib/x86_64"));
        NativeLibs.syncDirectory(apk.toString(), held.toString(), "x86_64");
        Path stale = held.resolve("libgone.so");
        Files.write(stale, PAYLOAD);
        java.io.Closeable release = blockDeletion(root.resolve("probe"), stale);
        if (release != null) {
            boolean failed = false;
            try { NativeLibs.syncDirectory(apk.toString(), held.toString(), "x86_64"); }
            catch (IOException expected) { failed = true; }
            finally { release.close(); }
            check(failed, "Unremovable stale library fails the sync");
            NativeLibs.syncDirectory(apk.toString(), held.toString(), "x86_64");
            check(!Files.exists(stale), "Stale library removed once it is free");
        }

        // Windows names are case-insensitive: a differently cased listed library stays.
        Path cased = Files.createDirectories(root.resolve("cased/lib/x86_64"));
        Files.write(cased.resolve("LibKeep.so"), PAYLOAD);
        if (Files.exists(cased.resolve("libkeep.so"))) {
            NativeLibs.syncDirectory(apk.toString(), cased.toString(), "x86_64");
            check(Files.exists(cased.resolve("libkeep.so")), "Differently cased library kept");
        }
    }

    /**
     * Makes {@code file} undeletable by a mechanism this runtime honours, verified first on
     * a probe file under {@code probeRoot}; returns how to undo it, or null if none works
     * here (the caller then skips the case).
     */
    private static java.io.Closeable blockDeletion(Path probeRoot, Path file) throws IOException {
        Path probeDir = Files.createDirectories(probeRoot);
        Path probe = probeDir.resolve("probe.so");
        for (int mechanism = 0; mechanism < 4; mechanism++) {
            if (!Files.exists(probe)) Files.write(probe, PAYLOAD);
            probe.toFile().setWritable(true);
            java.io.Closeable undoProbe = applyBlock(mechanism, probe);
            if (undoProbe == null) continue;
            boolean blocked;
            try {
                Files.delete(probe);
                blocked = false;
            } catch (IOException denied) {
                blocked = true;
            } finally {
                undoProbe.close();
            }
            if (blocked) {
                System.out.println("deletion blocked by mechanism " + mechanism);
                return applyBlock(mechanism, file);
            }
        }
        return null;
    }

    private static java.io.Closeable applyBlock(int mechanism, Path file) {
        Path dir = file.getParent();
        try {
            if (mechanism == 0 && windows()) {
                // Deny DELETE on the file and delete-child on its directory to Everyone:
                // honoured by every Windows delete, POSIX-semantics ones included.
                if (!run("icacls", file.toString(), "/deny", "*S-1-1-0:(D)")
                        || !run("icacls", dir.toString(), "/deny", "*S-1-1-0:(DC)")) {
                    run("icacls", file.toString(), "/remove:d", "*S-1-1-0");
                    run("icacls", dir.toString(), "/remove:d", "*S-1-1-0");
                    return null;
                }
                return () -> {
                    run("icacls", file.toString(), "/remove:d", "*S-1-1-0");
                    run("icacls", dir.toString(), "/remove:d", "*S-1-1-0");
                };
            }
            if (mechanism == 1 && windows()) {
                // java.io opens without FILE_SHARE_DELETE on a Windows JDK.
                return new java.io.FileInputStream(file.toFile());
            }
            if (mechanism == 3) {
                // A read-only file, where the runtime's delete honours the attribute.
                if (!file.toFile().setWritable(false)) return null;
                return () -> file.toFile().setWritable(true);
            }
            if (mechanism == 2 && !windows()) {
                Files.setPosixFilePermissions(dir,
                        java.nio.file.attribute.PosixFilePermissions.fromString("r-xr-xr-x"));
                return () -> Files.setPosixFilePermissions(dir,
                        java.nio.file.attribute.PosixFilePermissions.fromString("rwxr-xr-x"));
            }
        } catch (Exception unavailable) {
            return null;
        }
        return null;
    }

    private static boolean windows() {
        return System.getProperty("os.name", "").startsWith("Windows");
    }

    private static boolean run(String... command) {
        try {
            Process p = new ProcessBuilder(command).redirectErrorStream(true).start();
            p.getInputStream().readAllBytes();
            return p.waitFor() == 0;
        } catch (Exception unavailable) {
            return false;
        }
    }

    private static boolean junction(Path link, Path target) {
        if (!System.getProperty("os.name", "").startsWith("Windows")) return false;
        try {
            Process p = new ProcessBuilder("cmd", "/c", "mklink", "/J", link.toString(),
                    target.toString()).redirectErrorStream(true).start();
            p.getInputStream().readAllBytes();
            return p.waitFor() == 0 && Files.exists(link);
        } catch (Exception unavailable) {
            return false;
        }
    }

    public static void main(String[] args) throws Exception {
        Path root = Files.createTempDirectory("gesher-native-library-paths-");
        try {
            discovery(root);
            sync(root);
            Path apk = root.resolve("fixture.apk");
            Path destination = Files.createDirectory(root.resolve("lib"));
            archive(apk, "lib/arm64-v8a/libother.so", "lib/x86_64/libvalid.so",
                    "lib/x86_64/lib\u05e9\u05dc\u05d5\u05dd.so", "lib/x86_64/nested/ignored.so",
                    "lib/x86_64/lib with spaces.so", "lib/x86_64/CONIN$notes.so",
                    "lib/x86_64/COM10.so", "lib/x86_64/LPT\u2074.so",
                    "assets/ignored.so", "lib/x86_64/not-a-library.txt");
            check(NativeLibs.ABI.equals(NativeLibs.abiFor(apk.toString())), "ABI preference");
            check(NativeLibs.extract(apk.toString(), destination.toString()) == 6,
                    "Only direct selected-ABI libraries are extracted");
            check(Arrays.equals(PAYLOAD, Files.readAllBytes(destination.resolve("libvalid.so"))),
                    "Native library contents preserved");
            check(Arrays.equals(PAYLOAD, Files.readAllBytes(destination.resolve("lib\u05e9\u05dc\u05d5\u05dd.so"))),
                    "Unicode native library contents preserved");
            for (String ordinary : new String[]{"lib with spaces.so", "CONIN$notes.so", "COM10.so", "LPT\u2074.so"}) {
                check(Arrays.equals(PAYLOAD, Files.readAllBytes(destination.resolve(ordinary))),
                        "Ordinary name is not mistaken for a device: " + ordinary);
            }
            check(!Files.exists(destination.resolve("libother.so")), "Other ABI is not copied");
            check(!Files.exists(destination.resolve("nested")), "Nested ZIP entry is not copied");

            Path sentinel = root.resolve("outside.so");
            byte[] original = {42, 43, 44};
            Files.write(sentinel, original);
            Files.write(destination.resolve("libvalid.so"), original);
            for (String invalid : new String[]{"..\\outside.so", "nested\\..\\..\\outside.so",
                    "C:outside.so", "C:\\outside.so", "\\\\server\\share\\outside.so",
                    "libvalid.so:stream.so", "bad?.so", "bad*.so", "bad|name.so",
                    "bad<name.so", "bad>name.so", "bad\"name.so", "bad\u0001name.so",
                    "NUL.so", "con.so", "PRN.so", "aux.so", "COM1.so", "LPT9.so",
                    "COM\u00b9.so", "LPT\u00b3.so", "CONIN$.so", "conout$.so",
                    "NUL .so", "cOn   .so", "COM1 .so", "LPT\u00b3 .so", "conout$ .so"}) {
                archive(apk, "lib/x86_64/libvalid.so", "lib/x86_64/" + invalid);
                boolean rejected = false;
                try { NativeLibs.extract(apk.toString(), destination.toString(), "x86_64"); }
                catch (IOException expected) { rejected = true; }
                check(rejected, "Reject Windows path/device syntax: " + invalid);
                check(Arrays.equals(original, Files.readAllBytes(sentinel)), "Outside file unchanged");
                check(Arrays.equals(original, Files.readAllBytes(destination.resolve("libvalid.so"))),
                        "Validate all names before modifying the first library");
            }
            archive(apk, "lib/arm64-v8a/libarm.so");
            check("arm64-v8a".equals(NativeLibs.abiFor(apk.toString())), "ARM64 fallback");
            check(NativeLibs.extract(apk.toString(), destination.toString()) == 1,
                    "ARM64 extraction remains supported");
            System.out.println("NativeLibraryPathsTest all passed: " + checks + " checks");
        } finally {
            try (java.util.stream.Stream<Path> paths = Files.walk(root)) {
                for (Path path : (Iterable<Path>) paths.sorted(Comparator.reverseOrder())::iterator) {
                    Files.deleteIfExists(path);
                }
            }
        }
    }
}
