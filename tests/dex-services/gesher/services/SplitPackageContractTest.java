/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package gesher.services;

import android.app.LoadedApk;
import android.content.ComponentName;
import android.content.pm.ApplicationInfo;
import android.content.pm.ComponentInfo;
import android.content.pm.PackageInfo;
import android.content.pm.PackageManager;
import android.content.pm.ProviderInfo;
import android.content.res.ApkAssets;
import android.content.res.AssetManager;
import android.content.res.CompatibilityInfo;
import android.content.res.Configuration;
import android.content.res.Resources;
import android.os.Looper;
import android.util.DisplayMetrics;
import com.gesher.server.ApkPackage;
import com.gesher.server.PackageManagerService;
import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.lang.reflect.Constructor;
import java.lang.reflect.Field;
import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Method;
import java.nio.file.Files;
import java.util.Arrays;
import java.util.Collections;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.function.Consumer;
import java.util.zip.ZipEntry;
import java.util.zip.ZipFile;
import java.util.zip.ZipOutputStream;

/** Real Installer publication, AOSP cluster parsing, split DEX loaders and resources. */
public final class SplitPackageContractTest {
    private static final String PACKAGE = "com.gesher.splitcontract";
    private static int checks;
    private static void require(boolean value, String reason) {
        if (!value) throw new AssertionError(reason);
        checks++;
    }
    private static Object construct(Class<?> type, Class<?>[] params, Object... args) throws Exception {
        Constructor<?> c = type.getDeclaredConstructor(params); c.setAccessible(true); return c.newInstance(args);
    }
    private static Method method(Class<?> type, String name, Class<?>... params) throws Exception {
        Method m = type.getDeclaredMethod(name, params); m.setAccessible(true); return m;
    }
    private static Field field(Class<?> type, String name) throws Exception {
        Field f = type.getDeclaredField(name); f.setAccessible(true); return f;
    }
    private static File cluster(File data, String name, Map<String, byte[]> files) throws IOException {
        File archive = new File(data, name + ".apks");
        try (ZipOutputStream out = new ZipOutputStream(new FileOutputStream(archive))) {
            for (Map.Entry<String, byte[]> entry : files.entrySet()) {
                out.putNextEntry(new ZipEntry(entry.getKey()));
                out.write(entry.getValue()); out.closeEntry();
            }
        }
        return archive;
    }
    private static void component(ComponentInfo component, String suffix, ApplicationInfo ai) {
        require((PACKAGE + "." + suffix).equals(component.name)
                && PACKAGE.equals(component.packageName)
                && "feature.entry".equals(component.splitName)
                && (PACKAGE + ":feature").equals(component.processName),
                "Feature component manifest identity/process was lost: " + suffix);
        require(component.applicationInfo.uid == ai.uid
                && ai.sourceDir.equals(component.applicationInfo.sourceDir),
                "Feature component changed package ownership: " + suffix);
    }
    private static ApplicationInfo metadata(ApkPackage pkg) {
        PackageInfo info = pkg.packageInfo(ApkPackage.ALL);
        ApplicationInfo ai = info.applicationInfo;
        require(Arrays.equals(ai.splitNames, new String[]{"feature.core", "feature.entry"})
                && Arrays.equals(info.splitNames, ai.splitNames),
                "Split names came from filenames or did not survive PackageInfo generation");
        require(ai.splitSourceDirs.length == 2
                && ai.splitSourceDirs[0].endsWith("/split_z_core.apk")
                && ai.splitSourceDirs[1].endsWith("/split_a_entry.apk")
                && Arrays.equals(ai.splitSourceDirs, ai.splitPublicSourceDirs),
                "Split paths were not aligned with sorted manifest names");
        require(Arrays.equals(ai.splitClassLoaderNames, new String[]{"dalvik.system.PathClassLoader",
                "dalvik.system.DelegateLastClassLoader"}) && ai.requestsIsolatedSplitLoading(),
                "Isolated split class-loader policy was discarded");
        require(ai.splitDependencies != null && Arrays.equals(ai.splitDependencies.get(0), new int[]{-1})
                && Arrays.equals(ai.splitDependencies.get(1), new int[]{0})
                && Arrays.equals(ai.splitDependencies.get(2), new int[]{1}),
                "Feature-to-feature dependency graph is missing or incorrectly indexed");
        require(info.activities != null && info.activities.length == 1
                && info.services != null && info.services.length == 1
                && info.providers != null && info.providers.length == 1,
                "Feature activity/service/provider was not merged into the package");
        component(info.activities[0], "FeatureActivity", ai);
        component(info.services[0], "FeatureService", ai);
        component(info.providers[0], "FeatureProvider", ai);
        require(!info.services[0].exported && !info.providers[0].exported
                && (PACKAGE + ".READ").equals(info.providers[0].readPermission),
                "Feature privacy/permission declarations changed");
        require(info.permissions != null && info.permissions.length == 1
                && (PACKAGE + ".READ").equals(info.permissions[0].name)
                && PACKAGE.equals(info.permissions[0].packageName)
                && info.permissions[0].protectionLevel == android.content.pm.PermissionInfo.PROTECTION_SIGNATURE,
                "Split provider's signature permission lost its defining owner");
        require((PACKAGE + ".FeatureActivity").equals(pkg.launcherActivity(null).name),
                "Feature launcher activity was not discoverable");
        return ai;
    }
    private static void loadFeature(ApplicationInfo ai) throws Exception {
        // A null ActivityThread is AOSP's supported preloading path: exercise the real
        // LoadedApk loaders without registering this test process as the installed app.
        LoadedApk loaded = new LoadedApk(null, ai, CompatibilityInfo.DEFAULT_COMPATIBILITY_INFO,
                null, false, true, false);
        Method splitLoader = method(LoadedApk.class, "getSplitClassLoader", String.class);
        ClassLoader core = (ClassLoader) splitLoader.invoke(loaded, "feature.core");
        ClassLoader entry = (ClassLoader) splitLoader.invoke(loaded, "feature.entry");
        require(entry.getParent() == core && core.getParent() == loaded.getClassLoader(),
                "Declared feature dependency did not select the parent class loader");
        require("dalvik.system.DelegateLastClassLoader".equals(entry.getClass().getName()),
                "Feature's manifest-selected loader implementation was ignored");
        require("base:core:entry".equals(entry.loadClass(PACKAGE + ".EntryMarker")
                .getMethod("value").invoke(null)), "Split DEX could not call its base/feature dependency");
        require(entry.loadClass(PACKAGE + ".FeatureActivity").getClassLoader() == entry
                && entry.loadClass(PACKAGE + ".FeatureProvider").getClassLoader() == entry,
                "Feature components were defined by the wrong loader");
        try {
            loaded.getClassLoader().loadClass(PACKAGE + ".EntryMarker");
            throw new AssertionError("Base loader gained an isolated feature's classes");
        } catch (ClassNotFoundException expected) { checks++; }
        try {
            splitLoader.invoke(loaded, "feature.absent");
            throw new AssertionError("Unknown split loaded");
        } catch (InvocationTargetException expected) {
            require(expected.getCause() instanceof PackageManager.NameNotFoundException,
                    "Unknown split did not preserve NameNotFoundException");
        }
        String[] paths = (String[]) method(LoadedApk.class, "getSplitPaths", String.class)
                .invoke(loaded, "feature.entry");
        require(Arrays.equals(paths, ai.splitSourceDirs), "Feature resources omitted a dependency");
        AssetManager.Builder builder = new AssetManager.Builder().addApkAssets(ApkAssets.loadFromPath(ai.sourceDir));
        for (String path : paths) builder.addApkAssets(ApkAssets.loadFromPath(path));
        try (AssetManager assets = builder.build()) {
            DisplayMetrics metrics = new DisplayMetrics(); metrics.setToDefaults();
            Configuration config = new Configuration(); config.setToDefaults();
            Resources resources = new Resources(assets, metrics, config);
            for (String part : new String[]{"base", "core", "entry"}) {
                int id = resources.getIdentifier(part + "_value", "string", PACKAGE);
                require(id != 0 && (part + "-resource").equals(resources.getString(id)),
                        "Missing real compiled " + part + " resource");
            }
        }
    }
    @SuppressWarnings("unchecked")
    public static void main(String[] args) throws Exception {
        require(args.length == 1, "Pass split-package-fixtures.zip");
        System.loadLibrary("android_runtime");
        AssetManager.createSystemAssetsInZygoteLocked(false,
                System.getenv("ANDROID_ROOT") + "/framework/framework-res.apk");
        if (Looper.myLooper() == null) Looper.prepareMainLooper();
        File data = Files.createTempDirectory("gesher-split-package-").toFile();
        android.system.Os.setenv("ANDROID_DATA", data.getPath(), true);
        android.system.Os.setenv("GESHER_MICROG_SPOOF", "0", true);
        Map<String, byte[]> inputs = new LinkedHashMap<>();
        try (ZipFile zip = new ZipFile(args[0])) {
            for (String name : new String[]{"base", "core", "entry", "core-major", "foreign"}) {
                try (java.io.InputStream in = zip.getInputStream(zip.getEntry(name + ".apk"))) {
                    inputs.put(name, in.readAllBytes());
                }
            }
        }
        Map<String, byte[]> good = new LinkedHashMap<>();
        good.put("base.apk", inputs.get("base"));
        // Physical order intentionally contradicts manifest name/dependency order.
        good.put("split_a_entry.apk", inputs.get("entry"));
        good.put("split_z_core.apk", inputs.get("core"));
        File archive = cluster(data, "valid", good);
        PackageManagerService pm = new PackageManagerService(Collections.emptyList());
        field(PackageManagerService.class, "mPackageEventSender").set(pm, (Consumer<android.content.Intent>) event -> {});
        Class<?> atmType = Class.forName("com.gesher.server.ActivityTaskManagerService");
        Object atm = construct(atmType, new Class<?>[]{PackageManagerService.class}, pm);
        Class<?> installerType = Class.forName("com.gesher.server.Installer");
        Object installer = construct(installerType, new Class<?>[]{PackageManagerService.class, atmType, Map.class},
                pm, atm, new LinkedHashMap<>());
        Method install = method(installerType, "install", String.class);
        String[] result = (String[]) install.invoke(installer, archive.getPath());
        require("installed".equals(result[1]), "Valid cluster was not installed");
        ApplicationInfo ai = metadata(pm.get(PACKAGE));
        loadFeature(ai);
        Class<?> notifications = Class.forName("com.gesher.server.NotificationManagerService");
        Class<?> amType = Class.forName("com.gesher.server.ActivityManagerService");
        Object am = construct(amType, new Class<?>[]{atmType, PackageManagerService.class, notifications},
                atm, pm, construct(notifications, new Class<?>[0]));
        Object providers = field(amType, "mProviders").get(am);
        Method providersFor = method(providers.getClass(), "providersFor", ApplicationInfo.class);
        ApplicationInfo secondary = new ApplicationInfo(ai); secondary.processName = PACKAGE + ":feature";
        require(((List<ProviderInfo>) providersFor.invoke(providers, ai)).isEmpty()
                && ((List<ProviderInfo>) providersFor.invoke(providers, secondary)).size() == 1,
                "Feature provider was assigned to the wrong app process");
        require(pm.getProviderInfo(new ComponentName("fixture.not.installed",
                PACKAGE + ".FeatureProvider"), 0, 0) == null,
                "Feature provider became owned by an unrelated package");
        result = (String[]) install.invoke(installer, archive.getPath());
        require("unchanged".equals(result[1]), "Same archive did not use unchanged path");
        metadata(pm.get(PACKAGE));
        // A lone unchanged base keeps its installed splits, including their graph.
        File baseOnly = new File(data, "base-only.apk"); Files.write(baseOnly.toPath(), inputs.get("base"));
        result = (String[]) install.invoke(installer, baseOnly.getPath());
        require("unchanged".equals(result[1]), "Unchanged lone base discarded installed splits");
        metadata(pm.get(PACKAGE));
        List<ApkPackage> reloaded = (List<ApkPackage>) method(installerType, "loadInstalled").invoke(null);
        require(reloaded.size() == 1, "Cluster vanished on registry reload");
        loadFeature(metadata(reloaded.get(0)));
        File external = new File(data, "external"); external.mkdirs();
        File externalBase = new File(external, "base.apk"); Files.write(externalBase.toPath(), inputs.get("base"));
        Files.write(new File(external, "split_unrelated.apk").toPath(), inputs.get("foreign"));
        require(ApkPackage.parse(externalBase.getPath()).applicationInfo(0).splitNames == null,
                "Single-APK parse absorbed an unrelated sibling APK");
        try {
            method(ApkPackage.class, "parseCodeDirectory", String.class).invoke(null, external.getPath());
            throw new AssertionError("External directory accepted as installed package code");
        } catch (InvocationTargetException expected) {
            require(expected.getCause() instanceof SecurityException, "External cluster failed for the wrong reason");
        }
        invalidUpdates(data, inputs, good, pm, installer, install);
        System.out.println("SplitPackageContractTest all passed: " + checks + " checks");
    }
    private static void invalidUpdates(File data, Map<String, byte[]> inputs, Map<String, byte[]> good,
            PackageManagerService pm, Object installer, Method install) throws Exception {
        byte[] registry = Files.readAllBytes(new File(data, "system/packages.list").toPath());
        ApkPackage before = pm.get(PACKAGE);
        for (String bad : new String[]{"missing-dependency", "duplicate", "foreign", "core-major"}) {
            Map<String, byte[]> files = new LinkedHashMap<>(good);
            if (bad.equals("missing-dependency")) files.remove("split_z_core.apk");
            else if (bad.equals("duplicate")) files.put("split_duplicate.apk", inputs.get("core"));
            else files.put("split_z_core.apk", inputs.get(bad));
            try {
                install.invoke(installer, cluster(data, bad, files).getPath());
                throw new AssertionError("Invalid split set replaced working install: " + bad);
            } catch (InvocationTargetException expected) {
                require(expected.getCause() instanceof Exception, "Wrong invalid-cluster failure: " + bad);
            }
            require(pm.get(PACKAGE) == before && Arrays.equals(registry,
                    Files.readAllBytes(new File(data, "system/packages.list").toPath())),
                    "Invalid split set changed published package/registry: " + bad);
            File[] generations = new File(data, "app").listFiles();
            require(generations != null && generations.length == 1,
                    "Invalid split set left a partial code generation: " + bad);
        }
    }
}
