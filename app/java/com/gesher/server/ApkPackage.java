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
import android.content.pm.ComponentInfo;
import android.content.pm.PackageInfo;
import android.content.pm.PackageManager;
import android.content.pm.ProviderInfo;
import android.content.pm.ResolveInfo;
import android.content.pm.ServiceInfo;
import android.os.Process;

import com.android.internal.pm.parsing.PackageInfoCommonUtils;
import com.android.internal.pm.parsing.PackageParser2;
import com.android.internal.pm.parsing.pkg.ParsedPackage;
import com.android.internal.pm.pkg.component.ParsedActivity;
import com.android.internal.pm.pkg.component.ParsedComponent;
import com.android.internal.pm.pkg.component.ParsedIntentInfo;

import java.io.File;
import java.io.IOException;
import java.util.ArrayList;
import java.util.Collections;
import java.util.List;
import java.util.Set;

/**
 * An installed APK, parsed the way PackageManagerService parses one at install: with
 * framework.jar's {@link PackageParser2} (ParsingPackageUtils: the whole manifest, with
 * services, receivers, providers, permissions, meta-data and intent filters). Its
 * {@link PackageInfo}, {@link ApplicationInfo} and {@link ActivityInfo}s come from
 * {@link PackageInfoCommonUtils}, the framework's PackageInfoUtils for packages outside
 * PackageManagerService, plus what installing adds (paths, uid, data directories,
 * enabled). Certificates are not collected (gesher does not verify signatures).
 */
public final class ApkPackage {
    /** Flags for a PackageInfo with everything: every component, meta-data, permissions. */
    public static final long ALL = PackageManager.GET_ACTIVITIES
            | PackageManager.GET_RECEIVERS | PackageManager.GET_SERVICES
            | PackageManager.GET_PROVIDERS | PackageManager.GET_META_DATA
            | PackageManager.GET_CONFIGURATIONS | PackageManager.GET_PERMISSIONS
            | PackageManager.GET_SHARED_LIBRARY_FILES;

    /** Both kinds of components, as PackageManagerService adds when neither is asked for. */
    private static final long DIRECT_BOOT = PackageManager.MATCH_DIRECT_BOOT_AWARE
            | PackageManager.MATCH_DIRECT_BOOT_UNAWARE;

    public final String apkPath;
    public final ParsedPackage parsed;
    public final String packageName;
    public final SharedLibraryGraph sharedLibraries;
    private final List<String> mLauncherActivities = new ArrayList<>();
    private long mInstallTime = System.currentTimeMillis();
    private long mUpdateTime = mInstallTime;
    private String mDataDir;
    private String mNativeLibraryDir;  // null: <data dir>/lib
    private String mPrimaryCpuAbi;     // null: no native code (NativeLibs.abiFor)
    private final boolean mHasCode;

    private ApkPackage(String apkPath, ParsedPackage parsed) throws IOException {
        this.apkPath = apkPath;
        this.parsed = parsed;
        this.packageName = parsed.getPackageName();
        // Resolve the newly parsed APK, including before an install replaces the old one.
        sharedLibraries = SharedLibraryRegistry.resolve(parsed);
        mHasCode = parsed.isDeclaredHavingCode() && hasDex(apkPath);
        for (ParsedActivity a : parsed.getActivities()) {
            for (ParsedIntentInfo info : a.getIntents()) {
                IntentFilter filter = info.getIntentFilter();
                if (filter.hasAction(Intent.ACTION_MAIN)
                        && filter.hasCategory(Intent.CATEGORY_LAUNCHER)) {
                    mLauncherActivities.add(a.getName());
                    break;
                }
            }
        }
        setDataDir(env("ANDROID_DATA", "/data") + "/data/" + packageName);
    }

    /** PackageParser2's callback outside system_server: no compat change is enabled. */
    private static final class ParserCallback extends PackageParser2.Callback {
        @Override
        public boolean isChangeEnabled(long changeId, ApplicationInfo appInfo) {
            return false;
        }

        @Override
        public boolean hasFeature(String feature) {
            return false;
        }

        @Override
        public Set<String> getHiddenApiWhitelistedApps() {
            return Collections.emptySet();
        }

        @Override
        public Set<String> getInstallConstraintsAllowlist() {
            return Collections.emptySet();
        }
    }

