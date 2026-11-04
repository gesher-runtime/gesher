/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.server;

import android.content.ComponentName;
import android.content.Intent;
import android.content.pm.ActivityInfo;
import android.content.pm.ApplicationInfo;
import android.content.pm.PackageInfo;
import android.content.pm.PackageManager;
import android.content.pm.ResolveInfo;
import android.content.pm.ServiceInfo;
import android.content.pm.Signature;
import android.content.pm.SigningDetails;
import android.content.pm.SigningInfo;
import android.os.Process;

import java.io.File;
import java.util.ArrayList;
import java.util.List;

/**
 * Synthetic package definitions for Google Play Services (GMS) and Google Play Store.
 * Provides microG-style stand-in package information, signatures, and service resolution.
 */
public final class GmsPackageSupport {
    public static final String PACKAGE_GMS = "com.google.android.gms";
    public static final String PACKAGE_VENDING = "com.android.vending";

    // A current Play services: apps compare it with the version their client libraries
    // need (Gmail 2026: 264280000) and, below it, post "update Google Play services".
    public static final int GMS_VERSION_CODE = 264434000;
    public static final String GMS_VERSION_NAME = "26.44.34 (190400-000000000)";

    public static final int VENDING_VERSION_CODE = 83000000;
    public static final String VENDING_VERSION_NAME = "83.0.00";

    // Google's app signing certificate (SHA-1 38:91:8A:45:3D:07:19:93:54:F8:B1:9A:F0:5E:C6:56:2C:ED:57:88),
    // public in every APK Google signs with it (Play services, Gmail...). The client
    // libraries' GoogleSignatureVerifier accepts Play services only with one of Google's
    // certificates: with any other, apps report it "not available" (error 9,
    // SERVICE_INVALID) and Gmail refuses to start. Only the certificate; no key.
    public static final String GOOGLE_CERT_HEX =
            "308204433082032ba003020102020900c2e08746644a308d300d06092a864886f70d010104050030" +
            "74310b3009060355040613025553311330110603550408130a43616c69666f726e69613116301406" +
            "03550407130d4d6f756e7461696e205669657731143012060355040a130b476f6f676c6520496e63" +
            "2e3110300e060355040b1307416e64726f69643110300e06035504031307416e64726f6964301e17" +
            "0d3038303832313233313333345a170d3336303130373233313333345a3074310b30090603550406" +
            "13025553311330110603550408130a43616c69666f726e6961311630140603550407130d4d6f756e" +
            "7461696e205669657731143012060355040a130b476f6f676c6520496e632e3110300e060355040b" +
            "1307416e64726f69643110300e06035504031307416e64726f696430820120300d06092a864886f7" +
            "0d01010105000382010d00308201080282010100ab562e00d83ba208ae0a966f124e29da11f2ab56" +
            "d08f58e2cca91303e9b754d372f640a71b1dcb130967624e4656a7776a92193db2e5bfb724a91e77" +
            "188b0e6a47a43b33d9609b77183145ccdf7b2e586674c9e1565b1f4c6a5955bff251a63dabf9c55c" +
            "27222252e875e4f8154a645f897168c0b1bfc612eabf785769bb34aa7984dc7e2ea2764cae8307d8" +
            "c17154d7ee5f64a51a44a602c249054157dc02cd5f5c0e55fbef8519fbe327f0b1511692c5a06f19" +
            "d18385f5c4dbc2d6b93f68cc2979c70e18ab93866b3bd5db8999552a0e3b4c99df58fb918bedc182" +
            "ba35e003c1b4b10dd244a8ee24fffd333872ab5221985edab0fc0d0b145b6aa192858e79020103a3" +
            "81d93081d6301d0603551d0e04160414c77d8cc2211756259a7fd382df6be398e4d786a53081a606" +
            "03551d2304819e30819b8014c77d8cc2211756259a7fd382df6be398e4d786a5a178a4763074310b" +
            "3009060355040613025553311330110603550408130a43616c69666f726e69613116301406035504" +
            "07130d4d6f756e7461696e205669657731143012060355040a130b476f6f676c6520496e632e3110" +
            "300e060355040b1307416e64726f69643110300e06035504031307416e64726f6964820900c2e087" +
            "46644a308d300c0603551d13040530030101ff300d06092a864886f70d010104050003820101006d" +
            "d252ceef85302c360aaace939bcff2cca904bb5d7a1661f8ae46b2994204d0ff4a68c7ed1a531ec4" +
            "595a623ce60763b167297a7ae35712c407f208f0cb109429124d7b106219c084ca3eb3f9ad5fb871" +
            "ef92269a8be28bf16d44c8d9a08e6cb2f005bb3fe2cb96447e868e731076ad45b33f6009ea19c161" +
            "e62641aa99271dfd5228c5c587875ddb7f452758d661f6cc0cccb7352e424cc4365c523532f73251" +
            "37593c4ae341f4db41edda0d0b1071a7c440f0fe9ea01cb627ca674369d084bd2fd911ff06cdbf2c" +
            "fa10dc0f893ae35762919048c7efc64c7144178342f70581c9de573af55b390dd7fdb9418631895d" +
            "5f759f30112687ff621410c069308a";

