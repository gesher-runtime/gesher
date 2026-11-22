/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.server;

import com.gesher.app.NativeLibs;

import java.io.File;
import java.io.IOException;

/** Fixed addon slots; all checks precede manifest parsing and app code loading. */
public final class MicrogAddons {
    public static final java.util.List<String> PACKAGES = java.util.List.of(
            GmsPackageSupport.PACKAGE_GMS, GmsPackageSupport.PACKAGE_VENDING,
            GmsSignatureSpoofing.PACKAGE_GSF);

    private MicrogAddons() { }

    static ApkPackage load(File apk, String expectedPackage) throws Exception {
        if (!GmsSignatureSpoofing.verifyAddonApk(expectedPackage, apk.getPath())) {
            throw new SecurityException("Untrusted microG addon for " + expectedPackage);
        }
        ApkPackage parsed = ApkPackage.parse(apk.getPath());
        parsed.setSystemAddon(true);
        return parsed;
    }

    /**
     * The staged addon {@code packageName} when {@code codePath} (its ApplicationInfo's code
     * directory) is that addon's own {@code $ANDROID_ROOT/priv-app/<package>} slot, verified
     * as the system server loads it; null for any other package or directory.
     */
    public static ApkPackage stagedAddon(String packageName, String codePath) throws Exception {
        if (packageName == null || codePath == null || !PACKAGES.contains(packageName)) return null;
        String androidRoot = System.getenv("ANDROID_ROOT");
        if (androidRoot == null || androidRoot.isEmpty()) androidRoot = "/system";
        File slot = new File(androidRoot, "priv-app/" + packageName).getCanonicalFile();
        if (!slot.equals(new File(codePath).getCanonicalFile())) return null;
        File apk = new File(slot, "base.apk");
        if (!java.nio.file.Files.isRegularFile(apk.toPath(), java.nio.file.LinkOption.NOFOLLOW_LINKS)) {
            return null;
        }
        return load(apk, packageName);
    }

    /**
     * The package manager's native-library setup for a staged {@code priv-app} addon, as
     * at a system package scan: the APK's libraries for its best ABI in
     * {@code <code dir>/lib/<isa>} (stale ones removed), and the package's
     * {@code nativeLibraryDir} and {@code primaryCpuAbi}. The package is changed only
     * once its directory is ready; returns how many libraries the APK has.
     */
    static int prepareNativeLibraries(ApkPackage pkg) throws IOException {
        File codeDir = new File(pkg.apkPath).getParentFile();
        if (codeDir == null) throw new IOException("No code directory: " + pkg.apkPath);
        String abi = NativeLibs.abiFor(pkg.apkPath);
        String libDir = new File(codeDir, "lib/"
                + NativeLibs.instructionSetDir(abi != null ? abi : NativeLibs.ABI)).getPath();
        // Without native code this only clears libraries an earlier release left.
        int count = NativeLibs.syncDirectory(pkg.apkPath, libDir,
                abi != null ? abi : NativeLibs.ABI);
        pkg.setNativeLibraryDir(libDir);
        pkg.setPrimaryCpuAbi(abi);
        return count;
    }

    /** Packaging preflight, with no Binder connection, account or network traffic. */
    public static void main(String[] args) {
        if (args.length != 3 || !"verify".equals(args[0])) {
            throw new IllegalArgumentException("MicrogAddons verify PACKAGE APK");
        }
        com.gesher.app.Digests.install();
        System.loadLibrary("android_runtime");
        if (!GmsSignatureSpoofing.verifyAddonApk(args[1], args[2])) {
            throw new SecurityException("Rejected microG addon for " + args[1]);
        }
        System.out.println("Verified official microG addon: " + args[1]);
    }
}
