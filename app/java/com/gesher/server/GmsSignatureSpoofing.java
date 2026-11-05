/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.server;

import android.content.pm.PackageInfo;
import android.content.pm.PackageManager;
import android.content.pm.Signature;
import android.content.pm.SigningDetails;
import android.content.pm.SigningInfo;
import com.android.apksig.ApkVerifier;
import com.android.apksig.apk.ApkUtils;
import com.android.apksig.util.DataSources;

import java.io.File;
import java.io.FileInputStream;
import java.io.RandomAccessFile;
import java.security.MessageDigest;
import java.security.cert.X509Certificate;
import java.util.List;

/**
 * Optional Google certificate compatibility for official microG Services/Companion.
 * APK contents and the current signer are verified before substituting a certificate.
 * Manifest fake-signature metadata cannot select a certificate or authorize spoofing.
 */
public final class GmsSignatureSpoofing {
    public static final String ENV_MICROG_SPOOF = "GESHER_MICROG_SPOOF";
    public static final String PERMISSION_FAKE_PACKAGE_SIGNATURE =
            "android.permission.FAKE_PACKAGE_SIGNATURE";
    public static final String MICROG_CERT_SHA256 =
            "9bd06727e62796c0130eb6dab39b73157451582cbd138e86c468acc395d14165";
    public static final String PACKAGE_GSF = "com.google.android.gsf";
    // Upstream GsfProxy v0.1.0 is v1-only. Permit this exact signed artifact,
    // rather than treating arbitrary v1 metadata/unsigned entries as trusted code.
    public static final String LEGACY_GSF_SHA256 =
            "86891b174301f06a1c84187b545a0a2a57044c6b768f3e84e865908743349692";

    private GmsSignatureSpoofing() { }

    public static boolean isEnabled() {
        String value = System.getenv(ENV_MICROG_SPOOF);
        return "1".equals(value) || "true".equalsIgnoreCase(value);
    }

    public static boolean isAllowlistedPackage(String name) {
        return GmsPackageSupport.PACKAGE_GMS.equals(name)
                || GmsPackageSupport.PACKAGE_VENDING.equals(name);
    }

    public static boolean isAddonPackage(String name) {
        return isAllowlistedPackage(name) || PACKAGE_GSF.equals(name);
    }

    /** The same policy is used by package signatures and permission enforcement. */
    public static boolean isTrustedPackage(ApkPackage pkg) {
        return pkg != null && isEnabled() && isAllowlistedPackage(pkg.packageName)
                && pkg.parsed.getRequestedPermissions().contains(PERMISSION_FAKE_PACKAGE_SIGNATURE)
                && pkg.hasVerifiedMicrogSigner();
    }

    private static String sha256(String path) throws Exception {
        MessageDigest digest = MessageDigest.getInstance("SHA-256");
        try (FileInputStream in = new FileInputStream(path)) {
            byte[] block = new byte[64 * 1024];
            for (int n; (n = in.read(block)) != -1;) digest.update(block, 0, n);
        }
        StringBuilder hex = new StringBuilder(64);
        for (byte b : digest.digest()) {
            hex.append(Character.forDigit((b >>> 4) & 15, 16));
            hex.append(Character.forDigit(b & 15, 16));
        }
        return hex.toString();
    }

    /** Verify the expected manifest name without executing code or parsing shared libraries. */
    public static boolean verifyAddonApk(String expectedPackage, String path) {
        if (!isAddonPackage(expectedPackage)) return false;
        try (RandomAccessFile file = new RandomAccessFile(path, "r")) {
            String actual = ApkUtils.getPackageNameFromBinaryAndroidManifest(
                    ApkUtils.getAndroidManifest(DataSources.asDataSource(file)));
            return expectedPackage.equals(actual) && verifyOfficialApk(path);
        } catch (Exception | LinkageError e) {
            return false;
        }
    }

    public static boolean isOfficialMicrogCertificate(byte[] certificate) {
        if (certificate == null || certificate.length == 0) return false;
        try {
            byte[] digest = MessageDigest.getInstance("SHA-256").digest(certificate);
            StringBuilder hex = new StringBuilder(64);
            for (byte b : digest) {
                hex.append(Character.forDigit((b >>> 4) & 15, 16));
                hex.append(Character.forDigit(b & 15, 16));
            }
            return MICROG_CERT_SHA256.contentEquals(hex);
        } catch (java.security.NoSuchAlgorithmException e) {
            throw new AssertionError(e);
        }
    }

    /** A copied signing block alone is insufficient: verify the signed content digests. */
    public static boolean verifyOfficialApk(String path) {
        try {
            // apksig verifies file content without Android's missing incremental-fs JNI.
            ApkVerifier.Result result = new ApkVerifier.Builder(new File(path))
                    .setMinCheckedPlatformVersion(36).setMaxCheckedPlatformVersion(36)
                    .build().verify();
            List<X509Certificate> signers = result.getSignerCertificates();
            return result.isVerified()
                    && (result.isVerifiedUsingV2Scheme() || result.isVerifiedUsingV3Scheme()
                            || result.isVerifiedUsingV31Scheme()
                            || (result.isVerifiedUsingV1Scheme()
                                    && LEGACY_GSF_SHA256.equals(sha256(path))))
                    && signers.size() == 1
                    && isOfficialMicrogCertificate(signers.get(0).getEncoded());
        } catch (Exception | LinkageError e) {
            SystemServer.log("microg: APK verification denied for " + path + ": "
                    + e.getClass().getSimpleName());
            return false;
        }
    }

    static PackageInfo maybeSpoofPackageInfo(ApkPackage pkg, PackageInfo pi, long flags) {
        if ((flags & (PackageManager.GET_SIGNATURES
                        | PackageManager.GET_SIGNING_CERTIFICATES)) == 0
                || !isTrustedPackage(pkg)) {
            return pi;
        }
        Signature google = GmsPackageSupport.getGoogleSignature();
        if ((flags & PackageManager.GET_SIGNATURES) != 0) {
            pi.signatures = new Signature[] { google };
        }
        if ((flags & PackageManager.GET_SIGNING_CERTIFICATES) != 0) {
            pi.signingInfo = new SigningInfo(new SigningDetails(new Signature[] { google },
                    SigningDetails.SignatureSchemeVersion.SIGNING_BLOCK_V2, null, null));
        }
        return pi;
    }
}
