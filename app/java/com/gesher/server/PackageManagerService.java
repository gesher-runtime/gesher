/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.server;

import android.content.ComponentName;
import android.content.Intent;
import android.content.pm.ActivityInfo;
import android.content.pm.ApplicationInfo;
import android.content.pm.IPackageManager;
import android.content.pm.PackageInfo;
import android.content.pm.PackageManager;
import android.content.pm.ParceledListSlice;
import android.content.pm.ProviderInfo;
import android.content.pm.ResolveInfo;
import android.content.pm.ServiceInfo;

import java.util.ArrayList;
import java.util.Collection;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;

/**
 * {@code package}: PackageManagerService, knowing the APKs it was given and the installed
 * packages ({@link Installer}; parsed with PackageParser2, {@link ApkPackage}). All of them
 * run as the same uid. Packages come and go while it serves (install, uninstall): readers
 * see one version of the map, replaced as a whole.
 */
public final class PackageManagerService extends IPackageManager.Default {
    private volatile Map<String, ApkPackage> mPackages = new LinkedHashMap<>();

    public PackageManagerService(Collection<ApkPackage> packages) {
        Map<String, ApkPackage> map = new LinkedHashMap<>();
        for (ApkPackage p : packages) {
            map.put(p.packageName, p);
        }
        mPackages = map;
    }

    /** Adds or replaces a package (an install or an update). */
    synchronized void add(ApkPackage p) {
        Map<String, ApkPackage> map = new LinkedHashMap<>(mPackages);
        map.put(p.packageName, p);
        mPackages = map;
    }

    /** Removes a package (an uninstall); returns it, or null. */
    synchronized ApkPackage remove(String packageName) {
        Map<String, ApkPackage> map = new LinkedHashMap<>(mPackages);
        ApkPackage p = map.remove(packageName);
        mPackages = map;
        return p;
    }

    /** The packages, now. */
    Collection<ApkPackage> packages() {
        return mPackages.values();
    }

    public ApkPackage get(String packageName) {
        return packageName == null ? null : mPackages.get(packageName);
    }

    @Override
    public boolean isPackageAvailable(String packageName, int userId) {
        return get(packageName) != null;
    }

    @Override
    public PackageInfo getPackageInfo(String packageName, long flags, int userId) {
        ApkPackage p = get(packageName);
        return p == null ? null : p.packageInfo(flags);
    }

    @Override
    public ApplicationInfo getApplicationInfo(String packageName, long flags, int userId) {
        ApkPackage p = get(packageName);
        return p == null ? null : p.applicationInfo(flags);
    }

    @Override
    public int getTargetSdkVersion(String packageName) {
        ApkPackage p = get(packageName);
        return p == null ? -1 : p.parsed.getTargetSdkVersion();
    }

    @Override
    public ActivityInfo getActivityInfo(ComponentName className, long flags, int userId) {
        ApkPackage p = get(className.getPackageName());
        return p == null ? null : p.activityInfo(className.getClassName(), flags);
    }

    @Override
    public ResolveInfo resolveIntent(Intent intent, String resolvedType, long flags,
            int userId) {
        List<ResolveInfo> all = query(intent, resolvedType, flags);
        return all.isEmpty() ? null : all.get(0);
    }

    @Override
    public ParceledListSlice queryIntentActivities(Intent intent, String resolvedType,
            long flags, int userId) {
        return new ParceledListSlice<>(query(intent, resolvedType, flags));
    }

    private List<ResolveInfo> query(Intent intent, String resolvedType, long flags) {
        List<ResolveInfo> result = new ArrayList<>();
        for (ApkPackage p : mPackages.values()) {
            result.addAll(p.queryIntentActivities(intent, resolvedType, flags));
        }
        return result;
    }

    /** The services {@code intent} resolves to, in every package. */
    List<ResolveInfo> services(Intent intent, String resolvedType, long flags) {
        List<ResolveInfo> result = new ArrayList<>();
        for (ApkPackage p : mPackages.values()) {
            result.addAll(p.queryIntentServices(intent, resolvedType, flags));
        }
        return result;
    }

    /** The manifest receivers {@code intent} reaches, in every package. */
    List<ResolveInfo> receivers(Intent intent, String resolvedType, long flags) {
        List<ResolveInfo> result = new ArrayList<>();
        for (ApkPackage p : mPackages.values()) {
            result.addAll(p.queryIntentReceivers(intent, resolvedType, flags));
        }
        return result;
    }

    /** The provider with authority {@code authority} (one of its authorities), or null. */
    ProviderInfo provider(String authority, long flags) {
        for (ApkPackage p : mPackages.values()) {
            for (ProviderInfo info : p.providers(flags)) {
                if (info.authority == null) continue;
                for (String a : info.authority.split(";")) {
                    if (a.equals(authority)) return info;
                }
            }
        }
        return null;
    }

