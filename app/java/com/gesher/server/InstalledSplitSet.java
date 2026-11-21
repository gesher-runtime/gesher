/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.server;

import android.content.pm.PackageInfo;
import android.content.pm.PackageManager;
import android.content.pm.PackageParser.PackageParserException;
import android.content.pm.Signature;
import android.content.pm.parsing.ApkLite;
import java.io.File;
import java.io.IOException;
import java.nio.file.Files;
import java.nio.file.LinkOption;
import java.nio.file.Path;
import java.util.Arrays;
import java.util.LinkedHashMap;
import java.util.Locale;
import java.util.Map;
import java.util.Set;

/** One installed generation, read and materialized only under the Installer lock. */
final class InstalledSplitSet {
    private static final String[] SIDECARS = {".dm", ".digests", ".digests.signature"};
    private final ApkPackage installed;
    private final Map<String, Apk> apks = new LinkedHashMap<>();
    private final Path directory;

    private static final class Apk {
        final Path path;
        final int revision;
        Apk(String path, int revision) {
            this.path = new File(path).toPath();
            this.revision = revision;
        }
    }

    InstalledSplitSet(ApkPackage installed, Set<String> removals) throws PackageParserException {
        this.installed = installed;
        PackageInfo info = installed.packageInfo(0);
        directory = new File(installed.apkPath).toPath().getParent();
        apks.put(null, new Apk(installed.apkPath, info.baseRevisionCode));
        // PackageInfo may expose an empty name array while ApplicationInfo
        // uses null for a base-only package's split paths.
        if (info.splitNames != null && info.splitNames.length != 0) {
            String[] paths = info.applicationInfo.splitSourceDirs;
            int[] revisions = info.splitRevisionCodes;
            if (paths == null || revisions == null || info.splitNames.length != paths.length
                    || info.splitNames.length != revisions.length) {
                throw invalid("Installed split inventory is inconsistent");
            }
            for (int i = 0; i < info.splitNames.length; i++) {
                apks.put(info.splitNames[i], new Apk(paths[i], revisions[i]));
            }
        }
        for (String removal : removals) {
            if (!apks.containsKey(removal)) throw invalid("No installed split named " + removal);
        }
    }

    void validate(Map<String, ApkLite> staged, Signature[] signers) throws PackageParserException {
        Signature[] installedSigners = installed.verifiedSignatures();
        if (installedSigners == null || (!staged.isEmpty() && !Arrays.equals(installedSigners, signers))) {
            throw invalid("Existing signatures are inconsistent");
        }
        for (Map.Entry<String, ApkLite> entry : staged.entrySet()) {
            ApkLite apk = entry.getValue();
            if (apk.getLongVersionCode() != installed.versionCode()) {
                throw invalid("Staged APK version differs from the installed package");
            }
            Apk previous = apks.get(entry.getKey());
            if (previous != null && apk.getRevisionCode() < previous.revision) {
                throw new PackageParserException(PackageManager.INSTALL_FAILED_VERSION_DOWNGRADE,
                        "Staged APK revision is older than the installed revision");
            }
        }
    }

    void materialize(File canonical, Set<String> names, Set<String> staged, Set<String> removed)
            throws IOException, PackageParserException {
        if (!Files.isDirectory(directory, LinkOption.NOFOLLOW_LINKS)) {
            throw invalid("Installed code generation no longer exists");
        }
        for (Map.Entry<String, Apk> entry : apks.entrySet()) {
            String split = entry.getKey();
            if (split == null ? names.contains("base.apk")
                    : staged.contains(split) || removed.contains(split)) continue;
            String name = apkName(split);
            if (!names.add(name.toLowerCase(Locale.ROOT))) {
                throw invalid("Staged and inherited split filenames collide");
            }
            Path source = entry.getValue().path;
            if (!directory.equals(source.getParent()) || !Files.isRegularFile(source, LinkOption.NOFOLLOW_LINKS)) {
                throw invalid("Installed split APK is missing or has an invalid path");
            }
            Path destination = canonical.toPath().resolve(name);
            try {
                // The old generation is immutable and private to the installer. Link it
                // into the private commit directory, then let normal installation create
                // and verify an independent copy in the new installed generation.
                Files.createLink(destination, source);
            } catch (IOException | UnsupportedOperationException cannotLink) {
                Files.copy(source, destination);
            }
            String oldName = source.getFileName().toString();
            String oldStem = oldName.substring(0, oldName.length() - 4);
            String newStem = name.substring(0, name.length() - 4);
            for (String extension : SIDECARS) {
                Path sidecar = directory.resolve(oldStem + extension);
                if (Files.isRegularFile(sidecar, LinkOption.NOFOLLOW_LINKS)) {
                    Files.copy(sidecar, canonical.toPath().resolve(newStem + extension));
                }
            }
        }
    }

    static void checkSplitName(String split) {
        if (split == null || !split.matches("[A-Za-z0-9_][A-Za-z0-9_.-]{0,120}")) {
            throw new IllegalArgumentException("Invalid split name");
        }
    }

    static String apkName(String split) {
        if (split == null) return "base.apk";
        checkSplitName(split);
        return "split_" + split + ".apk";
    }

    static PackageParserException invalid(String message) {
        return new PackageParserException(PackageManager.INSTALL_FAILED_INVALID_APK, message);
    }
}
