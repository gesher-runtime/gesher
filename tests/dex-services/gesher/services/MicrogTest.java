/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package gesher.services;

import android.content.pm.PackageInfo;
import android.content.pm.PackageManager;
import android.content.res.AssetManager;

import java.io.File;
import java.lang.reflect.Method;
import java.util.Collections;

/** Offline runtime regression checks; optional args[1] is an official GmsCore APK. */
public final class MicrogTest {
    private static void require(boolean value, String message) {
        if (!value) throw new AssertionError(message);
    }

    public static void main(String[] args) throws Exception {
        System.loadLibrary("android_runtime");
        AssetManager.createSystemAssetsInZygoteLocked(false,
                System.getenv("ANDROID_ROOT") + "/framework/framework-res.apk");
        Class<?> gate = Class.forName("com.gesher.server.GmsSignatureSpoofing");
        Method allowed = gate.getMethod("isAllowlistedPackage", String.class);
        require((Boolean) allowed.invoke(null, "com.google.android.gms"), "GmsCore slot missing");
        require((Boolean) allowed.invoke(null, "com.android.vending"), "Companion slot missing");
        for (String name : new String[] {null, "com.google.android.gsf", "com.example.untrusted"}) {
            require(!(Boolean) allowed.invoke(null, name), "unexpected spoof slot: " + name);
        }
        Method verify = gate.getMethod("verifyOfficialApk", String.class);
        Method verifyAddon = gate.getMethod("verifyAddonApk", String.class, String.class);
        for (String fixture : new String[] {"v2only.apk", "rotated.apk", "unsigned.apk"}) {
            require(!(Boolean) verify.invoke(null, new File(args[0], fixture).getPath()),
                    "accepted a non-microG signer: " + fixture);
        }
        System.out.println("MicrogTest: non-microG signers and unsigned APKs denied");

        Class<?> pmClass = Class.forName("com.gesher.server.PackageManagerService");
        Object pm = pmClass.getConstructor(java.util.Collection.class)
                .newInstance(Collections.emptyList());
        Method get = pmClass.getMethod("getPackageInfo", String.class, long.class, int.class);
        PackageInfo fallback = (PackageInfo) get.invoke(pm, "com.google.android.gms",
                (long) PackageManager.GET_SIGNATURES, 0);
        require(fallback != null && fallback.signatures.length == 1, "lost synthetic fallback");
        System.out.println("MicrogTest: synthetic fallback preserved without installed GmsCore");

        if (args.length > 1) {
            require((Boolean) verify.invoke(null, args[1]), "official APK integrity check failed");
            require((Boolean) verifyAddon.invoke(null, "com.google.android.gms", args[1]),
                    "official GmsCore package gate failed");
            require(!(Boolean) verifyAddon.invoke(null, "com.android.vending", args[1]),
                    "GmsCore was accepted in the Companion slot");
            require(!(Boolean) verifyAddon.invoke(null, "com.example.untrusted", args[1]),
                    "official signer bypassed the package allowlist");
            Class<?> apkClass = Class.forName("com.gesher.server.ApkPackage");
            Object apk = apkClass.getMethod("parse", String.class).invoke(null, args[1]);
            PackageInfo real = (PackageInfo) apkClass.getMethod("packageInfo", long.class)
                    .invoke(apk, (long) (PackageManager.GET_SIGNATURES
                            | PackageManager.GET_SIGNING_CERTIFICATES));
            boolean enabled = (Boolean) gate.getMethod("isEnabled").invoke(null);
            require(real.signatures[0].equals(fallback.signatures[0]) == enabled,
                    "legacy signature did not respect the opt-in");
            require(real.signingInfo.getApkContentsSigners()[0].equals(fallback.signatures[0])
                            == enabled, "modern signature did not respect the opt-in");
            System.out.println("MicrogTest: official GmsCore signatures, enabled=" + enabled);
            Object realPm = pmClass.getConstructor(java.util.Collection.class)
                    .newInstance(Collections.singletonList(apk));
            require(get.invoke(realPm, "com.android.vending", 0L, 0) == null,
                    "synthetic Companion hid the missing real addon");
            Class<?> am = Class.forName("com.gesher.server.ActivityManagerService");
            Class<?> accounts = Class.forName("com.gesher.server.AccountManagerService");
            Object service = accounts.getConstructor(am, pmClass).newInstance(null, realPm);
            android.accounts.Account[] found = (android.accounts.Account[]) accounts
                    .getMethod("getAccountsAsUser", String.class, int.class, String.class)
                    .invoke(service, "com.google", 0, "com.google.android.gms");
            for (android.accounts.Account account : found) {
                require(!"user@gesher.local".equals(account.name), "dummy account leaked to microG");
            }
            System.out.println("MicrogTest: no dummy Google account with installed GmsCore");
            ServiceRecoveryTest.main(new String[] {args[1]});
        }
        if (args.length > 2 && !args[2].isEmpty()) {
            require(!(Boolean) verify.invoke(null, args[2]), "modified APK contents accepted");
            System.out.println("MicrogTest: modified APK contents denied");
        }
        if (args.length > 3 && !args[3].isEmpty()) {
            require((Boolean) verifyAddon.invoke(null, "com.android.vending", args[3]),
                    "official Companion verification failed");
            require(!(Boolean) verifyAddon.invoke(null, "com.google.android.gms", args[3]),
                    "Companion was accepted in the Services slot");
            System.out.println("MicrogTest: genuine Companion package and signer verified");
        }
        if (args.length > 4 && !args[4].isEmpty()) {
            require((Boolean) verifyAddon.invoke(null, "com.google.android.gsf", args[4]),
                    "official legacy GSF proxy verification failed");
            require(!(Boolean) verifyAddon.invoke(null, "com.android.vending", args[4]),
                    "GSF proxy was accepted in a spoofable slot");
            System.out.println("MicrogTest: exact-pinned legacy GSF proxy verified without spoofing");
        }
        System.out.println("MicrogTest all passed");
    }
}
