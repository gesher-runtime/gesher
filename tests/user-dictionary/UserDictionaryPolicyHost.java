/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package com.gesher.server;

import android.content.ComponentName;
import android.content.AttributionSource;
import android.content.ContentProvider;
import android.content.ContentProviderOperation;
import android.content.ContentValues;
import android.content.ContextWrapper;
import android.content.OperationApplicationException;
import android.content.pm.ApplicationInfo;
import android.content.pm.PackageInfo;
import android.content.pm.PackageManager;
import android.content.pm.ProviderInfo;
import android.content.pm.ResolveInfo;
import android.content.pm.ServiceInfo;
import android.database.Cursor;
import android.net.Uri;
import android.os.Binder;
import android.provider.UserDictionary.Words;
import java.io.File;
import java.lang.reflect.Field;
import java.lang.reflect.Method;
import java.lang.reflect.InvocationTargetException;
import java.nio.file.Files;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collections;
import java.util.List;
import java.util.concurrent.atomic.AtomicReference;
import android.app.AppOpsManager;
import android.view.textservice.SpellCheckerInfo;
import android.view.textservice.TextServicesManager;
import com.android.internal.pm.parsing.pkg.PackageImpl;
import com.android.internal.pm.parsing.pkg.ParsedPackage;
import com.android.internal.pm.pkg.component.ParsedProviderImpl;
import com.android.internal.textservice.ITextServicesManager;
import sun.misc.Unsafe;

