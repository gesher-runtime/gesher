/* Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.app.shell;

import com.gesher.app.PackageBundles;
import com.gesher.server.ApkPackage;
import java.io.File;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.file.Files;
import java.util.ArrayList;
import java.util.List;
import java.util.zip.ZipEntry;
import java.util.zip.ZipFile;

/** Temporary resource inputs for an APK or a split APK bundle; never installs anything. */
final class IconApks implements AutoCloseable {
    final String base;
    final String[] paths;
    private final File temporary;

    private IconApks(String base, String[] paths, File temporary) {
        this.base = base;
        this.paths = paths;
        this.temporary = temporary;
    }

    static IconApks open(File source) throws IOException {
        if (!PackageBundles.isBundle(source)) {
            return new IconApks(source.getAbsolutePath(),
                    new String[] {source.getAbsolutePath()}, null);
        }
        File dir = Files.createTempDirectory("gesher-icon-").toFile();
        try (ZipFile zip = new ZipFile(source)) {
            // The APKs the installer would install, not an APK set's alternatives.
            List<ZipEntry> entries = new ArrayList<>();
            for (String path : PackageBundles.select(zip,
                    PackageBundles.Device.gesher(android.os.Build.VERSION.SDK_INT))) {
                if (entries.size() >= 512) throw new IOException("too many APKs in " + source);
                entries.add(zip.getEntry(path));
            }
            List<String> paths = new ArrayList<>();
            String base = null;
            long remaining = 1024L * 1024 * 1024;
            byte[] buffer = new byte[64 * 1024];
            for (int i = 0; i < entries.size(); i++) {
                // Generated names also keep archive paths out of the filesystem.
                File apk = new File(dir, i + ".apk");
                try (InputStream in = zip.getInputStream(entries.get(i));
                        OutputStream out = Files.newOutputStream(apk.toPath())) {
                    int count;
                    while ((count = in.read(buffer)) != -1) {
                        remaining -= count;
                        if (remaining < 0) throw new IOException("APK bundle too large: " + source);
                        out.write(buffer, 0, count);
                    }
                }
                if (ApkPackage.isBaseApk(apk)) {
                    if (base != null) throw new IOException("ambiguous base APK in " + source);
                    base = apk.getAbsolutePath();
                } else {
                    paths.add(apk.getAbsolutePath());
                }
            }
            if (base == null) throw new IOException("no unique base APK in " + source);
            paths.add(0, base);
            return new IconApks(base, paths.toArray(new String[0]), dir);
        } catch (IOException | RuntimeException e) {
            remove(dir);
            throw e;
        }
    }

    @Override public void close() { if (temporary != null) remove(temporary); }

    private static void remove(File dir) {
        File[] files = dir.listFiles();
        if (files != null) for (File file : files) file.delete();
        dir.delete();
    }
}
