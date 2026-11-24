/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package com.gesher.app;

import java.io.File;
import java.io.IOException;
import java.nio.file.Files;
import java.util.LinkedHashMap;
import java.util.Map;
import java.util.zip.ZipEntry;
import java.util.zip.ZipOutputStream;

/** The production bundle classification on real ZIP files; no APK parser or device. */
public final class PackageBundlesTest {
    private static int checks;
    private static void check(boolean ok, String message) {
        if (!ok) throw new AssertionError(message);
        checks++;
    }
    private static File zip(File dir, String name, Map<String, byte[]> entries) throws IOException {
        File file = new File(dir, name);
        try (ZipOutputStream out = new ZipOutputStream(Files.newOutputStream(file.toPath()))) {
            for (Map.Entry<String, byte[]> entry : entries.entrySet()) {
                out.putNextEntry(new ZipEntry(entry.getKey()));
                out.write(entry.getValue());
                out.closeEntry();
            }
        }
        return file;
    }
    private static Map<String, byte[]> entries(String... names) {
        Map<String, byte[]> map = new LinkedHashMap<>();
        for (String name : names) map.put(name, name.endsWith("/") ? new byte[0] : new byte[] {7});
        return map;
    }

    public static void main(String[] args) throws Exception {
        File work = new File(args[0]);
        Files.createDirectories(work.toPath());
        detection(work);
        System.out.println("PackageBundlesTest all passed: " + checks + " checks");
    }

    private static void detection(File work) throws Exception {
        Map<String, byte[]> apk = entries("AndroidManifest.xml", "classes.dex", "assets/inner.apk");
        check(!PackageBundles.isBundle(zip(work, "plain.apk", apk)), "An APK is not a bundle");
        check(!PackageBundles.isBundle(zip(work, "plain.xapk", apk)),
                "An APK saved as .xapk was classified by its suffix");
        check(!PackageBundles.isBundle(zip(work, "plain.apks", apk)),
                "An APK saved as .apks was classified by its suffix");
        check(PackageBundles.isBundle(zip(work, "set.apk", entries("base.apk", "split_config.xxhdpi.apk"))),
                "A ZIP of APKs saved as .apk was not recognized as a bundle");
        check(PackageBundles.isBundle(zip(work, "set.apkm", entries("info.json", "base.apk"))),
                "An APKM was not recognized as a bundle");
        check(PackageBundles.isBundle(zip(work, "set.apks", entries("toc.pb", "splits/base-master.apk"))),
                "A bundletool APK set was not recognized as a bundle");
        check(!PackageBundles.isBundle(zip(work, "empty.xapk", entries("manifest.json", "dir.apk/"))),
                "A ZIP without APK files was classified as a bundle");
        File notZip = new File(work, "garbage.apkm");
        Files.write(notZip.toPath(), new byte[] {1, 2, 3});
        check(!PackageBundles.isBundle(notZip), "A file that is not a ZIP was classified as a bundle");
    }
}
