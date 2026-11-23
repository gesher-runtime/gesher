/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package com.gesher.server;

import android.Manifest;
import android.app.ActivityThread;
import android.content.ComponentName;
import android.content.pm.ApplicationInfo;
import android.content.pm.PackageInfo;
import android.content.pm.PackageManager;
import android.content.pm.PermissionInfo;
import android.content.pm.ProviderInfo;
import android.os.Binder;
import android.os.Bundle;
import android.os.Process;
import com.android.internal.pm.parsing.pkg.PackageImpl;
import com.android.internal.pm.pkg.component.ParsedProviderImpl;
import com.android.internal.pm.pkg.component.ParsedUsesPermissionImpl;
import java.io.File;
import java.lang.reflect.Field;
import java.nio.file.Files;
import java.util.ArrayList;
import java.util.List;
import java.util.Map;
import java.util.Set;
import java.util.function.Consumer;
import java.util.stream.Collectors;
import sun.misc.Unsafe;

/** Actual PMS visibility and cross-user policy; declared manifest/enrollment/JNI boundaries. */
public final class ProviderEnumerationPolicyHost {
    private static Unsafe unsafe;
    private static int checks;
    private interface Work { void run() throws Exception; }
    private static Field field(Class<?> type, String name) throws Exception {
        Field result = type.getDeclaredField(name); result.setAccessible(true); return result;
    }
    private static void set(Object value, String name, Object data) throws Exception {
        field(value.getClass(), name).set(value, data);
    }
    private static void check(boolean value, String message) {
        if (!value) throw new AssertionError(message);
        checks++;
    }
    private static void rejects(Class<? extends Throwable> type, Work work, String message) throws Exception {
        try { work.run(); }
        catch (Throwable error) {
            if (!type.isInstance(error)) throw new AssertionError(message + ": " + error, error);
            checks++; return;
        }
        throw new AssertionError(message + ": no exception");
    }
    private static void as(int pid, int uid, Work work) throws Exception {
        long previous = Binder.clearCallingIdentity();
        try {
            Binder.restoreCallingIdentity(((long)uid << 32) | (pid & 0xffffffffL));
            check(Binder.getCallingPid() == pid && Binder.getCallingUid() == uid,
                    "Native caller identity boundary");
            work.run();
        } finally { Binder.restoreCallingIdentity(previous); }
    }
    private static ApkPackage record(File directory, String name, int uid,
            boolean provider, boolean exported) throws Exception {
        return record(directory, name, uid, provider, exported, parsed -> { });
    }
    private static ApkPackage record(File directory, String name, int uid,
            boolean provider, boolean exported, Consumer<PackageImpl> declarations) throws Exception {
        String path = new File(directory, name + ".apk").getPath();
        PackageImpl parsed = (PackageImpl)PackageImpl.forTesting(name, path);
        parsed.setTargetSdkVersion(36); parsed.setEnabled(true);
        if (provider) {
            ParsedProviderImpl component = new ParsedProviderImpl();
            component.setName(name + ".Records"); component.setPackageName(name);
            component.setProcessName(name); component.setAuthority(name + ".records");
            component.setEnabled(true); component.setExported(exported);
            Bundle metadata = new Bundle(); metadata.putBoolean("fixture.marker", true);
            component.setMetaData(metadata); parsed.addProvider(component);
        }
        declarations.accept(parsed); parsed.hideAsFinal();
        ApkPackage result = (ApkPackage)unsafe.allocateInstance(ApkPackage.class);
        set(result, "packageName", name); set(result, "parsed", parsed); set(result, "mUid", uid);
        set(result, "apkPath", path); set(result, "sharedLibraries", SharedLibraryGraph.EMPTY);
        result.setDataDir(new File(directory, name).getPath());
        return result;
    }
    private static Set<String> packages(List<ProviderInfo> providers) {
        return providers.stream().map(p -> p.packageName).collect(Collectors.toSet());
    }

