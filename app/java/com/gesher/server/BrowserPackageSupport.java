/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.server;

import android.content.ComponentName;
import android.content.Intent;
import android.content.IntentFilter;
import android.content.pm.ActivityInfo;
import android.content.pm.ApplicationInfo;
import android.content.pm.PackageInfo;
import android.content.pm.PackageManager;
import android.content.pm.ResolveInfo;
import android.content.pm.Signature;
import android.os.Process;

import java.util.ArrayList;
import java.util.List;

/**
 * The Windows default browser as a synthetic package, {@code com.gesher.browser}, as
 * {@link GmsPackageSupport} models Google Play services: one activity with the filter of a
 * browser (VIEW, BROWSABLE and DEFAULT, http and https, any host). Apps that look for a
 * browser before they open a page (AppAuth's BrowserSelector: queryIntentActivities with
 * GET_RESOLVED_FILTER, then getPackageInfo with GET_SIGNATURES) find it; nothing else
 * of an app runs in it. ActivityTaskManagerService starts no activity for it: a VIEW of an
 * http(s) URL that resolves to it, explicitly ({@code setPackage}, a component) or not,
 * goes to Windows (WindowsHost.openUrl, the user's default browser). It has no custom
 * tabs service: AppAuth then opens the page as a plain browser intent.
 */
public final class BrowserPackageSupport {
    public static final String PACKAGE = "com.gesher.browser";
    public static final String ACTIVITY = PACKAGE + ".WindowsBrowserActivity";
    public static final String LABEL = "Windows default browser";

    public static final int VERSION_CODE = 1;
    public static final String VERSION_NAME = "1.0";

    /**
     * The package's certificate (self-signed, CN=Gesher Browser, EC P-256): AppAuth's
     * BrowserDescriptor hashes it; nothing verifies it.
     */
    public static final String CERT_HEX =
            "308201d030820175a00302010202142584534610c7988e59555b8dd966c9735ff8b501300a0608" +
            "2a8648ce3d040302303c310f300d060355040a0c064765736865723110300e060355040b0c0741" +
            "6e64726f69643117301506035504030c0e4765736865722042726f777365723020170d32363131" +
            "30333137353235385a180f32303736313032313137353235385a303c310f300d060355040a0c06" +
            "4765736865723110300e060355040b0c07416e64726f69643117301506035504030c0e47657368" +
            "65722042726f777365723059301306072a8648ce3d020106082a8648ce3d03010703420004faa9" +
            "b14139fef43f3ac0956d5f897e41d6132f7aaa4b82445f7e2676789346fc92d150d2a3dde21b18" +
            "61274b90673b34d6f3e6b84140ea0ce53f185db30aacc9a3533051301d0603551d0e0416041478" +
            "2d7aa9710f282c928c8f14abf83e7fd15db30e301f0603551d23041830168014782d7aa9710f28" +
            "2c928c8f14abf83e7fd15db30e300f0603551d130101ff040530030101ff300a06082a8648ce3d" +
            "0403020349003046022100fcfea064a5c2086b018922025d04c74249ed413fc597a6f8960c90e9" +
            "7ca4403b02210085da25ad5318d8df67a6820d581c2270e20b43b56c4538188c17bcdb257867d5";

    private static volatile Signature sSignature;

    private BrowserPackageSupport() {
    }

    public static boolean isBrowserPackage(String packageName) {
        return PACKAGE.equals(packageName);
    }

    public static Signature getSignature() {
        if (sSignature == null) {
            sSignature = new Signature(CERT_HEX);
        }
        return sSignature;
    }

    /** The activity's filter: a full browser's, as BrowserSelector.isFullBrowser asks. */
    public static IntentFilter filter() {
        IntentFilter filter = new IntentFilter(Intent.ACTION_VIEW);
        filter.addCategory(Intent.CATEGORY_DEFAULT);
        filter.addCategory(Intent.CATEGORY_BROWSABLE);
        filter.addDataScheme("http");
        filter.addDataScheme("https");
        return filter;
    }

