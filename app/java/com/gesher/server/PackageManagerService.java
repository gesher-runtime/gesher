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
import android.content.pm.ResolveInfo;

import java.util.ArrayList;
import java.util.Collection;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;

/**
 * {@code package}: PackageManagerService, knowing the APKs it was given (parsed with
 * PackageParser2, {@link ApkPackage}). All of them run as the same uid.
 */
public final class PackageManagerService extends IPackageManager.Default {
    private final Map<String, ApkPackage> mPackages = new LinkedHashMap<>();

    public PackageManagerService(Collection<ApkPackage> packages) {
        for (ApkPackage p : packages) {
            mPackages.put(p.packageName, p);
        }
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

    @Override
    public boolean hasSystemFeature(String name, int version) {
        return false;
    }

    @Override
    public String getNameForUid(int uid) {
        return mPackages.isEmpty() ? null : mPackages.keySet().iterator().next();
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
