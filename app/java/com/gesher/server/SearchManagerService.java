/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package com.gesher.server;

import android.app.GesherSettingsContext;
import android.app.ISearchManager;
import android.app.SearchableInfo;
import android.content.ComponentName;
import android.content.Intent;
import android.content.pm.ActivityInfo;
import android.content.pm.PackageManager;
import android.content.pm.ResolveInfo;
import android.content.res.Resources;
import android.os.Binder;
import android.os.Bundle;
import android.os.Process;

import java.util.ArrayList;
import java.util.List;
import java.util.function.Function;

/** Resolves installed search declarations using Android's package visibility policy. */
final class SearchManagerService extends ISearchManager.Default {
    private static final String SEARCHABLE = "android.app.searchable";
    private static final String DEFAULT_SEARCHABLE = "android.app.default_searchable";
    private static final long METADATA = PackageManager.GET_META_DATA;
    private static final List<String> SEARCH_ACTIONS = List.of(Intent.ACTION_SEARCH, Intent.ACTION_WEB_SEARCH);

    private final PackageManagerService mPackages;
    private final CallerIdentity mCallers;
    private final Function<ActivityInfo, SearchableInfo> mMetadataReader;

    SearchManagerService(PackageManagerService packages, CallerIdentity callers) {
        this(packages, callers, activity -> SearchableInfo.getActivityMetaData(
                GesherSettingsContext.get(), activity, 0));
    }

    // The resource reader is a dependency; permission and lifecycle checks always
    // surround it. Ordinary services use only the framework APK/XML parser above.
    SearchManagerService(PackageManagerService packages, CallerIdentity callers,
            Function<ActivityInfo, SearchableInfo> metadataReader) {
        mPackages = packages;
        mCallers = callers;
        mMetadataReader = metadataReader;
    }

    @Override public SearchableInfo getSearchableInfo(ComponentName activity) {
        if (activity == null) return null;
        String name = activity.getPackageName();
        // Authenticate the process independently of the requested component. AOSP
        // filters by package visibility, not package-name equality: explicit
        // <queries>, QUERY_ALL_PACKAGES, legacy targets and shared UIDs count.
        boolean system = mCallers.isSystem();
        String callerPackage = system ? "android" : mCallers.callerPackage();
        int callingUid = system ? Process.SYSTEM_UID : mCallers.callerUid();
        ApkPackage pkg = mPackages.getVisibleSearchPackage(callingUid, callerPackage, name);
        if (pkg == null) return null;

        ActivityInfo source = pkg.activityInfo(activity.getClassName(), METADATA);
        if (source == null) return null;
        SearchableInfo result = readSearchable(pkg, source);
        if (result == null) {
            ComponentName target = defaultSearchActivity(name, source);
            if (target != null && !target.equals(activity)) {
                result = readSearchable(pkg, pkg.activityInfo(target.getClassName(), METADATA));
            }
        }
        // APK content is read outside package-manager locks. Recheck both activities
        // against one enabled-state snapshot before publishing the lookup result.
        if (result == null || mPackages.getVisibleSearchPackage(callingUid, callerPackage, name) != pkg
                || !activitiesStillEnabled(pkg, activity, result.getSearchActivity())) {
            return null;
        }
        return result;
    }

    private static boolean activitiesStillEnabled(ApkPackage pkg, ComponentName source, ComponentName target) {
        ActivityInfo[] enabled = pkg.packageInfo(PackageManager.GET_ACTIVITIES).activities;
        if (enabled == null) return false;
        boolean foundSource = false;
        boolean foundTarget = false;
        for (ActivityInfo info : enabled) {
            foundSource |= source.getClassName().equals(info.name);
            foundTarget |= target.getClassName().equals(info.name);
            if (foundSource && foundTarget) return true;
        }
        return false;
    }

    private static ComponentName defaultSearchActivity(String pkg, ActivityInfo source) {
        String target = defaultSearchName(source.metaData);
        if (target == null) target = defaultSearchName(source.applicationInfo.metaData);
        // Android treats the deprecated "*" value as no app-local search target.
        if (target == null || target.isEmpty() || "*".equals(target)) return null;
        return ComponentName.createRelative(pkg, target);
    }

    private static String defaultSearchName(Bundle data) {
        return data == null ? null : data.getString(DEFAULT_SEARCHABLE);
    }

    private SearchableInfo readSearchable(ApkPackage pkg, ActivityInfo activity) {
        if (activity == null || activity.metaData == null || activity.metaData.getInt(SEARCHABLE) == 0
                || !handlesSearch(pkg, activity)) return null;

        long identity = Binder.clearCallingIdentity();
        try {
            // Reuse the framework parser to preserve resource references, suggestion
            // provider metadata, action keys and Android's validation of searchable XML.
            return mMetadataReader.apply(activity);
        } catch (Resources.NotFoundException missing) {
            return null;
        } finally {
            Binder.restoreCallingIdentity(identity);
        }
    }

    private static boolean handlesSearch(ApkPackage pkg, ActivityInfo activity) {
        for (String action : SEARCH_ACTIONS) {
            for (ResolveInfo resolved : pkg.queryIntentActivities(new Intent(action), null, 0)) {
                if (activity.name.equals(resolved.activityInfo.name)) return true;
            }
        }
        return false;
    }

    @Override public List<SearchableInfo> getSearchablesInGlobalSearch() {
        return new ArrayList<>();
    }

    @Override public List<ResolveInfo> getGlobalSearchActivities() {
        return new ArrayList<>();
    }
}
