/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package gesher.services;

import com.gesher.app.NativeLibs;
import java.io.IOException;
import java.nio.file.Files;
import java.nio.file.Path;
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

    public static void main(String[] args) throws Exception {
        Path root = Files.createTempDirectory("gesher-native-library-paths-");
        try {
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
