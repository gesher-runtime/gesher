/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package gesher.services;

import android.system.ErrnoException;
import android.system.Os;
import android.system.OsConstants;
import com.gesher.server.Zygote;
import java.io.File;
import java.io.FileDescriptor;
import java.io.FileInputStream;
import java.nio.charset.StandardCharsets;
import java.nio.file.DirectoryStream;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.Paths;
import java.util.Arrays;

/** Actual Java/libcore file operations on a private SMB share supplied by the Windows runner. */
public final class UncFileContractTest {
    private static int checks;
    private static void require(boolean value, String reason) {
        if (!value) throw new AssertionError(reason);
        checks++;
    }
    private static void denied(String path, int flags, int error) throws Exception {
        try {
            FileDescriptor descriptor = Os.open(path, flags, 0600);
            Os.close(descriptor);
        } catch (ErrnoException expected) {
            require(expected.errno == error, "Wrong errno for malformed UNC namespace: " + expected);
            return;
        }
        throw new AssertionError("Malformed UNC namespace reached a file: " + path);
    }
    private static void readNative(String path, byte[] expected) throws Exception {
        FileDescriptor descriptor = Os.open(path, OsConstants.O_RDONLY, 0);
        try {
            byte[] actual = new byte[expected.length];
            int offset = 0;
            while (offset < actual.length) {
                int count = Os.read(descriptor, actual, offset, actual.length - offset);
                if (count <= 0) break;
                offset += count;
            }
            require(offset == actual.length && Arrays.equals(expected, actual),
                    "Native UNC path reached different contents: " + path);
        } finally { Os.close(descriptor); }
    }

    public static void main(String[] args) throws Exception {
        require(args.length == 1, "Pass the native UNC root of a private writable test share");
        System.loadLibrary("android_runtime");
        String guest = Zygote.fromWindowsPath(args[0]);
        require(guest.startsWith("/mnt/unc/"), "The fixture must use an actual UNC share");
        Path root = Paths.get(guest);
        require(root.normalize().toString().equals(guest), "Java Path changed the UNC share identity");
        require(new File(guest).getAbsolutePath().equals(guest), "Java File changed the UNC share identity");
        Path directory = Files.createTempDirectory(root, "gesher-unc-");
        Path child = directory.resolve("תיקייה 😀");
        Path original = child.resolve("קובץ data.txt");
        Path renamed = child.resolve("renamed קובץ.txt");
        byte[] content = "Real UNC file contents: שלום 😀\n".getBytes(StandardCharsets.UTF_8);
        try {
            Files.createDirectory(child);
            Files.write(original, content);
            require(original.toFile().isFile() && original.toFile().length() == content.length,
                    "java.io.File did not inspect the created UNC file");
            require(Arrays.equals(Files.readAllBytes(original), content), "NIO UNC contents changed");
            try (FileInputStream input = new FileInputStream(original.toFile())) {
                require(Arrays.equals(input.readAllBytes(), content), "java.io UNC contents changed");
            }
            String canonical = original.toFile().getCanonicalPath();
            require(canonical.startsWith("/mnt/unc/"), "Canonical UNC path lost its guest namespace: " + canonical);
            require(Files.isSameFile(original, Paths.get(canonical)), "Canonical UNC path names another file");
            require(Os.realpath(original.toString()).startsWith("/mnt/unc/"), "Native realpath lost the UNC root");
            try (DirectoryStream<Path> entries = Files.newDirectoryStream(child)) {
                int count = 0;
                for (Path entry : entries) {
                    require(entry.getFileName().equals(original.getFileName()), "UNC directory returned a wrong entry");
                    count++;
                }
                require(count == 1, "UNC directory enumeration missed or duplicated its file");
            }
            Files.move(original, renamed);
            require(!Files.exists(original) && Arrays.equals(Files.readAllBytes(renamed), content),
                    "UNC rename did not preserve the file and contents");
            String windows = Zygote.windowsPath(renamed.toString());
            require(Zygote.fromWindowsPath(windows).equals(renamed.toString()), "UNC native round trip changed identity");
            readNative(windows, content);
            readNative(windows.replace('\\', '/'), content);
            readNative("\\\\?\\UNC\\" + windows.substring(2), content);
            denied("/mnt/unc/server", OsConstants.O_RDONLY, OsConstants.ENOENT);
            denied("/mnt/unc/./pipe/name", OsConstants.O_RDONLY, OsConstants.ENOENT);
            denied("/mnt/unc/server", OsConstants.O_CREAT | OsConstants.O_WRONLY, OsConstants.EACCES);
        } finally {
            Files.deleteIfExists(original);
            Files.deleteIfExists(renamed);
            Files.deleteIfExists(child);
            Files.deleteIfExists(directory);
        }
        require(!Files.exists(directory), "Owned UNC fixture files survived cleanup");
        System.out.println("UncFileContractTest all passed: " + checks + " checks");
    }
}
