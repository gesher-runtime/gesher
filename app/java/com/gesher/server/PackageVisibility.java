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

import java.util.HashSet;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.Set;
import java.util.StringTokenizer;
import java.util.function.Predicate;

/**
 * Android 16's static package visibility rules over one installed-package snapshot.
 * The runtime has no retained interaction, overlay-actor, instant-app
 * or SDK-sandbox visibility graph; this class does not invent those relationships.
 */
final class PackageVisibility {
    private static final class Entry {
        final String name;
        final int uid;
        final ApkPackage apk;
        final int targetSdk;
        Entry nextAppId; // linked only during construction, immutable after publication

        Entry(String name, int uid, ApkPackage apk, int targetSdk) {
            this.name = name;
            this.uid = uid;
            this.apk = apk;
            this.targetSdk = targetSdk;
        }
    }

    private final Map<String, Entry> mPackages;
    /** One primitive-key group index; constructed privately, then read-only. */
    private static final class AppIdIndex {
        private int[] keys;
        private Entry[] heads;
        private int size;

        AppIdIndex(int expectedEntries) {
            // Start below the package count: isolated inventories avoid repeated
            // small rehashes, while shared-UID inventories can compact at finish.
            int capacity = Math.max(8, Integer.highestOneBit(expectedEntries));
            keys = new int[capacity]; heads = new Entry[capacity];
        }

        private int slot(int key) {
            int hash = key ^ (key >>> 16);
            int slot = hash & (heads.length - 1);
            while (heads[slot] != null && keys[slot] != key) slot = (slot + 1) & (heads.length - 1);
            return slot;
        }

        Entry get(int key) { return heads[slot(key)]; }

        void prepend(Entry entry) {
            int key = UserHandle.getAppId(entry.uid), slot = slot(key);
            if (heads[slot] == null) {
                if (size >= (long)heads.length * 2 / 3) {
                    grow(); slot = slot(key);
                }
                keys[slot] = key; size++;
            }
            entry.nextAppId = heads[slot]; heads[slot] = entry;
        }

        private void grow() {
            if (heads.length >= (1 << 29)) throw new OutOfMemoryError("Too many package app-ID groups");
            rehash(heads.length * 2);
        }

        private void rehash(int capacity) {
            int[] oldKeys = keys; Entry[] oldHeads = heads;
            keys = new int[capacity]; heads = new Entry[capacity];
            for (int i = 0; i < oldHeads.length; i++) if (oldHeads[i] != null) {
                int slot = slot(oldKeys[i]); keys[slot] = oldKeys[i]; heads[slot] = oldHeads[i];
            }
        }

        void finish() {
            int capacity = 8;
            while (size > (long)capacity * 2 / 3) {
                if (capacity >= (1 << 29)) throw new OutOfMemoryError("Too many package app-ID groups");
                capacity *= 2;
            }
            if (capacity < heads.length) rehash(capacity);
            for (int i = 0; i < heads.length; i++) {
                Entry reversed = null;
                for (Entry entry = heads[i]; entry != null;) {
                    Entry next = entry.nextAppId;
                    entry.nextAppId = reversed; reversed = entry; entry = next;
                }
                heads[i] = reversed;
            }
        }
    }

    private final AppIdIndex mPackagesByAppId;
    private final Set<String> mProtectedBroadcasts = new HashSet<>();
    private final Predicate<String> mPlatformProtectedBroadcast;

    PackageVisibility(Map<String, ApkPackage> installed, boolean browser,
            Predicate<String> protectedBroadcast) {
        mPlatformProtectedBroadcast = protectedBroadcast;
        mPackages = new LinkedHashMap<>(installed.size() + 4);
        mPackages.put("android", new Entry("android", Process.SYSTEM_UID, null, 36));
        for (ApkPackage apk : installed.values()) {
            mPackages.put(apk.packageName, new Entry(apk.packageName, apk.uid(), apk,
                    apk.parsed.getTargetSdkVersion()));
            mProtectedBroadcasts.addAll(apk.parsed.getProtectedBroadcasts());
        }
        // Preserve the identity of the existing host definitions. These entries
        // do not supply an installed APK manifest or any additional visibility edge.
        if (browser) addHost(BrowserPackageSupport.createApplicationInfo(0));
        mPackagesByAppId = new AppIdIndex(mPackages.size());
        for (Entry entry : mPackages.values()) {
            mPackagesByAppId.prepend(entry);
        }
        // Reverse each group once to retain installed-package order, without a
        // separate list object/backing array for every isolated application UID.
        mPackagesByAppId.finish();
    }