    private static void visibility(File directory) throws Exception {
        ApkPackage source = record(directory, "fixture.enumeration.source", 10042, true, false,
                parsed -> {
                    parsed.addQueriesPackage("fixture.enumeration.visible");
                    parsed.addQueriesProvider(UserDictionaryProvider.AUTHORITY);
                });
        ApkPackage hidden = record(directory, "fixture.enumeration.hidden", 10043, true, false);
        ApkPackage visible = record(directory, "fixture.enumeration.visible", 10044, true, false);
        ApkPackage exported = record(directory, "fixture.enumeration.exported", 10045, true, true);
        PackageManagerService pm = new PackageManagerService(List.of(source, hidden, visible, exported), false);
        int dictionaryUid = pm.getPackageUid(UserDictionaryProvider.PACKAGE, 0, 0);
        ComponentName dictionary = new ComponentName(UserDictionaryProvider.PACKAGE, UserDictionaryProvider.CLASS);
        as(812341, source.uid(), () -> {
            Set<String> result = packages(pm.queryContentProviders(null, 0, 0, null).getList());
            check(result.contains(source.packageName), "Caller retains its own private provider");
            check(result.contains(visible.packageName), "A declared package query exposes its provider metadata");
            check(!result.contains(hidden.packageName), "Visibility regression: hidden provider metadata exposed");
            check(!result.contains(exported.packageName), "An exported component alone does not expose its package");
            check(result.contains(UserDictionaryProvider.PACKAGE), "A declared authority query exposes dictionary metadata");
            check(pm.isPackageAvailable(dictionary.getPackageName(), 0)
                    && pm.getPackageInfo(dictionary.getPackageName(), 0, 0) != null
                    && pm.getApplicationInfo(dictionary.getPackageName(), 0, 0) != null
                    && pm.getPackageUid(dictionary.getPackageName(), 0, 0) == dictionaryUid
                    && pm.getNameForUid(dictionaryUid) != null,
                    "Authority query grants visibility throughout dictionary package metadata");
            check(pm.resolveContentProvider(UserDictionaryProvider.AUTHORITY, 0, 0) != null
                    && pm.getProviderInfo(dictionary, 0, 0) != null,
                    "Authority query grants direct provider metadata visibility");
            check(pm.queryContentProviders(hidden.packageName, hidden.uid(), 0, null).getList().isEmpty(),
                    "Explicit process/UID does not bypass package visibility");
            check(packages(pm.queryContentProviders(null, 0, PackageManager.GET_META_DATA,
                    "fixture.marker").getList()).equals(Set.of(source.packageName, visible.packageName)),
                    "Metadata-key lookup preserves the same visibility filter");
            check(packages(pm.queryContentProviders(source.packageName, source.uid(), 0, null).getList())
                    .equals(Set.of(source.packageName)), "Explicit own-process lookup remains usable");
        });
        pm.beginPackageRemoval(visible);
        as(812341, source.uid(), () -> check(!packages(pm.queryContentProviders(null, 0, 0, null)
                .getList()).contains(visible.packageName), "Pending removal leaves the visibility snapshot"));
        pm.endPackageRemoval(visible);
        as(812341, source.uid(), () -> check(packages(pm.queryContentProviders(null, 0, 0, null)
                .getList()).contains(visible.packageName), "Aborted removal restores the existing visibility relationship"));
        as(812399, 10099, () -> check(packages(pm.queryContentProviders(null, 0, 0, null).getList())
                .isEmpty(), "Unknown ordinary UID sees no installed application providers"));
        as(812345, hidden.uid(), () -> {
            check(!pm.isPackageAvailable(dictionary.getPackageName(), 0)
                    && pm.getPackageInfo(dictionary.getPackageName(), 0, 0) == null
                    && pm.getApplicationInfo(dictionary.getPackageName(), 0, 0) == null
                    && pm.getPackageUid(dictionary.getPackageName(), 0, 0) == -1
                    && pm.getPackagesForUid(dictionaryUid) == null
                    && pm.getNamesForUids(new int[]{dictionaryUid})[0] == null,
                    "An unrelated app cannot discover dictionary package metadata by name or UID");
            check(pm.resolveContentProvider(UserDictionaryProvider.AUTHORITY, 0, 0) == null
                    && pm.getProviderInfo(dictionary, 0, 0) == null
                    && pm.getInstalledPackages(0, 0).getList().stream().noneMatch(
                            p -> dictionary.getPackageName().equals(((PackageInfo)p).packageName))
                    && pm.getInstalledApplications(0, 0).getList().stream().noneMatch(
                            p -> dictionary.getPackageName().equals(((ApplicationInfo)p).packageName)),
                    "Resolved, named and enumerated metadata share the visibility check");
        });
        as(812300, Process.SYSTEM_UID, () -> check(packages(pm.queryContentProviders(null, 0, 0, null)
                .getList()).contains(hidden.packageName), "Actual system caller retains its inventory"));
    }

