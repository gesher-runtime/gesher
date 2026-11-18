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
import android.content.pm.InstallSourceInfo;
import android.content.pm.PackageInfo;
import android.content.pm.PackageManager;
import android.content.pm.ParceledListSlice;
import android.content.pm.ProviderInfo;
import android.content.pm.PermissionGroupInfo;
import android.content.pm.Signature;
import android.content.pm.SigningDetails;
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
    private PackageEnabledStore mEnabledStore; // guarded by this; snapshot reads are immutable
    private final boolean mTransientEnabled;
    private final PermissionPolicy mPermissions = new PermissionPolicy(this);
    private final Executor mPackageEventExecutor = Executors.newSingleThreadExecutor(task -> {
        Thread thread = new Thread(task, "gesher-package-events");
        thread.setDaemon(true);
        return thread;
    });
    private Consumer<Intent> mPackageEventSender = PackageManagerService::broadcastPackageEvent;
    private Consumer<ApkPackage> mEnabledStateKiller = this::restartEnabledPackage;
    private volatile Consumer<EnabledChange> mEnabledStateListener;
    private volatile Consumer<EnabledChange> mEnabledStateRestarter;

    /** Server-local lifecycle wiring: an app's PACKAGE_CHANGED extras confer no authority. */
    void setEnabledStateListener(Consumer<EnabledChange> listener, Consumer<EnabledChange> restarter) {
        mEnabledStateListener = listener;
        mEnabledStateRestarter = restarter;
    }
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
        this(packages, true);
    }

    PackageManagerService(Collection<ApkPackage> packages, boolean persistentEnabled) {
        mTransientEnabled = !persistentEnabled;
        Map<String, ApkPackage> map = new LinkedHashMap<>();
        for (ApkPackage p : packages) {
            map.put(p.packageName, p);
        }
        mPackages = map;
        enabledStore();
    }

    private synchronized PackageEnabledStore enabledStore() {
        if (mEnabledStore == null) {
            java.nio.file.Path system = new java.io.File(Device.dataDir(), "system").toPath();
            PackageEnabledStore store = mTransientEnabled ? new PackageEnabledStore()
                    : new PackageEnabledStore(system.resolve("package-enabled-state").toFile(),
                            system.resolve("package-enabled-owners"));
            for (ApkPackage pkg : mPackages.values()) pkg.attachEnabledState(store, store.owner(pkg.packageName));
            mEnabledStore = store;
        }
        return mEnabledStore;
    }

    /** Finish owner I/O before an installer commits its durable package registry. */
    synchronized void prepareAdd(ApkPackage p) {
        PackageEnabledStore store = enabledStore();
        if (!p.hasEnabledState(store)) p.attachEnabledState(store, store.owner(p.packageName));
    }

    /** Adds or replaces a package (an install or an update). */
    synchronized void add(ApkPackage p) {
        prepareAdd(p);
        Map<String, ApkPackage> map = new LinkedHashMap<>(mPackages);
        map.put(p.packageName, p);
        mPackages = map;
        PackageManager.invalidatePackageInfoCache();
    }

    /** Removes a package (an uninstall); returns it, or null. */
    synchronized ApkPackage remove(String packageName) {
        Map<String, ApkPackage> map = new LinkedHashMap<>(mPackages);
        ApkPackage p = map.get(packageName);
        if (p != null) enabledStore().retire(packageName, p.enabledStateOwner());
        map.remove(packageName);
        mPackages = map;
        if (p != null) {
            p.clearComponentStates();
            PackageManager.invalidatePackageInfoCache();
        }
        return p;
    }

    /** A completed durable publication, as distinct from boot scanning or an unchanged APK. */
    void packageInstalled(ApkPackage pkg, boolean replacing, Object publicationLock) {
        final String name = pkg.packageName;
        final int uid = pkg.uid();
        mPackageEventExecutor.execute(() -> {
            // A session also holds the installer monitor around its final consent/code
            // checks. Do not call application Binders until that outer lock is released.
            synchronized (publicationLock) { }
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
    public synchronized ParceledListSlice queryIntentActivityOptions(ComponentName caller, Intent[] specifics,
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

    // A query spanning multiple records must not straddle an atomic enabled-state
    // batch. These readers do metadata/filter work only, never application callbacks.
    private synchronized List<ResolveInfo> query(Intent intent, String resolvedType, long flags) {
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
    synchronized List<ResolveInfo> services(Intent intent, String resolvedType, long flags) {
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
    synchronized List<ResolveInfo> receivers(Intent intent, String resolvedType, long flags) {
        List<ResolveInfo> result = new ArrayList<>();
        for (ApkPackage p : mPackages.values()) {
            if (isRemovalPending(p.packageName)) continue;
            result.addAll(p.queryIntentReceivers(intent, resolvedType, flags));
        }
        return result;
    }

    /** The provider with authority {@code authority} (one of its authorities), or null. */
    synchronized ProviderInfo provider(String authority, long flags) {
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

    /** No mainline (APEX/module) packages: Gesher's framework is not updated by modules. */
    @Override
    public List<android.content.pm.ModuleInfo> getInstalledModules(int flags) {
        return new ArrayList<>();
    }

    /** Usage hints for dexopt scheduling; Gesher compiles on install, not by use. */
    @Override
    public void notifyPackageUse(String packageName, int reason) {
    }

    @Override
    public synchronized ParceledListSlice queryIntentContentProviders(Intent intent, String resolvedType,
            long flags, int userId) {
        List<ResolveInfo> result = new ArrayList<>();
        if (intent == null || userId != 0) return new ParceledListSlice<>(result);
        for (ApkPackage p : mPackages.values()) {
            if (isRemovalPending(p.packageName)) continue;
            result.addAll(p.queryIntentContentProviders(intent, resolvedType, flags));
        }
        return new ParceledListSlice<>(result);
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
    public synchronized ParceledListSlice getInstalledPackages(long flags, int userId) {
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
    public synchronized ParceledListSlice<PackageInfo> getPackagesHoldingPermissions(String[] permissions,
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
    public synchronized ParceledListSlice getInstalledApplications(long flags, int userId) {
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

    /** Installed overrides survive restart/update and retire with the installation. */
    @Override
    public int getComponentEnabledSetting(ComponentName componentName, int userId) {
        // ComputerEngine checks cross-user access before looking up a component's
        // user; a privileged lookup of a nonexistent user reports DISABLED.
        enforceCrossUser(userId);
        if (componentName == null) return PackageManager.COMPONENT_ENABLED_STATE_DEFAULT;
        if (userId != 0) return PackageManager.COMPONENT_ENABLED_STATE_DISABLED;
        String pkg = componentName.getPackageName();
        ApkPackage installed = get(pkg);
        if (installed != null) return installed.componentEnabledState(componentName.getClassName());
        // As Settings.getComponentEnabledSettingLPr: an unknown package throws, an unknown
        // class of a known package is DEFAULT. Gesher's synthetic GMS/Play and browser
        // packages are known packages whose components keep their manifest state.
        if (get(pkg) == null && !synthetic(pkg) && !BrowserPackageSupport.isBrowserPackage(pkg)) {
            throw new IllegalArgumentException("Unknown package: " + pkg);
        }
        return PackageManager.COMPONENT_ENABLED_STATE_DEFAULT;
    }

    @Override
    public void setComponentEnabledSetting(ComponentName componentName, int newState, int flags,
            int userId, String callingPackage) {
        if (userId != 0) return;
        setComponentEnabledSettings(List.of(new PackageManager.ComponentEnabledSetting(
                componentName, newState, flags)), userId, callingPackage);
    }

    @Override
    public void setApplicationEnabledSetting(String packageName, int newState, int flags,
            int userId, String callingPackage) {
        if (userId != 0) return;
        setComponentEnabledSettings(List.of(new PackageManager.ComponentEnabledSetting(
                packageName, newState, flags)), userId, callingPackage);
    }

    @Override
    public synchronized void setComponentEnabledSettings(
            List<PackageManager.ComponentEnabledSetting> settings, int userId,
            String callingPackage) {
        // AOSP returns for a nonexistent user before validating caller-owned input.
        if (userId != 0) return;
        if (settings == null) throw new IllegalArgumentException("settings is null");
        if (settings.isEmpty()) throw new IllegalArgumentException("settings is empty");
        enforceCrossUser(userId);
        // Capture caller-owned input, then validate the entire batch before any state,
        // persistence, cache, process or broadcast change.
        List<PackageManager.ComponentEnabledSetting> captured = new ArrayList<>(settings);
        java.util.Set<ComponentName> components = new java.util.HashSet<>();
        java.util.Set<String> applications = new java.util.HashSet<>();
        Map<String, Integer> componentFlags = new LinkedHashMap<>();
        for (PackageManager.ComponentEnabledSetting setting : captured) {
            if (setting == null) throw new IllegalArgumentException("null enabled setting");
            checkEnabledState(setting.getEnabledState());
            String name = setting.getPackageName();
            checkEnabledPackage(name);
            if (setting.isComponent()) {
                ComponentName component = setting.getComponentName();
                if (!components.add(component)) throw new IllegalArgumentException("Duplicate component: " + component);
                Integer previous = componentFlags.putIfAbsent(name, setting.getEnabledFlags());
                if (previous != null && ((previous ^ setting.getEnabledFlags()) & PackageManager.DONT_KILL_APP) != 0) {
                    throw new IllegalArgumentException("Conflicting DONT_KILL_APP flags for " + name);
                }
                if (!hasComponent(component)) throw new IllegalArgumentException("Component class "
                        + component.getClassName() + " does not exist in " + name);
            } else if (!applications.add(name)) {
                throw new IllegalArgumentException("Duplicate package: " + name);
            }
        }
        PackageEnabledStore store = enabledStore();
        Map<String, PackageEnabledStore.Entry> changed = new LinkedHashMap<>();
        Map<String, EnabledChange> events = new LinkedHashMap<>();
        for (PackageManager.ComponentEnabledSetting setting : captured) {
            ApkPackage pkg = get(setting.getPackageName());
            int state = setting.getEnabledState();
            if (setting.isComponent() && state > PackageManager.COMPONENT_ENABLED_STATE_DISABLED) {
                // Android accepts the enum but ignores application-only states for components.
                continue;
            }
            PackageEnabledStore.Entry accumulated = changed.get(pkg.packageName);
            PackageEnabledStore.State before = accumulated == null ? pkg.enabledState() : accumulated.state();
            PackageEnabledStore.State after = setting.isComponent()
                    ? before.component(setting.getClassName(), state) : before.application(state);
            if (before.equals(after)) continue;
            changed.put(pkg.packageName, new PackageEnabledStore.Entry(pkg.enabledStateOwner(), after));
            EnabledChange event = events.computeIfAbsent(pkg.packageName, ignored -> new EnabledChange(pkg));
            event.components.add(setting.isComponent() ? setting.getClassName() : pkg.packageName);
            event.dontKill &= (setting.getEnabledFlags() & PackageManager.DONT_KILL_APP) != 0;
        }
        if (changed.isEmpty()) return;
        // Synchronous durability is also used for flags=0. SYNCHRONOUS therefore never
        // returns early, and a failed write publishes neither state nor side effects.
        store.commit(changed);
        for (EnabledChange event : events.values()) {
            if (!event.dontKill) event.pkg.retireEnabledProcesses();
        }
        PackageManager.invalidatePackageInfoCache();
        mPackageEventExecutor.execute(() -> {
            synchronized (PackageManagerService.this) { } // never call app code under PM's monitor
            for (EnabledChange event : events.values()) {
                Consumer<EnabledChange> listener = mEnabledStateListener;
                if (listener != null && event.current(this)) listener.accept(event);
                if (!event.dontKill && event.restartCurrent(this)) {
                    try {
                        Consumer<EnabledChange> restarter = mEnabledStateRestarter;
                        if (restarter != null) restarter.accept(event);
                        else mEnabledStateKiller.accept(event.pkg);
                    }
                    catch (RuntimeException failure) {
                        SystemServer.log("enabled-state process restart unavailable: " + failure.getClass().getSimpleName());
                    }
                }
                Intent intent = new Intent(Intent.ACTION_PACKAGE_CHANGED,
                        Uri.fromParts("package", event.pkg.packageName, null))
                        .putExtra(Intent.EXTRA_UID, event.pkg.uid())
                        .putExtra(Intent.EXTRA_CHANGED_COMPONENT_NAME, event.components.get(0))
                        .putExtra(Intent.EXTRA_CHANGED_COMPONENT_NAME_LIST, event.components.toArray(new String[0]))
                        .putExtra(Intent.EXTRA_DONT_KILL_APP, event.dontKill);
                if (!event.components.contains(event.pkg.packageName)) {
                    intent.addFlags(Intent.FLAG_RECEIVER_REGISTERED_ONLY);
                }
                sendPackageEvent(intent);
            }
        });
    }

    static final class EnabledChange {
        final ApkPackage pkg;
        final List<String> components = new ArrayList<>();
        boolean dontKill = true;
        EnabledChange(ApkPackage pkg) { this.pkg = pkg; }

        boolean current(PackageManagerService pm) {
            return pm.get(pkg.packageName) == pkg && !pm.isRemovalPending(pkg.packageName);
        }
        boolean disablesApplication(PackageManagerService pm) {
            if (!current(pm) || !components.contains(pkg.packageName)) return false;
            int enabled = pkg.enabledState().application();
            return enabled != PackageManager.COMPONENT_ENABLED_STATE_ENABLED
                    && enabled != PackageManager.COMPONENT_ENABLED_STATE_DEFAULT;
        }
        boolean disables(PackageManagerService pm, ApkPackage owner, String component, ApkPackage.ComponentKind kind) {
            if (owner != pkg || !current(pm)) return false;
            if (disablesApplication(pm)) return true;
            int enabled = pkg.enabledState().component(component);
            return components.contains(component) && enabled != PackageManager.COMPONENT_ENABLED_STATE_ENABLED
                    && enabled != PackageManager.COMPONENT_ENABLED_STATE_DEFAULT;
        }
        boolean restartCurrent(PackageManagerService pm) {
            // Android's immediate non-KEEP package-change dispatch does not retract
            // its restart when a later KEEP call changes the setting. ATM captures
            // the affected process generation so this event cannot kill a new one.
            return current(pm);
        }
    }

    private void restartEnabledPackage(ApkPackage pkg) {
        Services.Created services = Services.sLast;
        if (services != null && services.pm == this) services.atm.restartForEnabledState(pkg);
    }

    private void enforceCrossUser(int userId) {
        if (userId < 0) throw new IllegalArgumentException("Invalid userId " + userId);
        int callingUid = Binder.getCallingUid();
        if (android.os.UserHandle.getUserId(callingUid) != userId
                && callingUid != android.os.Process.ROOT_UID
                && callingUid != android.os.Process.SYSTEM_UID
                && mPermissions.checkProcessPermission("android.permission.INTERACT_ACROSS_USERS_FULL",
                        Binder.getCallingPid(), callingUid) != PackageManager.PERMISSION_GRANTED
                && mPermissions.checkProcessPermission("android.permission.INTERACT_ACROSS_USERS",
                        Binder.getCallingPid(), callingUid) != PackageManager.PERMISSION_GRANTED) {
            throw new SecurityException("Package access requires cross-user permission");
        }
    }

    private static void checkEnabledState(int state) {
        if (state != PackageManager.COMPONENT_ENABLED_STATE_DEFAULT
                && state != PackageManager.COMPONENT_ENABLED_STATE_ENABLED
                && state != PackageManager.COMPONENT_ENABLED_STATE_DISABLED
                && state != PackageManager.COMPONENT_ENABLED_STATE_DISABLED_USER
                && state != PackageManager.COMPONENT_ENABLED_STATE_DISABLED_UNTIL_USED) {
            throw new IllegalArgumentException("Invalid new component state: " + state);
        }
    }

    /** Keep the existing identity boundary; changing an app never trusts callingPackage. */
    private void checkEnabledPackage(String packageName) {
        ApkPackage p = get(packageName);
        if (p == null || isRemovalPending(p.packageName)) {
            throw new IllegalArgumentException("Unknown package: " + packageName);
        }
        int callingUid = Binder.getCallingUid();
        if (callingUid != p.uid() && callingUid != android.os.Process.SYSTEM_UID
                && callingUid != android.os.Process.ROOT_UID
                && Binder.getCallingPid() != android.os.Process.myPid()
                && mPermissions.checkProcessPermission(
                        "android.permission.CHANGE_COMPONENT_ENABLED_STATE",
                        Binder.getCallingPid(), callingUid) != PackageManager.PERMISSION_GRANTED) {
            throw new SecurityException("Attempt to change component state; pid="
                    + Binder.getCallingPid() + ", uid=" + callingUid
                    + ", package=" + packageName);
        }
    }

    /** Whether the package declares the component (activity or alias, service, receiver, provider). */
    private boolean hasComponent(ComponentName component) {
        ApkPackage p = get(component.getPackageName());
        if (p == null) return false;
        long all = PackageManager.MATCH_DISABLED_COMPONENTS;
        String name = component.getClassName();
        if (p.activityInfo(name, all) != null || p.serviceInfo(name, all) != null
                || p.receiverInfo(name, all) != null) {
            return true;
        }
        for (ProviderInfo info : p.providers(all)) {
            if (name.equals(info.name)) return true;
        }
        return false;
    }

    @Override
    public int getApplicationEnabledSetting(String packageName, int userId) {
        // Unlike the component getter, AOSP checks user existence before permissions.
        if (userId != 0) return PackageManager.COMPONENT_ENABLED_STATE_DISABLED;
        enforceCrossUser(userId);
        ApkPackage installed = get(packageName);
        if (installed != null) return installed.applicationEnabledState();
        if (synthetic(packageName)
                || BrowserPackageSupport.isBrowserPackage(packageName)) {
            return PackageManager.COMPONENT_ENABLED_STATE_ENABLED;
        }
        throw new IllegalArgumentException("Unknown package: " + packageName);
    }

    /**
     * The device's system features (docs/e2e.md): a phone-sized window with a mouse
     * (touchscreen and faketouch, matching {@link com.gesher.app.DeviceProfile#configuration()},
     * both orientations) whose launcher could host app widgets (apps reach
     * AppWidgetManager only on devices with app_widgets, and phone apps assume it).
     * Telephony, Wi-Fi, camera, sensors and the rest are absent. Bluetooth
     * (docs/bluetooth.md) is there when the host has a radio: FEATURE_BLUETOOTH with an
     * adapter, FEATURE_BLUETOOTH_LE when Windows reports LE support (apps such as
     * companion apps check it before anything else).
     */
    static final android.util.ArrayMap<String, android.content.pm.FeatureInfo> FEATURES =
            new android.util.ArrayMap<>();

    static {
        for (String name : new String[] {PackageManager.FEATURE_APP_WIDGETS,
                PackageManager.FEATURE_TOUCHSCREEN,
                PackageManager.FEATURE_FAKETOUCH, PackageManager.FEATURE_SCREEN_PORTRAIT,
                PackageManager.FEATURE_SCREEN_LANDSCAPE}) {
            android.content.pm.FeatureInfo f = new android.content.pm.FeatureInfo();
            f.name = name;
            FEATURES.put(name, f);
        }
        try {
            addBluetoothFeatures(FEATURES, WindowsBluetooth.adapter(0));
        } catch (LinkageError e) {
            // No runtime natives (a host JVM loading the class): no radio to report.
        }
    }

    /** The Bluetooth features of {@code adapter} (none without a radio). */
    static void addBluetoothFeatures(android.util.ArrayMap<String, android.content.pm.FeatureInfo> features,
            WindowsBluetooth.Adapter adapter) {
        if (!adapter.present) return;
        java.util.List<String> names = new java.util.ArrayList<>();
        names.add(PackageManager.FEATURE_BLUETOOTH);
        if (adapter.le()) names.add(PackageManager.FEATURE_BLUETOOTH_LE);
        for (String name : names) {
            android.content.pm.FeatureInfo f = new android.content.pm.FeatureInfo();
            f.name = name;
            features.put(name, f);
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

    private synchronized PackageVisibility packageVisibility() {
        Map<String, ApkPackage> packages = new LinkedHashMap<>(mPackages);
        for (String removing : mRemoving.keySet()) packages.remove(removing);
        boolean browser = !mPackages.containsKey(BrowserPackageSupport.PACKAGE)
                && !mRemoving.containsKey(BrowserPackageSupport.PACKAGE);
        boolean gms = !mPackages.containsKey(GmsPackageSupport.PACKAGE_GMS)
                && !mRemoving.containsKey(GmsPackageSupport.PACKAGE_GMS);
        boolean vending = !mPackages.containsKey(GmsPackageSupport.PACKAGE_VENDING)
                && !mPackages.containsKey(GmsPackageSupport.PACKAGE_GMS)
                && !mRemoving.containsKey(GmsPackageSupport.PACKAGE_VENDING);
        return new PackageVisibility(packages, browser, gms, vending, this::isProtectedBroadcast);
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
        PackageVisibility visibility = packageVisibility();
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
    public boolean canRequestPackageInstalls(String packageName, int userId) {
        if (userId != 0 || packageName == null || isRemovalPending(packageName)) {
            return false;
        }
        if ("android".equals(packageName)) {
            return true;
        }
        ApkPackage pkg = get(packageName);
        // Android exposes this capability only to installers targeting O or later.
        // Gesher permits the request; PackageInstallerService still asks the host
        // for fresh consent on every commit, regardless of this query's result.
        return pkg != null && pkg.parsed.getTargetSdkVersion() >= android.os.Build.VERSION_CODES.O
                && pkg.parsed.getRequestedPermissions()
                .contains(android.Manifest.permission.REQUEST_INSTALL_PACKAGES);
    }

    @Override
    public String getInstallerPackageName(String packageName) {
        InstallSourceInfo info = getInstallSourceInfo(packageName,
                android.os.UserHandle.getUserId(Binder.getCallingUid()));
        if (info == null) throw new IllegalArgumentException("Unknown package: " + packageName);
        return info.getInstallingPackageName();
    }

    /** Capture the actual attached code before entering Installer or PM (ATM calls PM). */
    private ApkPackage installSourceCaller(int uid) {
        ActivityTaskManagerService atm = mInstallSourceAtm;
        return atm == null ? null : atm.authenticatedPackageCodeForPid(Binder.getCallingPid(), uid);
    }

    @Override
    public InstallSourceInfo getInstallSourceInfo(String packageName, int userId) {
        Objects.requireNonNull(packageName);
        enforceCrossUser(userId);
        if (userId != 0) return null;
        int uid = Binder.getCallingUid();
        ApkPackage caller = installSourceCaller(uid);
        synchronized (this) {
            ApkPackage target = get(packageName);
            if (isRemovalPending(packageName) || (target == null && !"android".equals(packageName)
                    && !synthetic(packageName) && !BrowserPackageSupport.isBrowserPackage(packageName))) {
                return null;
            }
            // Revalidate the authenticated caller under the same lock as target, source, UID and
            // removal state. A replacement between ATM authentication and this snapshot confers
            // no authority to disclose retired history. This path is O(1) for the target itself.
            boolean selfCaller = caller != null && caller == target && get(caller.packageName) == caller
                    && !isRemovalPending(caller.packageName) && caller.uid() == uid;
            boolean core = uid >= 0 && android.os.UserHandle.getAppId(uid)
                    < android.os.Process.FIRST_APPLICATION_UID;
            PackageVisibility visibility = core || selfCaller ? null : packageVisibility();
            if (visibility != null && !visibility.canSee(uid, packageName)) return null;
            PackageInstallSource source = target == null ? PackageInstallSource.UNKNOWN : target.installSource();
            String installer = null, initiator = null;
            if (source.isInstaller(get(source.installer())) && !isRemovalPending(source.installer())
                    && (visibility == null || visibility.canSee(uid, source.installer()))) {
                installer = source.installer();
            }
            if (source.isInitiator(get(source.initiator())) && !isRemovalPending(source.initiator())) {
                // A reassignment never makes the initiator's visibility depend on the new installer.
                if (visibility == null || visibility.canSee(uid, source.initiator())) initiator = source.initiator();
            } else if (selfCaller) {
                // Only this authenticated target owns its retired history. This runtime has
                // no shared-user admission; coincident legacy numeric UIDs are not identity.
                initiator = source.initiator();
            }
            return new InstallSourceInfo(initiator, initiator == null ? null : source.signingInfo(),
                    null, installer, null, source.category());
        }
    }

    @Override
    public void setInstallerPackageName(String targetPackage, String installerPackageName) {
        int uid = Binder.getCallingUid();
        CallerIdentity callers = mInstallSourceCallers;
        if (callers == null) throw new SecurityException("Unknown calling UID: " + uid);
        // Never enter ATM while holding Installer or PM. Currentness is checked again below.
        String name = callers.callerPackage();
        ApkPackage caller = installSourceCaller(uid);
        boolean system = "android".equals(name) && uid == android.os.Process.SYSTEM_UID;
        if ((!system && (caller == null || !name.equals(caller.packageName))) || android.os.UserHandle.getUserId(uid) != 0) {
            throw new SecurityException("Unknown calling UID: " + uid);
        }
        Installer storage = mInstallSourceStorage;
        if (storage == null) throw new IllegalStateException("Package metadata storage is not attached");
        synchronized (storage) {
            synchronized (this) {
                if (!system && (get(name) != caller || caller.uid() != uid || isRemovalPending(name))) {
                    throw new SecurityException("Calling package changed during installer reassignment");
                }
                PackageVisibility visibility = packageVisibility();
                ApkPackage target = get(targetPackage);
                if (target == null || isRemovalPending(targetPackage)
                        || !visibility.canSee(uid, targetPackage)) {
                    throw new IllegalArgumentException("Unknown target package: " + targetPackage);
                }
                ApkPackage installer = installerPackageName == null ? null : get(installerPackageName);
                if (installerPackageName != null && (installer == null
                        || isRemovalPending(installerPackageName)
                        || !visibility.canSee(uid, installerPackageName))) {
                    throw new IllegalArgumentException("Unknown installer package: " + installerPackageName);
                }
                SigningDetails signing = system ? SigningDetails.UNKNOWN : caller.verifiedSigningDetails();
                int targetSdk = system ? android.os.Build.VERSION.SDK_INT : caller.parsed.getTargetSdkVersion();
                if (installer != null && !sameCurrentSignatures(signing, installer)) {
                    throw new SecurityException("Caller does not have same cert as new installer package "
                            + installerPackageName);
                }
                PackageInstallSource source = target.installSource();
                ApkPackage previous = get(source.installer());
                if (source.isInstaller(previous) && !isRemovalPending(previous.packageName)) {
                    if (!sameCurrentSignatures(signing, previous)) {
                        throw new SecurityException("Caller does not have same cert as old installer package "
                                + previous.packageName);
                    }
                } else if (!system && !mPermissions.granted(android.Manifest.permission.INSTALL_PACKAGES, name)) {
                    // AOSP change 150857253: denial on every SDK; R+ reports SecurityException.
                    if (targetSdk >= android.os.Build.VERSION_CODES.R) {
                        throw new SecurityException("Neither user " + uid + " nor current process has "
                                + android.Manifest.permission.INSTALL_PACKAGES);
                    }
                    return;
                }
                try {
                    storage.persistInstallSource(target, source.withInstaller(installer));
                } catch (java.io.IOException failure) {
                    throw new IllegalStateException("Cannot persist installer attribution", failure);
                }
            }
        }
    }

    /** PackageManagerServiceUtils.compareSignatures uses current certs, not rotation capability. */
    private static boolean sameCurrentSignatures(SigningDetails caller, ApkPackage installer) {
        Signature[] first = caller == null || caller == SigningDetails.UNKNOWN ? null : caller.getSignatures();
        Signature[] second = installer.verifiedSignatures();
        return first != null && second != null && first.length == second.length
                && new java.util.HashSet<>(java.util.Arrays.asList(first))
                        .equals(new java.util.HashSet<>(java.util.Arrays.asList(second)));
    }

    private volatile CallerIdentity mInstallSourceCallers;
    private volatile ActivityTaskManagerService mInstallSourceAtm;
    private volatile Installer mInstallSourceStorage;

    void attachInstallSourceCallers(ActivityTaskManagerService atm) {
        mInstallSourceAtm = atm;
        mInstallSourceCallers = new CallerIdentity(this, atm);
    }

    synchronized void attachInstallSourceStorage(Installer installer) {
        if (mInstallSourceStorage != null && mInstallSourceStorage != installer) {
            throw new IllegalStateException("Package metadata storage already attached");
        }
        mInstallSourceStorage = installer;
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
        attachInstallSourceCallers(atm);
        attachInstallSourceStorage(installer);
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