    private static volatile Signature sGoogleSignature;

    private GmsPackageSupport() {
    }

    public static boolean isSyntheticPackage(String packageName) {
        return PACKAGE_GMS.equals(packageName) || PACKAGE_VENDING.equals(packageName);
    }

    public static Signature getGoogleSignature() {
        if (sGoogleSignature == null) {
            sGoogleSignature = new Signature(GOOGLE_CERT_HEX);
        }
        return sGoogleSignature;
    }

    public static PackageInfo createSyntheticPackageInfo(String packageName, long flags) {
        if (!isSyntheticPackage(packageName)) return null;

        PackageInfo pi = new PackageInfo();
        pi.packageName = packageName;
        pi.versionCode = PACKAGE_GMS.equals(packageName) ? GMS_VERSION_CODE : VENDING_VERSION_CODE;
        pi.setLongVersionCode(pi.versionCode);
        pi.versionName = PACKAGE_GMS.equals(packageName) ? GMS_VERSION_NAME : VENDING_VERSION_NAME;
        pi.firstInstallTime = System.currentTimeMillis();
        pi.lastUpdateTime = pi.firstInstallTime;
        pi.applicationInfo = createSyntheticApplicationInfo(packageName, flags);

        if ((flags & PackageManager.GET_SIGNATURES) != 0
                || (flags & PackageManager.GET_SIGNING_CERTIFICATES) != 0) {
            pi.signatures = new Signature[] { getGoogleSignature() };
        }
        // What GET_SIGNING_CERTIFICATES asks for (WhatsApp's AppIdentity of Play services:
        // "signingInfo was null" ends its process).
        if ((flags & PackageManager.GET_SIGNING_CERTIFICATES) != 0) {
            pi.signingInfo = new SigningInfo(new SigningDetails(
                    new Signature[] { getGoogleSignature() },
                    SigningDetails.SignatureSchemeVersion.SIGNING_BLOCK_V3, null, null));
        }

        if ((flags & PackageManager.GET_SERVICES) != 0 && PACKAGE_GMS.equals(packageName)) {
            pi.services = createSyntheticServices(packageName);
        }

        if ((flags & PackageManager.GET_RECEIVERS) != 0 && PACKAGE_GMS.equals(packageName)) {
            pi.receivers = createSyntheticReceivers(packageName);
        }

        return pi;
    }

    private static volatile String sGmsCodeApk;

    /** gesher-camera.apk next to dalvikvm.exe (drive-less, '/'), or null. */
    private static String gmsCodeApk() {
        String apk = sGmsCodeApk;
        if (apk == null) {
            try {
                String exe = Zygote.exePath();
                apk = (exe.substring(0, exe.lastIndexOf('\\') + 1) + "gesher-camera.apk")
                        .replace('\\', '/').replaceFirst("^[A-Za-z]:", "");
                if (!new File(apk).isFile()) apk = "";
            } catch (Throwable t) {
                apk = "";
            }
            sGmsCodeApk = apk;
        }
        return apk.isEmpty() ? null : apk;
    }

