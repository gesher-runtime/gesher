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
                        info.flags = xml.getAttributeIntValue(ANDROID, "permissionFlags", 0);
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
        PermissionInfo declared = declarations().permissions.get(name);
        return declared == null ? null : new PermissionInfo(declared);
    }

    boolean isProtectedBroadcast(String name) {
        load();
        // A failed framework catalog must not authorize an unknown sender.
        return name != null && (isPlatformPrefix(name) || !mFrameworkRead
                || mProtectedBroadcasts.contains(name));
    }

    /** Positive catalog evidence, for exemptions that must not be granted on load failure. */
    boolean isKnownProtectedBroadcast(String name) {
        load();
        return name != null && (isPlatformPrefix(name)
                || mFrameworkRead && mProtectedBroadcasts.contains(name));
    }

    /** Android 16 PackageManagerService.isProtectedBroadcast: prefixes outside any manifest. */
    private static boolean isPlatformPrefix(String name) {
        return name.startsWith("android.net.netmon.lingerExpired")
                || name.startsWith("com.android.server.sip.SipWakeupTimer")
                || name.startsWith("com.android.internal.telephony.data-reconnect")
                || name.startsWith("android.net.netmon.launchCaptivePortalApp");
    }

    Map<String, PermissionInfo> permissions() {
        load();
        Map<String, PermissionInfo> result = new LinkedHashMap<>(mFramework);
        for (PermissionInfo info : declarations().permissions.values()) {
            result.put(info.name, new PermissionInfo(info));
        }
        return result;
    }

    /** Application declarations of one immutable published package map. */
    private record Declarations(Map<String, ApkPackage> packages, Map<String, PermissionInfo> permissions) { }
    // Keyed by PM's published map object, like PermissionPolicy's UID index: a
    // replacement, removal or rollback publishes another map. The entries are
    // private and only copies leave this class.
    private volatile Declarations mDeclarations;

    /** Do not retain a removed generation's declarations until the next query. */
    void invalidateDeclarations() { mDeclarations = null; }

    private Declarations declarations() {
        Map<String, ApkPackage> packages = mPm.packageSnapshot();
        Declarations current = mDeclarations;
        if (current != null && current.packages == packages) return current;
        Map<String, PermissionInfo> result = new LinkedHashMap<>();
        Map<String, ApkPackage> owners = new java.util.HashMap<>();
        java.util.Set<String> conflicts = new java.util.HashSet<>();
        for (ApkPackage pkg : packages.values()) {
            for (ParsedPermission permission : pkg.parsed.getPermissions()) {
                String name = permission.getName();
                // Neither a platform definition nor its reserved namespace may be
                // replaced by an application's declaration.
                if (name == null || mFramework.containsKey(name)
                        || name.startsWith("android.permission.") || conflicts.contains(name)) continue;
                ApkPackage owner = owners.get(name);
                // A package cannot acquire another app's signature permission by
                // declaring the same permission name first. Conflicts fail closed.
                if (owner != null && !pkg.signaturesMatch(owner)) {
                    result.remove(name);
                    conflicts.add(name);
                    continue;
                }
                owners.put(name, pkg);
                // Same-signer duplicates take the last declaration, with the first
                // declaration's position in the catalogue unchanged.
                result.put(name, declaredPermission(pkg, permission));
            }
        }
        current = new Declarations(packages, result);
        mDeclarations = current;
        return current;
    }

    private static PermissionInfo declaredPermission(ApkPackage pkg, ParsedPermission parsed) {
        PermissionInfo info = new PermissionInfo(parsed.getBackgroundPermission());
        info.name = parsed.getName();
        info.packageName = pkg.packageName;
        info.protectionLevel = parsed.getProtectionLevel();
        info.flags = parsed.getFlags();
        info.group = parsed.getGroup();
        info.labelRes = parsed.getLabelRes();
        info.descriptionRes = parsed.getDescriptionRes();
        return info;
    }

    Map<String, PermissionGroupInfo> groups() { load(); return new LinkedHashMap<>(mGroups); }
}