    /** Parses {@code apkPath} with PackageParser2. */
    public static ApkPackage parse(String apkPath) throws Exception {
        ParsedPackage pkg;
        try (PackageParser2 parser = new PackageParser2(null /* separateProcesses */,
                null /* displayMetrics: the defaults */, null /* cacher */,
                new ParserCallback())) {
            pkg = parser.parsePackage(new File(apkPath), 0 /* flags */, false /* useCaches */);
        }
        return new ApkPackage(apkPath, pkg);
    }

    /** One line about what was parsed, for logs (and the tests' checks). */
    public String describe() {
        int activities = 0;
        for (ParsedActivity a : parsed.getActivities()) {
            // The parser adds the system's AppDetailsActivity to every package.
            if (!PackageManager.APP_DETAILS_ACTIVITY_CLASS_NAME.equals(a.getName())) {
                activities++;
            }
        }
        return packageName + " parsed by PackageParser2: "
                + activities + " activities, "
                + parsed.getServices().size() + " services, "
                + parsed.getReceivers().size() + " receivers, "
                + parsed.getProviders().size() + " providers, targetSdk "
                + parsed.getTargetSdkVersion();
    }

    /** The app's data directory (default {@code $ANDROID_DATA/data/<package>}). */
    public void setDataDir(String dataDir) {
        mDataDir = dataDir;
    }

    public String dataDir() {
        return mDataDir;
    }

    /**
     * Where the package's native libraries are (an installed package's
     * {@code <code path>/lib/x86_64}); by default {@code <data dir>/lib}.
     */
    public void setNativeLibraryDir(String dir) {
        mNativeLibraryDir = dir;
    }

    /** The ABI of the package's native code (x86_64, arm64-v8a), or null if it has none. */
    public void setPrimaryCpuAbi(String abi) {
        mPrimaryCpuAbi = abi;
    }

    public String nativeLibraryDir() {
        return mNativeLibraryDir != null ? mNativeLibraryDir : mDataDir + "/lib";
    }

    /** When it was first installed and last updated (PackageInfo's times). */
    public void setInstallTimes(long firstInstallTime, long lastUpdateTime) {
        mInstallTime = firstInstallTime;
        mUpdateTime = lastUpdateTime;
    }

    public long firstInstallTime() {
        return mInstallTime;
    }

    public long versionCode() {
        return parsed.getLongVersionCode();
    }

    /** The PackageInfo PackageManager.getPackageInfo answers for these flags. */
    public PackageInfo packageInfo(long flags) {
        if ((flags & DIRECT_BOOT) == 0) {
            flags |= DIRECT_BOOT;
        }
        PackageInfo pi = PackageInfoCommonUtils.generate(parsed, flags, 0 /* userId */);
        installed(pi.applicationInfo);
        for (ComponentInfo[] components : new ComponentInfo[][] {pi.activities,
                pi.receivers, pi.services, pi.providers}) {
            if (components == null) continue;
            for (ComponentInfo c : components) {
                component(c, pi.applicationInfo);
            }
        }
        pi.firstInstallTime = mInstallTime;
        pi.lastUpdateTime = mUpdateTime;
        return pi;
    }

    public ApplicationInfo applicationInfo(long flags) {
        return packageInfo(flags & ~(PackageManager.GET_ACTIVITIES
                | PackageManager.GET_RECEIVERS | PackageManager.GET_SERVICES
                | PackageManager.GET_PROVIDERS | PackageManager.GET_PERMISSIONS
                | PackageManager.GET_CONFIGURATIONS)).applicationInfo;
    }

    /** The activity named {@code className}, or null. */
    public ActivityInfo activityInfo(String className, long flags) {
        PackageInfo pi = packageInfo(flags | PackageManager.GET_ACTIVITIES);
        if (pi.activities != null) {
            for (ActivityInfo a : pi.activities) {
                if (a.name.equals(className)) {
                    return a;
                }
            }
        }
        return null;
    }

    /** The Activity to launch: {@code requested}, or the first MAIN/LAUNCHER one. */
    public ActivityInfo launcherActivity(String requested) {
        String name = requested != null ? qualify(requested)
                : !mLauncherActivities.isEmpty() ? mLauncherActivities.get(0)
                : null;
        if (name == null) {
            throw new IllegalStateException(apkPath + " has no MAIN/LAUNCHER activity;"
                    + " name one with --activity");
        }
        ActivityInfo a = activityInfo(name, 0);
        if (a == null) {
            throw new IllegalStateException(apkPath + " declares no activity " + name);
        }
        return a;
    }