/** Actual provider/PM policy with declared host identity and JNI boundaries. */
public final class UserDictionaryPolicyHost {
    private static int checks;
    private static Unsafe unsafe;
    private static Field field(Class<?> type, String name) throws Exception {
        Field result = type.getDeclaredField(name); result.setAccessible(true); return result;
    }
    private static void set(Object value, String name, Object data) throws Exception {
        field(value.getClass(), name).set(value, data);
    }
    private static ThreadLocal<AttributionSource> attribution(ContentProvider provider) throws Exception {
        ThreadLocal<AttributionSource> result = new ThreadLocal<>();
        field(ContentProvider.class, "mCallingAttributionSource").set(provider, result);
        return result;
    }
    private static ApkPackage record(File directory, String name, int uid,
            String authority, boolean enabled) throws Exception {
        return record(directory, name, uid, authority, enabled, null);
    }
    private static ApkPackage record(File directory, String name, int uid,
            String authority, boolean enabled, String requiredLibrary) throws Exception {
        PackageImpl parsed = (PackageImpl)PackageImpl.forTesting(name, new File(directory, name + ".apk").getPath());
        parsed.setTargetSdkVersion(36);
        if (requiredLibrary != null) parsed.addUsesLibrary(requiredLibrary);
        if (authority != null) {
            ParsedProviderImpl provider = new ParsedProviderImpl();
            provider.setName(name + ".Dictionary"); provider.setPackageName(name);
            provider.setAuthority(authority); provider.setProcessName(name);
            provider.setEnabled(enabled); provider.setExported(true);
            android.os.Bundle metadata = new android.os.Bundle();
            metadata.putBoolean("fixture.marker", true); provider.setMetaData(metadata);
            parsed.addProvider(provider);
        }
        parsed.hideAsFinal();
        ApkPackage apk = (ApkPackage)unsafe.allocateInstance(ApkPackage.class);
        set(apk, "packageName", name); set(apk, "parsed", parsed); set(apk, "mUid", uid);
        set(apk, "apkPath", new File(directory, name + ".apk").getPath());
        set(apk, "sharedLibraries", SharedLibraryGraph.EMPTY);
        apk.setDataDir(new File(directory, name).getPath());
        return apk;
    }
    private static void check(boolean result, String why) {
        if (!result) throw new AssertionError(why); checks++;
    }
    private interface Work { void run() throws Exception; }
    private static void as(int pid, int uid, Work action) throws Exception {
        long saved = Binder.clearCallingIdentity();
        try {
            Binder.restoreCallingIdentity(((long)uid << 32) | (pid & 0xffffffffL));
            check(Binder.getCallingPid() == pid && Binder.getCallingUid() == uid, "Declared Binder identity boundary");
            action.run();
        } finally { Binder.restoreCallingIdentity(saved); }
    }
    private static void rejects(Class<? extends Throwable> type, Work action) throws Exception {
        try { action.run(); }
        catch (Throwable problem) {
            check(type.isInstance(problem), "Expected " + type.getName() + ", got " + problem);
            return;
        }
        throw new AssertionError("Expected " + type.getName());
    }
    private static void denied(UserDictionaryProvider provider) throws Exception {
        check(Words.CONTENT_TYPE.equals(provider.getType(Words.CONTENT_URI)), "Anonymous MIME remains public");
        try (Cursor cursor = provider.query(Words.CONTENT_URI, null, "invalid SQL!", null, "invalid sort!")) {
            check(cursor.getCount() == 0, "Unauthorized query yields empty cursor before SQL");
            check(cursor.getColumnCount() == 6, "Denied default projection");
            check(cursor.getNotificationUri() == null, "Denied cursor cannot observe database changes");
        }
        try (Cursor cursor = provider.query(Uri.parse("content://user_dictionary/missing"),
                new String[]{Words.WORD, Words.WORD, Words.LOCALE}, null, null, null)) {
            check(cursor.getCount() == 0 && Arrays.equals(cursor.getColumnNames(),
                    new String[]{Words.WORD, Words.WORD, Words.LOCALE}), "Denied query ordering matches AOSP");
        }
        rejects(IllegalArgumentException.class, () -> provider.query(Words.CONTENT_URI,
                new String[]{"unknown"}, null, null, null));
        check(provider.insert(Words.CONTENT_URI, null) == null, "Denied insert precedes missing-word validation");
        check(provider.update(Uri.parse("content://user_dictionary/missing"), null, null, null) == 0,
                "Denied update precedes URI/value validation");
        check(provider.delete(Uri.parse("content://user_dictionary/missing"), null, null) == 0,
                "Denied delete precedes URI validation");
        rejects(IllegalArgumentException.class, () -> provider.insert(
                Uri.parse("content://user_dictionary/words/1"), null));
        check(provider.bulkInsert(Words.CONTENT_URI,
                new ContentValues[]{new ContentValues(), new ContentValues()}) == 2,
                "Inherited AOSP bulkInsert returns attempted rows even when inserts are denied");
        ArrayList<ContentProviderOperation> operations = new ArrayList<>();
        operations.add(ContentProviderOperation.newInsert(Words.CONTENT_URI).withValue(Words.WORD, "blocked").build());
        rejects(OperationApplicationException.class, () -> provider.applyBatch(operations));
    }
    private static void metadata(PackageManagerService pm) throws Exception {
        ProviderInfo info = pm.resolveContentProvider(UserDictionaryProvider.AUTHORITY, 0, 0);
        check(info != null && UserDictionaryProvider.PACKAGE.equals(info.packageName), "Provider authority is discoverable");
        check(info.exported && info.enabled && !info.multiprocess, "Actual provider flags");
        check(info.readPermission == null && info.writePermission == null && !info.grantUriPermissions,
                "Removed dictionary permissions do not grant access");
        check(pm.resolveContentProvider(UserDictionaryProvider.AUTHORITY, 0, 10) == null, "Unknown user does not own a dictionary");
        ComponentName component = new ComponentName(info.packageName, info.name);
        check(pm.getProviderInfo(component, 0, 0) != null && pm.getProviderInfo(component, 0, 10) == null,
                "Component lookup follows supported user");
        check(pm.resolveContentProvider(UserDictionaryProvider.AUTHORITY,
                PackageManager.MATCH_DIRECT_BOOT_AWARE, 0) == null
                && pm.getProviderInfo(component, PackageManager.MATCH_DIRECT_BOOT_AWARE, 0) == null,
                "Direct-boot-aware-only queries exclude the unaware provider");
        check(pm.resolveContentProvider(UserDictionaryProvider.AUTHORITY,
                PackageManager.MATCH_DIRECT_BOOT_UNAWARE, 0) != null, "Explicit unaware lookup");
        info.authority = "mutated"; info.applicationInfo.enabled = false;
        check(pm.resolveContentProvider(UserDictionaryProvider.AUTHORITY, 0, 0).applicationInfo.enabled,
                "Metadata copies cannot mutate provider state");
        check(pm.isPackageAvailable(UserDictionaryProvider.PACKAGE, 0), "Built-in package availability");
        check(!pm.isPackageAvailable(UserDictionaryProvider.PACKAGE, 10), "Unavailable user package");
        PackageInfo minimal = pm.getPackageInfo(UserDictionaryProvider.PACKAGE, 0, 0);
        PackageInfo full = pm.getPackageInfo(UserDictionaryProvider.PACKAGE, PackageManager.GET_PROVIDERS, 0);
        check(minimal != null && minimal.providers == null && full.providers.length == 1, "GET_PROVIDERS flag");
        check(pm.getPackageInfo(UserDictionaryProvider.PACKAGE,
                PackageManager.GET_PROVIDERS | PackageManager.MATCH_DIRECT_BOOT_AWARE, 0).providers == null,
                "Package provider inventory respects direct-boot flags");
        check(minimal.signatures == null && minimal.signingInfo == null, "No invented package signing identity");
        ApplicationInfo app = pm.getApplicationInfo(UserDictionaryProvider.PACKAGE, 0, 0);
        check((app.flags & (ApplicationInfo.FLAG_SYSTEM | ApplicationInfo.FLAG_INSTALLED | ApplicationInfo.FLAG_HAS_CODE))
                == (ApplicationInfo.FLAG_SYSTEM | ApplicationInfo.FLAG_INSTALLED | ApplicationInfo.FLAG_HAS_CODE), "Built-in code is present");
        check(app.sourceDir.endsWith("/framework/gesher-app.jar"), "Metadata points to real bundled code container");
        check(new File(app.dataDir).equals(UserDictionaryProvider.dataDirectory())
                && new File(app.dataDir).getParentFile().getName().equals("system"),
                "Runtime-owned storage is separate from ordinary APK installation/removal paths");
        check(app.uid == android.os.Process.SYSTEM_UID && pm.getPackageUid(UserDictionaryProvider.PACKAGE, 0, 0) == app.uid,
                "Package UID matches actual system-server ownership");
        check(Arrays.asList(pm.getPackagesForUid(app.uid)).contains(UserDictionaryProvider.PACKAGE), "UID package reverse lookup");
        check(pm.getPackageGids(UserDictionaryProvider.PACKAGE, 0, 0).length == 0, "Built-in supplementary GID inventory");
        check(pm.getInstalledPackages(0, 0).getList().stream().anyMatch(
                p -> UserDictionaryProvider.PACKAGE.equals(((PackageInfo)p).packageName)), "Installed package enumeration");
        check(pm.getInstalledApplications(0, 0).getList().stream().anyMatch(
                p -> UserDictionaryProvider.PACKAGE.equals(((ApplicationInfo)p).packageName)), "Installed application enumeration");
        check(pm.queryContentProviders("system", app.uid, 0, null).getList().size() == 1, "Provider process/UID enumeration");
        check(pm.queryContentProviders("system", 10042, 0, null).getList().isEmpty(), "Other UID does not own built-in provider");
        check(pm.queryContentProviders(null, 0, PackageManager.MATCH_DIRECT_BOOT_AWARE, null).getList().isEmpty(),
                "Provider enumeration respects direct-boot flags");
        check(pm.queryContentProviders(null, 0, 0, "absent-metadata").getList().isEmpty(), "Provider metadata-key filter");
        as(71303, 110042, () -> check(pm.queryContentProviders(null, 0, 0, null).getList().isEmpty(),
                "Global provider enumeration follows the supported calling user"));
        check(pm.remove(UserDictionaryProvider.PACKAGE) == null && pm.getProviderInfo(component, 0, 0) != null,
                "Ordinary registry removal cannot retire built-in provider");
        PackageVisibility visibility = new PackageVisibility(Collections.emptyMap(), false, false, false, action -> false);
        check(visibility.contains(UserDictionaryProvider.PACKAGE)
                && visibility.canSee(10042, UserDictionaryProvider.PACKAGE), "Built-in provider package visibility");
        // Constructed package metadata is isolated to this fixture. No APK runs.
        ApkPackage reserved = (ApkPackage)unsafe.allocateInstance(ApkPackage.class);
        Field name = ApkPackage.class.getDeclaredField("packageName"); name.setAccessible(true);
        name.set(reserved, UserDictionaryProvider.PACKAGE);
        try { UserDictionaryProvider.checkPackageAdmission(reserved); throw new AssertionError("Reserved package admitted"); }
        catch (android.content.pm.PackageParser.PackageParserException denied) {
            check(denied.error == PackageManager.INSTALL_FAILED_UPDATE_INCOMPATIBLE, "Builtin update has Android status");
        }
    }