    private static void crossUserOutbound(File directory) throws Exception {
        ApkPackage source = record(directory, "fixture.enumeration.outbound", 10042, true, false);
        PackageManagerService pm = new PackageManagerService(List.of(source), false);
        as(812341, source.uid(), () -> {
            rejects(SecurityException.class,
                    () -> pm.queryContentProviders(source.packageName, 1010042, 0, null),
                    "Cross-user outbound regression: unavailable user returned before authorization");
            rejects(SecurityException.class,
                    () -> pm.queryContentProviders("missing.process", 1010043, 0, "missing.metadata"),
                    "Cross-user permission precedes all empty-result conditions");
            check(packages(pm.queryContentProviders(null, 1010042, 0, null).getList())
                    .contains(source.packageName), "Global query ignores the UID argument and uses the caller's user");
            check(packages(pm.queryContentProviders(source.packageName, source.uid(), 0, null).getList())
                    .equals(Set.of(source.packageName)), "Same-user process query needs no cross-user grant");
            rejects(IllegalArgumentException.class,
                    () -> pm.queryContentProviders("system", -100000, 0, null), "Invalid derived user rejected");
        });
        for (int uid : new int[]{Process.ROOT_UID, Process.SYSTEM_UID}) {
            as(812300, uid, () -> {
                check(pm.queryContentProviders(source.packageName, 1010042, 0, null).getList().isEmpty(),
                        "Authorized root/system caller receives the unavailable-user empty result");
                check(packages(pm.queryContentProviders(UserDictionaryProvider.PROCESS,
                        pm.getPackageUid(UserDictionaryProvider.PACKAGE, 0, 0), 0, null).getList())
                        .contains(UserDictionaryProvider.PACKAGE), "Authorized root/system caller sees the actual builtin");
            });
        }
    }

    private static void crossUserInbound(File directory) throws Exception {
        PackageManagerService pm = new PackageManagerService(List.of(), false);
        as(812341, 1010042, () -> {
            rejects(SecurityException.class,
                    () -> pm.queryContentProviders("system", Process.SYSTEM_UID, 0, null),
                    "Cross-user inbound regression: user 0 builtin exposed before authorization");
            rejects(SecurityException.class,
                    () -> pm.queryContentProviders("missing.process", 10043, 0, null),
                    "Missing process cannot bypass inbound cross-user authorization");
            check(pm.queryContentProviders(null, Process.SYSTEM_UID, 0, null).getList().isEmpty(),
                    "Global query for the caller's unavailable user stays empty without crossing users");
        });
    }

    private static ApkPackage permissionRecord(File directory, String name, int uid,
            String permission) throws Exception {
        return record(directory, name, uid, false, false,
                parsed -> parsed.addUsesPermission(new ParsedUsesPermissionImpl(permission, 0, Set.of())));
    }

    @SuppressWarnings("unchecked")
    private static void frameworkPermissions(PermissionPolicy policy) throws Exception {
        // Binary framework-resource parsing is the declared host boundary. These
        // are Android16r4's actual protection flags, never replacement grants.
        Object catalog = field(PermissionPolicy.class, "mCatalog").get(policy);
        set(catalog, "mLoaded", true); set(catalog, "mFrameworkRead", true);
        Map<String, PermissionInfo> definitions = (Map<String, PermissionInfo>)field(
                catalog.getClass(), "mFramework").get(catalog);
        PermissionInfo basic = new PermissionInfo(); basic.packageName = "android";
        basic.name = Manifest.permission.INTERACT_ACROSS_USERS;
        basic.protectionLevel = PermissionInfo.PROTECTION_SIGNATURE | PermissionInfo.PROTECTION_FLAG_PRIVILEGED
                | PermissionInfo.PROTECTION_FLAG_DEVELOPMENT | PermissionInfo.PROTECTION_FLAG_ROLE;
        definitions.put(basic.name, basic);
        PermissionInfo full = new PermissionInfo(); full.packageName = "android";
        full.name = Manifest.permission.INTERACT_ACROSS_USERS_FULL;
        full.protectionLevel = PermissionInfo.PROTECTION_SIGNATURE | PermissionInfo.PROTECTION_FLAG_INSTALLER
                | PermissionInfo.PROTECTION_FLAG_MODULE | PermissionInfo.PROTECTION_FLAG_ROLE;
        definitions.put(full.name, full);
        PermissionInfo internet = new PermissionInfo(); internet.packageName = "android";
        internet.name = Manifest.permission.INTERNET;
        internet.protectionLevel = PermissionInfo.PROTECTION_NORMAL;
        definitions.put(internet.name, internet);
    }

