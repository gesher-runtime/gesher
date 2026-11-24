/* Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.app;

import java.io.File;
import java.io.IOException;
import java.util.Enumeration;
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
}
