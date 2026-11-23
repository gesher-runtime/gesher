/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package gesher.services;

import android.app.ContentProviderHolder;
import android.app.IApplicationThread;
import android.content.ComponentName;
import android.content.IContentProvider;
import android.content.pm.ApplicationInfo;
import android.content.pm.PackageManager;
import android.content.pm.ProviderInfo;
import android.content.pm.Signature;
import android.content.res.AssetManager;
import android.os.Binder;
import android.os.IBinder;
import android.os.Bundle;
import android.system.Os;
import com.android.internal.pm.parsing.pkg.PackageImpl;
import com.android.internal.pm.pkg.component.ParsedProviderImpl;
import com.gesher.server.ActivityTaskManagerService;
import com.gesher.server.ApkPackage;
import com.gesher.server.PackageManagerService;
import com.gesher.server.SharedLibraryGraph;
import java.lang.reflect.Constructor;
import java.lang.reflect.Field;
import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Method;
import java.lang.reflect.Proxy;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.Collection;
import java.util.Comparator;
import java.util.List;
import java.util.Map;
import java.util.stream.Stream;
import sun.misc.Unsafe;

/**
 * ContentProviders.providersFor orders providers by initOrder descending and keeps tie
 * order, with one entry per provider component: a provider declared twice (as merged
 * library manifests do) is bound once, from its first declaration.
 */
public final class ProviderInitOrderTest {
    private static final String PKG = "fixture.provider.initorder";
    private static final int UID = 12346;

    private static void require(boolean value, String reason) {
        if (!value) throw new AssertionError(reason);
    }

    private static Field field(Class<?> type, String name) throws Exception {
        Field f = type.getDeclaredField(name);
        f.setAccessible(true);
        return f;
    }

    private static Method method(Class<?> type, String name, Class<?>... parameters) throws Exception {
        Method m = type.getDeclaredMethod(name, parameters);
        m.setAccessible(true);
        return m;
    }

    private static Object construct(Class<?> type, Class<?>[] parameters, Object... args) throws Exception {
        Constructor<?> c = type.getDeclaredConstructor(parameters);
        c.setAccessible(true);
        try {
            return c.newInstance(args);
        } catch (InvocationTargetException failure) {
            Throwable cause = failure.getCause();
            if (cause instanceof Exception) throw (Exception) cause;
            if (cause instanceof Error) throw (Error) cause;
            throw failure;
        }
    }

    private static ParsedProviderImpl addProvider(PackageImpl parsed, String shortName, String processName,
            int initOrder, boolean enabled) {
        ParsedProviderImpl provider = new ParsedProviderImpl();
        provider.setName(PKG + "." + shortName);
        provider.setPackageName(PKG);
        provider.setAuthority(PKG + "." + shortName);
        provider.setProcessName(processName);
        provider.setInitOrder(initOrder);
        provider.setEnabled(enabled);
        provider.setExported(false);
        parsed.addProvider(provider);
        return provider;
    }

    private static List<String> names(List<ProviderInfo> infos) {
        List<String> result = new ArrayList<>(infos.size());
        for (ProviderInfo info : infos) result.add(info.name);
        return result;
    }

    private static ApkPackage packageFor(PackageImpl parsed) throws Exception {
        Field unsafeField = Unsafe.class.getDeclaredField("theUnsafe");
        unsafeField.setAccessible(true);
        Unsafe unsafe = (Unsafe) unsafeField.get(null);
        parsed.hideAsFinal();
        ApkPackage pkg = (ApkPackage) unsafe.allocateInstance(ApkPackage.class);
        field(ApkPackage.class, "packageName").set(pkg, PKG);
        field(ApkPackage.class, "parsed").set(pkg, parsed);
        field(ApkPackage.class, "apkPath").set(pkg, parsed.getBaseApkPath());
        field(ApkPackage.class, "mDataDir").set(pkg, "/fixture/" + PKG + "/data");
        field(ApkPackage.class, "mUid").set(pkg, UID);
        field(ApkPackage.class, "sharedLibraries").set(pkg, SharedLibraryGraph.EMPTY);
        field(ApkPackage.class, "mSigningIdentityChecked").set(pkg, true);
        field(ApkPackage.class, "mVerifiedSignatures").set(pkg,
                new Signature[] { new Signature(new byte[] { 1 }) });
        return pkg;
    }

    private static PackageImpl manifest() {
        PackageImpl parsed = (PackageImpl) PackageImpl.forTesting(PKG, "/fixture/" + PKG + "/base.apk");
        parsed.setEnabled(true);
        parsed.setTargetSdkVersion(36);
        parsed.setProcessName(PKG);
        return parsed;
    }