    private void addHost(ApplicationInfo info) {
        mPackages.put(info.packageName, new Entry(info.packageName, info.uid, null,
                info.targetSdkVersion));
    }

    boolean contains(String name) { return mPackages.containsKey(name); }

    int uid(String name) { return mPackages.get(name).uid; }

    ApkPackage apk(String name) {
        Entry entry = mPackages.get(name);
        return entry == null ? null : entry.apk;
    }

    /** Exact admitted UID identity; host definitions and other users confer no ownership. */
    ApkPackage firstPackageForUid(int uid) {
        for (Entry entry = mPackagesByAppId.get(UserHandle.getAppId(uid)); entry != null;
                entry = entry.nextAppId) {
            if (entry.uid == uid && entry.apk != null) return entry.apk;
        }
        return null;
    }

    String[] installedPackageNamesForUid(int uid) {
        Entry first = mPackagesByAppId.get(UserHandle.getAppId(uid));
        int count = 0;
        for (Entry entry = first; entry != null; entry = entry.nextAppId) {
            if (entry.uid == uid && entry.apk != null) count++;
        }
        if (count == 0) return null;
        String[] names = new String[count];
        int index = 0;
        for (Entry entry = first; entry != null; entry = entry.nextAppId) {
            if (entry.uid == uid && entry.apk != null) names[index++] = entry.name;
        }
        return names;
    }

    /** Legacy Binder uses one compatibility UID for unrelated installed APKs. */
    boolean canSeeFromPackage(String sourceName, String targetName) {
        Entry source = mPackages.get(sourceName);
        Entry target = mPackages.get(targetName);
        if (source == null || target == null) return false;
        return source == target || UserHandle.getAppId(target.uid) < Process.FIRST_APPLICATION_UID
                || queriesAll(source) || isStaticSharedLibrary(target) || forceQueryable(target)
                || declaresVisibility(source, target);
    }

    boolean canSee(int callingUid, String targetName) {
        Entry target = mPackages.get(targetName);
        if (target == null || callingUid < 0) return false;
        int callingAppId = UserHandle.getAppId(callingUid);
        int targetAppId = UserHandle.getAppId(target.uid);
        if (callingAppId < Process.FIRST_APPLICATION_UID
                || targetAppId < Process.FIRST_APPLICATION_UID
                || callingAppId == targetAppId) return true;

        Entry sources = mPackagesByAppId.get(callingAppId);
        if (sources == null) return false;
        for (Entry entry = sources; entry != null; entry = entry.nextAppId) {
            if (queriesAll(entry)) return true;
        }
        if (isStaticSharedLibrary(target)) return true;
        // Force-queryable is shared by all packages with the target app ID, as in
        // AppsFilter's app-ID set. Only real system addons can opt in via metadata.
        Entry targets = mPackagesByAppId.get(targetAppId);
        for (Entry entry = targets; entry != null; entry = entry.nextAppId) {
            if (forceQueryable(entry)) return true;
        }
        // Manifest edges are app-ID pairs too: declaring or matching one installed
        // sibling exposes every package that actually shares that UID.
        for (Entry sibling = targets; sibling != null; sibling = sibling.nextAppId) {
            for (Entry source = sources; source != null; source = source.nextAppId) {
                if (declaresVisibility(source, sibling)) return true;
            }
        }
        return false;
    }

    private static boolean queriesAll(Entry source) {
        return source.targetSdk < 30 || source.apk != null && source.apk.parsed
                .getRequestedPermissions().contains(Manifest.permission.QUERY_ALL_PACKAGES);
    }

    private static boolean isStaticSharedLibrary(Entry target) {
        return target.apk != null && target.apk.parsed.isStaticSharedLibrary();
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
        // Android preserves both the current installer and the still-installed initiator.
        if (source.apk.installSource().isInstaller(target.apk)
                || source.apk.installSource().isInitiator(target.apk)) return true;
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
