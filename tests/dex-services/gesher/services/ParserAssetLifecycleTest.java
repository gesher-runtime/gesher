/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package gesher.services;

import android.content.pm.parsing.ApkLite;
import android.content.pm.parsing.ApkLiteParseUtils;
import android.content.pm.parsing.PackageLite;
import android.content.pm.parsing.result.ParseResult;
import android.content.pm.parsing.result.ParseTypeImpl;
import android.content.res.ApkAssets;
import android.content.res.AssetManager;
import android.content.res.XmlResourceParser;
import android.util.SparseArray;
import com.android.internal.pm.split.DefaultSplitAssetLoader;
import com.android.internal.pm.split.SplitAssetDependencyLoader;
import com.android.internal.pm.split.SplitAssetLoader;
import java.io.File;
import java.io.IOException;
import java.lang.reflect.Field;
import java.nio.file.Files;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collections;
import java.util.IdentityHashMap;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.Set;
import java.util.zip.ZipFile;
import org.xmlpull.v1.XmlPullParser;

/** Real private ApkAssets ownership, including failed split loads and source truncation. */
public final class ParserAssetLifecycleTest {
    private static int checks;

    private static void require(boolean value, String reason) {
        if (!value) throw new AssertionError(reason);
        checks++;
    }

    private static Field field(Class<?> type, String name) throws Exception {
        Field value = type.getDeclaredField(name);
        value.setAccessible(true);
        return value;
    }

    private static long pointer(ApkAssets assets) throws Exception {
        return field(ApkAssets.class, "mNativePtr").getLong(assets);
    }

    private static List<ApkAssets> privateAssets(AssetManager manager, ApkAssets[] shared) {
        Set<ApkAssets> system = Collections.newSetFromMap(new IdentityHashMap<>());
        Collections.addAll(system, shared);
        List<ApkAssets> result = new ArrayList<>();
        for (ApkAssets asset : manager.getApkAssets()) {
            if (!system.contains(asset)) result.add(asset);
        }
        return result;
    }

    @SuppressWarnings("unchecked")
    private static List<ApkAssets> partiallyLoaded(SplitAssetLoader loader) throws Exception {
        // Retain the objects across close, even when no AssetManager was published.
        // This inspects ownership only; it never closes an asset on the loader's behalf.
        return new ArrayList<>((List<ApkAssets>) field(loader.getClass(), "mOwnedApkAssets").get(loader));
    }

    private static PackageLite paths(ApkLite base, PackageLite original, String[] splits) {
        return new PackageLite(original.getPath(), original.getBaseApkPath(), base,
                original.getSplitNames(), original.getIsFeatureSplits(), original.getUsesSplitNames(),
                original.getConfigForSplit(), splits, original.getSplitRevisionCodes(),
                original.getTargetSdk(), original.getRequiredSplitTypes(), original.getSplitTypes());
    }

    private static void manifest(AssetManager assets, String path) throws Exception {
        int cookie = assets.findCookieForPath(path);
        require(cookie != 0, "Loaded APK has no asset cookie: " + path);
        try (XmlResourceParser parser = assets.openXmlResourceParser(cookie, "AndroidManifest.xml")) {
            int event;
            do { event = parser.next(); }
            while (event != XmlPullParser.START_TAG && event != XmlPullParser.END_DOCUMENT);
            require("manifest".equals(parser.getName()), "Actual compiled manifest did not open");
        }
    }

    private static void closeAndRewrite(SplitAssetLoader loader, List<ApkAssets> owned,
            List<AssetManager> managers, ApkAssets[] shared, Map<String, byte[]> sources)
            throws Exception {
        long[] sharedPointers = new long[shared.length];
        for (int i = 0; i < shared.length; i++) {
            sharedPointers[i] = pointer(shared[i]);
            require(sharedPointers[i] != 0, "Shared system asset was already closed");
        }
        for (ApkAssets asset : owned) require(pointer(asset) != 0, "Private asset closed while in use");
        loader.close();
        for (AssetManager manager : managers) {
            require(field(AssetManager.class, "mObject").getLong(manager) == 0,
                    "Loader left a manager native reference alive");
        }
        for (ApkAssets asset : owned) {
            require(pointer(asset) == 0, "Loader left a private APK mapping alive after close");
        }
        loader.close();
        for (int i = 0; i < shared.length; i++) {
            require(pointer(shared[i]) == sharedPointers[i], "Loader destroyed borrowed system assets");
        }
        // Keep both the loader and every private ApkAssets strongly reachable while exercising
        // the same CREATE/TRUNCATE_EXISTING write that previously failed in Installer's fixture.
        for (Map.Entry<String, byte[]> source : sources.entrySet()) {
            File file = new File(source.getKey());
            Files.write(file.toPath(), new byte[]{'P', 'K', 3, 4});
            require(Files.size(file.toPath()) == 4, "Source APK was not actually truncated");
            Files.write(file.toPath(), source.getValue());
            require(Arrays.equals(Files.readAllBytes(file.toPath()), source.getValue()),
                    "Source APK content did not survive replacement");
        }
        for (ApkAssets asset : owned) require(pointer(asset) == 0, "Closed private asset was revived");
    }