    private String qualify(String name) {
        if (name.startsWith(".")) {
            return packageName + name;
        }
        if (name.indexOf('.') < 0) {
            return packageName + "." + name;
        }
        return name;
    }

    /**
     * The activities whose intent filters match {@code intent}, as
     * PackageManager.queryIntentActivities answers (an explicit component matches itself).
     */
    public List<ResolveInfo> queryIntentActivities(Intent intent, String resolvedType,
            long flags) {
        List<ResolveInfo> result = new ArrayList<>();
        if (intent.getPackage() != null && !intent.getPackage().equals(packageName)) {
            return result;
        }
        ComponentName component = intent.getComponent();
        if (component != null) {
            if (component.getPackageName().equals(packageName)) {
                ActivityInfo a = activityInfo(component.getClassName(), flags);
                if (a != null) {
                    ResolveInfo ri = new ResolveInfo();
                    ri.activityInfo = a;
                    result.add(ri);
                }
            }
            return result;
        }
        for (Match m : match(parsed.getActivities(), intent, resolvedType, flags)) {
            ActivityInfo a = activityInfo(m.name, flags);
            if (a != null) result.add(m.resolveInfo(flags, a, null));
        }
        return result;
    }

    /**
     * The services whose intent filters match {@code intent} (an explicit component matches
     * itself), as PackageManager.queryIntentServices answers.
     */
    public List<ResolveInfo> queryIntentServices(Intent intent, String resolvedType,
            long flags) {
        List<ResolveInfo> result = new ArrayList<>();
        if (intent.getPackage() != null && !intent.getPackage().equals(packageName)) {
            return result;
        }
        ComponentName component = intent.getComponent();
        if (component != null) {
            ServiceInfo s = component.getPackageName().equals(packageName)
                    ? serviceInfo(component.getClassName(), flags) : null;
            if (s != null) {
                ResolveInfo ri = new ResolveInfo();
                ri.serviceInfo = s;
                result.add(ri);
            }
            return result;
        }
        for (Match m : match(parsed.getServices(), intent, resolvedType, flags)) {
            ServiceInfo s = serviceInfo(m.name, flags);
            if (s != null) result.add(m.resolveInfo(flags, null, s));
        }
        return result;
    }

    /**
     * The manifest receivers {@code intent} reaches: the explicit component, or the ones
     * whose intent filters match it (PackageManager.queryBroadcastReceivers).
     */
    public List<ResolveInfo> queryIntentReceivers(Intent intent, String resolvedType,
            long flags) {
        List<ResolveInfo> result = new ArrayList<>();
        if (intent.getPackage() != null && !intent.getPackage().equals(packageName)) {
            return result;
        }
        ComponentName component = intent.getComponent();
        if (component != null) {
            ActivityInfo a = component.getPackageName().equals(packageName)
                    ? receiverInfo(component.getClassName(), flags) : null;
            if (a != null) {
                ResolveInfo ri = new ResolveInfo();
                ri.activityInfo = a;
                result.add(ri);
            }
            return result;
        }
        for (Match m : match(parsed.getReceivers(), intent, resolvedType, flags)) {
            ActivityInfo a = receiverInfo(m.name, flags);
            if (a != null) result.add(m.resolveInfo(flags, a, null));
        }
        return result;
    }

    /** The service named {@code className}, or null. */
    public ServiceInfo serviceInfo(String className, long flags) {
        PackageInfo pi = packageInfo(flags | PackageManager.GET_SERVICES);
        if (pi.services != null) {
            for (ServiceInfo s : pi.services) {
                if (s.name.equals(className)) return s;
            }
        }
        return null;
    }

    /** The manifest receiver named {@code className}, or null. */
    public ActivityInfo receiverInfo(String className, long flags) {
        PackageInfo pi = packageInfo(flags | PackageManager.GET_RECEIVERS);
        if (pi.receivers != null) {
            for (ActivityInfo a : pi.receivers) {
                if (a.name.equals(className)) return a;
            }
        }
        return null;
    }

    /** The content providers the manifest declares. */
    public List<ProviderInfo> providers(long flags) {
        PackageInfo pi = packageInfo(flags | PackageManager.GET_PROVIDERS);
        List<ProviderInfo> result = new ArrayList<>();
        if (pi.providers != null) Collections.addAll(result, pi.providers);
        return result;
    }

    /** A component whose intent filter matched, and how. */
    private static final class Match {
        final String name;
        final IntentFilter filter;
        final ParsedIntentInfo info;
        final int match;

