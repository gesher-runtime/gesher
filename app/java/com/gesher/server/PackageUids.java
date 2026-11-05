/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.server;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.nio.channels.FileChannel;
import java.nio.channels.FileLock;
import java.nio.file.Files;
import java.nio.file.StandardCopyOption;
import java.util.HashSet;
import java.util.Properties;
import java.util.Set;

/** Stable Android app identities. This is Binder identity, not a Windows sandbox. */
public final class PackageUids {
    private PackageUids() { }

    public static synchronized int uidFor(String packageName) {
        // Keep the historical standalone runtime compatible when the addon is off.
        if (!GmsSignatureSpoofing.isEnabled()) return 10000;
        if (packageName == null || packageName.isEmpty()) {
            throw new IllegalArgumentException("Missing package name for UID allocation");
        }
        File directory = new File(Device.dataDir(), "system");
        if (!directory.isDirectory() && !directory.mkdirs()) {
            throw new IllegalStateException("Cannot create the package identity directory");
        }
        File registry = new File(directory, "package-uids");
        try (FileOutputStream lockFile = new FileOutputStream(
                    new File(directory, "package-uids.lock"), true);
                FileChannel channel = lockFile.getChannel(); FileLock lock = channel.lock()) {
            Properties ids = new Properties();
            if (registry.isFile()) {
                try (FileInputStream input = new FileInputStream(registry)) { ids.load(input); }
            }
            Set<Integer> used = new HashSet<>();
            int next = 10000;
            for (String name : ids.stringPropertyNames()) {
                int uid = Integer.parseInt(ids.getProperty(name));
                if (uid < 10000 || uid > 19999 || !used.add(uid)) {
                    throw new IllegalStateException("Corrupt package UID registry");
                }
                next = Math.max(next, uid + 1);
            }
            if (ids.containsKey(packageName)) return Integer.parseInt(ids.getProperty(packageName));
            if (next > 19999) throw new IllegalStateException("No application UIDs available");
            ids.setProperty(packageName, Integer.toString(next));
            File temporary = File.createTempFile("package-uids-", ".tmp", directory);
            try {
                try (FileOutputStream output = new FileOutputStream(temporary)) {
                    ids.store(output, "Gesher stable package identities; never reuse allocated UIDs");
                    output.getFD().sync();
                }
                try {
                    Files.move(temporary.toPath(), registry.toPath(),
                            StandardCopyOption.ATOMIC_MOVE, StandardCopyOption.REPLACE_EXISTING);
                } catch (java.nio.file.AtomicMoveNotSupportedException unavailable) {
                    Files.move(temporary.toPath(), registry.toPath(), StandardCopyOption.REPLACE_EXISTING);
                }
            } finally {
                if (temporary.exists()) temporary.delete();
            }
            return next;
        } catch (Exception failure) {
            throw new IllegalStateException("Cannot allocate a persistent application UID", failure);
        }
    }
}
