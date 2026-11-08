/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.server;

import java.io.File;

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
