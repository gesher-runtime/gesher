/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.server;

import android.content.pm.Signature;

import java.io.File;
import java.io.IOException;
import java.security.MessageDigest;
import java.security.NoSuchAlgorithmException;
import java.util.Arrays;
import java.util.HexFormat;
import java.util.Properties;

/** The desktop user's authorization for one installed APK generation. */
final class SandboxOptOut {
    private SandboxOptOut() {}

    private static File file() {
        return new File(Device.dataDir(), "system/sandbox-opt-outs.properties");
    }

    private static String receipt(ApkPackage pkg) {
        Signature[] signers = pkg.verifiedSignatures();
        if (signers == null || signers.length == 0 || pkg.firstInstallTime() <= 0) {
            throw new SecurityException("The installed app has no verified signing identity");
        }
        try {
            MessageDigest digest = MessageDigest.getInstance("SHA-256");
            String[] hashes = Arrays.stream(signers).map(signer ->
                    HexFormat.of().formatHex(digest.digest(signer.toByteArray()))).sorted().toArray(String[]::new);
            return pkg.firstInstallTime() + ":" + pkg.versionCode() + ":" + pkg.apkPath + ":"
                    + String.join(",", hashes);
        } catch (NoSuchAlgorithmException impossible) {
            throw new AssertionError(impossible);
        }
    }

    static synchronized boolean isApproved(ApkPackage pkg) {
        try {
            Properties approved = Device.read(file());
            return approved != null && approved.containsKey(pkg.packageName)
                    && receipt(pkg).equals(approved.getProperty(pkg.packageName));
        } catch (IllegalArgumentException | SecurityException malformed) {
            return false;
        }
    }

    private static synchronized void write(String packageName, String receipt) throws IOException {
        Properties approved = Device.read(file());
        if (approved == null) approved = new Properties();
        if (receipt == null) approved.remove(packageName);
        else approved.setProperty(packageName, receipt);
        Device.write(file(), approved);
    }

    static String change(String packageName, String operation, Installer installer,
            PackageManagerService pm, ActivityTaskManagerService atm) throws IOException {
        if (installer == null || !SystemServer.isProtectedDevice() || Zygote.nativeProtectedRuntimeEnabled()) {
            throw new UnsupportedOperationException("App isolation settings require the installed desktop sandbox");
        }
        if (packageName == null || packageName.length() > 255 ||
                !packageName.matches("[A-Za-z][A-Za-z0-9_]*(\\.[A-Za-z0-9_]+)+")) {
            throw new IllegalArgumentException("Invalid installed app package");
        }
        ApkPackage pkg = pm.get(packageName);
        if (pkg == null || pm.isRemovalPending(packageName) || pkg.firstInstallTime() <= 0
                || pkg.uid() < android.os.Process.FIRST_APPLICATION_UID) {
            throw new IllegalArgumentException("No eligible installed app " + packageName);
        }
        if ("status".equals(operation)) return isApproved(pkg) ? "unisolated" : "isolated";
        boolean unisolated = "unisolated".equals(operation);
        if (!unisolated && !"isolated".equals(operation)) {
            throw new IllegalArgumentException("Unknown app isolation setting");
        }
        String verified = unisolated ? receipt(pkg) : null;
        if (unisolated) {
            if (isApproved(pkg)) return "unisolated";
            AccountSecretStore.ensureLoaded();
            if (!nativeConfirm(packageName)) return "cancelled";
        }
        synchronized (installer) {
            if (pm.get(packageName) != pkg || pm.isRemovalPending(packageName)) {
                throw new IllegalStateException("The installed app changed. Try again to review the current app.");
            }
            write(packageName, verified);
            atm.forceStopPackage(packageName);
        }
        return operation;
    }

    private static native boolean nativeConfirm(String packageName);
}
