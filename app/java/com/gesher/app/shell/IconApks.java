/* Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.app.shell;

import java.io.File;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.file.Files;
import java.util.ArrayList;
import java.util.List;
import java.util.Locale;
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
        String name = source.getName().toLowerCase(Locale.ROOT);
        if (!name.endsWith(".apks") && !name.endsWith(".xapk")) {
            return new IconApks(source.getAbsolutePath(),
                    new String[] {source.getAbsolutePath()}, null);
        }
        File dir = Files.createTempDirectory("gesher-icon-").toFile();
        try (ZipFile zip = new ZipFile(source)) {
            List<ZipEntry> entries = new ArrayList<>();
            ZipEntry base = null;
            ZipEntry candidate = null;
            int candidates = 0;
            for (java.util.Enumeration<? extends ZipEntry> e = zip.entries(); e.hasMoreElements();) {
                ZipEntry entry = e.nextElement();
                String leaf = new File(entry.getName()).getName();
                if (entry.isDirectory() || !leaf.endsWith(".apk")) continue;
                if (entries.size() >= 512) throw new IOException("too many APKs in " + source);
                entries.add(entry);
                if (leaf.equals("base.apk") || leaf.equals("base-master.apk")
                        || leaf.equals("universal.apk")) {
                    if (base != null) throw new IOException("ambiguous base APK in " + source);
                    base = entry;
                } else if (!leaf.startsWith("split_") && !leaf.startsWith("config.")
                        && !entry.getName().contains("/")) {
                    candidate = entry;
                    candidates++;
                }
            }
            if (base == null && candidates == 1) base = candidate;
            if (base == null) throw new IOException("no unique base APK in " + source);
            entries.remove(base);
            entries.add(0, base);
            List<String> paths = new ArrayList<>();
            long remaining = 1024L * 1024 * 1024;
            byte[] buffer = new byte[64 * 1024];
            for (ZipEntry entry : entries) {
                // Generated names also keep archive paths out of the filesystem.
                File apk = new File(dir, paths.size() + ".apk");
                try (InputStream in = zip.getInputStream(entry);
                        OutputStream out = Files.newOutputStream(apk.toPath())) {
                    int count;
                    while ((count = in.read(buffer)) != -1) {
                        remaining -= count;
                        if (remaining < 0) throw new IOException("APK bundle too large: " + source);
                        out.write(buffer, 0, count);
                    }
                }
                paths.add(apk.getAbsolutePath());
            }
            return new IconApks(paths.get(0), paths.toArray(new String[0]), dir);
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
