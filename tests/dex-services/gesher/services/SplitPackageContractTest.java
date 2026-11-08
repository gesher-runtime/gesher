/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package gesher.services;

import android.app.LoadedApk;
import android.content.ComponentName;
import android.content.pm.ApplicationInfo;
import android.content.pm.ComponentInfo;
import android.content.pm.PackageInfo;
import android.content.pm.PackageInstaller;
import android.content.pm.PackageManager;
import android.content.pm.PackageParser;
import android.content.pm.ProviderInfo;
import android.content.res.ApkAssets;
import android.content.res.AssetManager;
import android.content.res.CompatibilityInfo;
import android.content.res.Configuration;
import android.content.res.Resources;
import android.os.Looper;
import android.util.DisplayMetrics;
import com.android.internal.os.ApplicationSharedMemory;
import com.gesher.server.ApkPackage;
import com.gesher.server.PackageManagerService;
import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.OutputStream;
import java.lang.reflect.Constructor;
import java.lang.reflect.Field;
import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Method;
import java.lang.reflect.Proxy;
import java.nio.file.Files;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collections;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.concurrent.Executor;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicReference;
import java.util.function.Consumer;
import java.util.function.Supplier;
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
                && ai.sourceDir.equals(component.applicationInfo.sourceDir)
                && component.applicationInfo.flags == ai.flags
                && component.applicationInfo.privateFlags == ai.privateFlags,
                "Feature component changed package ownership: " + suffix);
    }
    private static ApplicationInfo metadata(ApkPackage pkg) {
        PackageInfo info = pkg.packageInfo(ApkPackage.ALL);
        ApplicationInfo ai = info.applicationInfo;
        int publicFlags = ApplicationInfo.FLAG_DEBUGGABLE | ApplicationInfo.FLAG_LARGE_HEAP
                | ApplicationInfo.FLAG_USES_CLEARTEXT_TRAFFIC | ApplicationInfo.FLAG_HARDWARE_ACCELERATED
                | ApplicationInfo.FLAG_EXTRACT_NATIVE_LIBS | ApplicationInfo.FLAG_SUPPORTS_RTL
                | ApplicationInfo.FLAG_INSTALLED | ApplicationInfo.FLAG_HAS_CODE;
        int privateFlags = ApplicationInfo.PRIVATE_FLAG_DIRECT_BOOT_AWARE
                | ApplicationInfo.PRIVATE_FLAG_DEFAULT_TO_DEVICE_PROTECTED_STORAGE
                | ApplicationInfo.PRIVATE_FLAG_ISOLATED_SPLIT_LOADING
                | ApplicationInfo.PRIVATE_FLAG_ACTIVITIES_RESIZE_MODE_UNRESIZEABLE;
        require((ai.flags & publicFlags) == publicFlags
                && (ai.privateFlags & privateFlags) == privateFlags,
                "Parsed manifest policy flags were not published in ApplicationInfo");
        require((ai.flags & (ApplicationInfo.FLAG_ALLOW_BACKUP | ApplicationInfo.FLAG_SYSTEM)) == 0
                && (ai.privateFlags & ApplicationInfo.PRIVATE_FLAG_PRIVILEGED) == 0,
                "Absent or disabled manifest/install capabilities were invented");
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
        // Installer publishes PackageManager cache nonces on every actual update.
        // This isolated server VM needs the same writable native region that
        // SystemServer establishes before it accepts the first installation.
        ApplicationSharedMemory.setInstance(ApplicationSharedMemory.create());
        require(ApplicationSharedMemory.getInstance().isMutable(),
                "Split installer fixture did not acquire writable native cache memory");
        AssetManager.createSystemAssetsInZygoteLocked(false,
                System.getenv("ANDROID_ROOT") + "/framework/framework-res.apk");
        if (Looper.myLooper() == null) Looper.prepareMainLooper();
        File data = Files.createTempDirectory("gesher-split-package-").toFile();
        android.system.Os.setenv("ANDROID_DATA", data.getPath(), true);
        android.system.Os.setenv("GESHER_MICROG_SPOOF", "0", true);
        Map<String, byte[]> inputs = new LinkedHashMap<>();
        try (ZipFile zip = new ZipFile(args[0])) {
            for (String name : new String[]{"base", "core", "entry", "entry-revised", "core-major", "foreign",
                    "base-required", "core-typed", "base-required-signed"}) {
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
        cachedSourceUpdates(data, inputs);
        System.out.println("SplitPackageContractTest all passed: " + checks + " checks");
    }

    private static String[] codeGenerations(File data) {
        String[] names = new File(data, "app").list();
        if (names == null) return new String[0];
        Arrays.sort(names);
        return names;
    }

    /** Real filesystem additions/replacements at the exact cached base APK path. */
    private static void cachedSourceUpdates(File parent, Map<String, byte[]> inputs) throws Exception {
        File data = new File(parent, "cached-source-device");
        File source = new File(data, "source");
        require(source.mkdirs(), "Could not create cached-source fixture");
        android.system.Os.setenv("ANDROID_DATA", data.getPath(), true);
        File base = new File(source, "base.apk");
        File core = new File(source, "split_z_core.apk");
        File entry = new File(source, "split_a_entry.apk");
        Files.write(base.toPath(), inputs.get("base"));
        long baseTimestamp = base.lastModified();
        long baseLength = base.length();
        PackageManagerService pm = new PackageManagerService(Collections.emptyList());
        field(PackageManagerService.class, "mPackageEventSender").set(pm,
                (Consumer<android.content.Intent>) event -> {});
        Class<?> atmType = Class.forName("com.gesher.server.ActivityTaskManagerService");
        Object atm = construct(atmType, new Class<?>[]{PackageManagerService.class}, pm);
        Class<?> installerType = Class.forName("com.gesher.server.Installer");
        Object installer = construct(installerType, new Class<?>[]{PackageManagerService.class, atmType, Map.class},
                pm, atm, new LinkedHashMap<>());
        Method install = method(installerType, "install", String.class);
        String[] result = (String[]) install.invoke(installer, base.getPath());
        require("installed".equals(result[1]) && pm.get(PACKAGE).applicationInfo(0).splitNames == null,
                "Base-only source was not initially installed without features");
        require(new File(data, "system/install-sources.list").isFile(), "Source cache was not populated");

        Files.write(core.toPath(), inputs.get("core"));
        result = (String[]) install.invoke(installer, base.getPath());
        require("updated".equals(result[1]) && Arrays.equals(
                pm.get(PACKAGE).applicationInfo(0).splitNames, new String[]{"feature.core"}),
                "Source cache hid a new feature beside an unchanged base");
        Files.write(entry.toPath(), inputs.get("entry"));
        result = (String[]) install.invoke(installer, base.getPath());
        require("updated".equals(result[1]), "Source cache hid a second feature addition");
        loadFeature(metadata(pm.get(PACKAGE)));
        String beforePath = result[2];
        result = (String[]) install.invoke(installer, base.getPath());
        require("unchanged".equals(result[1]) && beforePath.equals(result[2]),
                "Validated identical split input unnecessarily replaced its code generation");

        Files.write(entry.toPath(), inputs.get("entry-revised"));
        result = (String[]) install.invoke(installer, base.getPath());
        require("updated".equals(result[1]) && !beforePath.equals(result[2])
                && Arrays.equals(pm.get(PACKAGE).packageInfo(0).splitRevisionCodes, new int[]{0, 7}),
                "Source cache hid a valid same-path feature replacement");
        loadFeature(metadata(pm.get(PACKAGE)));

        ApkPackage before = pm.get(PACKAGE);
        byte[] registry = Files.readAllBytes(new File(data, "system/packages.list").toPath());
        String[] generations = codeGenerations(data);
        Files.write(core.toPath(), inputs.get("core-major"));
        try {
            install.invoke(installer, base.getPath());
            throw new AssertionError("Cached base bypassed validation of an invalid replacement split");
        } catch (InvocationTargetException expected) {
            require(expected.getCause() instanceof IOException,
                    "Invalid cached-source split failed outside cluster validation");
        }
        require(pm.get(PACKAGE) == before && Arrays.equals(registry,
                Files.readAllBytes(new File(data, "system/packages.list").toPath())),
                "Invalid cached-source update replaced the published package or registry");
        require(Arrays.equals(generations, codeGenerations(data)),
                "Invalid cached-source update left a partial code generation");
        Files.write(core.toPath(), inputs.get("core"));
        result = (String[]) install.invoke(installer, base.getPath());
        require("unchanged".equals(result[1]), "Restoring the installed feature did not recover");
        metadata(pm.get(PACKAGE));
        require(base.length() == baseLength && base.lastModified() == baseTimestamp,
                "Cache regression accidentally modified the base source");

        Files.delete(core.toPath());
        Files.delete(entry.toPath());
        require(method(installerType, "recordedSource", File.class).invoke(installer, base) == null,
                "A cached source with no companions skipped the installed split inventory");
        result = (String[]) install.invoke(installer, base.getPath());
        require("unchanged".equals(result[1]),
                "Opening the same lone base stopped preserving its installed feature set");
        metadata(pm.get(PACKAGE));

        // PackageInstallerService supports MODE_FULL_INSTALL only. Its empty
        // auxiliary metadata is still an explicit complete APK set, not an
        // instruction to inherit the previously installed optional splits.
        Class<?> metadataType = Class.forName("com.gesher.server.InstallSessionMetadata");
        Object sessionMetadata = construct(metadataType, new Class<?>[]{File.class}, source);
        Method fullInstall = method(installerType, "install", String.class, metadataType);
        result = (String[]) fullInstall.invoke(installer, base.getPath(), sessionMetadata);
        require("updated".equals(result[1]) && pm.get(PACKAGE).applicationInfo(0).splitNames == null
                && pm.getProviderInfo(new ComponentName(PACKAGE, PACKAGE + ".FeatureProvider"), 0, 0) == null
                && pm.getActivityInfo(new ComponentName(PACKAGE, PACKAGE + ".FeatureActivity"), 0, 0) == null,
                "Full installation retained optional split code or components from its predecessor");

        Files.write(core.toPath(), inputs.get("core"));
        Files.write(entry.toPath(), inputs.get("entry"));
        result = (String[]) install.invoke(installer, base.getPath());
        require("updated".equals(result[1]), "Feature set could not be installed after full replacement");
        metadata(pm.get(PACKAGE));
        result = (String[]) install.invoke(installer, cluster(data, "base-only-full",
                Collections.singletonMap("base.apk", inputs.get("base"))).getPath());
        require("updated".equals(result[1]) && pm.get(PACKAGE).applicationInfo(0).splitNames == null,
                "A complete base-only archive accidentally inherited old optional splits");

        // A base that explicitly requires split types may not be published without
        // those types, even when no uses-split dependency makes parsing itself fail.
        Files.write(base.toPath(), inputs.get("base-required"));
        Files.write(core.toPath(), inputs.get("core-typed"));
        result = (String[]) fullInstall.invoke(installer, base.getPath(), sessionMetadata);
        require("updated".equals(result[1]), "Complete required-split set was not installed");
        loadFeature(metadata(pm.get(PACKAGE)));
        Files.delete(core.toPath());
        Files.delete(entry.toPath());
        result = (String[]) install.invoke(installer, base.getPath());
        require("unchanged".equals(result[1]), "Raw required base lost its working split set");
        before = pm.get(PACKAGE);
        registry = Files.readAllBytes(new File(data, "system/packages.list").toPath());
        generations = codeGenerations(data);
        for (String mode : new String[]{"full-session", "archive-without-types", "archive-without-splits"}) {
            try {
                if (mode.equals("full-session")) {
                    fullInstall.invoke(installer, base.getPath(), sessionMetadata);
                } else {
                    Map<String, byte[]> incomplete = new LinkedHashMap<>();
                    incomplete.put("base.apk", inputs.get("base-required"));
                    if (mode.equals("archive-without-types")) {
                        incomplete.put("split_z_core.apk", inputs.get("core"));
                    }
                    install.invoke(installer, cluster(data, mode, incomplete).getPath());
                }
                throw new AssertionError("Required split set was silently discarded: " + mode);
            } catch (InvocationTargetException expected) {
                require(expected.getCause() instanceof PackageParser.PackageParserException
                        && ((PackageParser.PackageParserException) expected.getCause()).error
                                == PackageManager.INSTALL_FAILED_MISSING_SPLIT
                        && expected.getCause().getMessage().startsWith("INSTALL_FAILED_MISSING_SPLIT:"),
                        "Incomplete required split set failed outside required-split validation: " + mode);
            }
            require(pm.get(PACKAGE) == before && Arrays.equals(registry,
                    Files.readAllBytes(new File(data, "system/packages.list").toPath()))
                    && Arrays.equals(generations, codeGenerations(data)),
                    "Missing required split changed package state or leaked staging files: " + mode);
        }
        loadFeature(metadata(pm.get(PACKAGE)));
        missingSplitSession(data, inputs.get("base-required-signed"), pm, atm, installer);
    }

    /** Real signed session input must preserve the validation category in its callback. */
    private static void missingSplitSession(File data, byte[] base,
            PackageManagerService pm, Object atm, Object installer) throws Exception {
        Class<?> serviceType = Class.forName("com.gesher.server.PackageInstallerService");
        Class<?> consentType = Class.forName(serviceType.getName() + "$Consent");
        Class<?> transportType = Class.forName("com.gesher.server.InstallFileBridge$Transport");
        Object consent = Proxy.newProxyInstance(consentType.getClassLoader(),
                new Class<?>[]{consentType}, (proxy, called, values) -> true);
        Object transport = Proxy.newProxyInstance(transportType.getClassLoader(),
                new Class<?>[]{transportType}, (proxy, called, values) -> {
                    if (called.getName().equals("create")) {
                        return android.os.ParcelFileDescriptor.createSocketPair();
                    }
                    ((android.os.ParcelFileDescriptor) values[0]).close();
                    return null;
                });
        List<Runnable> jobs = new ArrayList<>();
        android.content.pm.IPackageInstaller service = (android.content.pm.IPackageInstaller)
                construct(serviceType, new Class<?>[]{PackageManagerService.class,
                        Executor.class, consentType, Supplier.class, transportType},
                        pm, (Executor) jobs::add, consent, (Supplier<String>) () -> "android", transport);
        method(serviceType, "attach", atm.getClass(), installer.getClass()).invoke(service, atm, installer);
        PackageInstaller.SessionParams params = new PackageInstaller.SessionParams(
                PackageInstaller.SessionParams.MODE_FULL_INSTALL);
        params.appPackageName = PACKAGE;
        int id = service.createSession(params, "android", null, 0);
        PackageInstaller.Session session = new PackageInstaller.Session(service.openSession(id));
        try (OutputStream out = session.openWrite("base.apk", 0, base.length)) {
            out.write(base);
            session.fsync(out);
        }
        ApkPackage before = pm.get(PACKAGE);
        byte[] registry = Files.readAllBytes(new File(data, "system/packages.list").toPath());
        String[] generations = codeGenerations(data);
        AtomicReference<android.content.Intent> result = new AtomicReference<>();
        AtomicInteger callbacks = new AtomicInteger();
        android.content.IntentSender receiver = new android.content.IntentSender(
                (android.content.IIntentSender) new android.content.IIntentSender.Stub() {
                    @Override public void send(int code, android.content.Intent intent, String type,
                            android.os.IBinder token, android.content.IIntentReceiver finished,
                            String permission, android.os.Bundle options) {
                        callbacks.incrementAndGet();
                        result.set(intent);
                    }
                });
        session.commit(receiver);
        require(result.get() == null && jobs.size() == 1,
                "Missing-split session completion was not queued through the installer");
        jobs.remove(0).run();
        android.content.Intent status = result.get();
        require(callbacks.get() == 1 && status != null
                        && status.getIntExtra(PackageInstaller.EXTRA_STATUS, -1)
                                == PackageInstaller.STATUS_FAILURE_INCOMPATIBLE
                        && status.getIntExtra(PackageInstaller.EXTRA_SESSION_ID, -1) == id
                        && PACKAGE.equals(status.getStringExtra(PackageInstaller.EXTRA_PACKAGE_NAME))
                        && status.getStringExtra(PackageInstaller.EXTRA_STATUS_MESSAGE)
                                .startsWith("INSTALL_FAILED_MISSING_SPLIT:"),
                "Missing required split was reported as a storage error or lost callback identity");
        require(pm.get(PACKAGE) == before && Arrays.equals(registry,
                        Files.readAllBytes(new File(data, "system/packages.list").toPath()))
                        && Arrays.equals(generations, codeGenerations(data)),
                "Failed missing-split session altered the installed package or its code generations");
        require(service.getSessionInfo(id) == null && jobs.isEmpty(),
                "Failed missing-split session did not retire exactly once");
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
