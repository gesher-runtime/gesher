/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.server;

import java.nio.charset.StandardCharsets;
import java.nio.file.Path;
import java.security.MessageDigest;
import java.util.HexFormat;
import java.util.List;

/** Android's trusted side of the optional machine-installed runtime authority. */
public final class ProtectedRuntime {
    private ProtectedRuntime() {}

    /** Native startup state; an Android environment variable cannot enable this mode. */
    public static boolean enabled() {
        return Zygote.nativeProtectedRuntimeEnabled();
    }

    /** The service keeps the staged APK pinned while this verifier reads it and apps run. */
    static ApkEnrollmentVerifier.Identity verify(ApkPackage pkg) throws Exception {
        Path apk = Path.of(pkg.apkPath).toAbsolutePath();
        ApkEnrollmentVerifier.Identity identity = ApkEnrollmentVerifier.verify(
                apk.getParent(), pkg.packageName);
        if (!"base.apk".equals(apk.getFileName().toString())
                || identity.archives().size() != 1
                || identity.archives().get(0).splitName() != null) {
            // Enrollment of split archives requires pins for the complete set. Never
            // authorize a partial package using only the base archive's digest.
            throw new SecurityException("Protected launch requires a complete single-APK enrollment");
        }
        return identity;
    }

    static int spawn(ApkPackage pkg, String processName, long startSeq, int targetSdk,
            List<String> environment) throws Exception {
        ApkEnrollmentVerifier.Identity identity = verify(pkg);
        String signerSet = String.join("\n", identity.currentSignerSha256());
        String signerDigest = HexFormat.of().formatHex(MessageDigest.getInstance("SHA-256")
                .digest(signerSet.getBytes(StandardCharsets.UTF_8)));
        int pid = Zygote.nativeSpawnProtected(pkg.packageName, processName,
                Zygote.windowsPath(pkg.apkPath), identity.archives().get(0).sha256(), signerDigest,
                pkg.uid(), targetSdk, startSeq, environment.stream()
                        // These are derived from the service's sealed realm and exact
                        // process record. The remaining presentation/network settings
                        // pass through its independently enforced allowlist.
                        .filter(value -> !value.startsWith("GESHER_BINDER=")
                                && !value.startsWith("GESHER_UID=")
                                && !value.startsWith("GESHER_BINDER_UID=")
                                && !value.startsWith("GESHER_LOG_FILE="))
                        .toArray(String[]::new));
        if (pid <= 0) {
            throw new IllegalStateException("Protected application launch failed: Win32 error " + -pid);
        }
        return pid;
    }

    /** Explicit companion entry point, using Windows' own elevation/consent flow. */
    public static void launch(String apkPath) {
        int error = Zygote.nativeStartProtectedRuntime(Zygote.windowsPath(
                Path.of(apkPath).toAbsolutePath().toString()));
        if (error != 0) {
            throw new IllegalStateException("Protected runtime launch failed: Win32 error " + error);
        }
    }
}
