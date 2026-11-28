/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package com.gesher.app;

import android.system.ErrnoException;
import android.system.Os;
import com.gesher.server.Device;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.nio.file.Files;
import java.nio.file.LinkOption;
import java.nio.file.Path;

/** Publishes launch code without changing the permissions of the original APK. */
public final class ApkCode {
    private ApkCode() { }

    public static File snapshot(String sourcePath) throws IOException {
        File source = new File(sourcePath);
        // Code that is already read-only (an installed base.apk, or an APK its owner
        // published read-only) satisfies the dynamic-code admission boundary as it
        // is. Keep its path: the oat file, app image and JIT profile are keyed by it.
        if (source.isFile() && !source.canWrite()) {
            return source;
        }
        Path root = new File(new File(Device.dataDir()).getCanonicalFile(), "standalone-code").toPath();
        Files.createDirectories(root);
        if (!Files.isDirectory(root, LinkOption.NOFOLLOW_LINKS)) {
            throw new IOException("APK snapshot root is not a real directory: " + root);
        }
        File snapshot = Files.createTempFile(root, "launch-", ".apk").toFile();
        try {
            copy(new File(sourcePath), snapshot, false, true);
        } catch (IOException failure) {
            try {
                Files.deleteIfExists(snapshot.toPath());
            } catch (IOException cleanup) {
                failure.addSuppressed(cleanup);
            }
            throw failure;
        }
        snapshot.deleteOnExit();
        return snapshot;
    }

    public static void copy(File from, File to, boolean sync, boolean readOnly) throws IOException {
        byte[] buffer = new byte[64 * 1024];
        try (FileInputStream in = new FileInputStream(from);
                FileOutputStream out = new FileOutputStream(to)) {
            if (readOnly) {
                // The opened handle can fill the new file while subsequent opens are read-only.
                try {
                    Os.fchmod(out.getFD(), 0444);
                } catch (ErrnoException failure) {
                    throw new IOException("Cannot make staged APK read-only: " + to, failure);
                }
            }
            for (int n; (n = in.read(buffer)) > 0;) {
                out.write(buffer, 0, n);
            }
            if (sync) out.getFD().sync();
        }
    }
}