    private static ProviderInfo named(List<ProviderInfo> providers, String shortName) {
        ProviderInfo found = null;
        for (ProviderInfo info : providers) {
            if (!(PKG + "." + shortName).equals(info.name)) continue;
            require(found == null, "Provider component appeared twice: " + shortName);
            found = info;
        }
        return found;
    }

    /** Also run on a host JVM with only native framework initialization adapted. */
    public static void declarationContracts() throws Exception {
        PackageImpl parsed = manifest();
        ParsedProviderImpl first = addProvider(parsed, "Canonical", PKG, 40, true);
        first.setReadPermission(PKG + ".READ");
        first.setWritePermission(PKG + ".WRITE");
        Bundle metadata = new Bundle();
        metadata.putString("declaration", "first");
        first.setMetaData(metadata);
        addProvider(parsed, "Canonical", PKG + ":remote", 50, true)
                .setAuthority(PKG + ".later");
        addProvider(parsed, "Identical", PKG, 10, true);
        addProvider(parsed, "Identical", PKG, 10, true);
        addProvider(parsed, "Disabled", PKG, 20, false);
        addProvider(parsed, "Disabled", PKG, 30, true);
        addProvider(parsed, "Enabled", PKG, 60, true);
        addProvider(parsed, "Enabled", PKG, 70, false);
        addProvider(parsed, "Unaware", PKG, 80, true);
        addProvider(parsed, "Unaware", PKG, 90, true).setDirectBootAware(true);
        addProvider(parsed, "Aware", PKG, 100, true).setDirectBootAware(true);
        addProvider(parsed, "Aware", PKG, 110, true);
        ApkPackage pkg = packageFor(parsed);
        List<ProviderInfo> enabled = pkg.providers(PackageManager.GET_META_DATA);
        require(enabled.size() == 5, "Registered provider count includes a duplicate or disabled first");
        ProviderInfo canonical = named(enabled, "Canonical");
        require(canonical != null && canonical.initOrder == 40 && PKG.equals(canonical.processName)
                        && (PKG + ".Canonical").equals(canonical.authority)
                        && (PKG + ".READ").equals(canonical.readPermission)
                        && (PKG + ".WRITE").equals(canonical.writePermission)
                        && "first".equals(canonical.metaData.getString("declaration")),
                "First provider metadata was replaced by a later declaration");
        require(named(enabled, "Identical") != null, "Identical duplicate lost its single component");
        require(named(enabled, "Disabled") == null, "Enabled duplicate replaced disabled first declaration");
        require(named(enabled, "Enabled").initOrder == 60, "Disabled duplicate replaced enabled first declaration");
        require(named(pkg.providers(PackageManager.MATCH_DIRECT_BOOT_AWARE), "Unaware") == null,
                "Boot-aware duplicate replaced the canonical unaware component");
        require(named(pkg.providers(PackageManager.MATCH_DIRECT_BOOT_UNAWARE), "Aware") == null,
                "Boot-unaware duplicate replaced the canonical aware component");
        require(named(pkg.providers(PackageManager.MATCH_DIRECT_BOOT_AWARE), "Aware").initOrder == 100,
                "Canonical aware component lost its metadata");
        require(named(pkg.providers(PackageManager.MATCH_DIRECT_BOOT_UNAWARE), "Unaware").initOrder == 80,
                "Canonical unaware component lost its metadata");
        ProviderInfo disabled = named(pkg.providers(PackageManager.MATCH_DISABLED_COMPONENTS), "Disabled");
        require(disabled != null && !disabled.enabled && disabled.initOrder == 20,
                "Disabled component query did not preserve the first declaration");
        Method setEnabled = method(ApkPackage.class, "setComponentEnabledState", String.class, int.class);
        setEnabled.invoke(pkg, PKG + ".Disabled", PackageManager.COMPONENT_ENABLED_STATE_ENABLED);
        require(named(pkg.providers(0), "Disabled").initOrder == 20,
                "Installed enable override did not use the first provider metadata");
        setEnabled.invoke(pkg, PKG + ".Canonical", PackageManager.COMPONENT_ENABLED_STATE_DISABLED);
        require(named(pkg.providers(0), "Canonical") == null, "Installed disable override was ignored");
        setEnabled.invoke(pkg, PKG + ".Canonical", PackageManager.COMPONENT_ENABLED_STATE_DEFAULT);

        Field unsafeField = Unsafe.class.getDeclaredField("theUnsafe");
        unsafeField.setAccessible(true);
        Unsafe unsafe = (Unsafe) unsafeField.get(null);
        PackageManagerService pm = (PackageManagerService) unsafe.allocateInstance(PackageManagerService.class);
        field(PackageManagerService.class, "mPackages").set(pm, Map.of(PKG, pkg));
        field(PackageManagerService.class, "mRemoving").set(pm, Map.of());
        Class<?> cpType = Class.forName("com.gesher.server.ContentProviders");
        Method declared = method(cpType, "declaredProvider", PackageManagerService.class, String.class);
        ProviderInfo authority = (ProviderInfo) declared.invoke(null, pm, PKG + ".Canonical");
        require(authority != null && authority.initOrder == 40 && (PKG + ".READ").equals(authority.readPermission),
                "Authority consumer did not use canonical metadata");
        require(declared.invoke(null, pm, PKG + ".later") == null,
                "Ignored duplicate introduced an authority");
        require(pm.resolveContentProvider(PKG + ".Canonical", 0, 0).initOrder == 40
                        && pm.resolveContentProvider(PKG + ".later", 0, 0) == null,
                "PackageManager authority resolution disagrees with provider registration");
        require(pm.getProviderInfo(new ComponentName(PKG, PKG + ".Canonical"), 0, 0).initOrder == 40,
                "PackageManager component lookup disagrees with provider registration");
        System.out.println("Provider declaration contracts passed");
    }

