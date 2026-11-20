/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package gesher.services;

import android.app.IApplicationThread;
import android.content.pm.ApplicationInfo;
import android.content.pm.PackageManager;
import android.content.pm.ProviderInfo;
import android.content.pm.Signature;
import android.content.res.AssetManager;
import android.os.Binder;
import android.os.IBinder;
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
 * library manifests do) is bound once, from its later declaration.
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

    private static void addProvider(PackageImpl parsed, String shortName, String processName,
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
    }

    private static List<String> names(List<ProviderInfo> infos) {
        List<String> result = new ArrayList<>(infos.size());
        for (ProviderInfo info : infos) result.add(info.name);
        return result;
    }

    @SuppressWarnings("unchecked")
    public static void run() throws Exception {
        Field unsafeField = Unsafe.class.getDeclaredField("theUnsafe");
        unsafeField.setAccessible(true);
        Unsafe unsafe = (Unsafe) unsafeField.get(null);

        PackageImpl parsed = (PackageImpl) PackageImpl.forTesting(PKG, "/fixture/" + PKG + "/base.apk");
        parsed.setEnabled(true);
        parsed.setTargetSdkVersion(36);
        parsed.setProcessName(PKG);
        addProvider(parsed, "ZeroFirst", PKG, 0, true);
        addProvider(parsed, "Negative", PKG, -10, true);
        addProvider(parsed, "HundredFirst", PKG, 100, true);
        addProvider(parsed, "DisabledHigh", PKG, 500, false);
        addProvider(parsed, "OtherProcessHigh", PKG + ":remote", 999, true);
        addProvider(parsed, "MinInt", PKG, Integer.MIN_VALUE, true);
        addProvider(parsed, "MaxInt", PKG, Integer.MAX_VALUE, true);
        addProvider(parsed, "HundredSecond", PKG, 100, true);
        addProvider(parsed, "ZeroSecond", PKG, 0, true);
        // Declared twice with one authority, as Firefox declares
        // androidx.startup.InitializationProvider; Android keeps the later declaration.
        addProvider(parsed, "Duplicated", PKG, 40, true);
        addProvider(parsed, "Duplicated", PKG, 50, true);
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
                require(info.initOrder == 50, "Duplicated provider kept its earlier declaration");
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

        System.out.println("ProviderInitOrderTest all passed");
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