    private static boolean allowed(UserDictionaryProvider provider) throws Exception {
        Method permission = UserDictionaryProvider.class.getDeclaredMethod("canCallerAccessUserDictionary");
        permission.setAccessible(true);
        try { return (Boolean)permission.invoke(provider); }
        catch (InvocationTargetException problem) {
            if (problem.getCause() instanceof Exception) throw (Exception)problem.getCause();
            throw problem;
        }
    }

    @SuppressWarnings("unchecked")
    private static void authenticatedCallers(File directory) throws Exception {
        String name = "fixture.dictionary.enabled";
        ApkPackage pkg = record(directory, name, 10042, name + ".provider", true);
        PackageManagerService pm = new PackageManagerService(List.of(pkg), false);
        check(pm.queryContentProviders(null, 0, PackageManager.MATCH_SYSTEM_ONLY, null)
                .getList().stream().noneMatch(p -> name.equals(p.packageName)),
                "System-only provider enumeration excludes ordinary installed packages");
        List<ProviderInfo> filtered = pm.queryContentProviders(null, 0, 0, "fixture.marker").getList();
        check(filtered.size() == 1 && name.equals(filtered.get(0).packageName)
                && filtered.get(0).metaData == null, "Metadata-key filtering does not require or imply GET_META_DATA");
        check(pm.queryContentProviders(null, 0, PackageManager.GET_META_DATA, "fixture.marker")
                .getList().get(0).metaData.getBoolean("fixture.marker"), "Explicit metadata values are returned");
        // Parsed manifest, process enrollment and enabled-service responses are
        // explicit host fixture boundaries; the production permission code and
        // framework AppOps/AttributionSource methods remain under test.
        ActivityTaskManagerService tasks = (ActivityTaskManagerService)unsafe.allocateInstance(ActivityTaskManagerService.class);
        set(tasks, "mPm", pm);
        Class<?> processType = Class.forName(ActivityTaskManagerService.class.getName() + "$ProcessRecord");
        Object process = unsafe.allocateInstance(processType);
        set(process, "pid", 827331); set(process, "uid", 10042); set(process, "packageName", name);
        set(tasks, "mProcesses", new ArrayList<>(List.of(process)));
        UserDictionaryProvider provider = new UserDictionaryProvider(pm, tasks, new File(directory, "auth-not-opened.db"));
        ThreadLocal<AttributionSource> source = attribution(provider);
        AppOpsManager appOps = (AppOpsManager)unsafe.allocateInstance(AppOpsManager.class);
        DeviceServices.AppOpsService appOpsService = new DeviceServices.AppOpsService();
        set(appOps, "mService", appOpsService);
        field(AppOpsManager.class, "sService").set(null, appOpsService);
        AppOpsManager.disableCheckPackageCache();
        Object transport = field(ContentProvider.class, "mTransport").get(provider);
        set(transport, "mAppOpsManager", appOps);
        TextServicesManager text = (TextServicesManager)unsafe.allocateInstance(TextServicesManager.class);
        AtomicReference<SpellCheckerInfo[]> enabled = new AtomicReference<>(new SpellCheckerInfo[0]);
        set(text, "mService", new ITextServicesManager.Default() {
            @Override public SpellCheckerInfo[] getEnabledSpellCheckers(int user) {
                check(Binder.getCallingPid() == android.os.Process.myPid(), "Service enumeration clears incoming identity");
                return enabled.get();
            }
        });
        set(provider, "mTextServiceManager", text);
        ResolveInfo service = new ResolveInfo(); service.serviceInfo = new ServiceInfo();
        service.serviceInfo.packageName = name; service.serviceInfo.name = name + ".SpellChecker";
        service.serviceInfo.applicationInfo = new ApplicationInfo();
        service.serviceInfo.applicationInfo.uid = 10042;
        SpellCheckerInfo spell = (SpellCheckerInfo)unsafe.allocateInstance(SpellCheckerInfo.class);
        set(spell, "mService", service);
        as(827331, 10042, () -> {
            source.set(new AttributionSource(10042, name, null));
            check(!allowed(provider), "Installed ordinary caller is not enabled");
            enabled.set(new SpellCheckerInfo[]{spell});
            check(allowed(provider), "Authenticated enabled spell checker admitted");
            check(Binder.getCallingPid() == 827331 && Binder.getCallingUid() == 10042, "Permission check restores incoming identity");
            source.set(new AttributionSource(10042, "fixture.dictionary.other", null));
            check(!allowed(provider), "Claimed package cannot replace authenticated process package");
            source.set(new AttributionSource(10042, name, null));
            service.serviceInfo.applicationInfo.uid = 10043;
            check(!allowed(provider), "Enabled service UID must match caller");
            service.serviceInfo.applicationInfo.uid = 10042;
            service.serviceInfo.packageName = "fixture.dictionary.other";
            check(!allowed(provider), "Enabled service package must match caller");
            service.serviceInfo.packageName = name;
            set(process, "dead", true);
            check(!allowed(provider), "Dead attachment cannot retain enabled-service access");
            set(process, "dead", false);
            check(allowed(provider), "Live enrolled caller still admitted");
            pm.beginPackageRemoval(pkg);
            check(!allowed(provider), "Pending removal revokes access before registry retirement");
            pm.endPackageRemoval(pkg);
            check(allowed(provider), "Aborted removal preserves the installed enabled owner");
            int localUid = android.os.Process.myUid();
            set(process, "pid", android.os.Process.myPid()); set(process, "uid", localUid);
            set(pkg, "mUid", localUid); service.serviceInfo.applicationInfo.uid = localUid;
            source.set(new AttributionSource(localUid, name, null));
            as(android.os.Process.myPid(), localUid, () -> {
                enabled.set(new SpellCheckerInfo[0]);
                check(!allowed(provider), "Local attached app does not inherit provider-process exemption");
                enabled.set(new SpellCheckerInfo[]{spell});
                check(allowed(provider), "Local authenticated enabled service remains usable");
            });
            set(process, "pid", 827331); set(process, "uid", 10042); set(pkg, "mUid", 10042);
            service.serviceInfo.applicationInfo.uid = 10042;
            source.set(new AttributionSource(10042, name, null));
            pm.remove(name);
            check(!allowed(provider), "Removed owner loses dictionary access");
        });
        check(!new File(directory, "auth-not-opened.db").exists(), "Policy checks do not create storage");
        for (boolean componentEnabled : new boolean[]{true, false}) {
            ApkPackage conflict = record(directory, "fixture.dictionary.conflict" + componentEnabled,
                    10044, "fixture.other;user_dictionary", componentEnabled);
            try { UserDictionaryProvider.checkPackageAdmission(conflict); throw new AssertionError("Authority alias admitted"); }
            catch (android.content.pm.PackageParser.PackageParserException denied) {
                check(denied.error == PackageManager.INSTALL_FAILED_CONFLICTING_PROVIDER,
                        "Even disabled authority alias cannot replace built-in owner");
            }
            rejects(IllegalArgumentException.class, () -> pm.prepareAdd(conflict));
        }
    }

