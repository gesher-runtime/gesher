/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
import java.io.File;
import java.nio.file.Files;
import java.nio.file.Path;

/** Host-only Device boundary; Installer methods are extracted unchanged. */
final class Device {
    static String root;
    static String dataDir() { return root; }
}

public final class PortablePathsHost {
    private static int checks;
    private static void check(boolean value, String message) {
        if (!value) throw new AssertionError(message);
        ++checks;
    }
    private static void same(String input, Path expected, String message) throws Exception {
        check(new File(Installer.resolveCodePath(input)).equals(expected.toFile()), message);
    }
    public static void main(String[] args) throws Exception {
        Path work = Path.of(args[0]).toRealPath();
        Path old = Files.createDirectories(work.resolve("old/data/app/com.fixture.copied-7"));
        Path copied = Files.createDirectories(work.resolve("copied/data/app/com.fixture.copied-7"));
        Device.root = work.resolve("copied/data").toString();
        same(old.toString(), copied, "A copied profile must not reuse the still-existing original APK path");
        check(Files.isDirectory(old), "Original profile remains untouched");
        same(copied.toString(), copied, "Current profile path remains valid");
        same(work.resolve("missing/app/com.fixture.copied-7").toString(), copied,
                "A moved profile resolves its own APK generation");
        Path absent = work.resolve("old/data/app/com.fixture.absent-3");
        same(absent.toString(), absent, "Missing generation remains missing for parser rejection");
        check(Installer.resolveCodePath(null) == null, "Null path remains null");
        check(Installer.resolveCodePath("").isEmpty(), "Empty path remains empty");
        same(".", Path.of("."), "Current-directory leaf is not relocated");
        same("..", Path.of(".."), "Parent-directory leaf is not relocated");
        Path ordinaryFile = work.resolve("copied/data/app/com.fixture.file-1");
        Files.writeString(ordinaryFile, "not a directory");
        Path missingFile = work.resolve("old/data/app/com.fixture.file-1");
        same(missingFile.toString(), missingFile, "A file cannot be adopted as installed code");
        Path linked = work.resolve("copied/data/app/com.fixture.link-1");
        Path outside = Files.createDirectories(work.resolve("outside/com.fixture.link-1"));
        boolean links = true;
        try { Files.createSymbolicLink(linked, outside); }
        catch (UnsupportedOperationException | java.nio.file.FileSystemException unsupported) {
            links = false;
            System.out.println("SKIP: host does not permit symbolic-link creation: " + unsupported);
        }
        if (links) {
            Path oldLink = work.resolve("old/data/app/com.fixture.link-1");
            same(oldLink.toString(), oldLink, "A linked code directory is not adopted");
            Path linkedRoot = Files.createDirectories(work.resolve("linked/data"));
            Files.createSymbolicLink(linkedRoot.resolve("app"), copied.getParent());
            Device.root = linkedRoot.toString();
            try {
                Installer.resolveCodePath(old.toString());
                throw new AssertionError("A linked installed-code root must remain rejected");
            } catch (SecurityException expected) { ++checks; }
        }
        System.out.println("PASS: production Installer relocation (" + checks + " checks)");
    }
}
