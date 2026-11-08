/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package com.gesher.server;

import android.Manifest;
import android.content.Intent;
import android.content.IntentFilter;
import android.content.pm.ApplicationInfo;
import android.content.res.Resources;
import android.os.Process;
import android.os.UserHandle;

import com.android.internal.pm.parsing.pkg.ParsedPackage;
import com.android.internal.pm.pkg.component.ParsedIntentInfo;
import com.android.internal.pm.pkg.component.ParsedMainComponent;
import com.android.internal.pm.pkg.component.ParsedPermission;
import com.android.internal.pm.pkg.component.ParsedProvider;

import java.util.ArrayList;
import java.util.HashSet;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.Set;
import java.util.StringTokenizer;
import java.util.function.Predicate;

/**
 * Android 16's static package visibility rules over one installed-package snapshot.
 * The runtime has no retained interaction, install-source, overlay-actor, instant-app
 * or SDK-sandbox visibility graph; this class does not invent those relationships.
 */
final class PackageVisibility {
    private static final class Entry {
        final String name;
        final int uid;
        final ApkPackage apk;
        final int targetSdk;

        Entry(String name, int uid, ApkPackage apk, int targetSdk) {
            this.name = name;
            this.uid = uid;
            this.apk = apk;
            this.targetSdk = targetSdk;
        }
    }

    private final Map<String, Entry> mPackages = new LinkedHashMap<>();
    private final Set<String> mProtectedBroadcasts = new HashSet<>();
    private final Predicate<String> mPlatformProtectedBroadcast;

    PackageVisibility(Map<String, ApkPackage> installed, boolean browser,
            boolean gms, boolean vending, Predicate<String> protectedBroadcast) {
        mPlatformProtectedBroadcast = protectedBroadcast;
        mPackages.put("android", new Entry("android", Process.SYSTEM_UID, null, 36));
        for (ApkPackage apk : installed.values()) {
            mPackages.put(apk.packageName, new Entry(apk.packageName, apk.uid(), apk,
                    apk.parsed.getTargetSdkVersion()));
            mProtectedBroadcasts.addAll(apk.parsed.getProtectedBroadcasts());
        }
        // Preserve the identity of the existing host definitions. These entries
        // do not supply an installed APK manifest or any additional visibility edge.
        if (browser) addHost(BrowserPackageSupport.createApplicationInfo(0));
        if (gms) addHost(GmsPackageSupport.createSyntheticApplicationInfo(
                GmsPackageSupport.PACKAGE_GMS, 0));
        if (vending) addHost(GmsPackageSupport.createSyntheticApplicationInfo(
                GmsPackageSupport.PACKAGE_VENDING, 0));
    }

    private void addHost(ApplicationInfo info) {
        mPackages.put(info.packageName, new Entry(info.packageName, info.uid, null,
                info.targetSdkVersion));
    }

    boolean contains(String name) { return mPackages.containsKey(name); }

    int uid(String name) { return mPackages.get(name).uid; }

    boolean canSee(int callingUid, String targetName) {
        Entry target = mPackages.get(targetName);
        if (target == null || callingUid < 0) return false;
        int callingAppId = UserHandle.getAppId(callingUid);
        int targetAppId = UserHandle.getAppId(target.uid);
        if (callingAppId < Process.FIRST_APPLICATION_UID
                || targetAppId < Process.FIRST_APPLICATION_UID
                || callingAppId == targetAppId) return true;

        List<Entry> sources = new ArrayList<>();
        for (Entry entry : mPackages.values()) {
            if (UserHandle.getAppId(entry.uid) != callingAppId) continue;
            sources.add(entry);
            if (entry.targetSdk < 30 || entry.apk != null && entry.apk.parsed
                    .getRequestedPermissions().contains(Manifest.permission.QUERY_ALL_PACKAGES)) {
                return true;
            }
        }
        if (sources.isEmpty()) return false;
        if (target.apk != null && target.apk.parsed.isStaticSharedLibrary()) return true;
        // Force-queryable is shared by all packages with the target app ID, as in
        // AppsFilter's app-ID set. Only real system addons can opt in via metadata.
        for (Entry entry : mPackages.values()) {
            if (UserHandle.getAppId(entry.uid) == targetAppId && forceQueryable(entry)) return true;
        }
        // Manifest edges are app-ID pairs too: declaring or matching one installed
        // sibling exposes every package that actually shares that UID.
        for (Entry sibling : mPackages.values()) {
            if (UserHandle.getAppId(sibling.uid) != targetAppId) continue;
            for (Entry source : sources) if (declaresVisibility(source, sibling)) return true;
        }
        return false;
    }