    private static ApkPackage constructed(ApkPackage fixture) throws Exception {
        // Parsed manifests remain the explicit fixture boundary. Exercise the real
        // constructor and library resolver used after parsing, not a null-success path.
        var constructor = ApkPackage.class.getDeclaredConstructor(String.class, ParsedPackage.class);
        constructor.setAccessible(true);
        try { return constructor.newInstance(fixture.apkPath, fixture.parsed); }
        catch (InvocationTargetException failure) {
            if (failure.getCause() instanceof Exception cause) throw cause;
            throw failure;
        }
    }

    private static void admissionRejected(ApkPackage pkg, int code) throws Exception {
        try {
            UserDictionaryProvider.checkPackageAdmission(pkg);
            throw new AssertionError("Reserved provider owner admitted: " + pkg.packageName);
        } catch (android.content.pm.PackageParser.PackageParserException rejected) {
            check(rejected.error == code, "Manifest admission lost the Android install error");
        }
    }

    private static void manifestAdmission(File directory) throws Exception {
        int ordinal = 0;
        for (boolean enabled : new boolean[]{false, true}) {
            for (boolean directBoot : new boolean[]{false, true}) {
                for (String aliases : new String[]{"user_dictionary", "user_dictionary;fixture.other",
                        "fixture.first;user_dictionary;fixture.last"}) {
                    ApkPackage fixture = record(directory, "fixture.dictionary.admission" + ordinal++,
                            10044, aliases, enabled);
                    ((ParsedProviderImpl)fixture.parsed.getProviders().get(0)).setDirectBootAware(directBoot);
                    ApkPackage real = constructed(fixture);
                    check(real.sharedLibraries != null && real.sharedLibraries.isEmpty(),
                            "Real package construction did not initialize the ordinary library graph");
                    admissionRejected(real, PackageManager.INSTALL_FAILED_CONFLICTING_PROVIDER);
                    rejects(IllegalArgumentException.class, () -> new PackageManagerService(List.of(real), false));
                    // The visibility fixture has authoritative parsed declarations
                    // without installed metadata. A missing graph must not waive ownership.
                    set(fixture, "sharedLibraries", null);
                    admissionRejected(fixture, PackageManager.INSTALL_FAILED_CONFLICTING_PROVIDER);
                }
            }
        }
        for (String authority : new String[]{null, "user_dictionary.extra", "prefix.user_dictionary",
                "fixture.first;fixture.last"}) {
            ApkPackage real = constructed(record(directory, "fixture.dictionary.allowed" + ordinal++,
                    10044, authority, false));
            UserDictionaryProvider.checkPackageAdmission(real);
            PackageManagerService pm = new PackageManagerService(List.of(real), false);
            check(pm.get(real.packageName) == real, "Distinct or absent authority was rejected");
        }
        ApkPackage reserved = constructed(record(directory, UserDictionaryProvider.PACKAGE,
                10044, null, false));
        admissionRejected(reserved, PackageManager.INSTALL_FAILED_UPDATE_INCOMPATIBLE);

        ApkPackage missingLibrary = record(directory, "fixture.dictionary.missing_library",
                10044, "fixture.other;user_dictionary", false, "fixture.dictionary.not_installed");
        set(missingLibrary, "sharedLibraries", null);
        admissionRejected(missingLibrary, PackageManager.INSTALL_FAILED_CONFLICTING_PROVIDER);
        try {
            constructed(missingLibrary);
            throw new AssertionError("Real construction silently satisfied an absent required library");
        } catch (java.io.IOException rejected) {
            check(rejected.getMessage().contains("INSTALL_FAILED_MISSING_SHARED_LIBRARY"),
                    "Real library admission failed for an unrelated reason");
        }
    }

