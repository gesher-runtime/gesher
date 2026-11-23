/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.server;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.nio.file.Files;
import java.nio.file.StandardCopyOption;
import java.security.MessageDigest;
import java.security.NoSuchAlgorithmException;

/** Disk operations for the BlobStore. Published files never back a writable guest descriptor. */
final class BlobStoreFiles {
    private BlobStoreFiles() { }

    static File snapshot(File source, File root, byte[] expected) throws IOException {
        File result = Files.createTempFile(root.toPath(), "data-", ".bin").toFile();
        boolean keep = false;
        try {
            if (expected == null) {
                Files.copy(source.toPath(), result.toPath(), StandardCopyOption.REPLACE_EXISTING);
                try (java.io.RandomAccessFile file = new java.io.RandomAccessFile(result, "rw")) {
                    file.getFD().sync();
                }
            } else {
                MessageDigest digest = sha256();
                try (InputStream input = Files.newInputStream(source.toPath());
                        FileOutputStream output = new FileOutputStream(result)) {
                    byte[] buffer = new byte[128 * 1024];
                    for (int read; (read = input.read(buffer)) != -1; ) {
                        digest.update(buffer, 0, read);
                        output.write(buffer, 0, read);
                    }
                    if (!MessageDigest.isEqual(expected, digest.digest())) {
                        throw new IOException("Blob does not match its declared SHA-256 digest");
                    }
                    output.getFD().sync();
                }
            }
            keep = true;
            return result;
        } finally {
            if (!keep) discard(result);
        }
    }

    static MessageDigest sha256() {
        try {
            return MessageDigest.getInstance("SHA-256");
        } catch (NoSuchAlgorithmException missing) {
            throw new AssertionError(missing);
        }
    }

    static void discard(File file) {
        if (file != null) {
            // Windows can keep a stale guest descriptor alive after a session changes.
            // The next maintenance pass retries deletion when its last handle is gone.
            try { Files.deleteIfExists(file.toPath()); }
            catch (IOException ignored) { }
        }
    }
}
