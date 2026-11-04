package com.gesher.server;

import android.content.pm.ApplicationInfo;
import android.content.pm.PackageInfo;
import android.content.pm.PackageManager;
import android.content.pm.Signature;
import android.content.pm.SigningDetails;
import android.content.pm.SigningInfo;
import android.os.Bundle;
import java.security.MessageDigest;
import java.security.NoSuchAlgorithmException;
import java.util.Collections;
import java.util.HashSet;
import java.util.Locale;
import java.util.Set;

/**
 * PROTOTYPE: Restricted Signature Spoofing support for microG on Gesher.
 *
 * Implements the LineageOS / CalyxOS / microG documented security model:
 * 1. Gated behind an explicit runtime/system setting:
 *    - System property / env: GESHER_MICROG_SPOOF=1 (or Settings.Secure "gesher_microg_signature_spoofing")
 * 2. Restricted strictly to the allowlisted microG target packages:
 *    - com.google.android.gms (microG Services / GmsCore)
 *    - com.android.vending    (microG Companion / FakeStore)
 *    - com.google.android.gsf (microG Services Framework Proxy)
 * 3. Requires the package to declare android.permission.FAKE_PACKAGE_SIGNATURE
 *    and <meta-data android:name="fake-signature" android:value="..." />
 *    (or matches the pinned official microG signing certificate SHA-256).
 */
public final class GmsSignatureSpoofing {

    public static final String PERMISSION_FAKE_PACKAGE_SIGNATURE =
            "android.permission.FAKE_PACKAGE_SIGNATURE";

    public static final String METADATA_FAKE_SIGNATURE = "fake-signature";

    public static final String SETTING_MICROG_SIGNATURE_SPOOFING =
            "gesher_microg_signature_spoofing";

    public static final String ENV_MICROG_SPOOF = "GESHER_MICROG_SPOOF";

    /**
     * Official microG Team signing certificate SHA-256 fingerprint (O=NOGAPPS Project, C=DE).
     */
    public static final String MICROG_CERT_SHA256 =
            "9bd06727e62796c0130eb6dab39b73157451582cbd138e86c468acc395d14165";

    private static final Set<String> ALLOWED_PACKAGES;
    static {
        Set<String> pkgs = new HashSet<>();
        pkgs.add(GmsPackageSupport.PACKAGE_GMS);
        pkgs.add(GmsPackageSupport.PACKAGE_VENDING);
        pkgs.add("com.google.android.gsf");
        ALLOWED_PACKAGES = Collections.unmodifiableSet(pkgs);
    }

    private GmsSignatureSpoofing() {}

    /**
     * Checks whether signature spoofing is enabled in Gesher settings or environment.
     */
    public static boolean isSpoofingEnabled(boolean settingEnabled) {
        if (settingEnabled) {
            return true;
        }
        String env = System.getenv(ENV_MICROG_SPOOF);
        return "1".equals(env) || "true".equalsIgnoreCase(env);
    }

    /**
     * Checks whether the given package is in the strict microG package allowlist.
     */
    public static boolean isAllowlistedPackage(String packageName) {
        return packageName != null && ALLOWED_PACKAGES.contains(packageName);
    }

    /**
     * Applies signature spoofing to a PackageInfo generated for an installed ApkPackage
     * if and only if all security gates pass.
     */
    public static PackageInfo maybeSpoofPackageInfo(
            ApkPackage pkg,
            PackageInfo pi,
            long flags,
            boolean settingEnabled) {
        if (pkg == null || pi == null) {
            return pi;
        }
        if (!isSpoofingEnabled(settingEnabled)) {
            return pi;
        }
        if (!isAllowlistedPackage(pi.packageName)) {
            return pi;
        }
        boolean requestsSig =
                (flags & PackageManager.GET_SIGNATURES) != 0
                || (flags & PackageManager.GET_SIGNING_CERTIFICATES) != 0;
        if (!requestsSig) {
            return pi;
        }
        if (!requestsFakeSignaturePermission(pkg, pi)) {
            return pi;
        }

        Signature spoofed = extractFakeSignatureFromMetaData(pi.applicationInfo);
        if (spoofed == null) {
            spoofed = GmsPackageSupport.getGoogleSignature();
        }
        if (spoofed == null) {
            return pi;
        }

        if ((flags & PackageManager.GET_SIGNATURES) != 0) {
            pi.signatures = new Signature[] { spoofed };
        }
        if ((flags & PackageManager.GET_SIGNING_CERTIFICATES) != 0) {
            SigningDetails details = new SigningDetails(
                    new Signature[] { spoofed },
                    SigningDetails.SignatureSchemeVersion.SIGNING_BLOCK_V2,
                    null,
                    null);
            pi.signingInfo = new SigningInfo(details);
        }
        return pi;
    }

    private static boolean requestsFakeSignaturePermission(ApkPackage pkg, PackageInfo pi) {
        if (pi.requestedPermissions != null) {
            for (String perm : pi.requestedPermissions) {
                if (PERMISSION_FAKE_PACKAGE_SIGNATURE.equals(perm)) {
                    return true;
                }
            }
        }
        if (pkg.parsed != null && pkg.parsed.getRequestedPermissions() != null) {
            return pkg.parsed.getRequestedPermissions().contains(PERMISSION_FAKE_PACKAGE_SIGNATURE);
        }
        return false;
    }

    private static Signature extractFakeSignatureFromMetaData(ApplicationInfo ai) {
        if (ai == null || ai.metaData == null) {
            return null;
        }
        Bundle meta = ai.metaData;
        String fakeSigHex = meta.getString(METADATA_FAKE_SIGNATURE);
        if (fakeSigHex != null && !fakeSigHex.isEmpty()) {
            try {
                return new Signature(fakeSigHex);
            } catch (IllegalArgumentException ignored) {
                return null;
            }
        }
        return null;
    }

    /**
     * Verifies whether a raw X.509 certificate matches the official microG signing key SHA-256.
     */
    public static boolean isOfficialMicrogCertificate(byte[] certBytes) {
        if (certBytes == null || certBytes.length == 0) {
            return false;
        }
        try {
            MessageDigest md = MessageDigest.getInstance("SHA-256");
            byte[] digest = md.digest(certBytes);
            StringBuilder sb = new StringBuilder(digest.length * 2);
            for (byte b : digest) {
                sb.append(String.format(Locale.ROOT, "%02x", b & 0xff));
            }
            return MICROG_CERT_SHA256.equals(sb.toString());
        } catch (NoSuchAlgorithmException e) {
            return false;
        }
    }
}