    private static boolean forceQueryable(Entry target) {
        if (target.apk == null || !target.apk.isSystemAddon()) return false;
        if (target.apk.parsed.isForceQueryable()) return true;
        Resources resources = Resources.getSystem();
        if (resources.getBoolean(com.android.internal.R.bool.config_forceSystemPackagesQueryable)) {
            return true;
        }
        for (String name : resources.getStringArray(
                com.android.internal.R.array.config_forceQueryablePackages)) {
            if (target.name.equals(name)) return true;
        }
        // Gesher has no platform-signed APK inventory or adb force-queryable override.
        return false;
    }

    private boolean declaresVisibility(Entry source, Entry target) {
        if (source.apk == null) return false;
        ParsedPackage querying = source.apk.parsed;
        if (querying.getQueriesPackages().contains(target.name)) return true;
        if (target.apk == null) {
            if (BrowserPackageSupport.PACKAGE.equals(target.name)) {
                for (Intent query : querying.getQueriesIntents()) {
                    if (matches(query, BrowserPackageSupport.filter(), false)) return true;
                }
            }
            return false;
        }
        ParsedPackage potentialTarget = target.apk.parsed;
        for (Intent query : querying.getQueriesIntents()) {
            if (matchesComponents(query, potentialTarget.getServices(), false)
                    || matchesComponents(query, potentialTarget.getActivities(), false)
                    || matchesComponents(query, potentialTarget.getReceivers(), true)
                    || matchesComponents(query, potentialTarget.getProviders(), false)) return true;
        }
        if (!querying.getQueriesProviders().isEmpty()) {
            for (ParsedProvider provider : potentialTarget.getProviders()) {
                if (!provider.isExported() || provider.getAuthority() == null) continue;
                StringTokenizer authorities = new StringTokenizer(provider.getAuthority(), ";");
                while (authorities.hasMoreTokens()) {
                    if (querying.getQueriesProviders().contains(authorities.nextToken())) return true;
                }
            }
        }
        for (String library : potentialTarget.getLibraryNames()) {
            if (querying.getUsesLibraries().contains(library)
                    || querying.getUsesOptionalLibraries().contains(library)) return true;
        }
        for (ParsedPermission permission : potentialTarget.getPermissions()) {
            if (querying.getRequestedPermissions().contains(permission.getName())) return true;
        }
        return false;
    }

    private boolean matchesComponents(Intent query,
            List<? extends ParsedMainComponent> components, boolean receiver) {
        for (ParsedMainComponent component : components) {
            if (!component.isExported()) continue;
            for (ParsedIntentInfo info : component.getIntents()) {
                if (matches(query, info.getIntentFilter(), receiver)) return true;
            }
        }
        return false;
    }

    private boolean matches(Intent query, IntentFilter filter, boolean receiver) {
        Set<String> ignored = null;
        if (receiver) {
            ignored = new HashSet<>();
            for (int i = 0; i < filter.countActions(); i++) {
                String action = filter.getAction(i);
                if (mProtectedBroadcasts.contains(action)
                        || mPlatformProtectedBroadcast.test(action)) ignored.add(action);
            }
        }
        // Visibility queries intentionally include disabled components and do not
        // require DEFAULT or the caller's permission to start the exported component.
        return filter.match(query.getAction(), query.getType(), query.getScheme(), query.getData(),
                query.getCategories(), "AppsFilter", true, ignored) > 0;
    }
}
