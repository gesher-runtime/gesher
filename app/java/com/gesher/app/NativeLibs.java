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
     * The best-matching ABI the APK actually ships native code for: the first of
     * {@link #SUPPORTED} that has a {@code lib/<abi>/*.so} entry, or {@code null}
     * if the APK has no native code for any of them.
     */
    public static String abiFor(String apk) throws IOException {
        try (ZipFile zip = new ZipFile(apk)) {
            for (String abi : SUPPORTED) {
                String prefix = "lib/" + abi + "/";
                for (Enumeration<? extends ZipEntry> e = zip.entries(); e.hasMoreElements(); ) {
                    String name = e.nextElement().getName();
                    if (name.startsWith(prefix) && name.endsWith(".so")
                            && name.indexOf('/', prefix.length()) < 0) {
                        return abi;
                    }
                }
            }
        }
        return null;
    }

    /**
     * Copies the APK's native libraries into {@code dir}; returns how many. The
     * ABI is the APK's best match ({@link #abiFor}); an arm64-v8a-only APK's
     * libraries are extracted and loaded through the translator, exactly as
     * x86_64 ones are.
     */
    public static int extract(String apk, String dir) throws IOException {
        String abi = abiFor(apk);
        if (abi == null) {
            return 0;
        }
        String prefix = "lib/" + abi + "/";
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
