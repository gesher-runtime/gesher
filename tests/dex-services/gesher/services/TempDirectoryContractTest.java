package gesher.services;

import android.system.Os;
import java.io.File;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.AccessDeniedException;
import java.nio.file.FileAlreadyExistsException;
import java.nio.file.Files;
import java.nio.file.NoSuchFileException;
import java.nio.file.Path;
import java.nio.file.Paths;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collections;
import java.util.List;

/** Fresh-VM tests: Android caches default temp paths at each API's first use. */
public final class TempDirectoryContractTest {
    private static int checks;

    private static void check(boolean condition, String message) {
        checks++;
        if (!condition) throw new AssertionError(message);
    }

    private static void ordinaryIo(String expected) throws Exception {
        Path parent = Paths.get(expected);
        check(parent.isAbsolute() && Files.isDirectory(parent), "Real absolute temporary directory");
        List<Path> cleanup = new ArrayList<>();
        byte[] bytes = "real temporary bytes: קבצים 😀".getBytes(StandardCharsets.UTF_8);
        try {
            Path directory = Files.createTempDirectory("gesher-temp-dir-");
            cleanup.add(directory);
            check(directory.getParent().equals(parent), "NIO temporary directory parent");
            check(Files.isDirectory(directory), "NIO directory exists on disk");

            Path file = Files.createTempFile("gesher-temp-file-", ".tmp");
            cleanup.add(file);
            check(file.getParent().equals(parent), "NIO temporary file parent");
            Files.write(file, bytes);
            check(Arrays.equals(bytes, Files.readAllBytes(file)), "NIO temporary file bytes");
            try {
                Files.createFile(file);
                throw new AssertionError("Existing temporary file was overwritten");
            } catch (FileAlreadyExistsException expectedFailure) {
                checks++;
            }
            check(Arrays.equals(bytes, Files.readAllBytes(file)), "Failed creation preserves bytes");

            File legacy = File.createTempFile("gesher-temp-legacy-", ".tmp");
            cleanup.add(legacy.toPath());
            check(legacy.toPath().getParent().equals(parent), "java.io temporary file parent");
            Files.write(legacy.toPath(), bytes);
            check(Arrays.equals(bytes, Files.readAllBytes(legacy.toPath())), "java.io temporary file bytes");

            Path child = directory.resolve("nested קבצים 😀.bin");
            cleanup.add(child);
            Files.write(child, bytes);
            check(Arrays.equals(bytes, Files.readAllBytes(child)), "Created directory accepts real files");
        } finally {
            Collections.reverse(cleanup);
            for (Path item : cleanup) {
                Files.deleteIfExists(item);
                check(!Files.exists(item), "Temporary file/directory was removed: " + item);
            }
        }
    }

    private static void failingIo(String mode) throws Exception {
        try {
            Path unexpected = Files.createTempDirectory("gesher-temp-must-fail-");
            Files.deleteIfExists(unexpected);
            throw new AssertionError("Invalid temporary directory unexpectedly succeeded");
        } catch (IOException expectedFailure) {
            if (mode.equals("missing")) {
                check(expectedFailure instanceof NoSuchFileException, "Missing directory preserves ENOENT");
            } else if (mode.equals("denied")) {
                check(expectedFailure instanceof AccessDeniedException, "Denied directory preserves EACCES");
            } else {
                checks++;
            }
            System.out.println("TempDirectoryContractTest expected NIO failure: " + expectedFailure);
        }
        try {
            File unexpected = File.createTempFile("gesher-temp-must-fail-", ".tmp");
            Files.deleteIfExists(unexpected.toPath());
            throw new AssertionError("Invalid java.io temporary directory unexpectedly succeeded");
        } catch (IOException expectedFailure) {
            checks++;
            System.out.println("TempDirectoryContractTest expected java.io failure: " + expectedFailure);
        }
    }

    public static void main(String[] args) throws Exception {
        if (args.length < 2) throw new IllegalArgumentException("mode expected-directory [app-cache]");
        String mode = args[0];
        String expected = args[1];
        check(expected.equals(System.getProperty("java.io.tmpdir")),
                "Bootstrap selected wrong directory: " + System.getProperty("java.io.tmpdir"));
        if (mode.equals("explicit")) {
            System.setProperty("java.io.tmpdir", expected + "/must-not-override");
            check(expected.equals(System.getProperty("java.io.tmpdir")), "Explicit -D remains immutable");
            System.setProperties(null);
            check(expected.equals(System.getProperty("java.io.tmpdir")), "Reset retains explicit -D");
            ordinaryIo(expected);
        } else if (mode.equals("cache")) {
            // Same order as ActivityThread.setupGraphicsSupport, before the
            // first File/Files temporary-file call in this fresh VM.
            String cache = args[2];
            check(Files.isDirectory(Paths.get(cache)), "App cache exists independently of host default");
            System.setProperty("java.io.tmpdir", cache);
            Os.setenv("TMPDIR", cache, true);
            check(cache.equals(System.getProperty("java.io.tmpdir")), "App cache replaces changeable default");
            check(cache.equals(System.getenv("TMPDIR")), "App cache updates native TMPDIR");
            System.setProperties(null);
            check(cache.equals(System.getProperty("java.io.tmpdir")), "Reset reads current native TMPDIR");
            ordinaryIo(cache);
        } else if (mode.equals("missing") || mode.equals("not-directory") || mode.equals("denied")) {
            failingIo(mode);
            check(expected.equals(System.getProperty("java.io.tmpdir")), "I/O failure does not replace selected path");
        } else if (mode.equals("default")) {
            check(expected.equals(System.getenv("TMPDIR")), "Java and native default directories agree");
            System.setProperty("java.io.tmpdir", expected + "/mutable-property-probe");
            check((expected + "/mutable-property-probe").equals(System.getProperty("java.io.tmpdir")),
                    "Automatically selected property remains mutable");
            System.setProperties(null);
            check(expected.equals(System.getProperty("java.io.tmpdir")), "Reset restores native default");
            ordinaryIo(expected);
        } else {
            throw new IllegalArgumentException("Unknown mode: " + mode);
        }
        System.out.println("TempDirectoryContractTest all passed: " + checks + " checks; mode=" + mode);
    }
}