    @SuppressWarnings("unchecked")
    public static void run() throws Exception {
        declarationContracts();

        PackageImpl parsed = manifest();
        addProvider(parsed, "ZeroFirst", PKG, 0, true);
        addProvider(parsed, "Negative", PKG, -10, true);
        addProvider(parsed, "HundredFirst", PKG, 100, true);
        addProvider(parsed, "DisabledHigh", PKG, 500, false);
        addProvider(parsed, "OtherProcessHigh", PKG + ":remote", 999, true);
        addProvider(parsed, "MinInt", PKG, Integer.MIN_VALUE, true);
        addProvider(parsed, "MaxInt", PKG, Integer.MAX_VALUE, true);
        addProvider(parsed, "HundredSecond", PKG, 100, true);
        addProvider(parsed, "ZeroSecond", PKG, 0, true);
        // A duplicate cannot move a registered provider to another process or replace
        // the first declaration's lifecycle order and permission metadata.
        addProvider(parsed, "Duplicated", PKG, 40, true).setReadPermission(PKG + ".READ");
        addProvider(parsed, "Duplicated", PKG + ":remote", 50, true);
        addProvider(parsed, "DisabledDuplicate", PKG, 70, false);
        addProvider(parsed, "DisabledDuplicate", PKG, 80, true);
        ApkPackage pkg = packageFor(parsed);

        PackageManagerService pm = (PackageManagerService) construct(PackageManagerService.class,
                new Class<?>[] { Collection.class, boolean.class }, List.of(pkg), false);
        ActivityTaskManagerService atm = (ActivityTaskManagerService) construct(
                ActivityTaskManagerService.class,
                new Class<?>[] { PackageManagerService.class }, pm);
        Class<?> cpType = Class.forName("com.gesher.server.ContentProviders");
        Object providers = construct(cpType,
                new Class<?>[] { PackageManagerService.class, ActivityTaskManagerService.class },
                pm, atm);

        Class<?> processType = Class.forName("com.gesher.server.ActivityTaskManagerService$ProcessRecord");
        IApplicationThread thread = new IApplicationThread.Default() {
            private final IBinder binder = new Binder();
            @Override public IBinder asBinder() { return binder; }
        };
        Object process;
        long oldId = Binder.clearCallingIdentity();
        try {
            Binder.restoreCallingIdentity(((long) UID << 32) | 912345L);
            process = construct(processType,
                    new Class<?>[] { ActivityTaskManagerService.class, int.class, IApplicationThread.class },
                    atm, 912345, thread);
        } finally {
            Binder.restoreCallingIdentity(oldId);
        }
        method(ActivityTaskManagerService.class, "assignPackage",
                processType, String.class, String.class).invoke(atm, process, PKG, PKG);

        ApplicationInfo mainAi = pm.getApplicationInfo(PKG, 0, 0);
        Method forAi = method(cpType, "providersFor", ApplicationInfo.class);
        Method forProcess = method(cpType, "providersFor", processType, ApplicationInfo.class);

        List<String> expected = List.of(
                PKG + ".MaxInt",
                PKG + ".HundredFirst",
                PKG + ".HundredSecond",
                PKG + ".Duplicated",
                PKG + ".ZeroFirst",
                PKG + ".ZeroSecond",
                PKG + ".Negative",
                PKG + ".MinInt");

        List<ProviderInfo> byAi = (List<ProviderInfo>) forAi.invoke(providers, mainAi);
        require(names(byAi).equals(expected),
                "providersFor(ApplicationInfo) wrong order: " + names(byAi));

        for (ProviderInfo info : byAi) {
            if (info.name.equals(PKG + ".Duplicated")) {
                require(info.initOrder == 40 && PKG.equals(info.processName)
                                && (PKG + ".READ").equals(info.readPermission),
                        "Duplicated provider replaced its first declaration metadata");
            }
        }

        List<ProviderInfo> byProcess = (List<ProviderInfo>) forProcess.invoke(providers, process, mainAi);
        require(names(byProcess).equals(expected),
                "providersFor(ProcessRecord, ApplicationInfo) wrong order: " + names(byProcess));

        ApplicationInfo remoteAi = new ApplicationInfo(mainAi);
        remoteAi.processName = PKG + ":remote";
        List<ProviderInfo> remoteProviders = (List<ProviderInfo>) forAi.invoke(providers, remoteAi);
        require(names(remoteProviders).equals(List.of(PKG + ".OtherProcessHigh")),
                "providersFor(remote ApplicationInfo) wrong list: " + names(remoteProviders));

        method(ApkPackage.class, "setComponentEnabledState", String.class, int.class)
                .invoke(pkg, PKG + ".HundredFirst", PackageManager.COMPONENT_ENABLED_STATE_DISABLED);
        List<ProviderInfo> afterDisable = (List<ProviderInfo>) forProcess.invoke(providers, process, mainAi);
        List<String> expectedAfterDisable = List.of(
                PKG + ".MaxInt",
                PKG + ".HundredSecond",
                PKG + ".Duplicated",
                PKG + ".ZeroFirst",
                PKG + ".ZeroSecond",
                PKG + ".Negative",
                PKG + ".MinInt");
        require(names(afterDisable).equals(expectedAfterDisable),
                "providersFor after disabling component wrong order: " + names(afterDisable));

        Map<?, ?> installing = (Map<?, ?>) field(cpType, "mInstalling").get(providers);
        require(installing.containsKey(process), "mInstalling missing target process");

        sameAuthorityPublication();
        System.out.println("ProviderInitOrderTest all passed");
    }