    /** Shared host/ART assertions. The host boundary cannot decode compiled resource XML. */
    public static int runOwnership(ApkLite base, PackageLite pkg, boolean readRealXml) throws Exception {
        checks = 0;
        ApkAssets[] system = AssetManager.getSystem().getApkAssets();
        require(system.length > 0, "Fixture needs a real shared system-asset owner");
        require(pkg.getSplitApkPaths().length == 2, "Fixture must contain two ordered splits");
        Map<String, byte[]> sources = new LinkedHashMap<>();
        for (String path : pkg.getAllApkPaths()) sources.put(path, Files.readAllBytes(new File(path).toPath()));

        DefaultSplitAssetLoader flat = new DefaultSplitAssetLoader(pkg, 0);
        AssetManager flatManager = flat.getBaseAssetManager();
        List<ApkAssets> flatAssets = privateAssets(flatManager, system);
        require(flatAssets.size() == 3 && flatAssets.get(0) == flat.getBaseApkAssets(),
                "Default loader lost its private base/split assets");
        require(flat.getBaseAssetManager() == flatManager && flat.getSplitAssetManager(1) == flatManager,
                "Default loader did not reuse its published manager");
        if (readRealXml) manifest(flatManager, pkg.getBaseApkPath());
        closeAndRewrite(flat, flatAssets, List.of(flatManager), system, sources);

        SparseArray<int[]> dependencies = SplitAssetDependencyLoader.createDependenciesFromPackage(pkg);
        SplitAssetDependencyLoader isolated = new SplitAssetDependencyLoader(pkg, dependencies, 0);
        AssetManager entry = isolated.getSplitAssetManager(1);
        AssetManager core = isolated.getSplitAssetManager(0);
        AssetManager baseManager = isolated.getBaseAssetManager();
        List<ApkAssets> isolatedAssets = privateAssets(entry, system);
        require(isolatedAssets.size() == 3 && privateAssets(core, system).size() == 2
                && privateAssets(baseManager, system).size() == 1,
                "Isolated loader did not construct the actual parent dependency chain");
        require(isolatedAssets.get(0) == isolated.getBaseApkAssets()
                && isolatedAssets.get(0) == privateAssets(baseManager, system).get(0)
                && isolatedAssets.get(1) == privateAssets(core, system).get(1),
                "Dependency arrays lost shared private asset identity");
        if (readRealXml) manifest(entry, pkg.getSplitApkPaths()[1]);
        closeAndRewrite(isolated, isolatedAssets, List.of(entry, core, baseManager), system, sources);

        String[] brokenPaths = pkg.getSplitApkPaths().clone();
        brokenPaths[1] += ".missing.apk";
        PackageLite broken = paths(base, pkg, brokenPaths);
        Map<String, byte[]> openedSources = new LinkedHashMap<>(sources);
        openedSources.remove(pkg.getSplitApkPaths()[1]);
        for (boolean isolatedLoad : new boolean[]{false, true}) {
            SplitAssetLoader loader = isolatedLoad
                    ? new SplitAssetDependencyLoader(broken, dependencies, 0)
                    : new DefaultSplitAssetLoader(broken, 0);
            try {
                if (isolatedLoad) loader.getSplitAssetManager(1); else loader.getBaseAssetManager();
                throw new AssertionError("Missing split unexpectedly loaded");
            } catch (IllegalArgumentException expected) {
                require(expected.getCause() instanceof IOException,
                        "Missing split did not preserve the real I/O failure");
            }
            List<ApkAssets> loaded = partiallyLoaded(loader);
            require(loaded.size() == 2, "Failed split lost already acquired private ownership");
            List<AssetManager> managers = new ArrayList<>();
            if (isolatedLoad) {
                managers.add(loader.getBaseAssetManager());
                managers.add(loader.getSplitAssetManager(0));
            }
            closeAndRewrite(loader, loaded, managers, system, openedSources);
        }

        // Exercise a base-plus-config load that fails before its first manager is published.
        // The explicit dependency graph is this loader's normal input; APK bytes stay real.
        SparseArray<int[]> configs = new SparseArray<>();
        configs.put(0, new int[]{-1, 1, 2});
        SplitAssetDependencyLoader failedBase = new SplitAssetDependencyLoader(broken, configs, 0);
        try {
            failedBase.getBaseAssetManager();
            throw new AssertionError("Missing base configuration unexpectedly loaded");
        } catch (IllegalArgumentException expected) {
            require(expected.getCause() instanceof IOException, "Configuration I/O failure was lost");
        }
        List<ApkAssets> pending = partiallyLoaded(failedBase);
        require(pending.size() == 2, "Unpublished base/config assets lost ownership");
        closeAndRewrite(failedBase, pending, Collections.emptyList(), system, openedSources);
        return checks;
    }

    public static void main(String[] args) throws Exception {
        if (args.length != 1) throw new IllegalArgumentException("Pass split-package-fixtures.zip");
        System.loadLibrary("android_runtime");
        AssetManager.createSystemAssetsInZygoteLocked(false,
                System.getenv("ANDROID_ROOT") + "/framework/framework-res.apk");
        File directory = Files.createTempDirectory("gesher-parser-assets-").toFile();
        try (ZipFile zip = new ZipFile(args[0])) {
            for (String name : new String[]{"base", "core", "entry"}) {
                Files.write(new File(directory, name + ".apk").toPath(),
                        zip.getInputStream(zip.getEntry(name + ".apk")).readAllBytes());
            }
        }
        ParseResult<PackageLite> parsed = ApkLiteParseUtils.parsePackageLite(
                ParseTypeImpl.forParsingWithoutPlatformCompat(), directory, 0);
        if (parsed.isError()) throw new AssertionError("Real cluster-lite parse failed: " + parsed.getErrorMessage());
        ParseResult<ApkLite> base = ApkLiteParseUtils.parseApkLite(
                ParseTypeImpl.forParsingWithoutPlatformCompat(), new File(directory, "base.apk"), 0);
        if (base.isError()) throw new AssertionError("Real base-lite parse failed: " + base.getErrorMessage());
        int result = runOwnership(base.getResult(), parsed.getResult(), true);
        for (File file : directory.listFiles()) Files.delete(file.toPath());
        Files.delete(directory.toPath());
        System.out.println("ParserAssetLifecycleTest all passed: " + result);
    }
}
