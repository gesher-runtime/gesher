/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.app;

import android.content.pm.ActivityInfo;
import android.content.pm.ApplicationInfo;
import android.content.pm.PackageInfo;
import android.content.res.ApkAssets;
import android.content.res.XmlResourceParser;
import android.os.Process;

import org.xmlpull.v1.XmlPullParser;

import java.util.ArrayList;
import java.util.List;

/**
 * The parts of an APK's binary AndroidManifest.xml that launching an Activity needs, read
 * through {@link ApkAssets#openXml} (libandroidfw's ResXMLTree, as PackageParser reads it),
 * into the {@link ApplicationInfo}, {@link ActivityInfo}s and {@link PackageInfo} that
 * PackageManagerService would hand an app.
 *
 * <p>Deliberately small: no splits, shared libraries, permissions, providers, services,
 * receivers, activity-aliases or meta-data. ParsingPackageUtils can replace it once the
 * stand-in package manager grows into a real one.
 */
final class AppManifest {
    static final String ANDROID_NS = "http://schemas.android.com/apk/res/android";

    final String apkPath;
    final ApplicationInfo applicationInfo = new ApplicationInfo();
    final List<ActivityInfo> activities = new ArrayList<>();
    final List<String> launcherActivities = new ArrayList<>();
    String versionName;
    long versionCode;

    private AppManifest(String apkPath) {
        this.apkPath = apkPath;
    }

    static AppManifest read(String apkPath) throws Exception {
        AppManifest m = new AppManifest(apkPath);
        ApkAssets assets = ApkAssets.loadFromPath(apkPath);
        try (XmlResourceParser parser = assets.openXml("AndroidManifest.xml")) {
            m.parse(parser);
        } finally {
            assets.close();
        }
        m.finish();
        return m;
    }

    private void parse(XmlResourceParser p) throws Exception {
        ApplicationInfo ai = applicationInfo;
        ai.targetSdkVersion = 0;
        ai.minSdkVersion = 1;
        ai.flags = ApplicationInfo.FLAG_HAS_CODE;
        ActivityInfo activity = null;  // the <activity> being read
        boolean inFilter = false;
        boolean main = false;
        boolean launcher = false;
        int type;
        while ((type = p.next()) != XmlPullParser.END_DOCUMENT) {
            if (type == XmlPullParser.END_TAG) {
                switch (p.getName()) {
                    case "activity":
                        activity = null;
                        break;
                    case "intent-filter":
                        if (activity != null && main && launcher) {
                            launcherActivities.add(activity.name);
                        }
                        inFilter = false;
                        break;
                }
                continue;
            }
            if (type != XmlPullParser.START_TAG) {
                continue;
            }
            switch (p.getName()) {
                case "manifest":
                    ai.packageName = p.getAttributeValue(null, "package");
                    versionName = p.getAttributeValue(ANDROID_NS, "versionName");
                    versionCode = p.getAttributeIntValue(ANDROID_NS, "versionCode", 0)
                            & 0xffffffffL;
                    break;
                case "uses-sdk":
                    ai.minSdkVersion = p.getAttributeIntValue(ANDROID_NS, "minSdkVersion", 1);
                    ai.targetSdkVersion = p.getAttributeIntValue(ANDROID_NS,
                            "targetSdkVersion", ai.minSdkVersion);
                    break;
                case "application":
                    String appClass = p.getAttributeValue(ANDROID_NS, "name");
                    if (appClass != null) {
                        ai.className = qualify(appClass);
                        ai.name = ai.className;
                    }
                    ai.theme = p.getAttributeResourceValue(ANDROID_NS, "theme", 0);
                    ai.labelRes = p.getAttributeResourceValue(ANDROID_NS, "label", 0);
                    if (ai.labelRes == 0) {
                        ai.nonLocalizedLabel = p.getAttributeValue(ANDROID_NS, "label");
                    }
                    ai.icon = p.getAttributeResourceValue(ANDROID_NS, "icon", 0);
                    if (p.getAttributeBooleanValue(ANDROID_NS, "debuggable", false)) {
                        ai.flags |= ApplicationInfo.FLAG_DEBUGGABLE;
                    }
                    if (!p.getAttributeBooleanValue(ANDROID_NS, "hasCode", true)) {
                        ai.flags &= ~ApplicationInfo.FLAG_HAS_CODE;
                    }
                    String factory = p.getAttributeValue(ANDROID_NS, "appComponentFactory");
                    if (factory != null) {
                        ai.appComponentFactory = qualify(factory);
                    }
                    break;
                case "activity":
                    activity = new ActivityInfo();
                    activity.name = qualify(p.getAttributeValue(ANDROID_NS, "name"));
                    activity.theme = p.getAttributeResourceValue(ANDROID_NS, "theme", 0);
                    activity.labelRes = p.getAttributeResourceValue(ANDROID_NS, "label", 0);
                    if (activity.labelRes == 0) {
                        activity.nonLocalizedLabel = p.getAttributeValue(ANDROID_NS, "label");
                    }
                    activity.exported = p.getAttributeBooleanValue(ANDROID_NS, "exported",
                            false);
                    activities.add(activity);
                    break;
                case "intent-filter":
                    inFilter = true;
                    main = false;
                    launcher = false;
                    break;
                case "action":
                    if (inFilter && "android.intent.action.MAIN".equals(
                            p.getAttributeValue(ANDROID_NS, "name"))) {
                        main = true;
                    }
                    break;
                case "category":
                    if (inFilter && "android.intent.category.LAUNCHER".equals(
                            p.getAttributeValue(ANDROID_NS, "name"))) {
                        launcher = true;
                    }
                    break;
            }
        }
    }