    private static void initializationFailure(File directory) throws Exception {
        Field context = field(android.app.GesherSettingsContext.class, "sContext");
        Object previous = context.get(null);
        File obstruction = new File(directory, "initialization-obstruction");
        Files.writeString(obstruction.toPath(), "ordinary filesystem failure");
        context.set(null, new ContextWrapper(null) {
            @Override public Object getSystemService(String name) { return null; }
            @Override public String getPackageName() { return "android"; }
        });
        try {
            UserDictionaryProvider provider = new UserDictionaryProvider(null, null,
                    new File(obstruction, "databases/user_dict.db"));
            IllegalStateException original;
            try {
                provider.holder();
                throw new AssertionError("Provider published after database-directory failure");
            } catch (IllegalStateException expected) {
                check(expected.getMessage().contains("Cannot create user dictionary"),
                        "Initial acquisition preserves the actual initialization error");
                original = expected;
            }
            check(provider.getContext() != null, "Real attachInfo installed context before onCreate failed");
            check(field(UserDictionaryProvider.class, "mOpenHelper").get(provider) == null,
                    "Failed initialization leaves no database helper");
            for (int attempt = 0; attempt < 3; attempt++) {
                try {
                    provider.holder();
                    throw new AssertionError("Partially attached provider was published on retry");
                } catch (IllegalStateException expected) {
                    check(expected.getCause() == original, "Retry preserves the original failure cause");
                }
            }
            Files.delete(obstruction.toPath());
            try {
                provider.holder();
                throw new AssertionError("Removing obstruction reused an irreversibly failed instance");
            } catch (IllegalStateException expected) {
                check(expected.getCause() == original, "Failed instance stays unavailable until recreated");
            }
        } finally {
            context.set(null, previous);
            Files.deleteIfExists(obstruction.toPath());
        }
    }

