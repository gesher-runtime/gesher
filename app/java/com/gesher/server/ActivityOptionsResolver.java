/*
 * Copyright (C) 2026 The Gesher Authors
 * Copyright (C) 2021 The Android Open Source Project
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.server;

import android.content.ComponentName;
import android.content.Intent;
import android.content.IntentFilter;
import android.content.UriRelativeFilter;
import android.content.UriRelativeFilterGroup;
import android.content.pm.ActivityInfo;
import android.content.pm.PackageInfo;
import android.content.pm.PackageManager;
import android.content.pm.ResolveInfo;
import android.os.UserHandle;

import com.android.internal.pm.pkg.component.ParsedActivity;
import com.android.internal.pm.pkg.component.ParsedIntentInfo;

import java.util.ArrayList;
import java.util.Comparator;
import java.util.Iterator;
import java.util.List;
import java.util.Map;
import java.util.Objects;

/**
 * Installed-state implementation of queryIntentActivityOptions. The options algorithm and
 * result ordering follow android-16.0.0_r4 ResolveIntentHelper / ComponentResolver. It is kept
 * separate from ordinary activity startup, which still has no Android preferred-activity or
 * ResolverActivity backend: an ambiguous implicit specific selects the first real candidate.
 */
final class ActivityOptionsResolver {
    private final Map<String, ApkPackage> mPackages;
    private final boolean mHostBrowser;

    ActivityOptionsResolver(Map<String, ApkPackage> packages, boolean hostBrowser) {
        mPackages = packages;
        mHostBrowser = hostBrowser;
    }

    List<ResolveInfo> query(ComponentName caller, Intent[] specifics, String[] specificTypes,
            Intent intent, String resolvedType, long flags) {
        Objects.requireNonNull(intent, "intent");
        // ComputerEngine clears these bits for ordinary callers. All installed callers in
        // the current package model are full apps; there is no instant-app caller state.
        flags &= ~(PackageManager.MATCH_VISIBLE_TO_INSTANT_APP_ONLY
                | PackageManager.MATCH_EXPLICITLY_VISIBLE_ONLY);
        final String resultsAction = intent.getAction();
        final List<ResolveInfo> results = activities(intent, resolvedType,
                flags | PackageManager.GET_RESOLVED_FILTER);
        int specificsPos = 0;
        if (specifics != null) {
            for (int i = 0; i < specifics.length; i++) {
                Intent specific = specifics[i];
                if (specific == null) continue;
                String action = specific.getAction();
                if (resultsAction != null && resultsAction.equals(action)) action = null;
                ComponentName component = specific.getComponent();
                ResolveInfo resolved = null;
                ActivityInfo activity;
                if (component == null) {
                    List<ResolveInfo> matches = activities(specific,
                            specificTypes == null ? null : specificTypes[i], flags);
                    if (matches.isEmpty()) continue;
                    resolved = matches.get(0);
                    activity = resolved.activityInfo;
                    component = new ComponentName(activity.packageName, activity.name);
                } else {
                    activity = activity(component, flags);
                    if (activity == null) continue;
                }
                // Deliberately scan only the generic tail. Different specific Intents may
                // target the same component and must retain their original array indices.
                for (int j = specificsPos; j < results.size(); j++) {
                    ResolveInfo generic = results.get(j);
                    if (sameComponent(generic.activityInfo, component)
                            || (action != null && generic.filter != null
                                    && generic.filter.matchAction(action))) {
                        results.remove(j--);
                        // This reuse (including action matches) is Android's explicit-specific
                        // behavior. Preserve its label/filter and ActivityInfo together.
                        if (resolved == null) resolved = generic;
                    }
                }
                if (resolved == null) {
                    resolved = new ResolveInfo();
                    resolved.activityInfo = activity;
                }
                resolved.specificIndex = i;
                results.add(specificsPos++, resolved);
            }
        }
        for (int i = specificsPos; i < results.size() - 1; i++) {
            IntentFilter filter = results.get(i).filter;
            if (filter == null) continue;
            Iterator<String> actions = filter.actionsIterator();
            if (actions == null) continue;
            while (actions.hasNext()) {
                String action = actions.next();
                if (resultsAction != null && resultsAction.equals(action)) continue;
                for (int j = i + 1; j < results.size(); j++) {
                    IntentFilter other = results.get(j).filter;
                    if (other != null && other.hasAction(action)) results.remove(j--);
                }
            }
        }
        if (caller != null) {
            for (int i = 0; i < results.size(); i++) {
                if (sameComponent(results.get(i).activityInfo, caller)) {
                    // Android removes the first occurrence, including when repeated specifics
                    // have deliberately retained more than one entry for this component.
                    results.remove(i);
                    break;
                }
            }
        }
        if ((flags & PackageManager.GET_RESOLVED_FILTER) == 0) {
            for (ResolveInfo result : results) result.filter = null;
        }
        return results;
    }

    private static boolean sameComponent(ActivityInfo activity, ComponentName component) {
        return component.getPackageName().equals(activity.packageName)
                && component.getClassName().equals(activity.name);
    }