    /**
     * Two provider classes of one package declaring one authority, as merged library
     * manifests can (issue #316): ComponentResolver registers the first declaration,
     * queryContentProviders omits the skipped one from bindApplication, and the
     * registered provider's publication succeeds.
     */
    @SuppressWarnings("unchecked")
    private static void sameAuthorityPublication() throws Exception {
        String authority = PKG + ".shared.fileprovider";
        PackageImpl parsed = manifest();
        addProvider(parsed, "Primary", PKG, 0, true).setAuthority(authority);
        // A higher initOrder would install the later declaration first if it were bound.
        addProvider(parsed, "Secondary", PKG, 50, true).setAuthority(authority);
        // Syncable: Android registers its free authority on a non-syncable copy.
        ParsedProviderImpl partial = addProvider(parsed, "Partial", PKG, 10, true);
        partial.setAuthority(authority + ";" + PKG + ".partial");
        partial.setSyncable(true);
        addProvider(parsed, "Independent", PKG, 5, true);
        ApkPackage pkg = packageFor(parsed);
        PackageManagerService pm = (PackageManagerService) construct(PackageManagerService.class,
                new Class<?>[] { Collection.class, boolean.class }, List.of(pkg), false);
        ActivityTaskManagerService atm = (ActivityTaskManagerService) construct(
                ActivityTaskManagerService.class, new Class<?>[] { PackageManagerService.class }, pm);
        Class<?> cpType = Class.forName("com.gesher.server.ContentProviders");
        Object providers = construct(cpType,
                new Class<?>[] { PackageManagerService.class, ActivityTaskManagerService.class }, pm, atm);
        Class<?> processType = Class.forName("com.gesher.server.ActivityTaskManagerService$ProcessRecord");
        IApplicationThread thread = new IApplicationThread.Default() {
            private final IBinder binder = new Binder();
            @Override public IBinder asBinder() { return binder; }
        };
        Object process;
        long oldId = Binder.clearCallingIdentity();
        try {
            Binder.restoreCallingIdentity(((long) UID << 32) | 912346L);
            process = construct(processType,
                    new Class<?>[] { ActivityTaskManagerService.class, int.class, IApplicationThread.class },
                    atm, 912346, thread);
        } finally {
            Binder.restoreCallingIdentity(oldId);
        }
        method(ActivityTaskManagerService.class, "assignPackage",
                processType, String.class, String.class).invoke(atm, process, PKG, PKG);
        synchronized (atm) {
            ((List<Object>) field(ActivityTaskManagerService.class, "mProcesses").get(atm)).add(process);
        }

        ApplicationInfo ai = pm.getApplicationInfo(PKG, 0, 0);
        List<ProviderInfo> bound = (List<ProviderInfo>) method(cpType, "providersFor", processType,
                ApplicationInfo.class).invoke(providers, process, ai);
        require(names(bound).equals(List.of(PKG + ".Partial", PKG + ".Independent", PKG + ".Primary")),
                "bindApplication providers for a shared authority: " + names(bound));
        ProviderInfo primary = named(bound, "Primary");
        require(authority.equals(primary.authority), "First declaration lost its authority");
        require((PKG + ".partial").equals(named(bound, "Partial").authority),
                "Partially skipped provider was bound with an authority it does not own");
        require(!named(bound, "Partial").isSyncable,
                "Syncable provider's later authority kept the first authority's syncable state");
        ProviderInfo secondary = pm.getProviderInfo(new ComponentName(PKG, PKG + ".Secondary"), 0, 0);
        require(secondary != null && secondary.authority == null,
                "Skipped declaration is not a registered component without an authority");

        Method publish = method(cpType, "publishContentProviders", processType, List.class);
        List<ContentProviderHolder> holders = new ArrayList<>();
        for (ProviderInfo info : bound) holders.add(holder(info));
        publish.invoke(providers, process, holders);
        Map<String, ContentProviderHolder> published =
                (Map<String, ContentProviderHolder>) field(cpType, "mPublished").get(providers);
        require(published.get(authority) != null && (PKG + ".Primary").equals(published.get(authority).info.name),
                "Registered provider was not published for its authority");
        require(published.get(PKG + ".partial") != null
                        && (PKG + ".Partial").equals(published.get(PKG + ".partial").info.name),
                "Partially skipped provider was not published for its own authority");
        require(published.get(PKG + ".Independent") != null, "Unrelated provider was not published");

        // A provider that was never bound has no admission; Android ignores its publication.
        ProviderInfo forged = new ProviderInfo(primary);
        forged.name = PKG + ".Secondary";
        publish.invoke(providers, process, List.of(holder(forged)));
        require((PKG + ".Primary").equals(published.get(authority).info.name),
                "Skipped declaration replaced the registered provider");

        ContentProviderHolder acquired = (ContentProviderHolder) method(cpType, "getContentProvider",
                processType, String.class, String.class).invoke(providers, null, null, authority);
        require(acquired != null && (PKG + ".Primary").equals(acquired.info.name)
                        && acquired.provider == published.get(authority).provider,
                "Authority acquisition did not reach the registered provider");
        System.out.println("Provider same-package authority contracts passed");
    }

