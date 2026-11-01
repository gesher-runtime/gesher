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
import android.os.Process;

import java.util.ArrayList;
import java.util.List;

/**
 * Synthetic package definitions for Google Play Services (GMS) and Google Play Store.
 * Provides microG-style stand-in package information, signatures, and service resolution.
 */
public final class GmsPackageSupport {
    public static final String PACKAGE_GMS = "com.google.android.gms";
    public static final String PACKAGE_VENDING = "com.android.vending";

    public static final int GMS_VERSION_CODE = 233013000;
    public static final String GMS_VERSION_NAME = "23.30.13 (190400-549429184)";

    public static final int VENDING_VERSION_CODE = 83000000;
    public static final String VENDING_VERSION_NAME = "83.0.00";

    // Valid X.509 DER certificate hex for Android/Google Play Services compatibility
    public static final String GOOGLE_CERT_HEX =
            "3082038e30820276a003020102020900fc38b97d2d28ce0d300d06092a864886f70d01010b0500" +
            "3074310b3009060355040613025553311330110603550408130a43616c69666f726e6961311630" +
            "140603550407130d4d6f756e7461696e205669657731143012060355040a130b476f6f676c6520" +
            "496e632e3110300e060355040b1307416e64726f69643110300e06035504031307416e64726f69" +
            "643020170d3236313130313132353431335a180f32303534303331393132353431335a3074310b" +
            "3009060355040613025553311330110603550408130a43616c69666f726e696131163014060355" +
            "0407130d4d6f756e7461696e205669657731143012060355040a130b476f6f676c6520496e632e" +
            "3110300e060355040b1307416e64726f69643110300e06035504031307416e64726f6964308201" +
            "22300d06092a864886f70d01010105000382010f003082010a0282010100c7b862a1dece88d289" +
            "ab35c455a863fb2d61f88cb4e54aeff6b7aa1fe05c8376e8d2abc4ea6f106b02eb8e818928a9dc" +
            "ca42b2b3db07076f4ae588279a5a9fd4ed7c47edd0bb27e5bcd759bb1895df7af70ffa64debf36" +
            "1a8a20bf48cb0dcaa72e823da266316e2163b45b685df67e1cf7114ecf609007dbdc3ae346781f" +
            "f65b496970ed582d8903e2dd8ded056c73881b8d466116eb0ba353f64e0e363142be6f3006269a" +
            "0a4e6ccae1ce54c83e78ed26a1ffe2f499b4b069c5c999807f659962955b29aad361b6cc884278" +
            "6fcfc96ff6fbaec4c400116e8113eaa15c7466e8cf3a3f6b2c4edbdb33c51296db1f0f1ae7d04a" +
            "7fb28b5f62bf38e91d03f14dcf0203010001a321301f301d0603551d0e04160414c1bafd7f3301" +
            "068848f0d3c661305a969d179f56300d06092a864886f70d01010b050003820101003c4aa598f0" +
            "3ea50d07d66590a8a1867416780a4257f2d7adf39bced16b38264ee9769e3ab3d3d82ee7526558" +
            "2fed518159c34b1ec5c8e2e87366b9c22974be496ed3f0989b57b51b4939f1dbe3d658bc4fe361" +
            "32f42b494fd3543e5ece659c275b1a6dc235ba81f00a47ab1c6bbeb6e65def63137728d356f7b6" +
            "d6688fa9b7f4994f5ea22d576857f588a43ad2926d29186779c31f6d50a44665b7629461e3cdf0" +
            "5c23f603c4cc73ca1b58154d0e8f5f5b9c6a17fa880d56036579a81ff83339438db8ea038b7b02" +
            "bb6f34fe421a5a6327d40b379e65d5ee2093d509facff659b6731855f5aab914fe8040442b8e36" +
            "f23ca43007ef16e3ff33c35e0bc0e7662d";

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

        if ((flags & PackageManager.GET_SERVICES) != 0 && PACKAGE_GMS.equals(packageName)) {
            pi.services = createSyntheticServices(packageName);
        }

        if ((flags & PackageManager.GET_RECEIVERS) != 0 && PACKAGE_GMS.equals(packageName)) {
            pi.receivers = createSyntheticReceivers(packageName);
        }

        return pi;
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
        ai.publicSourceDir = ai.sourceDir;
        ai.dataDir = "/data/data/" + packageName;
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