        Match(String name, IntentFilter filter, ParsedIntentInfo info, int match) {
            this.name = name;
            this.filter = filter;
            this.info = info;
            this.match = match;
        }

        ResolveInfo resolveInfo(long flags, ActivityInfo activity, ServiceInfo service) {
            ResolveInfo ri = new ResolveInfo();
            ri.activityInfo = activity;
            ri.serviceInfo = service;
            if ((flags & PackageManager.GET_RESOLVED_FILTER) != 0) {
                ri.filter = new IntentFilter(filter);
            }
            ri.match = match;
            ri.priority = filter.getPriority();
            ri.isDefault = filter.hasCategory(Intent.CATEGORY_DEFAULT);
            ri.labelRes = info.getLabelRes();
            ri.icon = info.getIcon();
            return ri;
        }
    }

    /**
     * The components whose intent filters match {@code intent}, the first matching filter of
     * each, as PackageManagerService's IntentResolver matches them ({@code
     * MATCH_DEFAULT_ONLY}: only filters with CATEGORY_DEFAULT).
     */
    private static List<Match> match(List<? extends ParsedComponent> components,
            Intent intent, String resolvedType, long flags) {
        List<Match> result = new ArrayList<>();
        String type = resolvedType != null ? resolvedType : intent.getType();
        boolean defaultOnly = (flags & PackageManager.MATCH_DEFAULT_ONLY) != 0;
        for (ParsedComponent c : components) {
            for (ParsedIntentInfo info : c.getIntents()) {
                IntentFilter filter = info.getIntentFilter();
                if (defaultOnly && !filter.hasCategory(Intent.CATEGORY_DEFAULT)) continue;
                int match = filter.match(intent.getAction(), type, intent.getScheme(),
                        intent.getData(), intent.getCategories(), "gesher");
                if (match < 0) continue;
                result.add(new Match(c.getName(), filter, info, match));
                break;
            }
        }
        return result;
    }

    /** What installing gives a package beyond its manifest: paths, uid, data, enabled. */
    private void installed(ApplicationInfo ai) {
        ai.sourceDir = apkPath;
        ai.publicSourceDir = apkPath;
        if (ai.processName == null) {
            ai.processName = packageName;
        }
        ai.uid = Process.myUid();  // every gesher process has the same uid
        ai.enabled = true;
        ai.flags |= ApplicationInfo.FLAG_INSTALLED;
        if (mHasCode) {
            // As installed packages carry it: LoadedApk loads code only with it.
            ai.flags |= ApplicationInfo.FLAG_HAS_CODE;
        }
        if (parsed.isRtlSupported()) {
            // android:supportsRtl (rtl track): the flag PackageImpl derives in hideAsFinal,
            // which this parse never reaches; without it views ignore RTL layouts.
            ai.flags |= ApplicationInfo.FLAG_SUPPORTS_RTL;
        }
        ai.dataDir = mDataDir;
        ai.credentialProtectedDataDir = mDataDir;
        ai.deviceProtectedDataDir = mDataDir;
        ai.nativeLibraryDir = nativeLibraryDir();
        ai.primaryCpuAbi = mPrimaryCpuAbi;
        // A long-lived PM record must not publish a cached graph after XML/jar updates.
        // Ordinary packages keep the cheap immutable empty graph path.
        try {
            SharedLibraryGraph graph = parsed.getUsesLibraries().isEmpty()
                    && parsed.getUsesOptionalLibraries().isEmpty() ? sharedLibraries
                    : SharedLibraryRegistry.resolve(parsed);
            graph.apply(ai);
        } catch (IOException invalidLibrary) {
            throw new IllegalStateException("shared libraries changed for " + packageName, invalidLibrary);
        }
    }

    private static void component(ComponentInfo c, ApplicationInfo ai) {
        c.applicationInfo = ai;
        c.packageName = ai.packageName;
        if (c.processName == null) {
            c.processName = ai.processName;
        }
    }

    /** Whether the APK carries code: a classes.dex. */
    private static boolean hasDex(String apkPath) {
        try (java.util.zip.ZipFile zip = new java.util.zip.ZipFile(apkPath)) {
            return zip.getEntry("classes.dex") != null;
        } catch (java.io.IOException e) {
            return false;
        }
    }

    private static String env(String name, String fallback) {
        String v = System.getenv(name);
        return v == null || v.isEmpty() ? fallback : v;
    }
}