    private boolean matchesPackage(ApkPackage pkg, long flags) {
        return ((flags & PackageManager.MATCH_SYSTEM_ONLY) == 0 || pkg.isSystemAddon())
                && ((flags & PackageManager.MATCH_DISABLED_COMPONENTS) != 0
                        || pkg.parsed.isEnabled());
    }

    private static boolean matchesActivity(ActivityInfo activity, long flags) {
        if ((flags & PackageManager.MATCH_DISABLED_COMPONENTS) == 0 && !activity.enabled) {
            return false;
        }
        long boot = flags & (PackageManager.MATCH_DIRECT_BOOT_AWARE
                | PackageManager.MATCH_DIRECT_BOOT_UNAWARE);
        return boot == 0 || (activity.directBootAware
                ? (boot & PackageManager.MATCH_DIRECT_BOOT_AWARE) != 0
                : (boot & PackageManager.MATCH_DIRECT_BOOT_UNAWARE) != 0);
    }

    private ActivityInfo activity(ComponentName component, long flags) {
        ApkPackage pkg = mPackages.get(component.getPackageName());
        if (pkg != null) {
            if (!matchesPackage(pkg, flags)) return null;
            // The parser's isSystem bit is not the verified addon's installed system state.
            // Check that state above, then let the real metadata generator apply other flags.
            ActivityInfo activity = pkg.activityInfo(component.getClassName(),
                    flags & ~PackageManager.MATCH_SYSTEM_ONLY);
            if (activity == null || !matchesActivity(activity, flags)) return null;
            activity.applicationInfo.enabled = pkg.parsed.isEnabled();
            return activity;
        }
        if (mHostBrowser && BrowserPackageSupport.isBrowserPackage(component.getPackageName())) {
            ActivityInfo activity = BrowserPackageSupport.activityInfo(component.getClassName(), flags);
            return activity != null && matchesActivity(activity, flags) ? activity : null;
        }
        return null;
    }

    private List<ResolveInfo> activities(Intent original, String type, long flags) {
        // ComputerEngine selects its package-specific resolver before substituting a
        // selector. A selector's package still restricts candidates, but uses MIME cuts.
        boolean packageQuery = original.getPackage() != null;
        Intent intent = original.getComponent() == null && original.getSelector() != null
                ? original.getSelector() : original;
        List<ResolveInfo> results = new ArrayList<>();
        if (intent.getComponent() != null) {
            ActivityInfo activity = activity(intent.getComponent(), flags);
            if (activity != null) {
                ResolveInfo result = new ResolveInfo();
                result.activityInfo = activity;
                result.userHandle = UserHandle.SYSTEM;
                results.add(result);
            }
            return results;
        }

        // IntentResolver visits exact/base MIME, wildcard MIME, */*, and scheme cuts in
        // that order, and keeps the first matching filter for each component. A package-
        // restricted query instead visits its filters in manifest order. Preserve both,
        // including stable ties, without modifying the parsed package's registered filters.
        List<List<ResolveInfo>> cuts = new ArrayList<>();
        for (int i = 0; i < 4; i++) cuts.add(new ArrayList<>());
        for (ApkPackage pkg : mPackages.values()) {
            if (!matchesPackage(pkg, flags)
                    || (intent.getPackage() != null && !intent.getPackage().equals(pkg.packageName))) {
                continue;
            }
            PackageInfo metadata = pkg.packageInfo((flags & ~PackageManager.MATCH_SYSTEM_ONLY)
                    | PackageManager.GET_ACTIVITIES);
            metadata.applicationInfo.enabled = pkg.parsed.isEnabled();
            if (metadata.activities == null) continue;
            for (ParsedActivity parsed : pkg.parsed.getActivities()) {
                ActivityInfo activity = null;
                for (ActivityInfo info : metadata.activities) {
                    if (parsed.getName().equals(info.name)) { activity = info; break; }
                }
                if (activity == null || !matchesActivity(activity, flags)) continue;
                ParsedIntentInfo chosen = null;
                int chosenCut = 4;
                int chosenMatch = 0;
                for (ParsedIntentInfo info : parsed.getIntents()) {
                    IntentFilter filter = info.getIntentFilter();
                    int cut = packageQuery ? 0 : filterCut(filter, intent, type);
                    if (cut >= chosenCut || ((flags & PackageManager.MATCH_DEFAULT_ONLY) != 0
                            && !filter.hasCategory(Intent.CATEGORY_DEFAULT))) continue;
                    int match = filter.match(intent.getAction(), type, intent.getScheme(),
                            intent.getData(), intent.getCategories(), "gesher-activity-options");
                    if (match < 0) continue;
                    chosen = info;
                    chosenCut = cut;
                    chosenMatch = match;
                }
                if (chosen != null) cuts.get(chosenCut).add(result(pkg, activity, chosen,
                        chosenMatch, flags));
            }
        }
        if (mHostBrowser) {
            // The Binder caller supplies the resolved MIME type. The general browser helper
            // otherwise falls back to Intent.getType(), unlike Android's resolver contract.
            Intent browserIntent = Objects.equals(type, intent.getType()) ? intent
                    : new Intent(intent).setDataAndType(intent.getData(), type);
            for (ResolveInfo browser : BrowserPackageSupport.queryIntentActivities(browserIntent, type, flags)) {
                if (!matchesActivity(browser.activityInfo, flags)) continue;
                browser.system = true;
                browser.userHandle = UserHandle.SYSTEM;
                browser.handleAllWebDataURI = true;
                cuts.get(packageQuery ? 0 : 3).add(browser);
            }
        }
        for (List<ResolveInfo> cut : cuts) results.addAll(cut);
        results.sort(ORDER);
        return results;
    }

