/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.server;

import com.gesher.app.NativeLibs;

import java.io.File;
import java.io.IOException;
import java.nio.file.DirectoryStream;
import java.nio.file.Files;
import java.nio.file.LinkOption;
import java.nio.file.Path;
import java.nio.file.attribute.FileTime;
import java.util.Enumeration;
import java.util.HashMap;
import java.util.Map;
import java.util.zip.ZipEntry;
import java.util.zip.ZipFile;

/** Fixed addon slots; all checks precede manifest parsing and app code loading. */
public final class MicrogAddons {
    public static final java.util.List<String> PACKAGES = java.util.List.of(
            GmsPackageSupport.PACKAGE_GMS, GmsPackageSupport.PACKAGE_VENDING,
            GmsSignatureSpoofing.PACKAGE_GSF);

    private MicrogAddons() { }

    static ApkPackage load(File apk, String expectedPackage) throws Exception {
        if (!GmsSignatureSpoofing.verifyAddonApk(expectedPackage, apk.getPath())) {
            throw new SecurityException("Untrusted microG addon for " + expectedPackage);
        }
        ApkPackage parsed = ApkPackage.parse(apk.getPath());
        parsed.setSystemAddon(true);
        return parsed;
    }

    /**
     * Configures {@code nativeLibraryDir} / {@code primaryCpuAbi} and extracts native
     * libraries for a staged {@code priv-app} addon when its directory is missing or out
     * of sync with the APK.
     */
    static int prepareNativeLibraries(ApkPackage pkg) throws IOException {
        File apkFile = new File(pkg.apkPath);
        File codeDir = apkFile.getParentFile();
        if (codeDir == null) return 0;
        String abi = NativeLibs.abiFor(pkg.apkPath);
        File libDir = new File(codeDir, "lib/"
                + NativeLibs.instructionSetDir(abi != null ? abi : NativeLibs.ABI));
        pkg.setNativeLibraryDir(libDir.getPath());
        pkg.setPrimaryCpuAbi(abi);
        if (abi == null) return 0;
        return syncNativeLibraries(apkFile, libDir, abi);
    }

    static int syncNativeLibraries(File apkFile, File libDir, String abi) throws IOException {
        Path libPath = libDir.toPath();
        Path libRoot = libPath.getParent();
        if ((libRoot != null && Files.isSymbolicLink(libRoot))
                || Files.isSymbolicLink(libPath)
                || (Files.exists(libPath, LinkOption.NOFOLLOW_LINKS)
                        && !Files.isDirectory(libPath, LinkOption.NOFOLLOW_LINKS))) {
            throw new IOException("Refusing non-directory native library path: " + libDir);
        }

        String prefix = "lib/" + abi + "/";
        Map<String, Long> expected = new HashMap<>();
        try (ZipFile zip = new ZipFile(apkFile)) {
            for (Enumeration<? extends ZipEntry> e = zip.entries(); e.hasMoreElements(); ) {
                ZipEntry entry = e.nextElement();
                String name = entry.getName();
                if (entry.isDirectory() || !name.startsWith(prefix) || !name.endsWith(".so")
                        || name.indexOf('/', prefix.length()) >= 0) {
                    continue;
                }
                expected.put(name.substring(prefix.length()), entry.getSize());
            }
        }
        if (expected.isEmpty()) return 0;

        long apkMtime = Files.getLastModifiedTime(apkFile.toPath()).toMillis();
        if (isNativeLibDirCurrent(libPath, expected, apkMtime)) {
            return 0;
        }

        int count = NativeLibs.extract(apkFile.getPath(), libDir.getPath(), abi);
        if (apkMtime > 0) {
            FileTime stamp = FileTime.fromMillis(apkMtime);
            for (String leaf : expected.keySet()) {
                Path out = libPath.resolve(leaf);
                if (Files.isRegularFile(out, LinkOption.NOFOLLOW_LINKS)) {
                    Files.setLastModifiedTime(out, stamp);
                }
            }
        }
        if (Files.isDirectory(libPath, LinkOption.NOFOLLOW_LINKS)) {
            try (DirectoryStream<Path> stream = Files.newDirectoryStream(libPath)) {
                for (Path entry : stream) {
                    String name = entry.getFileName().toString();
                    if (name.endsWith(".so") && !expected.containsKey(name)
                            && Files.isRegularFile(entry, LinkOption.NOFOLLOW_LINKS)) {
                        Files.deleteIfExists(entry);
                    }
                }
            }
        }
        return count;
    }

    private static boolean isNativeLibDirCurrent(Path libPath, Map<String, Long> expected,
            long apkMtime) throws IOException {
        if (apkMtime <= 0 || !Files.isDirectory(libPath, LinkOption.NOFOLLOW_LINKS)) {
            return false;
        }
        for (Map.Entry<String, Long> e : expected.entrySet()) {
            String leaf = e.getKey();
            long size = e.getValue();
            if (size < 0 || leaf.indexOf('\\') >= 0 || leaf.indexOf(':') >= 0
                    || ".".equals(leaf) || "..".equals(leaf)) {
                return false;
            }
            Path candidate = libPath.resolve(leaf);
            if (!Files.isRegularFile(candidate, LinkOption.NOFOLLOW_LINKS)
                    || Files.size(candidate) != size
                    || Files.getLastModifiedTime(candidate, LinkOption.NOFOLLOW_LINKS).toMillis()
                            != apkMtime) {
                return false;
            }
        }
        try (DirectoryStream<Path> stream = Files.newDirectoryStream(libPath)) {
            for (Path entry : stream) {
                String name = entry.getFileName().toString();
                if (name.endsWith(".so") && !expected.containsKey(name)) {
                    return false;
                }
            }
        }
        return true;
    }

    /** Packaging preflight, with no Binder connection, account or network traffic. */
    public static void main(String[] args) {
        if (args.length != 3 || !"verify".equals(args[0])) {
            throw new IllegalArgumentException("MicrogAddons verify PACKAGE APK");
        }
        com.gesher.app.Digests.install();
        System.loadLibrary("android_runtime");
        if (!GmsSignatureSpoofing.verifyAddonApk(args[1], args[2])) {
            throw new SecurityException("Rejected microG addon for " + args[1]);
        }
        System.out.println("Verified official microG addon: " + args[1]);
    }
}