    private static ContentProviderHolder holder(ProviderInfo info) {
        ContentProviderHolder holder = new ContentProviderHolder(new ProviderInfo(info));
        holder.info.applicationInfo = new ApplicationInfo(info.applicationInfo);
        Binder binder = new Binder("android.content.IContentProvider");
        holder.provider = (IContentProvider) Proxy.newProxyInstance(
                IContentProvider.class.getClassLoader(), new Class<?>[] { IContentProvider.class },
                (p, m, a) -> m.getName().equals("asBinder") ? binder : null);
        return holder;
    }

    public static void main(String[] args) throws Exception {
        System.loadLibrary("android_runtime");
        AssetManager.createSystemAssetsInZygoteLocked(false,
                System.getenv("ANDROID_ROOT") + "/framework/framework-res.apk");
        String previousData = System.getenv("ANDROID_DATA");
        Path data = Files.createTempDirectory("gesher-provider-init-order-");
        try {
            Os.setenv("ANDROID_DATA", data.toString(), true);
            run();
        } finally {
            if (previousData == null) Os.unsetenv("ANDROID_DATA");
            else Os.setenv("ANDROID_DATA", previousData, true);
            try (Stream<Path> walk = Files.walk(data)) {
                for (Path p : (Iterable<Path>) walk.sorted(Comparator.reverseOrder())::iterator) {
                    Files.deleteIfExists(p);
                }
            }
        }
    }
}