    private static ResolveInfo result(ApkPackage pkg, ActivityInfo activity,
            ParsedIntentInfo info, int match, long flags) {
        IntentFilter filter = info.getIntentFilter();
        ResolveInfo result = new ResolveInfo(filter.hasCategory(Intent.CATEGORY_BROWSABLE));
        result.activityInfo = activity;
        int priority = filter.getPriority();
        if (priority > 0 && (!pkg.isSystemAddon() || filter.hasAction(Intent.ACTION_VIEW)
                || filter.hasAction(Intent.ACTION_SEND) || filter.hasAction(Intent.ACTION_SENDTO)
                || filter.hasAction(Intent.ACTION_SEND_MULTIPLE))) priority = 0;
        if ((flags & PackageManager.GET_RESOLVED_FILTER) != 0) {
            result.filter = copyFilter(filter);
            result.filter.setPriority(priority);
        }
        result.priority = priority;
        result.match = match;
        result.isDefault = filter.hasCategory(Intent.CATEGORY_DEFAULT);
        result.labelRes = info.getLabelRes();
        result.nonLocalizedLabel = info.getNonLocalizedLabel();
        result.icon = info.getIcon();
        result.iconResourceId = info.getIcon();
        result.handleAllWebDataURI = filter.handleAllWebDataURI();
        result.system = pkg.isSystemAddon();
        result.userHandle = UserHandle.SYSTEM;
        return result;
    }

    private static IntentFilter copyFilter(IntentFilter source) {
        IntentFilter copy = new IntentFilter(source);
        // IntentFilter's copy constructor retains mutable relative groups and arrays in its
        // extras. Local callers must not acquire write access to the installed manifest.
        if (!source.getExtras().isEmpty()) copy.setExtras(source.getExtras().deepCopy());
        copy.clearUriRelativeFilterGroups();
        for (int i = 0; i < source.countUriRelativeFilterGroups(); i++) {
            UriRelativeFilterGroup original = source.getUriRelativeFilterGroup(i);
            UriRelativeFilterGroup group = new UriRelativeFilterGroup(original.getAction());
            for (UriRelativeFilter filter : original.getUriRelativeFilters()) {
                group.addUriRelativeFilter(filter); // UriRelativeFilter contains final scalar/string fields.
            }
            copy.addUriRelativeFilterGroup(group);
        }
        return copy;
    }

    /** The first lookup cut that would contain this filter; 4 means no lookup candidate. */
    private static int filterCut(IntentFilter filter, Intent intent, String type) {
        int cut = 4;
        if (type != null) {
            int slash = type.indexOf('/');
            if (slash > 0) {
                String base = type.substring(0, slash);
                if ("*".equals(base)) {
                    if (intent.getAction() != null && filter.hasAction(intent.getAction())
                            && filter.countDataTypes() > 0) cut = 0;
                } else {
                    boolean wildcard = type.length() == slash + 2 && type.charAt(slash + 1) == '*';
                    for (int i = 0; i < filter.countDataTypes(); i++) {
                        String registered = filter.getDataType(i);
                        int candidate = registered.equals("*") ? 2 : registered.equals(base) ? 1
                                : (wildcard ? registered.startsWith(base + "/")
                                        : registered.equals(type)) ? 0 : 4;
                        cut = Math.min(cut, candidate);
                    }
                }
            }
        } else if (intent.getScheme() == null && intent.getAction() != null
                && filter.countDataTypes() == 0 && filter.countDataSchemes() == 0
                && filter.hasAction(intent.getAction())) {
            cut = 0;
        }
        if (intent.getScheme() != null && filter.hasDataScheme(intent.getScheme())) cut = Math.min(cut, 3);
        return cut;
    }

    private static final Comparator<ResolveInfo> ORDER = (left, right) -> {
        int order = Integer.compare(right.priority, left.priority);
        if (order == 0) order = Integer.compare(right.preferredOrder, left.preferredOrder);
        if (order == 0) order = Boolean.compare(right.isDefault, left.isDefault);
        if (order == 0) order = Integer.compare(right.match, left.match);
        if (order == 0) order = Boolean.compare(right.system, left.system);
        if (order == 0) order = left.activityInfo.packageName.compareTo(right.activityInfo.packageName);
        return order;
    };
}
