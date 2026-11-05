/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.server;

import android.content.pm.PermissionGroupInfo;
import android.content.pm.PermissionInfo;
import android.content.res.AssetManager;
import android.content.res.XmlResourceParser;
import com.android.internal.pm.pkg.component.ParsedPermission;
import org.xmlpull.v1.XmlPullParser;
import java.util.LinkedHashMap;
import java.util.Map;

/** Permission definitions, without parsing framework-res as an installable application. */
final class PermissionCatalog {
    private static final String ANDROID = "http://schemas.android.com/apk/res/android";
    private final PackageManagerService mPm;
    private final Map<String, PermissionInfo> mFramework = new LinkedHashMap<>();
    private final Map<String, PermissionGroupInfo> mGroups = new LinkedHashMap<>();
    private final java.util.Set<String> mProtectedBroadcasts = new java.util.LinkedHashSet<>();
    private boolean mLoaded;
    private boolean mFrameworkRead;

    PermissionCatalog(PackageManagerService pm) { mPm = pm; }

    private synchronized void load() {
        if (mLoaded) return;
        String root = System.getenv("ANDROID_ROOT");
        String path = (root == null ? "/system" : root) + "/framework/framework-res.apk";
        // PackageParser2 on framework-res references optional framework classes that
        // are not in Gesher. The binary XML reader needs only the already loaded assets.
        try (AssetManager assets = new AssetManager()) {
            int cookie = assets.addAssetPath(path);
            if (cookie == 0) throw new java.io.IOException("framework assets unavailable");
            try (XmlResourceParser xml = assets.openXmlResourceParser(cookie, "AndroidManifest.xml")) {
                for (int event; (event = xml.next()) != XmlPullParser.END_DOCUMENT;) {
                    if (event != XmlPullParser.START_TAG) continue;
                    String name = xml.getAttributeValue(ANDROID, "name");
                    if (name == null) continue;
                    if ("protected-broadcast".equals(xml.getName())) {
                        mProtectedBroadcasts.add(name);
                    } else if ("permission".equals(xml.getName())) {
                        PermissionInfo info = new PermissionInfo(
                                xml.getAttributeValue(ANDROID, "backgroundPermission"));
                        info.name = name;
                        info.packageName = "android";
                        info.protectionLevel = xml.getAttributeIntValue(ANDROID, "protectionLevel", 0);
                        info.group = xml.getAttributeValue(ANDROID, "permissionGroup");
                        info.labelRes = xml.getAttributeResourceValue(ANDROID, "label", 0);
                        info.descriptionRes = xml.getAttributeResourceValue(ANDROID, "description", 0);
                        mFramework.put(name, info);
                    } else if ("permission-group".equals(xml.getName())) {
                        PermissionGroupInfo info = new PermissionGroupInfo();
                        info.name = name;
                        info.packageName = "android";
                        info.labelRes = xml.getAttributeResourceValue(ANDROID, "label", 0);
                        info.descriptionRes = xml.getAttributeResourceValue(ANDROID, "description", 0);
                        mGroups.put(name, info);
                    }
                }
            }
            mFrameworkRead = true;
        } catch (Exception | LinkageError e) {
            // Fail closed for unknown definitions; never turn a parse failure into grants.
            SystemServer.log("permissions: framework metadata unavailable: " + e);
        }
        PermissionInfo fake = new PermissionInfo();
        fake.name = GmsSignatureSpoofing.PERMISSION_FAKE_PACKAGE_SIGNATURE;
        fake.packageName = "android";
        fake.protectionLevel = PermissionInfo.PROTECTION_SIGNATURE;
        mFramework.put(fake.name, fake);
        mLoaded = true;
    }

    PermissionInfo get(String name) {
        load();
        PermissionInfo framework = mFramework.get(name);
        if (framework != null) return new PermissionInfo(framework);
        // Framework permission names cannot be replaced by an APK declaration.
        if (name == null || name.startsWith("android.permission.")) return null;
        PermissionInfo found = null;
        ApkPackage owner = null;
        for (ApkPackage pkg : mPm.packages()) {
            for (ParsedPermission parsed : pkg.parsed.getPermissions()) {
                if (!name.equals(parsed.getName())) continue;
                // A package cannot acquire another app's signature permission by
                // declaring the same permission name first. Conflicts fail closed.
                if (owner != null && !pkg.signaturesMatch(owner)) return null;
                PermissionInfo info = new PermissionInfo(parsed.getBackgroundPermission());
                info.name = parsed.getName();
                info.packageName = pkg.packageName;
                info.protectionLevel = parsed.getProtectionLevel();
                info.group = parsed.getGroup();
                info.labelRes = parsed.getLabelRes();
                info.descriptionRes = parsed.getDescriptionRes();
                found = info;
                owner = pkg;
            }
        }
        return found;
    }

    boolean isProtectedBroadcast(String name) {
        load();
        // A failed framework catalog must not authorize an unknown sender.
        return name != null && (!mFrameworkRead || mProtectedBroadcasts.contains(name));
    }

    Map<String, PermissionInfo> permissions() {
        load();
        Map<String, PermissionInfo> result = new LinkedHashMap<>(mFramework);
        for (ApkPackage pkg : mPm.packages()) {
            for (ParsedPermission permission : pkg.parsed.getPermissions()) {
                PermissionInfo info = get(permission.getName());
                if (info != null) result.putIfAbsent(info.name, info);
            }
        }
        return result;
    }

    Map<String, PermissionGroupInfo> groups() { load(); return new LinkedHashMap<>(mGroups); }
}