    public static void main(String[] args) throws Exception {
        System.load(args[0]);
        unsafe = (Unsafe)field(Unsafe.class, "theUnsafe").get(null);
        android.app.PropertyInvalidatedCache.setTestMode(true);
        ((android.app.PropertyInvalidatedCache<?, ?>)field(PackageManager.class,
                "sPackageInfoCache").get(null)).testPropertyName();
        File directory = Files.createTempDirectory("dictionary-policy-").toFile();
        try {
            File db = new File(directory, "never-opened.db");
            UserDictionaryProvider provider = new UserDictionaryProvider(null, null, db);
            // attachInfo normally creates this per-call state. This host path
            // deliberately leaves the database/context uninitialized; no storage
            // or service behavior is substituted for Android's implementation.
            attribution(provider);
            as(834121, 10042, () -> denied(provider));
            as(834121, android.os.Process.myUid(), () -> denied(provider));
            as(834121, android.os.Process.SHELL_UID, () -> denied(provider));
            check(!db.exists(), "Denied operations never open or create the database");
            Method allowed = UserDictionaryProvider.class.getDeclaredMethod("canCallerAccessUserDictionary");
            allowed.setAccessible(true);
            as(834121, android.os.Process.SYSTEM_UID,
                    () -> check((Boolean)allowed.invoke(provider), "Actual system UID admitted"));
            as(834121, android.os.Process.ROOT_UID,
                    () -> check((Boolean)allowed.invoke(provider), "Actual root UID admitted"));
            check((Boolean)allowed.invoke(provider), "Server-local provider call admitted");
            metadata(new PackageManagerService(Collections.emptyList(), false));
            authenticatedCallers(directory);
            manifestAdmission(directory);
            initializationFailure(directory);
            System.out.println("UserDictionaryPolicyHost all passed: " + checks + " checks");
        } finally { directory.delete(); }
    }
}