    private static void permissionBearing(File directory, File fixtures) throws Exception {
        ApkPackage platform = record(directory, "android", Process.SYSTEM_UID, false, false);
        ApkPackage basic = permissionRecord(directory, "fixture.enumeration.basic", 10042,
                Manifest.permission.INTERACT_ACROSS_USERS);
        ApkPackage full = permissionRecord(directory, "fixture.enumeration.full", 1010044,
                Manifest.permission.INTERACT_ACROSS_USERS_FULL);
        ApkPackage unsigned = permissionRecord(directory, "fixture.enumeration.unsigned", 10045,
                Manifest.permission.INTERACT_ACROSS_USERS);
        ApkPackage unrelated = record(directory, "fixture.enumeration.unrelated", 10046, false, false);
        for (ApkPackage pkg : List.of(platform, basic, full)) {
            set(pkg, "apkPath", new File(fixtures, "v2only.apk").getPath());
            check(pkg.hasVerifiedSigningIdentity(), "Real APK signature verification for permission-bearing fixture");
        }
        set(unsigned, "apkPath", new File(fixtures, "unsigned.apk").getPath());
        check(!unsigned.hasVerifiedSigningIdentity(), "Unsigned request has no verified signing identity");
        PackageManagerService pm = new PackageManagerService(List.of(platform, basic, full, unsigned, unrelated), false);
        int dictionaryUid = pm.getPackageUid(UserDictionaryProvider.PACKAGE, 0, 0);
        frameworkPermissions(pm.permissions());
        ActivityTaskManagerService tasks = (ActivityTaskManagerService)unsafe.allocateInstance(ActivityTaskManagerService.class);
        set(tasks, "mPm", pm);
        ArrayList<Object> enrolled = new ArrayList<>();
        Class<?> processType = Class.forName(ActivityTaskManagerService.class.getName() + "$ProcessRecord");
        ApkPackage[] callers = {basic, full, unsigned, unrelated};
        for (int i = 0; i < callers.length; i++) {
            Object process = unsafe.allocateInstance(processType);
            set(process, "pid", 812350 + i); set(process, "uid", callers[i].uid());
            set(process, "packageName", callers[i].packageName); enrolled.add(process);
        }
        set(tasks, "mProcesses", enrolled);
        pm.permissions().attach(new CallerIdentity(pm, tasks));
        as(812350, basic.uid(), () -> {
            check(pm.permissions().checkProcessPermission(Manifest.permission.INTERACT_ACROSS_USERS,
                    Binder.getCallingPid(), Binder.getCallingUid()) == PackageManager.PERMISSION_GRANTED,
                    "Production permission policy grants the requested verified signature permission");
            check(pm.queryContentProviders("missing.process", 1010042, 0, null).getList().isEmpty(),
                    "Genuine basic cross-user permission authorizes the unavailable-user result");
        });
        as(812351, full.uid(), () -> {
            check(pm.permissions().checkProcessPermission(Manifest.permission.INTERACT_ACROSS_USERS_FULL,
                    Binder.getCallingPid(), Binder.getCallingUid()) == PackageManager.PERMISSION_GRANTED,
                    "Production permission policy grants the fuller verified signature permission");
            check(pm.queryContentProviders(UserDictionaryProvider.PROCESS, dictionaryUid, 0, null).getList().isEmpty(),
                    "Cross-user permission does not grant package visibility without a manifest query");
        });
        as(812352, unsigned.uid(), () -> rejects(SecurityException.class,
                () -> pm.queryContentProviders("missing.process", 1010042, 0, null),
                "A permission request without verified signing does not authorize cross-user access"));
        as(812353, unrelated.uid(), () -> rejects(SecurityException.class,
                () -> pm.queryContentProviders("missing.process", 1010042, 0, null),
                "Unrelated enrolled package has no cross-user grant"));
        as(812353, basic.uid(), () -> rejects(SecurityException.class,
                () -> pm.queryContentProviders("missing.process", 1010042, 0, null),
                "Unrelated enrolled package cannot claim the UID with the grant"));
        as(812350, 10099, () -> rejects(SecurityException.class,
                () -> pm.queryContentProviders("missing.process", 1010042, 0, null),
                "Granted package still requires its authenticated UID"));
        as(812399, basic.uid(), () -> rejects(SecurityException.class,
                () -> pm.queryContentProviders("missing.process", 1010042, 0, null),
                "Unknown process cannot use another package's granted UID"));
    }

