/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.app;

import android.content.pm.dex.DexMetadataHelper;
import com.gesher.server.ApkPackage;

import java.io.File;
import java.io.IOException;
import java.nio.ByteBuffer;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.LinkOption;
import java.nio.file.NoSuchFileException;
import java.nio.file.Path;
import java.nio.file.StandardOpenOption;
import java.nio.file.attribute.BasicFileAttributes;
import java.security.DigestOutputStream;
import java.security.MessageDigest;
import java.security.NoSuchAlgorithmException;
import java.util.Map;
import java.util.TreeMap;

/** The host installer's complete selected inputs, before consent and before installation. */
public final class HostInstallSnapshot {
    /** Conventional companion names for a loose APK. Package ownership is checked by Installer. */
    public static File[] companionSplits(File apk) {
        File dir = apk.getAbsoluteFile().getParentFile();
        if (dir == null) return null;
        return dir.listFiles((d, name) -> (name.startsWith("split_") || name.startsWith("config."))
                && name.endsWith(".apk") && !name.equals(apk.getName()));
    }

    public static File dexMetadata(File apk) {
        String path = apk.getPath();
        return new File(path.endsWith(".apk") ? DexMetadataHelper.buildDexMetadataPathForApk(path)
                : path + ".dm");
    }

    /**
     * Capture into an empty owned directory. Each digest is computed over the bytes written,
     * and the aggregate includes every input name, so adding or dropping a companion also
     * invalidates a confirmation. The package and version are checked separately.
     */
    public static String capture(File source, Path destination) throws IOException {
        Map<String, byte[]> files = new TreeMap<>();
        copy(source, destination, files);
        File base = destination.resolve(source.getName()).toFile();
        if (!PackageBundles.isBundle(base)) {
            String packageName = ApkPackage.packageName(base);
            copyMetadata(source, destination, files);
            File[] splits = companionSplits(source);
            if (splits == null) throw new IOException("Cannot list APK companions for " + source);
            for (File split : splits) {
                copy(split, destination, files);
                Path captured = destination.resolve(split.getName());
                if (packageName.equals(ApkPackage.packageName(captured.toFile()))) {
                    copyMetadata(split, destination, files);
                } else {
                    Files.delete(captured);
                    files.remove(split.getName());
                }
            }
        }
        MessageDigest manifest = sha256();
        for (Map.Entry<String, byte[]> file : files.entrySet()) {
            byte[] name = file.getKey().getBytes(StandardCharsets.UTF_8);
            manifest.update(ByteBuffer.allocate(Integer.BYTES).putInt(name.length).array());
            manifest.update(name);
            manifest.update(file.getValue());
        }
        char[] hex = "0123456789abcdef".toCharArray();
        byte[] digest = manifest.digest();
        char[] text = new char[digest.length * 2];
        for (int i = 0; i < digest.length; i++) {
            text[i * 2] = hex[(digest[i] >>> 4) & 15];
            text[i * 2 + 1] = hex[digest[i] & 15];
        }
        return new String(text);
    }

    private static void copyMetadata(File apk, Path destination, Map<String, byte[]> files)
            throws IOException {
        File metadata = dexMetadata(apk);
        try {
            Files.readAttributes(metadata.toPath(), BasicFileAttributes.class, LinkOption.NOFOLLOW_LINKS);
        } catch (NoSuchFileException absent) {
            return;
        }
        copy(metadata, destination, files);
    }

    private static void copy(File source, Path destination, Map<String, byte[]> files)
            throws IOException {
        Path path = source.toPath();
        if (!Files.readAttributes(path, BasicFileAttributes.class).isRegularFile()) {
            throw new IOException("Install input is not a regular file: " + path);
        }
        String name = source.getName();
        // Two conventional names can point to the same .dm. Capture it only once.
        if (files.containsKey(name)) return;
        MessageDigest digest = sha256();
        try (var in = Files.newInputStream(path);
                var out = new DigestOutputStream(Files.newOutputStream(destination.resolve(name),
                        StandardOpenOption.CREATE_NEW, StandardOpenOption.WRITE), digest)) {
            in.transferTo(out);
        }
        files.put(name, digest.digest());
    }

    private static MessageDigest sha256() {
        try {
            return MessageDigest.getInstance("SHA-256");
        } catch (NoSuchAlgorithmException impossible) {
            throw new AssertionError(impossible);
        }
    }

    private HostInstallSnapshot() {}
}
