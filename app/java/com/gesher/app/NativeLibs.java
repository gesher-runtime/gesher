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
import java.util.Enumeration;
import java.util.zip.ZipEntry;
import java.util.zip.ZipFile;

/**
 * The APK's native libraries: what the package manager does at install time for an app
 * with {@code extractNativeLibs}. The {@code lib/x86_64/*.so} entries are copied into
 * {@code ApplicationInfo.nativeLibraryDir}, the app class loader's library search path,
 * where {@code System.loadLibrary} finds them; dalvikvm loads them with Gesher's ELF
 * loader (docs/native-libs.md).
 */
final class NativeLibs {
    /** The ABI Gesher runs: x86-64 Android code. */
    static final String ABI = "x86_64";

    private NativeLibs() {
    }

    /** Copies the APK's {@code lib/<ABI>/*.so} into {@code dir}; returns how many. */
    static int extract(String apk, String dir) throws IOException {
        String prefix = "lib/" + ABI + "/";
        int count = 0;
        byte[] buffer = new byte[64 * 1024];
        try (ZipFile zip = new ZipFile(apk)) {
            for (Enumeration<? extends ZipEntry> e = zip.entries(); e.hasMoreElements(); ) {
                ZipEntry entry = e.nextElement();
                String name = entry.getName();
                if (entry.isDirectory() || !name.startsWith(prefix) || !name.endsWith(".so")
                        || name.indexOf('/', prefix.length()) >= 0) {
                    continue;
                }
                File out = new File(dir, name.substring(prefix.length()));
                out.getParentFile().mkdirs();
                try (InputStream in = zip.getInputStream(entry);
                        OutputStream os = new FileOutputStream(out)) {
                    for (int n; (n = in.read(buffer)) > 0; ) {
                        os.write(buffer, 0, n);
                    }
                }
                count++;
            }
        }
        return count;
    }
}