    private static void boundApplication(ApkPackage pkg, Work work) throws Exception {
        // Main -> GesherAppDriver binds this field without enrolling an ATM PID.
        // Framework launch/manifest construction, not its permission result, is
        // the declared boundary; these are real ActivityThread accessors below.
        Field current = field(ActivityThread.class, "sCurrentActivityThread");
        Object previous = current.get(null);
        ActivityThread thread = (ActivityThread)unsafe.allocateInstance(ActivityThread.class);
        Object data = unsafe.allocateInstance(Class.forName("android.app.ActivityThread$AppBindData"));
        ApplicationInfo info = new ApplicationInfo(); info.packageName = pkg.packageName;
        info.uid = pkg.uid(); info.sourceDir = pkg.apkPath;
        set(data, "appInfo", info); set(thread, "mBoundApplication", data);
        try {
            current.set(null, thread);
            check(pkg.packageName.equals(ActivityThread.currentPackageName()),
                    "Actual ActivityThread exposes the locally bound application");
            work.run();
        } finally { current.set(null, previous); }
    }

    private static void localPermissions(File directory, File fixtures) throws Exception {
        int pid = Process.myPid(), uid = Process.myUid();
        ApkPackage ordinary = record(directory, "fixture.enumeration.local.ordinary", uid, true, false,
                parsed -> parsed.addUsesPermission(new ParsedUsesPermissionImpl(
                        Manifest.permission.INTERNET, 0, Set.of())));
        ApkPackage platform = record(directory, "android", Process.SYSTEM_UID, false, false);
        ApkPackage basic = permissionRecord(directory, "fixture.enumeration.local.basic", uid,
                Manifest.permission.INTERACT_ACROSS_USERS);
        ApkPackage full = permissionRecord(directory, "fixture.enumeration.local.full", uid,
                Manifest.permission.INTERACT_ACROSS_USERS_FULL);
        ApkPackage unsigned = permissionRecord(directory, "fixture.enumeration.local.unsigned", uid,
                Manifest.permission.INTERACT_ACROSS_USERS);
        ApkPackage absent = permissionRecord(directory, "fixture.enumeration.local.absent", uid,
                Manifest.permission.INTERACT_ACROSS_USERS);
        for (ApkPackage pkg : List.of(platform, basic, full)) {
            set(pkg, "apkPath", new File(fixtures, "v2only.apk").getPath());
            check(pkg.hasVerifiedSigningIdentity(), "Local positive uses actual APK signature verification");
        }
        set(unsigned, "apkPath", new File(fixtures, "unsigned.apk").getPath());
        check(!unsigned.hasVerifiedSigningIdentity(), "Local unsigned fixture is actually unverified");
        // This process has exactly one admitted standalone app identity at a time.
        PackageManagerService pm = new PackageManagerService(List.of(platform, ordinary), false);
        frameworkPermissions(pm.permissions());
        ActivityTaskManagerService tasks = (ActivityTaskManagerService)unsafe.allocateInstance(ActivityTaskManagerService.class);
        set(tasks, "mPm", pm);
        ArrayList<Object> enrolled = new ArrayList<>(); set(tasks, "mProcesses", enrolled);
        CallerIdentity identity = new CallerIdentity(pm, tasks);
        PermissionPolicy policy = pm.permissions(); policy.attach(identity);
        ActivityManagerService am = (ActivityManagerService)unsafe.allocateInstance(ActivityManagerService.class);
        set(am, "mPermissions", policy);

        as(pid, uid, () -> {
            check(identity.packageForPid(pid) == null, "Standalone process has no ATM application enrollment");
            boundApplication(ordinary, () -> {
                check(!ordinary.parsed.getRequestedPermissions().contains(Manifest.permission.INTERACT_ACROSS_USERS),
                        "Ordinary local application does not request cross-user access");
                rejects(SecurityException.class,
                        () -> pm.queryContentProviders("missing.process", 1010042, 0, null),
                        "Local application regression: bound app inherited service cross-user privilege");
                check(policy.checkProcessPermission(Manifest.permission.INTERACT_ACROSS_USERS, pid, uid)
                        == PackageManager.PERMISSION_DENIED, "Local ordinary app has no implicit cross-user grant");
                check(policy.checkProcessPermission(Manifest.permission.INTERNET, pid, uid)
                        == PackageManager.PERMISSION_GRANTED, "Declared normal permission remains usable locally");
                check(am.checkPermission(Manifest.permission.INTERNET, pid, uid)
                        == PackageManager.PERMISSION_GRANTED, "AMS normal-permission path uses the real local decision");
                check(am.checkPermissionForDevice(Manifest.permission.INTERACT_ACROSS_USERS, pid, uid, 0)
                        == PackageManager.PERMISSION_DENIED, "AMS device-permission path cannot inherit service privilege");
                check(policy.checkProcessPermission("fixture.permission.UNKNOWN", pid, uid)
                        == PackageManager.PERMISSION_DENIED, "Unknown local permission stays denied");
                check(packages(pm.queryContentProviders(ordinary.packageName, uid, 0, null).getList())
                        .equals(Set.of(ordinary.packageName)), "Local same-user provider lookup remains usable");
                check(policy.checkProcessPermission(Manifest.permission.INTERACT_ACROSS_USERS, pid, Process.SYSTEM_UID)
                        == PackageManager.PERMISSION_DENIED, "A bound app cannot claim the service's UID");
            });
            pm.remove(ordinary.packageName);
            pm.add(unsigned);
            boundApplication(unsigned, () -> rejects(SecurityException.class,
                    () -> pm.queryContentProviders("missing.process", 1010042, 0, null),
                    "Local unsigned permission request cannot obtain a signature grant"));
            pm.remove(unsigned.packageName);
            boundApplication(absent, () -> rejects(SecurityException.class,
                    () -> pm.queryContentProviders("missing.process", 1010042, 0, null),
                    "Bound package missing from the real registry cannot become a service caller"));
            for (ApkPackage pkg : List.of(full, basic)) {
                pm.add(pkg);
                String permission = pkg == basic ? Manifest.permission.INTERACT_ACROSS_USERS
                        : Manifest.permission.INTERACT_ACROSS_USERS_FULL;
                boundApplication(pkg, () -> {
                    check(policy.checkProcessPermission(permission, pid, uid) == PackageManager.PERMISSION_GRANTED,
                            "Verified local signature permission is granted by production policy");
                    check(am.checkPermission(permission, pid, uid) == PackageManager.PERMISSION_GRANTED,
                            "AMS retains the actual locally granted permission");
                    check(pm.queryContentProviders("missing.process", 1010042, 0, null).getList().isEmpty(),
                            "Authorized standalone app gets the unavailable-user result");
                    check(policy.checkProcessPermission(permission, pid, uid + 1) == PackageManager.PERMISSION_DENIED,
                            "Local granted package must use its actual UID");
                    check(policy.checkProcessPermission(permission, pid + 1, uid) == PackageManager.PERMISSION_DENIED,
                            "An unknown remote PID cannot use the local bound application's grant");
                });
                if (pkg == full) pm.remove(full.packageName);
            }
            boundApplication(basic, () -> {
                pm.beginPackageRemoval(basic);
                rejects(SecurityException.class,
                        () -> pm.queryContentProviders("missing.process", 1010042, 0, null),
                        "Pending removal revokes local permission authority");
                pm.endPackageRemoval(basic);
                check(policy.checkProcessPermission(Manifest.permission.INTERACT_ACROSS_USERS, pid, uid)
                        == PackageManager.PERMISSION_GRANTED, "Aborted removal restores the installed local owner");
                pm.remove(basic.packageName);
                rejects(SecurityException.class,
                        () -> pm.queryContentProviders("missing.process", 1010042, 0, null),
                        "Removed bound package fails closed instead of becoming a service");
                ApkPackage replacement = permissionRecord(directory, basic.packageName, uid + 1,
                        Manifest.permission.INTERACT_ACROSS_USERS);
                set(replacement, "apkPath", new File(fixtures, "v2only.apk").getPath()); pm.add(replacement);
                check(policy.checkProcessPermission(Manifest.permission.INTERACT_ACROSS_USERS, pid, uid)
                        == PackageManager.PERMISSION_DENIED, "Stale local UID cannot use a replacement's grant");
                check(policy.checkProcessPermission(Manifest.permission.INTERACT_ACROSS_USERS, pid, uid + 1)
                        == PackageManager.PERMISSION_DENIED, "A stale binding cannot forge the new registry UID");
                ApkPackage unverified = permissionRecord(directory, basic.packageName, uid,
                        Manifest.permission.INTERACT_ACROSS_USERS);
                set(unverified, "apkPath", new File(fixtures, "unsigned.apk").getPath()); pm.add(unverified);
                check(policy.checkProcessPermission(Manifest.permission.INTERACT_ACROSS_USERS, pid, uid)
                        == PackageManager.PERMISSION_DENIED, "A replacement cannot retain the old signing grant");
            });
            pm.remove(basic.packageName);
            pm.add(ordinary);
            boundApplication(ordinary, () -> {
                Object process = unsafe.allocateInstance(Class.forName(ActivityTaskManagerService.class.getName() + "$ProcessRecord"));
                set(process, "pid", pid); set(process, "uid", uid); set(process, "packageName", full.packageName);
                enrolled.add(process);
                try {
                    check(full.packageName.equals(identity.packageForPid(pid)), "Fixture attached the contradictory package");
                    check(policy.checkProcessPermission(Manifest.permission.INTERNET, pid, uid)
                            == PackageManager.PERMISSION_DENIED, "Contradictory attachment revokes even a real local grant");
                } finally { enrolled.clear(); }
            });
            check(ActivityThread.currentPackageName() == null, "No app is bound in the service-only control");
            check(policy.checkProcessPermission(Manifest.permission.INTERACT_ACROSS_USERS, pid, uid)
                    == PackageManager.PERMISSION_GRANTED, "Genuine local service without a bound app retains permission");
            check(pm.queryContentProviders("missing.process", 1010042, 0, null).getList().isEmpty(),
                    "Genuine service-local enumeration retains the authorized empty result");
            check(policy.checkProcessPermission(Manifest.permission.INTERACT_ACROSS_USERS, pid, uid + 1)
                    == PackageManager.PERMISSION_DENIED, "No-app service control still rejects an unrelated UID");
        });
        as(pid, Process.SYSTEM_UID, () -> {
            check(policy.checkProcessPermission(Manifest.permission.INTERACT_ACROSS_USERS, pid, Process.SYSTEM_UID)
                    == PackageManager.PERMISSION_GRANTED, "Actual service-local system identity stays authorized");
            check(pm.queryContentProviders("missing.process", 1010042, 0, null).getList().isEmpty(),
                    "Actual system caller retains its cross-user result");
        });
    }

    public static void main(String[] args) throws Exception {
        System.load(args[0]);
        unsafe = (Unsafe)field(Unsafe.class, "theUnsafe").get(null);
        android.app.PropertyInvalidatedCache.setTestMode(true);
        ((android.app.PropertyInvalidatedCache<?, ?>)field(PackageManager.class,
                "sPackageInfoCache").get(null)).testPropertyName();
        String selected = args[1];
        File directory = Files.createTempDirectory("provider-enumeration-").toFile();
        try {
            if (selected.equals("all") || selected.equals("visibility")) visibility(directory);
            if (selected.equals("all") || selected.equals("cross-user-outbound")) crossUserOutbound(directory);
            if (selected.equals("all") || selected.equals("cross-user-inbound")) crossUserInbound(directory);
            if (selected.equals("all") || selected.equals("cross-user-permissions")) permissionBearing(directory, new File(args[2]));
            if (selected.equals("all") || selected.equals("local-permissions")) localPermissions(directory, new File(args[2]));
            System.out.println("ProviderEnumerationPolicyHost all passed: " + checks + " checks; case=" + selected);
        } finally { directory.delete(); }
    }
}
