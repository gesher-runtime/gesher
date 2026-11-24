/* Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.app;

import java.io.File;
import java.io.IOException;
import java.io.InputStream;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collections;
import java.util.Enumeration;
import java.util.LinkedHashSet;
import java.util.List;
import java.util.Set;
import java.util.zip.ZipEntry;
import java.util.zip.ZipException;
import java.util.zip.ZipFile;

/** ZIP containers holding the APKs for a single app installation. */
public final class PackageBundles {
    private PackageBundles() {}

    /**
     * Whether {@code file} is a ZIP of APKs (an APK set, .xapk, .apkm, .apk+) rather than an
     * APK, by its content: an APK is a ZIP with {@code AndroidManifest.xml} at its root, a
     * bundle has none there and holds {@code .apk} entries. The file name is not consulted,
     * so a single APK saved as {@code .xapk} installs as the APK it is. A file that is not a
     * ZIP is not a bundle; the APK parser reports it.
     */
    public static boolean isBundle(File file) throws IOException {
        try (ZipFile zip = new ZipFile(file)) {
            if (zip.getEntry("AndroidManifest.xml") != null) return false;
            for (Enumeration<? extends ZipEntry> e = zip.entries(); e.hasMoreElements(); ) {
                ZipEntry entry = e.nextElement();
                if (!entry.isDirectory() && entry.getName().endsWith(".apk")) return true;
            }
            return false;
        } catch (ZipException notZip) {
            return false;
        }
    }

    /**
     * The device an APK set is matched against, as bundletool's {@code DeviceSpec} describes
     * one: its SDK level, its ABIs in order of preference, its density (0: every density
     * split is kept) and its languages (null: every language split is kept).
     */
    public static final class Device {
        final int sdk;
        final List<String> abis;
        final int densityDpi;
        final Set<String> languages;

        public Device(int sdk, String[] abis, int densityDpi, String[] locales) {
            this.sdk = sdk;
            this.abis = Collections.unmodifiableList(Arrays.asList(abis.clone()));
            this.densityDpi = densityDpi;
            if (locales == null) {
                languages = null;
            } else {
                Set<String> set = new LinkedHashSet<>();
                for (String locale : locales) set.add(ApkSetToc.language(locale));
                languages = Collections.unmodifiableSet(set);
            }
        }

        /**
         * Gesher's device at SDK level {@code sdk}: {@link NativeLibs#SUPPORTED}, with every
         * density and language split. The density is chosen per launch (GESHER_DISPLAY) and
         * the languages follow Windows' settings and per-app locales at any time, while no
         * store delivers a missing split later, so these are all installed and resources
         * pick among them at run time, as for a universal APK.
         */
        public static Device gesher(int sdk) {
            return new Device(sdk, NativeLibs.SUPPORTED, 0, null);
        }

        @Override public String toString() {
            return "SDK " + sdk + ", ABIs " + String.join(", ", abis)
                    + (densityDpi != 0 ? ", " + densityDpi + " dpi" : "")
                    + (languages != null ? ", languages " + String.join(", ", languages) : "");
        }
    }

    /** The most a toc.pb is read into memory (bundletool's are kilobytes). */
    private static final int MAX_TOC = 16 * 1024 * 1024;

    /**
     * The archive paths of the APKs that make up the install from bundle {@code zip}: for a
     * bundletool APK set (it has {@code toc.pb}), those {@code device} gets, as
     * {@code bundletool install-apks} chooses them ({@link ApkSetToc}); otherwise every
     * {@code .apk} entry, in archive order.
     */
    public static List<String> select(ZipFile zip, Device device) throws IOException {
        ZipEntry toc = zip.getEntry("toc.pb");
        if (toc != null) {
            if (toc.getSize() > MAX_TOC) throw new IOException("toc.pb is too large");
            byte[] bytes;
            try (InputStream in = zip.getInputStream(toc)) {
                bytes = in.readNBytes(MAX_TOC + 1);
            }
            if (bytes.length > MAX_TOC) throw new IOException("toc.pb is too large");
            List<String> paths;
            try {
                paths = ApkSetToc.select(bytes, device);
            } catch (IllegalArgumentException invalid) {
                throw new IOException("Invalid toc.pb", invalid);
            }
            for (String path : paths) {
                ZipEntry entry = zip.getEntry(path);
                if (entry == null || entry.isDirectory() || !path.endsWith(".apk")) {
                    throw new IOException("toc.pb names " + path + ", which the APK set does not hold");
                }
            }
            return paths;
        }
        List<String> paths = new ArrayList<>();
        for (Enumeration<? extends ZipEntry> e = zip.entries(); e.hasMoreElements(); ) {
            ZipEntry entry = e.nextElement();
            if (!entry.isDirectory() && entry.getName().endsWith(".apk")) paths.add(entry.getName());
        }
        return paths;
    }
}
