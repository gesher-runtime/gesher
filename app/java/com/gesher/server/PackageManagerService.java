/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.server;

import android.content.ComponentName;
import android.content.Intent;
import android.app.ActivityManager;
import android.net.Uri;
import android.os.Binder;
import android.os.RemoteException;
import android.content.pm.ActivityInfo;
import android.content.pm.ApplicationInfo;
import android.content.pm.IPackageManager;
import android.content.pm.PackageInfo;
import android.content.pm.PackageManager;
import android.content.pm.ParceledListSlice;
import android.content.pm.ProviderInfo;
import android.content.pm.PermissionGroupInfo;
import android.content.pm.Signature;
import android.content.pm.ResolveInfo;
import android.content.pm.ServiceInfo;
import android.content.pm.VersionedPackage;

import com.android.internal.pm.pkg.component.ParsedComponent;

import java.util.ArrayList;
import java.util.Collection;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.Objects;
import java.util.concurrent.Executor;
import java.util.concurrent.Executors;
import java.util.function.Consumer;

/**
 * {@code package}: PackageManagerService, knowing the APKs it was given and the installed
 * packages ({@link Installer}; parsed with PackageParser2, {@link ApkPackage}). Each has its own UID in real microG mode. Packages come and go while it serves (install, uninstall): readers
 * see one version of the map, replaced as a whole.
 */
public final class PackageManagerService extends IPackageManager.Default {
    private volatile Map<String, ApkPackage> mPackages = new LinkedHashMap<>();
    private final PermissionPolicy mPermissions = new PermissionPolicy(this);
    private final Executor mPackageEventExecutor = Executors.newSingleThreadExecutor(task -> {
        Thread thread = new Thread(task, "gesher-package-events");
        thread.setDaemon(true);
        return thread;
    });
    private Consumer<Intent> mPackageEventSender = PackageManagerService::broadcastPackageEvent;
    private final List<Consumer<String>> mPackageRemoval = new ArrayList<>();
    private final List<Consumer<String>> mPackageRemovalChecks = new ArrayList<>();
    private volatile Map<String, ApkPackage> mRemoving = new LinkedHashMap<>();

    boolean isRemovalPending(String name) { return mRemoving.containsKey(name); }

    synchronized void beginPackageRemoval(ApkPackage original) {
        if (original == null || mPackages.get(original.packageName) != original
                || mRemoving.containsKey(original.packageName)) throw new SecurityException("Package removal already pending or changed");
        Map<String, ApkPackage> next = new LinkedHashMap<>(mRemoving);
        next.put(original.packageName, original); mRemoving = next;
    }
    synchronized void endPackageRemoval(ApkPackage original) {
        if (mRemoving.get(original.packageName) != original) return;
        Map<String, ApkPackage> next = new LinkedHashMap<>(mRemoving);
        next.remove(original.packageName); mRemoving = next;
    }
    /** Installer holds its monitor and has restored every unresolved precommit journal. */
    synchronized void recoveredPackageRemovals() { mRemoving = new LinkedHashMap<>(); }

    /** Server-local capability cleanup; never callable through the package Binder. */
    synchronized void onPackageRemoval(Consumer<String> cleanup) { mPackageRemoval.add(cleanup); }

    synchronized void onPackageRemovalCheck(Consumer<String> check) { mPackageRemovalChecks.add(check); }

    void checkPackageRemoval(String name) {
        List<Consumer<String>> checks;
        synchronized (this) { checks = new ArrayList<>(mPackageRemovalChecks); }
        for (Consumer<String> check : checks) check.accept(name);
    }

    void revokePackageCapabilities(String name) {
        List<Consumer<String>> cleanup;
        synchronized (this) { cleanup = new ArrayList<>(mPackageRemoval); }
        for (Consumer<String> action : cleanup) action.accept(name);
    }

    public PermissionPolicy permissions() { return mPermissions; }

    @Override public String[] getSystemSharedLibraryNames() {
        try {
            return SharedLibraryRegistry.readSystem().names();
        } catch (java.io.IOException e) {
            throw new IllegalStateException("Cannot read system shared libraries", e);
        }
    }

    @Override public boolean isProtectedBroadcast(String action) {
        return mPermissions.isProtectedBroadcast(action);
    }