    public static ApplicationInfo createApplicationInfo(long flags) {
        ApplicationInfo ai = new ApplicationInfo();
        ai.packageName = PACKAGE;
        ai.name = LABEL;
        ai.nonLocalizedLabel = LABEL;
        ai.processName = PACKAGE;
        ai.flags = ApplicationInfo.FLAG_SYSTEM | ApplicationInfo.FLAG_INSTALLED;
        ai.enabled = true;
        ai.uid = Process.myUid();
        ai.targetSdkVersion = 34;
        ai.sourceDir = "/system/app/GesherBrowser.apk";
        ai.publicSourceDir = ai.sourceDir;
        ai.dataDir = "/data/data/" + PACKAGE;
        return ai;
    }

    public static ActivityInfo createActivityInfo(long flags) {
        ActivityInfo info = new ActivityInfo();
        info.packageName = PACKAGE;
        info.name = ACTIVITY;
        info.nonLocalizedLabel = LABEL;
        info.processName = PACKAGE;
        info.exported = true;
        info.enabled = true;
        info.launchMode = ActivityInfo.LAUNCH_MULTIPLE;
        info.applicationInfo = createApplicationInfo(flags);
        return info;
    }

    public static PackageInfo createPackageInfo(long flags) {
        PackageInfo pi = new PackageInfo();
        pi.packageName = PACKAGE;
        pi.versionCode = VERSION_CODE;
        pi.setLongVersionCode(VERSION_CODE);
        pi.versionName = VERSION_NAME;
        pi.firstInstallTime = System.currentTimeMillis();
        pi.lastUpdateTime = pi.firstInstallTime;
        pi.applicationInfo = createApplicationInfo(flags);
        if ((flags & PackageManager.GET_SIGNATURES) != 0
                || (flags & PackageManager.GET_SIGNING_CERTIFICATES) != 0) {
            pi.signatures = new Signature[] { getSignature() };
        }
        if ((flags & PackageManager.GET_ACTIVITIES) != 0) {
            pi.activities = new ActivityInfo[] { createActivityInfo(flags) };
        }
        return pi;
    }

    /** The activity info of {@code className} in the package, or null. */
    public static ActivityInfo activityInfo(String className, long flags) {
        return ACTIVITY.equals(className) ? createActivityInfo(flags) : null;
    }

    /**
     * The browser activity, if {@code intent} reaches it: the explicit component, or a
     * match of {@link #filter} (the package may be named; MATCH_DEFAULT_ONLY is met, the
     * filter has CATEGORY_DEFAULT), as ApkPackage.queryIntentActivities answers.
     */
    public static List<ResolveInfo> queryIntentActivities(Intent intent, String resolvedType,
            long flags) {
        List<ResolveInfo> result = new ArrayList<>();
        if (intent == null) return result;
        if (intent.getPackage() != null && !PACKAGE.equals(intent.getPackage())) {
            return result;
        }
        ComponentName component = intent.getComponent();
        if (component != null) {
            if (PACKAGE.equals(component.getPackageName())
                    && ACTIVITY.equals(component.getClassName())) {
                ResolveInfo ri = new ResolveInfo();
                ri.activityInfo = createActivityInfo(flags);
                result.add(ri);
            }
            return result;
        }
        IntentFilter filter = filter();
        String type = resolvedType != null ? resolvedType : intent.getType();
        int match = filter.match(intent.getAction(), type, intent.getScheme(), intent.getData(),
                intent.getCategories(), "gesher");
        if (match < 0) return result;
        ResolveInfo ri = new ResolveInfo();
        ri.activityInfo = createActivityInfo(flags);
        if ((flags & PackageManager.GET_RESOLVED_FILTER) != 0) {
            ri.filter = new IntentFilter(filter);
        }
        ri.match = match;
        ri.isDefault = true;
        ri.nonLocalizedLabel = LABEL;
        result.add(ri);
        return result;
    }
}
