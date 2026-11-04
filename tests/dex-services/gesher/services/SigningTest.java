/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package gesher.services;

import android.content.pm.PackageInfo;
import android.content.pm.PackageManager;
import android.content.pm.Signature;
import android.content.pm.SigningInfo;
import android.content.res.AssetManager;

import java.io.File;
import java.security.MessageDigest;

/**
 * An installed APK's signers (com.gesher.server.ApkPackage), as apps read their own:
 * WhatsApp builds its registration token from GET_SIGNATURES, and its server answered
 * ERROR_BAD_TOKEN while Gesher gave none. Collected from the APK's v3 or v2 block,
 * unverified; with a v3 proof-of-rotation, GET_SIGNATURES answers the oldest key and
 * GET_SIGNING_CERTIFICATES the current one with its history, as on Android 13 and up.
 * An unsigned APK has none. The fixtures and their keys: fixtures/generate.sh.
 * args[0]: the fixtures directory. Prints "SigningTest all passed".
 */
public final class SigningTest {
    /** The certificates' SHA-256 digests, as fixtures/generate.sh prints them. */
    private static final String OLD =
            "6f6811249a2eaa37f4f64fdd29f5bea366feee2edcbe19eb96f6c7f89993d853";
    private static final String NEW =
            "98f19a71a5c3a23864b2d7ed3d7b7132239cf973af934cd0c8beeba5906d06d7";

    private static void require(boolean condition, String message) {
        if (!condition) throw new AssertionError(message);
    }

    private static PackageInfo packageInfo(File apk, long flags) throws Exception {
        Class<?> apkPackage = Class.forName("com.gesher.server.ApkPackage");
        Object parsed = apkPackage.getMethod("parse", String.class).invoke(null, apk.getPath());
        return (PackageInfo) apkPackage.getMethod("packageInfo", long.class).invoke(parsed, flags);
    }

    private static String digest(Signature signature) throws Exception {
        StringBuilder hex = new StringBuilder();
        for (byte b : MessageDigest.getInstance("SHA-256").digest(signature.toByteArray())) {
            hex.append(String.format("%02x", b));
        }
        return hex.toString();
    }

    public static void main(String[] args) throws Exception {
        // android.os.Build (PackageParser2's) reads system properties, and the parser the
        // system resources: gesher-app's Main has both in an app.
        System.loadLibrary("android_runtime");
        AssetManager.createSystemAssetsInZygoteLocked(false,
                System.getenv("ANDROID_ROOT") + "/framework/framework-res.apk");
        File fixtures = new File(args[0]);

        // Rotated as WhatsApp's: the old key for SDK 24-32, the new one (v3.1) from 33.
        File rotated = new File(fixtures, "rotated.apk");
        PackageInfo legacy = packageInfo(rotated, PackageManager.GET_SIGNATURES);
        require(legacy.signatures != null && legacy.signatures.length == 1,
                "GET_SIGNATURES gave no signature for a signed APK");
        require(OLD.equals(digest(legacy.signatures[0])),
                "GET_SIGNATURES gave " + digest(legacy.signatures[0]) + ", not the oldest key");
        System.out.println("SigningTest: GET_SIGNATURES answers a rotated APK's oldest key");

        SigningInfo info = packageInfo(rotated, PackageManager.GET_SIGNING_CERTIFICATES)
                .signingInfo;
        require(info != null, "GET_SIGNING_CERTIFICATES gave no signingInfo");
        require(info.hasPastSigningCertificates() && !info.hasMultipleSigners(),
                "the rotated APK's signingInfo has no history");
        Signature[] history = info.getSigningCertificateHistory();
        require(history.length == 2 && OLD.equals(digest(history[0]))
                        && NEW.equals(digest(history[1])),
                "the signing history is not the old key, then the new one");
        require(NEW.equals(digest(info.getApkContentsSigners()[0])),
                "the APK's current signer is not the new key");
        System.out.println("SigningTest: GET_SIGNING_CERTIFICATES answers the current key "
                + "and its rotation history");

        // v2 only, with the old key; and none at all.
        PackageInfo v2 = packageInfo(new File(fixtures, "v2only.apk"),
                PackageManager.GET_SIGNATURES | PackageManager.GET_SIGNING_CERTIFICATES);
        require(v2.signatures != null && v2.signatures.length == 1
                        && OLD.equals(digest(v2.signatures[0]))
                        && v2.signingInfo != null && !v2.signingInfo.hasPastSigningCertificates(),
                "a v2-signed APK does not answer its key");
        PackageInfo unsigned = packageInfo(new File(fixtures, "unsigned.apk"),
                PackageManager.GET_SIGNATURES | PackageManager.GET_SIGNING_CERTIFICATES);
        require(unsigned.signatures == null && unsigned.signingInfo == null,
                "an unsigned APK has signatures");
        System.out.println("SigningTest: a v2-signed APK answers its key, an unsigned one none");
        System.out.println("SigningTest all passed");
    }
}