    @Override
    public ResolveInfo resolveService(Intent intent, String resolvedType, long flags,
            int userId) {
        List<ResolveInfo> all = services(intent, resolvedType, flags);
        return all.isEmpty() ? null : all.get(0);
    }

    @Override
    public ParceledListSlice queryIntentServices(Intent intent, String resolvedType,
            long flags, int userId) {
        return new ParceledListSlice<>(services(intent, resolvedType, flags));
    }

    @Override
    public ParceledListSlice queryIntentReceivers(Intent intent, String resolvedType,
            long flags, int userId) {
        return new ParceledListSlice<>(receivers(intent, resolvedType, flags));
    }

    @Override
    public ServiceInfo getServiceInfo(ComponentName className, long flags, int userId) {
        ApkPackage p = get(className.getPackageName());
        return p == null ? null : p.serviceInfo(className.getClassName(), flags);
    }

    @Override
    public ActivityInfo getReceiverInfo(ComponentName className, long flags, int userId) {
        ApkPackage p = get(className.getPackageName());
        return p == null ? null : p.receiverInfo(className.getClassName(), flags);
    }

    @Override
    public ProviderInfo resolveContentProvider(String name, long flags, int userId) {
        return provider(name, flags);
    }

    @Override
    public ProviderInfo getProviderInfo(ComponentName className, long flags, int userId) {
        ApkPackage p = get(className.getPackageName());
        if (p == null) return null;
        for (ProviderInfo info : p.providers(flags)) {
            if (info.name.equals(className.getClassName())) return info;
        }
        return null;
    }

    @Override
    public ParceledListSlice getInstalledPackages(long flags, int userId) {
        List<PackageInfo> result = new ArrayList<>();
        for (ApkPackage p : mPackages.values()) {
            result.add(p.packageInfo(flags));
        }
        return new ParceledListSlice<>(result);
    }

    @Override
    public ParceledListSlice getInstalledApplications(long flags, int userId) {
        List<ApplicationInfo> result = new ArrayList<>();
        for (ApkPackage p : mPackages.values()) {
            result.add(p.applicationInfo(flags));
        }
        return new ParceledListSlice<>(result);
    }

    @Override
    public int getComponentEnabledSetting(ComponentName componentName, int userId) {
        return PackageManager.COMPONENT_ENABLED_STATE_DEFAULT;
    }

    @Override
    public int getApplicationEnabledSetting(String packageName, int userId) {
        return PackageManager.COMPONENT_ENABLED_STATE_DEFAULT;
    }

    /**
     * The device's system features (docs/e2e.md): a phone-sized window with a mouse
     * (faketouch, both orientations) whose launcher could host app widgets (apps reach
     * AppWidgetManager only on devices with app_widgets, and phone apps assume it).
     * Telephony, Wi-Fi, camera, sensors, touchscreen and the rest are absent.
     */
    static final android.util.ArrayMap<String, android.content.pm.FeatureInfo> FEATURES =
            new android.util.ArrayMap<>();

    static {
        for (String name : new String[] {PackageManager.FEATURE_APP_WIDGETS,
                PackageManager.FEATURE_FAKETOUCH, PackageManager.FEATURE_SCREEN_PORTRAIT,
                PackageManager.FEATURE_SCREEN_LANDSCAPE}) {
            android.content.pm.FeatureInfo f = new android.content.pm.FeatureInfo();
            f.name = name;
            FEATURES.put(name, f);
        }
    }

    @Override
    public boolean hasSystemFeature(String name, int version) {
        android.content.pm.FeatureInfo f = FEATURES.get(name);
        return f != null && version <= f.version;
    }

    @Override
    public ParceledListSlice<android.content.pm.FeatureInfo> getSystemAvailableFeatures() {
        return new ParceledListSlice<>(new ArrayList<>(FEATURES.values()));
    }

    @Override
    public String getNameForUid(int uid) {
        Map<String, ApkPackage> packages = mPackages;
        return packages.isEmpty() ? null : packages.keySet().iterator().next();
    }

    @Override
    public String[] getPackagesForUid(int uid) {
        return mPackages.keySet().toArray(new String[0]);
    }

    @Override
    public String getInstallerPackageName(String packageName) {
        return null;
    }

    @Override
    public void notifyDexLoad(String loadingPackageName,
            java.util.Map<String, String> classLoaderContextMap, String loaderIsa) {
        // No dexopt to schedule.
    }

    @Override
    public ParceledListSlice queryProperty(String propertyName, int componentType) {
        // Manifest <property> tags are not indexed: none is found.
        return new ParceledListSlice<>(java.util.Collections.emptyList());
    }
}