    public static ApplicationInfo createSyntheticApplicationInfo(String packageName, long flags) {
        if (!isSyntheticPackage(packageName)) return null;

        ApplicationInfo ai = new ApplicationInfo();
        ai.packageName = packageName;
        ai.name = PACKAGE_GMS.equals(packageName) ? "Google Play services" : "Google Play Store";
        ai.flags = ApplicationInfo.FLAG_SYSTEM | ApplicationInfo.FLAG_INSTALLED;
        ai.enabled = true;
        ai.uid = Process.myUid();
        ai.targetSdkVersion = 34;
        ai.sourceDir = "/system/priv-app/" + (PACKAGE_GMS.equals(packageName) ? "PrebuiltGmsCore" : "Phonesky") + ".apk";
        // Play services' code, for apps that load it (createPackageContext with
        // CONTEXT_INCLUDE_CODE: the client library's ProviderInstaller): an APK that is
        // there, gesher-camera.apk, which carries ProviderInstallerImpl.
        String code = gmsCodeApk();
        // FLAG_HAS_CODE too: without it ActivityThread.getPackageInfo drops
        // CONTEXT_INCLUDE_CODE and the package's class loader is empty (Gmail: "Didn't
        // find class ...ProviderInstallerImpl on path: DexPathList[[]]").
        if (PACKAGE_GMS.equals(packageName) && code != null) {
            ai.sourceDir = code;
            ai.flags |= ApplicationInfo.FLAG_HAS_CODE;
        }
        ai.publicSourceDir = ai.sourceDir;
        ai.dataDir = "/data/data/" + packageName;
        // LoadedApk's class loader adds it as a native path (new File(nativeLibraryDir)):
        // without one, the package context's getClassLoader throws a NullPointerException
        // (Gmail's ProviderInstaller: "Failed to install provider").
        ai.nativeLibraryDir = "/data/app/" + packageName + "/lib";
        return ai;
    }

    private static ServiceInfo[] createSyntheticServices(String packageName) {
        String[] serviceNames = new String[] {
                "com.google.android.gms.auth.service.START",
                "com.google.android.gms.auth.api.signin.service.START",
                "com.google.android.gms.location.internal.GoogleLocationManagerService.START",
                "com.google.android.gms.gcm.GcmService",
                "com.google.android.c2dm.intent.REGISTER"
        };
        ServiceInfo[] services = new ServiceInfo[serviceNames.length];
        for (int i = 0; i < serviceNames.length; i++) {
            ServiceInfo si = new ServiceInfo();
            si.packageName = packageName;
            si.name = serviceNames[i];
            si.exported = true;
            si.enabled = true;
            si.applicationInfo = createSyntheticApplicationInfo(packageName, 0);
            services[i] = si;
        }
        return services;
    }

    private static ActivityInfo[] createSyntheticReceivers(String packageName) {
        String[] receiverNames = new String[] {
                "com.google.android.gms.gcm.GcmReceiver",
                "com.google.android.c2dm.intent.RECEIVE"
        };
        ActivityInfo[] receivers = new ActivityInfo[receiverNames.length];
        for (int i = 0; i < receiverNames.length; i++) {
            ActivityInfo ai = new ActivityInfo();
            ai.packageName = packageName;
            ai.name = receiverNames[i];
            ai.exported = true;
            ai.enabled = true;
            ai.applicationInfo = createSyntheticApplicationInfo(packageName, 0);
            receivers[i] = ai;
        }
        return receivers;
    }

    public static List<ResolveInfo> querySyntheticServices(Intent intent, String resolvedType, long flags) {
        List<ResolveInfo> result = new ArrayList<>();
        if (intent == null) return result;

        String pkg = intent.getPackage();
        ComponentName comp = intent.getComponent();
        String targetPkg = comp != null ? comp.getPackageName() : pkg;

        if (targetPkg != null && !isSyntheticPackage(targetPkg)) {
            return result;
        }

        String action = intent.getAction();
        if (action != null && (targetPkg == null || isSyntheticPackage(targetPkg))) {
            ResolveInfo ri = new ResolveInfo();
            ServiceInfo si = new ServiceInfo();
            si.packageName = PACKAGE_GMS;
            si.name = comp != null ? comp.getClassName() : action + "Service";
            si.enabled = true;
            si.exported = true;
            si.applicationInfo = createSyntheticApplicationInfo(PACKAGE_GMS, flags);
            ri.serviceInfo = si;
            result.add(ri);
        }
        return result;
    }
}
