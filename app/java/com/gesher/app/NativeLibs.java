/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.app;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.file.DirectoryStream;
import java.nio.file.Files;
import java.nio.file.LinkOption;
import java.nio.file.NoSuchFileException;
import java.nio.file.Path;
import java.nio.file.StandardCopyOption;
import java.nio.file.attribute.BasicFileAttributes;
import java.util.ArrayList;
import java.util.Enumeration;
import java.util.HashSet;
import java.util.List;
import java.util.Locale;
import java.util.Set;
import java.util.zip.ZipEntry;
import java.util.zip.ZipFile;

/**
 * The APK's native libraries: what the package manager does at install time for an app
 * with {@code extractNativeLibs}. The {@code lib/x86_64/*.so} entries are copied into
 * {@code ApplicationInfo.nativeLibraryDir}, the app class loader's library search path,
 * where {@code System.loadLibrary} finds them; dalvikvm loads them with Gesher's ELF
 * loader (docs/native-libs.md).
 */
public final class NativeLibs {
    /** The ABI Gesher runs natively: x86-64 Android code. */
    public static final String ABI = "x86_64";

    /**
     * The ABIs Gesher supports, in preference order: x86_64 natively, then
     * arm64-v8a through the translator (docs/arm64.md, ADR 0047). This is the
     * order of {@code Build.SUPPORTED_ABIS} / {@code ro.product.cpu.abilist}.
     */
    public static final String[] SUPPORTED = {"x86_64", "arm64-v8a"};

    private NativeLibs() {
    }

    /**
     * The best-matching ABI the APKs (a base APK and its splits) actually ship native code
     * for: the first of {@link #SUPPORTED} that one of them has a {@code lib/<abi>/*.so}
     * entry for, or {@code null} if none has native code for any of them. A split APK
     * install (.xapk, .apks) has its libraries in an ABI split (config.arm64_v8a.apk),
     * not in base.apk.
     */
    public static String abiFor(String... apks) throws IOException {
        int best = SUPPORTED.length;
        if (best == 0) return null;
        String[] prefixes = new String[best];
        for (int i = 0; i < best; i++) prefixes[i] = "lib/" + SUPPORTED[i] + "/";
        // Keep searching later splits for a preferred ABI, without reopening any
        // archive just to search it again for the fallback ABI.
        for (String apk : apks) {
            try (ZipFile zip = new ZipFile(apk)) {
                for (Enumeration<? extends ZipEntry> e = zip.entries(); e.hasMoreElements(); ) {
                    String name = e.nextElement().getName();
                    if (!name.endsWith(".so")) continue;
                    for (int i = 0; i < best; i++) {
                        String prefix = prefixes[i];
                        if (name.startsWith(prefix) && name.indexOf('/', prefix.length()) < 0) {
                            if (i == 0) return SUPPORTED[0];
                            best = i;
                            break;
                        }
                    }
                }
            }
        }
        return best < SUPPORTED.length ? SUPPORTED[best] : null;
    }

    /**
     * The ABI the package runs as ({@code ApplicationInfo.primaryCpuAbi}): the
     * APK's best match, or {@link #ABI} for an APK without native code (or one
     * that cannot be read).
     */
    public static String forApk(String... apks) {
        try {
            String abi = abiFor(apks);
            return abi != null ? abi : ABI;
        } catch (IOException e) {
            return ABI;
        }
    }

    /**
     * The directory name Android installs an ABI's libraries under
     * ({@code <code path>/lib/<name>}): {@code arm64} for arm64-v8a, the ABI's
     * own name otherwise (x86_64).
     */
    public static String instructionSetDir(String abi) {
        return "arm64-v8a".equals(abi) ? "arm64" : abi;
    }

    /**
     * Copies the APK's native libraries into {@code dir}; returns how many. The
     * ABI is the APK's best match ({@link #abiFor}); an arm64-v8a-only APK's
     * libraries are extracted and loaded through the translator, exactly as
     * x86_64 ones are.
     */
    public static int extract(String apk, String dir) throws IOException {
        String abi = abiFor(apk);
        return abi != null ? extract(apk, dir, abi) : 0;
    }

    /**
     * Copies the APK's native libraries for {@code abi} (the package's, {@link #abiFor}
     * over all its APKs) into {@code dir}; returns how many. A library already on disk
     * with identical contents is kept as it is, not rewritten.
     */
    public static int extract(String apk, String dir, String abi) throws IOException {
        return extract(apk, dir, abi, null);
    }

    /**
     * The package manager's native-library directory for a system package's code
     * directory: {@code dir} holds exactly the APK's libraries for {@code abi}, extracted
     * as {@link #extract} does, with {@code .so} files the APK no longer has removed.
     * A {@code dir} (or its {@code lib} parent) that is a link or not a directory is
     * refused, before anything is written. Returns how many libraries the APK has.
     */
    public static int syncDirectory(String apk, String dir, String abi) throws IOException {
        Path target = new File(dir).toPath();
        Path parent = target.getParent();
        if (parent != null) requireDirectoryOrAbsent(parent);
        requireDirectoryOrAbsent(target);
        HashSet<String> names = new HashSet<>();
        int count = extract(apk, dir, abi, names);
        if (Files.isDirectory(target, LinkOption.NOFOLLOW_LINKS)) {
            try (DirectoryStream<Path> stream = Files.newDirectoryStream(target)) {
                for (Path entry : stream) {
                    String name = entry.getFileName().toString();
                    // Windows names are case-insensitive: never remove a listed one.
                    if (!name.endsWith(".so") || names.contains(name.toLowerCase(Locale.ROOT))
                            || !Files.isRegularFile(entry, LinkOption.NOFOLLOW_LINKS)) {
                        continue;
                    }
                    try {
                        Files.deleteIfExists(entry);
                    } catch (IOException inUse) {
                        // A process may still map it; no package lists it any more.
                    }
                }
            }
        }
        return count;
    }

