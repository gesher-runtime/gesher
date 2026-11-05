/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.server;

import java.io.ByteArrayInputStream;
import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.nio.file.AtomicMoveNotSupportedException;
import java.nio.file.Files;
import java.nio.file.StandardCopyOption;
import java.util.Arrays;
import java.util.Properties;

/**
 * Account records sealed with Windows user-scoped DPAPI. The native bridge also
 * rejects app-process callers. No secret or encryption key is written in plaintext.
 * This protects stored records and the service API, not against arbitrary native
 * code running as the same Windows user outside Gesher's app identity model.
 */
public final class AccountSecretStore {
    private static boolean sLoaded;
    private static final byte[] MAGIC = {'G', 'A', 'C', '2'};
    private final File mFile;

    AccountSecretStore(File file) {
        mFile = file;
    }

    static synchronized void ensureLoaded() {
        if (!sLoaded) {
            String boot = System.getProperty("java.boot.class.path", "");
            for (String entry : boot.split(java.util.regex.Pattern.quote(File.pathSeparator))) {
                if (entry.replace('\\', '/').endsWith("/gesher-app.jar")) {
                    File library = new File(new File(entry).getParentFile(), "libgesher_account_crypto.so");
                    System.load(library.getAbsolutePath());
                    sLoaded = true;
                    return;
                }
            }
            throw new UnsatisfiedLinkError("Cannot locate the Gesher account crypto library");
        }
    }

    private static native byte[] nativeProtect(byte[] plaintext);
    private static native byte[] nativeUnprotect(byte[] ciphertext);

    private static byte[] protect(byte[] plaintext) throws IOException {
        try {
            ensureLoaded();
            byte[] sealed = nativeProtect(plaintext);
            if (sealed == null || sealed.length == 0) throw new IOException("DPAPI protection failed");
            return sealed;
        } catch (LinkageError e) {
            throw new IOException("Protected account storage is unavailable", e);
        }
    }

    private static byte[] unprotect(byte[] ciphertext) throws IOException {
        try {
            ensureLoaded();
            byte[] plain = nativeUnprotect(ciphertext);
            if (plain == null) throw new IOException("DPAPI authentication failed");
            return plain;
        } catch (LinkageError e) {
            throw new IOException("Protected account storage is unavailable", e);
        }
    }

    Properties read() throws IOException {
        if (!mFile.exists()) return null;
        byte[] record = Files.readAllBytes(mFile.toPath());
        if (record.length <= MAGIC.length
                || !Arrays.equals(MAGIC, Arrays.copyOf(record, MAGIC.length))) {
            throw new IOException("Invalid protected account store");
        }
        byte[] plain = unprotect(Arrays.copyOfRange(record, MAGIC.length, record.length));
        try {
            Properties values = new Properties();
            values.load(new ByteArrayInputStream(plain));
            if (!"2".equals(values.getProperty("version"))) {
                throw new IOException("Unsupported protected account store version");
            }
            return values;
        } finally {
            Arrays.fill(plain, (byte) 0);
        }
    }

    void write(Properties values) throws IOException {
        values.setProperty("version", "2");
        ByteArrayOutputStream bytes = new ByteArrayOutputStream();
        values.store(bytes, "Gesher protected accounts");
        byte[] plain = bytes.toByteArray();
        byte[] sealed;
        try {
            sealed = protect(plain);
        } finally {
            Arrays.fill(plain, (byte) 0);
        }
        File directory = mFile.getParentFile();
        if (!directory.isDirectory() && !directory.mkdirs()) {
            throw new IOException("Cannot create account store directory");
        }
        File temporary = new File(mFile.getPath() + ".tmp");
        boolean committed = false;
        try {
            try (FileOutputStream stream = new FileOutputStream(temporary)) {
                stream.write(MAGIC);
                stream.write(sealed);
                stream.getFD().sync();
            }
            try {
                Files.move(temporary.toPath(), mFile.toPath(), StandardCopyOption.ATOMIC_MOVE,
                        StandardCopyOption.REPLACE_EXISTING);
            } catch (AtomicMoveNotSupportedException e) {
                Files.move(temporary.toPath(), mFile.toPath(), StandardCopyOption.REPLACE_EXISTING);
            }
            committed = true;
        } finally {
            if (!committed) Files.deleteIfExists(temporary.toPath());
        }
    }
}