    private boolean synthetic(String name) {
        return GmsPackageSupport.isSyntheticPackage(name)
                && (!GmsPackageSupport.PACKAGE_VENDING.equals(name)
                        || get(GmsPackageSupport.PACKAGE_GMS) == null);
    }

    private PackageInfo installedInfo(ApkPackage pkg, long flags) {
        PackageInfo info = mPermissions.annotate(pkg.packageInfo(flags));
        // The compatibility parser can collect an unverified signing block. In real
        // microG mode no public signature query may turn that block into trust.
        if (GmsSignatureSpoofing.isEnabled()
                && (flags & (PackageManager.GET_SIGNATURES | PackageManager.GET_SIGNING_CERTIFICATES)) != 0
                && !pkg.hasVerifiedSigningIdentity()) {
            info.signatures = null;
            info.signingInfo = null;
        }
        return info;
    }

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
        PackageManager.invalidatePackageInfoCache();
    }

    /** Removes a package (an uninstall); returns it, or null. */
    synchronized ApkPackage remove(String packageName) {
        Map<String, ApkPackage> map = new LinkedHashMap<>(mPackages);
        ApkPackage p = map.remove(packageName);
        mPackages = map;
        if (p != null) PackageManager.invalidatePackageInfoCache();
        return p;
    }

    /** A completed durable publication, as distinct from boot scanning or an unchanged APK. */
    void packageInstalled(ApkPackage pkg, boolean replacing, Object publicationLock) {
        final String name = pkg.packageName;
        final int uid = pkg.uid();
        final String codePath = new java.io.File(pkg.apkPath).getParentFile().getPath();
        mPackageEventExecutor.execute(() -> {
            // A session also holds the installer monitor around its final consent/code
            // checks. Do not call application Binders until that outer lock is released.
            synchronized (publicationLock) { }
            try {
                com.gesher.app.shell.AppShell.registerQuietly(name, codePath, replacing);
                com.gesher.app.shell.AppShell.registerUrlProtocolsQuietly(name, codePath, replacing);
            } catch (Throwable t) {
                // Ignore shell registration failures
            }
            Intent added = new Intent(Intent.ACTION_PACKAGE_ADDED, Uri.fromParts("package", name, null))
                    .putExtra(Intent.EXTRA_UID, uid)
                    .putExtra(Intent.EXTRA_REPLACING, replacing);
            sendPackageEvent(added);
            if (replacing) {
                sendPackageEvent(new Intent(added).setAction(Intent.ACTION_PACKAGE_REPLACED));
                sendPackageEvent(new Intent(Intent.ACTION_MY_PACKAGE_REPLACED).setPackage(name));
            }
        });
    }

    /** Only after durable removal, with all old client capabilities revoked. */
    void packageRemoved(ApkPackage pkg, Object publicationLock) {
        final String name = pkg.packageName;
        final int uid = pkg.uid();
        mPackageEventExecutor.execute(() -> {
            synchronized (publicationLock) { }
            Intent removed = new Intent(Intent.ACTION_PACKAGE_REMOVED, Uri.fromParts("package", name, null))
                    .putExtra(Intent.EXTRA_UID, uid).putExtra(Intent.EXTRA_REPLACING, false)
                    .putExtra(Intent.EXTRA_DATA_REMOVED, true);
            sendPackageEvent(removed);
            sendPackageEvent(new Intent(removed).setAction(Intent.ACTION_PACKAGE_FULLY_REMOVED));
            // PackageUids retains a tombstone for this name. No UID_REMOVED claim is made.
        });
    }

    private void sendPackageEvent(Intent intent) {
        try { mPackageEventSender.accept(intent); }
        catch (RuntimeException failure) {
            SystemServer.log("package event unavailable: " + failure.getClass().getSimpleName());
        }
    }

    private static void broadcastPackageEvent(Intent intent) {
        long identity = Binder.clearCallingIdentity();
        try {
            ActivityManager.getService().broadcastIntentWithFeature(null, null, intent, null,
                    null, 0, null, null, null, null, null, -1, null, false, false, 0);
        } catch (RemoteException failure) {
            throw new IllegalStateException("Package event transport unavailable", failure);
        } finally { Binder.restoreCallingIdentity(identity); }
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
        if (userId != 0) return false;
        if ("android".equals(packageName)) return true;
        return get(packageName) != null || synthetic(packageName)
                || BrowserPackageSupport.isBrowserPackage(packageName);
    }

    @Override
    public PackageInfo getPackageInfo(String packageName, long flags, int userId) {
        if (userId != 0) return null;
        if ("android".equals(packageName)) {
            PackageInfo info = new PackageInfo();
            info.packageName = "android";
            info.applicationInfo = platformApplication();
            info.setLongVersionCode(android.os.Build.VERSION.SDK_INT);
            return info;
        }
        ApkPackage p = get(packageName);
        if (p != null) {
            return installedInfo(p, flags);
        }
        if (synthetic(packageName)) {
            return GmsPackageSupport.createSyntheticPackageInfo(packageName, flags);
        }
        if (BrowserPackageSupport.isBrowserPackage(packageName)) {
            return BrowserPackageSupport.createPackageInfo(flags);
        }
        return null;
    }

    @Override
    public PackageInfo getPackageInfoVersioned(VersionedPackage versionedPackage,
            long flags, int userId) {
        Objects.requireNonNull(versionedPackage);
        // AOSP uses the version to choose between static shared-library providers.
        // For ordinary packages (the single installed version Gesher publishes), it
        // resolves the package name exactly as the String overload does. Returning
        // Default's null makes ApplicationPackageManager throw NameNotFoundException
        // even when an application asks about its own installed package.
        return getPackageInfo(versionedPackage.getPackageName(), flags, userId);
    }

    @Override
    public String[] currentToCanonicalPackageNames(String[] names) {
        // There is no installed-package rename registry. Unknown and unchanged names
        // retain their positions, as they do on Android; the answer is not null.
        return names.clone();
    }

    @Override
    public String[] canonicalToCurrentPackageNames(String[] names) {
        return names.clone();
    }

    @Override
    public ApplicationInfo getApplicationInfo(String packageName, long flags, int userId) {
        if (userId != 0) return null;
        if ("android".equals(packageName)) return platformApplication();
        ApkPackage p = get(packageName);
        if (p != null) {
            return p.applicationInfo(flags);
        }
        if (synthetic(packageName)) {
            return GmsPackageSupport.createSyntheticApplicationInfo(packageName, flags);
        }
        if (BrowserPackageSupport.isBrowserPackage(packageName)) {
            return BrowserPackageSupport.createApplicationInfo(flags);
        }
        return null;
    }

    private ApplicationInfo platformApplication() {
        ApplicationInfo info = new ApplicationInfo();
        String root = System.getenv("ANDROID_ROOT");
        info.packageName = "android";
        info.sourceDir = (root == null ? "/system" : root) + "/framework/framework-res.apk";
        info.publicSourceDir = info.sourceDir;
        info.uid = android.os.Process.SYSTEM_UID;
        info.flags = ApplicationInfo.FLAG_SYSTEM | ApplicationInfo.FLAG_INSTALLED;
        info.enabled = true;
        info.targetSdkVersion = android.os.Build.VERSION.SDK_INT;
        return info;
    }

    @Override
    public int getTargetSdkVersion(String packageName) {
        ApkPackage p = get(packageName);
        return p == null ? -1 : p.parsed.getTargetSdkVersion();
    }

    @Override
    public ActivityInfo getActivityInfo(ComponentName className, long flags, int userId) {
        if (isRemovalPending(className.getPackageName())) return null;
        ApkPackage p = get(className.getPackageName());
        if (p == null && BrowserPackageSupport.isBrowserPackage(className.getPackageName())) {
            return BrowserPackageSupport.activityInfo(className.getClassName(), flags);
        }
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

    @Override
    public ParceledListSlice queryIntentActivityOptions(ComponentName caller, Intent[] specifics,
            String[] specificTypes, Intent intent, String resolvedType, long flags, int userId) {
        if (userId != 0) return new ParceledListSlice<>(new ArrayList<ResolveInfo>());
        Map<String, ApkPackage> packages;
        boolean hostBrowser;
        synchronized (this) {
            hostBrowser = !mPackages.containsKey(BrowserPackageSupport.PACKAGE);
            packages = new LinkedHashMap<>(mPackages);
            for (String removing : mRemoving.keySet()) packages.remove(removing);
        }
        return new ParceledListSlice<>(new ActivityOptionsResolver(packages, hostBrowser).query(
                caller, specifics, specificTypes, intent, resolvedType, flags));
    }

    private List<ResolveInfo> query(Intent intent, String resolvedType, long flags) {
        List<ResolveInfo> result = new ArrayList<>();
        for (ApkPackage p : mPackages.values()) {
            if (isRemovalPending(p.packageName)) continue;
            result.addAll(p.queryIntentActivities(intent, resolvedType, flags));
        }
        // The Windows default browser (BrowserPackageSupport), after the installed apps'
        // activities: an app's own http(s) filter still resolves first.
        if (intent != null && get(BrowserPackageSupport.PACKAGE) == null) {
            result.addAll(BrowserPackageSupport.queryIntentActivities(intent, resolvedType,
                    flags));
        }
        return result;
    }

    /** The services {@code intent} resolves to, in every package. */
    List<ResolveInfo> services(Intent intent, String resolvedType, long flags) {
        List<ResolveInfo> result = new ArrayList<>();
        for (ApkPackage p : mPackages.values()) {
            if (isRemovalPending(p.packageName)) continue;
            result.addAll(p.queryIntentServices(intent, resolvedType, flags));
        }
        if (result.isEmpty() && intent != null && get(GmsPackageSupport.PACKAGE_GMS) == null) {
            result.addAll(GmsPackageSupport.querySyntheticServices(intent, resolvedType, flags));
        }
        return result;
    }

    /** The manifest receivers {@code intent} reaches, in every package. */
    List<ResolveInfo> receivers(Intent intent, String resolvedType, long flags) {
        List<ResolveInfo> result = new ArrayList<>();
        for (ApkPackage p : mPackages.values()) {
            if (isRemovalPending(p.packageName)) continue;
            result.addAll(p.queryIntentReceivers(intent, resolvedType, flags));
        }
        return result;
    }

    /** The provider with authority {@code authority} (one of its authorities), or null. */
    ProviderInfo provider(String authority, long flags) {
        for (ApkPackage p : mPackages.values()) {
            if (isRemovalPending(p.packageName)) continue;
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
        if (isRemovalPending(className.getPackageName())) return null;
        ApkPackage p = get(className.getPackageName());
        return p == null ? null : p.serviceInfo(className.getClassName(), flags);
    }

    @Override
    public ActivityInfo getReceiverInfo(ComponentName className, long flags, int userId) {
        if (isRemovalPending(className.getPackageName())) return null;
        ApkPackage p = get(className.getPackageName());
        return p == null ? null : p.receiverInfo(className.getClassName(), flags);
    }

    @Override
    public ProviderInfo resolveContentProvider(String name, long flags, int userId) {
        return provider(name, flags);
    }

    @Override
    public ProviderInfo getProviderInfo(ComponentName className, long flags, int userId) {
        if (isRemovalPending(className.getPackageName())) return null;
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
            result.add(installedInfo(p, flags));
        }
        if (get(GmsPackageSupport.PACKAGE_GMS) == null) {
            result.add(GmsPackageSupport.createSyntheticPackageInfo(GmsPackageSupport.PACKAGE_GMS, flags));
        }
        if (get(GmsPackageSupport.PACKAGE_VENDING) == null
                && get(GmsPackageSupport.PACKAGE_GMS) == null) {
            result.add(GmsPackageSupport.createSyntheticPackageInfo(GmsPackageSupport.PACKAGE_VENDING, flags));
        }
        return new ParceledListSlice<>(result);
    }

    @Override
    public ParceledListSlice<PackageInfo> getPackagesHoldingPermissions(String[] permissions,
            long flags, int userId) {
        Objects.requireNonNull(permissions);
        List<PackageInfo> result = new ArrayList<>();
        if (userId != 0) return new ParceledListSlice<>(result);
        for (ApkPackage pkg : mPackages.values()) {
            if (isRemovalPending(pkg.packageName)) continue;
            if ((flags & PackageManager.MATCH_SYSTEM_ONLY) != 0 && !pkg.isSystemAddon()) continue;
            List<String> held = new ArrayList<>();
            for (String permission : permissions) {
                if (mPermissions.granted(permission, pkg.packageName)) held.add(permission);
            }
            if (held.isEmpty()) continue;
            PackageInfo info = installedInfo(pkg, flags);
            // Without GET_PERMISSIONS Android returns only the matching requested
            // names, preserving the query's order/duplicates. With it, keep the full
            // manifest permission inventory and the current grant flags instead.
            if ((flags & PackageManager.GET_PERMISSIONS) == 0) {
                info.requestedPermissions = held.toArray(new String[0]);
            }
            result.add(info);
        }
        return new ParceledListSlice<>(result);
    }

    @Override
    public ParceledListSlice getInstalledApplications(long flags, int userId) {
        List<ApplicationInfo> result = new ArrayList<>();
        for (ApkPackage p : mPackages.values()) {
            result.add(p.applicationInfo(flags));
        }
        if (get(GmsPackageSupport.PACKAGE_GMS) == null) {
            result.add(GmsPackageSupport.createSyntheticApplicationInfo(GmsPackageSupport.PACKAGE_GMS, flags));
        }
        if (get(GmsPackageSupport.PACKAGE_VENDING) == null
                && get(GmsPackageSupport.PACKAGE_GMS) == null) {
            result.add(GmsPackageSupport.createSyntheticApplicationInfo(GmsPackageSupport.PACKAGE_VENDING, flags));
        }
        return new ParceledListSlice<>(result);
    }

    @Override
    public int getComponentEnabledSetting(ComponentName componentName, int userId) {
        return PackageManager.COMPONENT_ENABLED_STATE_DEFAULT;
    }

    @Override
    public int getApplicationEnabledSetting(String packageName, int userId) {
        if (synthetic(packageName)
                || BrowserPackageSupport.isBrowserPackage(packageName)) {
            return PackageManager.COMPONENT_ENABLED_STATE_ENABLED;
        }
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
        if (uid == android.os.Process.SYSTEM_UID) return "android.uid.system";
        String[] names = getPackagesForUid(uid);
        return names == null || names.length == 0 ? null : names[0];
    }

    @Override
    public String[] getNamesForUids(int[] uids) {
        // AOSP distinguishes an absent input from positional unknown UID answers.
        if (uids == null || uids.length == 0) return null;
        String[] names = new String[uids.length];
        for (int i = 0; i < uids.length; i++) {
            names[i] = uids[i] == android.os.Process.SYSTEM_UID
                    ? "shared:android.uid.system" : getNameForUid(uids[i]);
        }
        return names;
    }

    @Override
    public String[] getPackagesForUid(int uid) {
        if (uid == android.os.Process.SYSTEM_UID) return new String[] {"android"};
        List<String> result = new ArrayList<>();
        for (ApkPackage pkg : mPackages.values()) if (pkg.uid() == uid) result.add(pkg.packageName);
        return result.isEmpty() ? null : result.toArray(new String[0]);
    }

    @Override
    public int getPackageUid(String name, long flags, int userId) {
        ApplicationInfo info = userId == 0 ? getApplicationInfo(name, flags, userId) : null;
        return info == null ? -1 : info.uid;
    }

    @Override
    public boolean[] canPackageQuery(String sourcePackageName, String[] targetPackageNames,
            int userId) {
        // Keep Android's positional result and nonexistent-user ordering, including
        // the null-array failure before looking up a user or package.
        boolean[] result = new boolean[targetPackageNames.length];
        if (userId != 0) return result;
        int callingUid = Binder.getCallingUid();
        if (android.os.UserHandle.getUserId(callingUid) != userId
                && callingUid != android.os.Process.ROOT_UID
                && callingUid != android.os.Process.SYSTEM_UID
                && mPermissions.checkProcessPermission("android.permission.INTERACT_ACROSS_USERS_FULL",
                        Binder.getCallingPid(), callingUid) != PackageManager.PERMISSION_GRANTED
                && mPermissions.checkProcessPermission("android.permission.INTERACT_ACROSS_USERS",
                        Binder.getCallingPid(), callingUid) != PackageManager.PERMISSION_GRANTED) {
            throw new SecurityException("canPackageQuery requires cross-user permission");
        }
        Map<String, ApkPackage> packages;
        boolean browser, gms, vending;
        synchronized (this) {
            packages = new LinkedHashMap<>(mPackages);
            for (String removing : mRemoving.keySet()) packages.remove(removing);
            browser = !mPackages.containsKey(BrowserPackageSupport.PACKAGE)
                    && !mRemoving.containsKey(BrowserPackageSupport.PACKAGE);
            gms = !mPackages.containsKey(GmsPackageSupport.PACKAGE_GMS)
                    && !mRemoving.containsKey(GmsPackageSupport.PACKAGE_GMS);
            vending = !mPackages.containsKey(GmsPackageSupport.PACKAGE_VENDING)
                    && !mPackages.containsKey(GmsPackageSupport.PACKAGE_GMS)
                    && !mRemoving.containsKey(GmsPackageSupport.PACKAGE_VENDING);
        }
        PackageVisibility visibility = new PackageVisibility(packages, browser, gms, vending,
                this::isProtectedBroadcast);
        boolean missing = !visibility.contains(sourcePackageName)
                || !visibility.canSee(callingUid, sourcePackageName);
        for (String target : targetPackageNames) {
            if (!missing && (!visibility.contains(target) || !visibility.canSee(callingUid, target))) {
                missing = true;
            }
        }
        if (missing) {
            throw new android.os.ParcelableException(new PackageManager.NameNotFoundException(
                    "Package(s) " + sourcePackageName + " and/or "
                            + java.util.Arrays.toString(targetPackageNames) + " not found."));
        }
        int sourceUid = visibility.uid(sourcePackageName);
        for (int i = 0; i < result.length; i++) {
            result[i] = visibility.canSee(sourceUid, targetPackageNames[i]);
        }
        return result;
    }

    @Override
    public int[] getPackageGids(String packageName, long flags, int userId) {
        // Gesher assigns package UIDs but no supplementary Linux groups. Null
        // means an unknown package to ApplicationPackageManager, not "no groups",
        // and makes even a query about the caller's own installed app throw.
        if (userId != 0 || isRemovalPending(packageName)) return null;
        if ("android".equals(packageName)) return new int[0];
        ApkPackage pkg = get(packageName);
        if (pkg == null || ((flags & PackageManager.MATCH_SYSTEM_ONLY) != 0 && !pkg.isSystemAddon())) {
            return null;
        }
        return new int[0];
    }

    @Override
    public int checkSignatures(String first, String second, int userId) {
        PackageInfo a = getPackageInfo(first, PackageManager.GET_SIGNATURES, userId);
        PackageInfo b = getPackageInfo(second, PackageManager.GET_SIGNATURES, userId);
        if (a == null || b == null) return PackageManager.SIGNATURE_UNKNOWN_PACKAGE;
        if (a.signatures == null) return b.signatures == null ? PackageManager.SIGNATURE_NEITHER_SIGNED : PackageManager.SIGNATURE_FIRST_NOT_SIGNED;
        if (b.signatures == null) return PackageManager.SIGNATURE_SECOND_NOT_SIGNED;
        java.util.Set<Signature> aa = new java.util.HashSet<>(java.util.Arrays.asList(a.signatures));
        java.util.Set<Signature> bb = new java.util.HashSet<>(java.util.Arrays.asList(b.signatures));
        return aa.equals(bb) ? PackageManager.SIGNATURE_MATCH : PackageManager.SIGNATURE_NO_MATCH;
    }

    @Override
    public int checkUidSignatures(int first, int second) {
        String[] a = getPackagesForUid(first), b = getPackagesForUid(second);
        if (a == null || b == null) return PackageManager.SIGNATURE_UNKNOWN_PACKAGE;
        if (first == second) return PackageManager.SIGNATURE_MATCH;
        for (String aa : a) for (String bb : b) {
            if (checkSignatures(aa, bb, 0) != PackageManager.SIGNATURE_MATCH) return PackageManager.SIGNATURE_NO_MATCH;
        }
        return PackageManager.SIGNATURE_MATCH;
    }

    @Override
    public boolean hasSigningCertificate(String name, byte[] certificate, int type) {
        if (certificate == null) return false;
        PackageInfo info = getPackageInfo(name, PackageManager.GET_SIGNING_CERTIFICATES, 0);
        if (info == null || info.signingInfo == null) return false;
        Signature[] signatures = info.signingInfo.hasMultipleSigners()
                ? info.signingInfo.getApkContentsSigners() : info.signingInfo.getSigningCertificateHistory();
        if (signatures == null) return false;
        for (Signature signature : signatures) {
            byte[] bytes = signature.toByteArray();
            if (type == PackageManager.CERT_INPUT_SHA256) {
                try { bytes = java.security.MessageDigest.getInstance("SHA-256").digest(bytes); }
                catch (java.security.NoSuchAlgorithmException e) { throw new AssertionError(e); }
            } else if (type != PackageManager.CERT_INPUT_RAW_X509) return false;
            if (java.util.Arrays.equals(bytes, certificate)) return true;
        }
        return false;
    }

    @Override public boolean hasUidSigningCertificate(int uid, byte[] certificate, int type) {
        String[] packages = getPackagesForUid(uid);
        if (packages != null) for (String pkg : packages) if (hasSigningCertificate(pkg, certificate, type)) return true;
        return false;
    }
    @Override public int checkPermission(String permission, String pkg, int userId) {
        return mPermissions.checkPermission(pkg, permission, "default:0", userId);
    }
    @Override public int checkUidPermission(String permission, int uid) {
        return mPermissions.checkUidPermission(uid, permission, 0);
    }
    @Override public void grantRuntimePermission(String pkg, String permission, int userId) {
        mPermissions.grantRuntimePermission(pkg, permission, "default:0", userId);
    }
    @Override public PermissionGroupInfo getPermissionGroupInfo(String name, int flags) {
        return mPermissions.getPermissionGroupInfo(name, flags);
    }

    @Override
    public String getInstallerPackageName(String packageName) {
        return null;
    }

    /**
     * Where an installed package came from: nowhere known (sideloaded through Gesher's
     * installer), but it is installed. IPackageManager.Default returns null, which
     * ApplicationPackageManager turns into NameNotFoundException for every package, so
     * code that asks about its own package (Flutter's package_info_plus, store checks)
     * failed.
     */
    @Override
    public android.content.pm.InstallSourceInfo getInstallSourceInfo(String packageName,
            int userId) {
        if (get(packageName) == null && !synthetic(packageName)
                && !BrowserPackageSupport.isBrowserPackage(packageName)) {
            return null;
        }
        return new android.content.pm.InstallSourceInfo(null, null, null, null);
    }

    private final PackageInstallerService mInstallerService = new PackageInstallerService(this);

    private java.io.File appMetadata(String packageName, int userId) {
        if (userId != 0) throw new SecurityException("Only user 0 is supported");
        if (mPermissions.checkProcessPermission("android.permission.GET_APP_METADATA",
                android.os.Binder.getCallingPid(), android.os.Binder.getCallingUid())
                != PackageManager.PERMISSION_GRANTED) throw new SecurityException("App metadata permission required");
        ApkPackage pkg = get(packageName);
        if (pkg == null) throw new android.os.ParcelableException(new PackageManager.NameNotFoundException());
        return new java.io.File(new java.io.File(pkg.apkPath).getParentFile(), InstallSessionMetadata.APP_METADATA);
    }

    @Override public android.os.ParcelFileDescriptor getAppMetadataFd(String packageName, int userId) {
        java.io.File file = appMetadata(packageName, userId);
        if (!file.isFile()) return null;
        try { return android.os.ParcelFileDescriptor.open(file, android.os.ParcelFileDescriptor.MODE_READ_ONLY); }
        catch (java.io.IOException e) { throw new IllegalStateException("App metadata unavailable", e); }
    }

    @Override public int getAppMetadataSource(String packageName, int userId) {
        return appMetadata(packageName, userId).isFile() ? 2 /* INSTALLER */ : 0 /* UNKNOWN */;
    }

    void attachInstaller(ActivityTaskManagerService atm, Installer installer) {
        mInstallerService.attach(atm, installer);
    }
    private final android.content.pm.IPackageInstaller mInstaller =
            StandIn.of(android.content.pm.IPackageInstaller.class, mInstallerService).asInterface();

    @Override
    public android.content.pm.IPackageInstaller getPackageInstaller() {
        return mInstaller;
    }

    @Override public void deletePackageAsUser(String packageName, int versionCode,
            android.content.pm.IPackageDeleteObserver observer, int userId, int flags) {
        mInstallerService.uninstallLegacy(packageName, versionCode, observer, userId, flags);
    }

    @Override
    public void notifyDexLoad(String loadingPackageName,
            java.util.Map<String, String> classLoaderContextMap, String loaderIsa) {
        // No dexopt to schedule.
    }

    @Override
    public void registerDexModule(String packageName, String dexModulePath,
            boolean isSharedModule, android.content.pm.IDexModuleRegisterCallback callback) {
        // Android 16 deliberately does not register/optimize these modules, but
        // completes the optional callback with failure. An inherited no-op leaves
        // callers waiting forever. The server's existing worker supplies the
        // asynchronous boundary, without retaining a client Binder identity or
        // holding the package monitor while application code runs.
        if (callback == null) return;
        mPackageEventExecutor.execute(() -> {
            try {
                callback.onDexModuleRegistered(dexModulePath, false,
                        "registerDexModule call not supported since Android U");
            } catch (RemoteException | RuntimeException failure) {
                SystemServer.log("dex module callback unavailable: "
                        + failure.getClass().getSimpleName());
            }
        });
    }

    @Override
    public PackageManager.Property getPropertyAsUser(String propertyName,
            String packageName, String className, int userId) {
        Objects.requireNonNull(propertyName);
        Objects.requireNonNull(packageName);
        if (userId != 0 || isRemovalPending(packageName)) return null;
        ApkPackage pkg = get(packageName);
        if (pkg == null) return null;
        if (className == null) return pkg.parsed.getProperties().get(propertyName);
        // Match AOSP PackageProperty's component lookup order. A component property
        // never falls back to a property of the same name on its application.
        for (int type : new int[] {PackageManager.TYPE_ACTIVITY, PackageManager.TYPE_PROVIDER,
                PackageManager.TYPE_RECEIVER, PackageManager.TYPE_SERVICE}) {
            List<? extends ParsedComponent> components = propertyComponents(pkg, type);
            // AOSP's property index retains repeated declarations; the last matching
            // class wins within each component family.
            for (int i = components.size() - 1; i >= 0; i--) {
                ParsedComponent component = components.get(i);
                if (className.equals(component.getName())) {
                    PackageManager.Property property = component.getProperties().get(propertyName);
                    if (property != null) return property;
                }
            }
        }
        return null;
    }

    @Override
    public ParceledListSlice<PackageManager.Property> queryProperty(String propertyName,
            int componentType) {
        Objects.requireNonNull(propertyName);
        List<PackageManager.Property> result = new ArrayList<>();
        if (android.os.UserHandle.getCallingUserId() != 0) return new ParceledListSlice<>(result);
        for (ApkPackage pkg : mPackages.values()) {
            if (isRemovalPending(pkg.packageName)) continue;
            if (componentType == PackageManager.TYPE_APPLICATION) {
                PackageManager.Property property = pkg.parsed.getProperties().get(propertyName);
                if (property != null) result.add(property);
            } else {
                for (ParsedComponent component : propertyComponents(pkg, componentType)) {
                    PackageManager.Property property = component.getProperties().get(propertyName);
                    if (property != null) result.add(property);
                }
            }
        }
        return new ParceledListSlice<>(result);
    }

    private static List<? extends ParsedComponent> propertyComponents(ApkPackage pkg, int type) {
        switch (type) {
            case PackageManager.TYPE_ACTIVITY: return pkg.parsed.getActivities();
            case PackageManager.TYPE_PROVIDER: return pkg.parsed.getProviders();
            case PackageManager.TYPE_RECEIVER: return pkg.parsed.getReceivers();
            case PackageManager.TYPE_SERVICE: return pkg.parsed.getServices();
            default: return java.util.Collections.emptyList();
        }
    }
}