    private String qualify(String name) {
        if (name == null) {
            return null;
        }
        if (name.startsWith(".")) {
            return applicationInfo.packageName + name;
        }
        if (name.indexOf('.') < 0) {
            return applicationInfo.packageName + "." + name;
        }
        return name;
    }

    /** Fills in what an installed package's ApplicationInfo has beyond the manifest. */
    private void finish() {
        ApplicationInfo ai = applicationInfo;
        if (ai.packageName == null) {
            throw new IllegalStateException(apkPath + ": the manifest has no package name");
        }
        if (ai.targetSdkVersion == 0) {
            ai.targetSdkVersion = ai.minSdkVersion;
        }
        ai.sourceDir = apkPath;
        ai.publicSourceDir = apkPath;
        ai.processName = ai.packageName;
        ai.uid = Process.myUid();
        ai.enabled = true;
        ai.flags |= ApplicationInfo.FLAG_INSTALLED
                | ApplicationInfo.FLAG_SUPPORTS_SMALL_SCREENS
                | ApplicationInfo.FLAG_SUPPORTS_NORMAL_SCREENS
                | ApplicationInfo.FLAG_SUPPORTS_LARGE_SCREENS
                | ApplicationInfo.FLAG_SUPPORTS_XLARGE_SCREENS
                | ApplicationInfo.FLAG_RESIZEABLE_FOR_SCREENS
                | ApplicationInfo.FLAG_SUPPORTS_SCREEN_DENSITIES
                | ApplicationInfo.FLAG_ALLOW_BACKUP;
        for (ActivityInfo a : activities) {
            a.applicationInfo = ai;
            a.packageName = ai.packageName;
            a.processName = ai.processName;
            a.enabled = true;
            a.launchMode = ActivityInfo.LAUNCH_MULTIPLE;
            a.screenOrientation = ActivityInfo.SCREEN_ORIENTATION_UNSPECIFIED;
            a.resizeMode = ActivityInfo.RESIZE_MODE_RESIZEABLE;
        }
    }

    void setDataDir(String dataDir) {
        ApplicationInfo ai = applicationInfo;
        ai.dataDir = dataDir;
        ai.credentialProtectedDataDir = dataDir;
        ai.deviceProtectedDataDir = dataDir;
        ai.nativeLibraryDir = dataDir + "/lib";
    }

    /** The Activity to launch: {@code requested}, or the first MAIN/LAUNCHER one. */
    ActivityInfo launcherActivity(String requested) {
        String name = requested != null ? qualify(requested)
                : !launcherActivities.isEmpty() ? launcherActivities.get(0)
                : null;
        if (name == null) {
            throw new IllegalStateException(apkPath + " has no MAIN/LAUNCHER activity;"
                    + " name one with --activity");
        }
        for (ActivityInfo a : activities) {
            if (a.name.equals(name)) {
                return a;
            }
        }
        throw new IllegalStateException(apkPath + " declares no activity " + name);
    }

    ActivityInfo findActivity(String className) {
        for (ActivityInfo a : activities) {
            if (a.name.equals(className)) {
                return a;
            }
        }
        return null;
    }

    PackageInfo packageInfo() {
        PackageInfo pi = new PackageInfo();
        pi.packageName = applicationInfo.packageName;
        pi.versionName = versionName;
        pi.setLongVersionCode(versionCode);
        pi.applicationInfo = applicationInfo;
        pi.activities = activities.toArray(new ActivityInfo[0]);
        pi.firstInstallTime = pi.lastUpdateTime = System.currentTimeMillis();
        return pi;
    }
}