    private static void requireDirectoryOrAbsent(Path path) throws IOException {
        BasicFileAttributes attributes;
        try {
            attributes = Files.readAttributes(path, BasicFileAttributes.class,
                    LinkOption.NOFOLLOW_LINKS);
        } catch (NoSuchFileException absent) {
            return;
        }
        // isOther: a Windows junction or other reparse point, not a plain directory.
        if (attributes.isSymbolicLink() || attributes.isOther() || !attributes.isDirectory()) {
            throw new IOException("Refusing native library path that is not a directory: "
                    + path);
        }
    }

    private static int extract(String apk, String dir, String abi, Set<String> names)
            throws IOException {
        String prefix = "lib/" + abi + "/";
        int count = 0;
        byte[] buffer = new byte[64 * 1024];
        byte[] existing = new byte[64 * 1024];
        try (ZipFile zip = new ZipFile(apk)) {
            List<ZipEntry> libraries = new ArrayList<>();
            for (Enumeration<? extends ZipEntry> e = zip.entries(); e.hasMoreElements(); ) {
                ZipEntry entry = e.nextElement();
                String name = entry.getName();
                if (entry.isDirectory() || !name.startsWith(prefix) || !name.endsWith(".so")
                        || name.indexOf('/', prefix.length()) >= 0) {
                    continue;
                }
                validateWindowsLeaf(name.substring(prefix.length()));
                libraries.add(entry);
            }
            // Validate the complete selected set before opening any destination.
            // ZIP uses '/', but Windows also interprets '\\' and ':' as path syntax.
            for (ZipEntry entry : libraries) {
                File out = new File(dir, entry.getName().substring(prefix.length()));
                if (names != null) names.add(out.getName().toLowerCase(Locale.ROOT));
                // Unchanged: keep the file a running process may have mapped.
                if (sameContents(zip, entry, out, buffer, existing)) {
                    count++;
                    continue;
                }
                out.getParentFile().mkdirs();
                // Write beside it and rename over it: a running process maps its
                // libraries from these files, and Windows refuses to truncate a
                // mapped file, but replaces it by rename. A name of its own, so
                // concurrent extractions do not share one; ATOMIC_MOVE is a plain
                // rename() (REPLACE_EXISTING would unlink the target first).
                File part = File.createTempFile(out.getName(), ".part", out.getParentFile());
                boolean moved = false;
                try {
                    try (InputStream in = zip.getInputStream(entry);
                            OutputStream os = new FileOutputStream(part)) {
                        for (int n; (n = in.read(buffer)) > 0; ) {
                            os.write(buffer, 0, n);
                        }
                    }
                    Files.move(part.toPath(), out.toPath(), StandardCopyOption.ATOMIC_MOVE);
                    moved = true;
                } finally {
                    if (!moved) part.delete();
                }
                count++;
            }
        }
        return count;
    }

    private static boolean sameContents(ZipFile zip, ZipEntry entry, File file, byte[] a,
            byte[] b) throws IOException {
        Path path = file.toPath();
        if (!Files.isRegularFile(path, LinkOption.NOFOLLOW_LINKS)) return false;
        long size = entry.getSize();
        if (size >= 0 && Files.size(path) != size) return false;
        try (InputStream expected = zip.getInputStream(entry);
                InputStream actual = Files.newInputStream(path, LinkOption.NOFOLLOW_LINKS)) {
            while (true) {
                int n = expected.readNBytes(a, 0, a.length);
                int m = actual.readNBytes(b, 0, n == 0 ? 1 : n);
                if (n != m) return false;
                if (n == 0) return true;
                for (int i = 0; i < n; i++) {
                    if (a[i] != b[i]) return false;
                }
            }
        }
    }

    private static void validateWindowsLeaf(String name) throws IOException {
        for (int i = 0; i < name.length(); i++) {
            char c = name.charAt(i);
            if (c < 32 || "<>:\"/\\|?*".indexOf(c) >= 0) {
                throw new IOException("Native library name is not a Windows file name");
            }
        }
        // Keep DOS device stems out of extraction, including their console aliases
        // and ASCII spaces before an extension (e.g. NUL .so).
        int stemEnd = name.indexOf('.');
        while (stemEnd > 0 && name.charAt(stemEnd - 1) == ' ') stemEnd--;
        String stem = name.substring(0, stemEnd).toUpperCase(Locale.ROOT);
        if (stem.equals("CON") || stem.equals("PRN") || stem.equals("AUX")
                || stem.equals("NUL") || stem.equals("CONIN$") || stem.equals("CONOUT$")
                || (stem.length() == 4
                && (stem.startsWith("COM") || stem.startsWith("LPT"))
                && "123456789\u00b9\u00b2\u00b3".indexOf(stem.charAt(3)) >= 0)) {
            throw new IOException("Native library name denotes a Windows device");
        }
    }
}
