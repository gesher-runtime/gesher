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
import android.content.pm.PermissionGroupInfo;
import android.content.pm.Signature;
import android.content.pm.ResolveInfo;
import android.content.pm.ServiceInfo;

import java.util.ArrayList;
import java.util.Collection;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;

/**
 * {@code package}: PackageManagerService, knowing the APKs it was given and the installed
 * packages ({@link Installer}; parsed with PackageParser2, {@link ApkPackage}). Each has its own UID in real microG mode. Packages come and go while it serves (install, uninstall): readers
 * see one version of the map, replaced as a whole.
 */
public final class PackageManagerService extends IPackageManager.Default {
    private volatile Map<String, ApkPackage> mPackages = new LinkedHashMap<>();
    private final PermissionPolicy mPermissions = new PermissionPolicy(this);

    public PermissionPolicy permissions() { return mPermissions; }

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

    private List<ResolveInfo> query(Intent intent, String resolvedType, long flags) {
        List<ResolveInfo> result = new ArrayList<>();
        for (ApkPackage p : mPackages.values()) {
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

    private final PackageInstallerService mInstallerService = new PackageInstallerService();
    private final android.content.pm.IPackageInstaller mInstaller =
            StandIn.of(android.content.pm.IPackageInstaller.class, mInstallerService).asInterface();

    @Override
    public android.content.pm.IPackageInstaller getPackageInstaller() {
        return mInstaller;
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
