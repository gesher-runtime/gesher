/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.server;

import android.content.pm.Signature;
import android.content.pm.parsing.ApkLite;
import android.content.res.AssetManager;
import android.util.JsonWriter;

import com.gesher.app.Digests;

import java.io.IOException;
import java.io.OutputStream;
import java.io.OutputStreamWriter;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.LinkOption;
import java.nio.file.Path;
import java.security.DigestInputStream;
import java.security.MessageDigest;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.HashSet;
import java.util.HexFormat;
import java.util.List;
import java.util.Locale;
import java.util.Set;

/**
 * Verifies APKs in a protected caller's staging directory without installing or running the app.
 * The caller must pin the staged directory and every archive against writes, replacements and
 * deletion for verification and enrollment. Neither command arguments nor this worker's output
 * carry authority outside the authenticated process and file pins held by that caller.
 */
public final class ApkEnrollmentVerifier {
    private ApkEnrollmentVerifier() {}

    public record Archive(String fileName, String splitName, String sha256) {}
    public record Identity(String packageName, long versionCode, List<String> currentSignerSha256,
            List<Archive> archives) {
        public Identity {
            currentSignerSha256 = List.copyOf(currentSignerSha256);
            archives = List.copyOf(archives);
        }
    }

    public static Identity verify(Path stagingDirectory, String expectedPackage) throws Exception {
        Path directory = stagingDirectory.toAbsolutePath();
        if (!directory.equals(directory.toFile().getCanonicalFile().toPath())
                || !Files.isDirectory(directory, LinkOption.NOFOLLOW_LINKS)) {
            throw new SecurityException("Enrollment stage is not a canonical directory");
        }
        Path base = directory.resolve("base.apk");
        List<Path> files;
        try (var entries = Files.list(directory)) {
            files = entries.sorted().toList();
        }
        if (!files.contains(base)) throw new IOException("Missing enrollment base.apk");

        ApkLite baseManifest = ApkPackage.codeApkLite(base.toFile());
        String packageName = baseManifest.getPackageName();
        long version = baseManifest.getLongVersionCode();
        if (baseManifest.getSplitName() != null || "android".equals(packageName)
                || !packageName.equals(expectedPackage)) {
            throw new SecurityException("Enrollment base package does not match");
        }
        Signature[] current = verifySigners(base);
        MessageDigest digest = MessageDigest.getInstance("SHA-256");
        var signerHashes = Arrays.stream(current).map(signature ->
                HexFormat.of().formatHex(digest.digest(signature.toByteArray()))).sorted().toList();

        Set<String> names = new HashSet<>();
        Set<String> splits = new HashSet<>();
        Set<String> requiredTypes = new HashSet<>();
        Set<String> providedTypes = new HashSet<>();
        List<Archive> archives = new ArrayList<>();
        for (Path file : files) {
            String name = file.getFileName().toString();
            boolean isBase = base.equals(file);
            if (!names.add(name.toLowerCase(Locale.ROOT)) || (!isBase
                    && !name.matches("split_[A-Za-z0-9_.-]+\\.apk"))) {
                throw new SecurityException("Unexpected or duplicate enrollment archive");
            }
            if (!file.equals(file.toFile().getCanonicalFile().toPath())
                    || !Files.isRegularFile(file, LinkOption.NOFOLLOW_LINKS)) {
                throw new SecurityException("Enrollment archive is not a canonical file");
            }
            ApkLite manifest = isBase ? baseManifest : ApkPackage.codeApkLite(file.toFile());
            String split = manifest.getSplitName();
            if (!isBase && (split == null || !splits.add(split)
                    || !packageName.equals(manifest.getPackageName())
                    || version != manifest.getLongVersionCode()
                    || !Arrays.equals(current, verifySigners(file)))) {
                throw new SecurityException("Conflicting enrollment split");
            }
            if (manifest.getRequiredSplitTypes() != null) requiredTypes.addAll(manifest.getRequiredSplitTypes());
            if (manifest.getSplitTypes() != null) providedTypes.addAll(manifest.getSplitTypes());
            try (var input = new DigestInputStream(Files.newInputStream(file), digest)) {
                input.transferTo(OutputStream.nullOutputStream());
            }
            archives.add(new Archive(name, split, HexFormat.of().formatHex(digest.digest())));
        }
        if (baseManifest.isSplitRequired() && (splits.isEmpty() || !providedTypes.containsAll(requiredTypes))) {
            throw new SecurityException("Missing required enrollment splits");
        }
        // The full parser checks the package manifest and the split dependency graph.
        // Confirm that it parsed exactly the same base and split set verified above.
        var parsed = ApkPackage.parseManifest(directory.toFile());
        String[] parsedSplits = parsed.getSplitNames();
        Set<String> fullSplitNames = parsedSplits == null ? Set.of() : Set.of(parsedSplits);
        if (!packageName.equals(parsed.getPackageName()) || version != parsed.getLongVersionCode()
                || !splits.equals(fullSplitNames)) {
            throw new SecurityException("Full enrollment manifest does not match the verified archives");
        }
        return new Identity(packageName, version, signerHashes, archives);
    }

    private static Signature[] verifySigners(Path archive) {
        Signature[] verified = ApkPackage.verifySignatures(archive.toString());
        if (verified == null || verified.length == 0) {
            throw new SecurityException("Enrollment archive signature verification failed");
        }
        return verified;
    }

    private static void emit(Identity identity, JsonWriter json) throws IOException {
        json.beginObject()
                .name("schema").value(1)
                .name("package").value(identity.packageName)
                .name("versionCode").value(Long.toString(identity.versionCode))
                .name("currentSignerSha256").beginArray();
        for (String signer : identity.currentSignerSha256) json.value(signer);
        json.endArray().name("archives").beginArray();
        for (Archive archive : identity.archives) {
            json.beginObject()
                    .name("file").value(archive.fileName)
                    .name("split").value(archive.splitName)
                    .name("sha256").value(archive.sha256)
                    .endObject();
        }
        json.endArray().endObject().flush();
    }

    /** Trusted isolated worker entry point. The first argument is a protected framework-res.apk. */
    public static void main(String[] args) {
        try {
            if (args.length != 3) {
                throw new IllegalArgumentException("Usage: ApkEnrollmentVerifier FRAMEWORK_RES STAGING_DIR PACKAGE");
            }
            Digests.install();
            System.loadLibrary("android_runtime");
            AssetManager.createSystemAssetsInZygoteLocked(false, args[0]);
            Identity identity = verify(Path.of(args[1]), args[2]);
            emit(identity, new JsonWriter(new OutputStreamWriter(System.out, StandardCharsets.UTF_8)));
            System.out.println();
            return;
        } catch (Exception | LinkageError failure) {
            System.err.println("APK enrollment verification failed: " + failure.getClass().getSimpleName()
                    + ": " + failure.getMessage());
        }
        System.exit(1);
    }
}
